#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Ansible callback plugin for maintenance progress tracking.

Sends real-time progress updates to:
- Prometheus Pushgateway (metrics)
- Webhook endpoints (Slack, Teams, etc.)
- Local JSON file (audit trail)

Enable in ansible.cfg:
    [defaults]
    callback_plugins = ./callbacks
    callback_whitelist = maintenance_progress
"""

import json
import os
import time
from datetime import datetime
from urllib.request import Request, urlopen
from urllib.error import URLError

from ansible.plugins.callback import CallbackBase


class CallbackModule(CallbackBase):
    CALLBACK_VERSION = 2.0
    CALLBACK_TYPE = "notification"
    CALLBACK_NAME = "maintenance_progress"
    CALLBACK_NEEDS_WHITELIST = True

    def __init__(self):
        super().__init__()
        self.start_time = None
        self.maintenance_id = None
        self.total_hosts = 0
        self.completed_hosts = 0
        self.failed_hosts = []
        self.current_phase = "initializing"
        self.host_results = {}

        # Configuration from environment
        self.pushgateway_url = os.environ.get("PROMETHEUS_PUSHGATEWAY", "")
        self.webhook_url = os.environ.get("MAINTENANCE_WEBHOOK", "")
        self.report_file = os.environ.get(
            "MAINTENANCE_REPORT_FILE",
            "/var/log/ansible/maintenance_progress.json"
        )

    def v2_playbook_on_start(self, playbook):
        """Called when playbook starts."""
        self.start_time = time.time()
        self.maintenance_id = os.environ.get(
            "MAINTENANCE_ID",
            datetime.now().strftime("%Y%m%d_%H%M%S")
        )
        self._display.banner(f"MAINTENANCE ID: {self.maintenance_id}")

    def v2_playbook_on_play_start(self, play):
        """Called when a play starts."""
        self.current_phase = play.get_name()
        hosts = play.get_variable_manager().get_inventory().get_hosts(play.hosts)
        self.total_hosts = len(hosts)
        self._update_metrics()

    def v2_runner_on_ok(self, result):
        """Called when a task succeeds."""
        host = result._host.get_name()
        task = result._task.get_name()

        if host not in self.host_results:
            self.host_results[host] = {"status": "in_progress", "tasks": []}

        self.host_results[host]["tasks"].append({
            "name": task,
            "status": "ok",
            "timestamp": datetime.now().isoformat()
        })

        # Check for phase completion markers
        if "RESUME" in task.upper() and "Return node to service" in task:
            self.host_results[host]["status"] = "completed"
            self.completed_hosts += 1
            self._update_metrics()
            self._send_webhook(f"Node {host} maintenance completed")

    def v2_runner_on_failed(self, result, ignore_errors=False):
        """Called when a task fails."""
        host = result._host.get_name()
        task = result._task.get_name()

        if not ignore_errors:
            self.failed_hosts.append(host)
            if host in self.host_results:
                self.host_results[host]["status"] = "failed"
            self._update_metrics()
            self._send_webhook(
                f"ALERT: Maintenance failed on {host} at task: {task}",
                alert=True
            )

    def v2_playbook_on_stats(self, stats):
        """Called when playbook completes."""
        duration = time.time() - self.start_time if self.start_time else 0

        summary = {
            "maintenance_id": self.maintenance_id,
            "duration_seconds": round(duration, 2),
            "total_hosts": self.total_hosts,
            "completed_hosts": self.completed_hosts,
            "failed_hosts": len(self.failed_hosts),
            "failed_host_list": self.failed_hosts,
            "success_rate": round(
                (self.completed_hosts / self.total_hosts * 100)
                if self.total_hosts > 0 else 0,
                2
            ),
            "timestamp": datetime.now().isoformat()
        }

        self._write_report(summary)
        self._push_final_metrics(summary)
        self._send_webhook(
            f"Maintenance {self.maintenance_id} complete: "
            f"{self.completed_hosts}/{self.total_hosts} succeeded, "
            f"{len(self.failed_hosts)} failed"
        )

        self._display.banner("MAINTENANCE SUMMARY")
        self._display.display(json.dumps(summary, indent=2))

    def _update_metrics(self):
        """Push current metrics to Prometheus Pushgateway."""
        if not self.pushgateway_url:
            return

        metrics = f"""# HELP slurm_maintenance_total Total nodes in maintenance batch
# TYPE slurm_maintenance_total gauge
slurm_maintenance_total{{maintenance_id="{self.maintenance_id}"}} {self.total_hosts}

# HELP slurm_maintenance_completed Nodes completed maintenance
# TYPE slurm_maintenance_completed gauge
slurm_maintenance_completed{{maintenance_id="{self.maintenance_id}"}} {self.completed_hosts}

# HELP slurm_maintenance_failed Nodes failed maintenance
# TYPE slurm_maintenance_failed gauge
slurm_maintenance_failed{{maintenance_id="{self.maintenance_id}"}} {len(self.failed_hosts)}

# HELP slurm_maintenance_progress Maintenance progress percentage
# TYPE slurm_maintenance_progress gauge
slurm_maintenance_progress{{maintenance_id="{self.maintenance_id}"}} {(self.completed_hosts / self.total_hosts * 100) if self.total_hosts > 0 else 0}

# HELP slurm_maintenance_phase Current maintenance phase
# TYPE slurm_maintenance_phase gauge
slurm_maintenance_phase{{maintenance_id="{self.maintenance_id}",phase="{self.current_phase}"}} 1
"""

        try:
            url = f"{self.pushgateway_url}/metrics/job/slurm_maintenance/instance/{self.maintenance_id}"
            req = Request(url, data=metrics.encode(), method="POST")
            req.add_header("Content-Type", "text/plain")
            urlopen(req, timeout=5)
        except URLError as e:
            self._display.warning(f"Failed to push metrics: {e}")

    def _push_final_metrics(self, summary):
        """Push final metrics after playbook completes."""
        if not self.pushgateway_url:
            return

        metrics = f"""# HELP slurm_maintenance_duration_seconds Total maintenance duration
# TYPE slurm_maintenance_duration_seconds gauge
slurm_maintenance_duration_seconds{{maintenance_id="{self.maintenance_id}"}} {summary['duration_seconds']}

# HELP slurm_maintenance_success_rate Percentage of nodes successfully maintained
# TYPE slurm_maintenance_success_rate gauge
slurm_maintenance_success_rate{{maintenance_id="{self.maintenance_id}"}} {summary['success_rate']}
"""
        try:
            url = f"{self.pushgateway_url}/metrics/job/slurm_maintenance/instance/{self.maintenance_id}"
            req = Request(url, data=metrics.encode(), method="POST")
            req.add_header("Content-Type", "text/plain")
            urlopen(req, timeout=5)
        except URLError as e:
            self._display.warning(f"Failed to push final metrics: {e}")

    def _send_webhook(self, message, alert=False):
        """Send notification to webhook."""
        if not self.webhook_url:
            return

        payload = {
            "text": message,
            "maintenance_id": self.maintenance_id,
            "timestamp": datetime.now().isoformat(),
            "alert": alert
        }

        try:
            req = Request(
                self.webhook_url,
                data=json.dumps(payload).encode(),
                method="POST"
            )
            req.add_header("Content-Type", "application/json")
            urlopen(req, timeout=5)
        except URLError as e:
            self._display.warning(f"Failed to send webhook: {e}")

    def _write_report(self, summary):
        """Write detailed report to file."""
        report = {
            **summary,
            "host_details": self.host_results
        }

        try:
            os.makedirs(os.path.dirname(self.report_file), exist_ok=True)
            with open(self.report_file, "w") as f:
                json.dump(report, f, indent=2)
        except IOError as e:
            self._display.warning(f"Failed to write report: {e}")
