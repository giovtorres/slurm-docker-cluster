#!/usr/bin/env python3
"""
Slurm Prometheus Exporter
Collects metrics from Slurm and exposes them in Prometheus format.
"""

import http.server
import re
import subprocess
import threading
import time
from typing import Any


class SlurmMetricsCollector:
    """Collects metrics from Slurm commands."""

    def __init__(self):
        self.metrics = {}
        self.lock = threading.Lock()
        self.last_update = 0
        self.cache_ttl = 5  # seconds

    def run_command(self, cmd: list[str]) -> str:
        """Run a shell command and return output."""
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=30,
            )
            return result.stdout if result.returncode == 0 else ""
        except (subprocess.TimeoutExpired, FileNotFoundError):
            return ""

    def collect_queue_metrics(self) -> dict[str, Any]:
        """Collect queue metrics from squeue."""
        metrics = {
            "slurm_queue_pending": 0,
            "slurm_queue_running": 0,
            "slurm_queue_completing": 0,
            "slurm_queue_total": 0,
        }

        output = self.run_command(["squeue", "-h", "-o", "%T"])
        if output:
            for line in output.strip().split("\n"):
                state = line.strip().upper()
                metrics["slurm_queue_total"] += 1
                if state in ("PENDING", "PD"):
                    metrics["slurm_queue_pending"] += 1
                elif state in ("RUNNING", "R"):
                    metrics["slurm_queue_running"] += 1
                elif state in ("COMPLETING", "CG"):
                    metrics["slurm_queue_completing"] += 1

        return metrics

    def collect_node_metrics(self) -> dict[str, Any]:
        """Collect node metrics from sinfo."""
        metrics = {
            "slurm_nodes_total": 0,
            "slurm_nodes_idle": 0,
            "slurm_nodes_allocated": 0,
            "slurm_nodes_mixed": 0,
            "slurm_nodes_down": 0,
            "slurm_nodes_drain": 0,
        }

        output = self.run_command(["sinfo", "-h", "-o", "%T"])
        if output:
            for line in output.strip().split("\n"):
                state = line.strip().lower()
                metrics["slurm_nodes_total"] += 1
                if "idle" in state:
                    metrics["slurm_nodes_idle"] += 1
                elif "alloc" in state:
                    metrics["slurm_nodes_allocated"] += 1
                elif "mix" in state:
                    metrics["slurm_nodes_mixed"] += 1
                elif "down" in state:
                    metrics["slurm_nodes_down"] += 1
                elif "drain" in state:
                    metrics["slurm_nodes_drain"] += 1

        return metrics

    def collect_cpu_metrics(self) -> dict[str, Any]:
        """Collect CPU metrics from sinfo."""
        metrics = {
            "slurm_cpus_total": 0,
            "slurm_cpus_allocated": 0,
            "slurm_cpus_idle": 0,
            "slurm_cpus_other": 0,
        }

        # Format: ALLOCATED/IDLE/OTHER/TOTAL
        output = self.run_command(["sinfo", "-h", "-o", "%C"])
        if output:
            line = output.strip().split("\n")[0]
            parts = line.split("/")
            if len(parts) >= 4:
                try:
                    metrics["slurm_cpus_allocated"] = int(parts[0])
                    metrics["slurm_cpus_idle"] = int(parts[1])
                    metrics["slurm_cpus_other"] = int(parts[2])
                    metrics["slurm_cpus_total"] = int(parts[3])
                except ValueError:
                    pass

        return metrics

    def collect_partition_metrics(self) -> list[dict[str, Any]]:
        """Collect per-partition metrics."""
        partitions = []

        output = self.run_command(["sinfo", "-h", "-o", "%P|%a|%D|%c|%T"])
        if output:
            for line in output.strip().split("\n"):
                parts = line.split("|")
                if len(parts) >= 5:
                    name = parts[0].rstrip("*")
                    partitions.append({
                        "name": name,
                        "available": 1 if parts[1] == "up" else 0,
                        "nodes": int(parts[2]) if parts[2].isdigit() else 0,
                        "cpus_per_node": parts[3],
                        "state": parts[4],
                    })

        return partitions

    def collect_scheduler_metrics(self) -> dict[str, Any]:
        """Collect scheduler-related metrics."""
        metrics = {
            "slurm_scheduler_backfill_depth": 0,
            "slurm_scheduler_last_cycle": 0,
        }

        # Try to get scheduler stats from sdiag
        output = self.run_command(["sdiag"])
        if output:
            for line in output.split("\n"):
                if "Last cycle" in line:
                    match = re.search(r"(\d+)", line)
                    if match:
                        metrics["slurm_scheduler_last_cycle"] = int(match.group(1))
                elif "Depth Mean" in line:
                    match = re.search(r"(\d+)", line)
                    if match:
                        metrics["slurm_scheduler_backfill_depth"] = int(match.group(1))

        return metrics

    def collect_job_metrics(self) -> dict[str, Any]:
        """Collect job completion metrics from sacct."""
        metrics = {
            "slurm_jobs_completed_1h": 0,
            "slurm_jobs_failed_1h": 0,
            "slurm_jobs_cancelled_1h": 0,
            "slurm_jobs_timeout_1h": 0,
        }

        # Jobs in last hour
        output = self.run_command([
            "sacct", "-S", "now-1hour", "-E", "now",
            "-X", "-n", "--format=State", "--noheader"
        ])
        if output:
            for line in output.strip().split("\n"):
                state = line.strip().upper()
                if state == "COMPLETED":
                    metrics["slurm_jobs_completed_1h"] += 1
                elif state == "FAILED":
                    metrics["slurm_jobs_failed_1h"] += 1
                elif state == "CANCELLED":
                    metrics["slurm_jobs_cancelled_1h"] += 1
                elif state == "TIMEOUT":
                    metrics["slurm_jobs_timeout_1h"] += 1

        return metrics

    def collect_all(self) -> dict[str, Any]:
        """Collect all metrics."""
        now = time.time()

        with self.lock:
            # Return cached metrics if still valid
            if now - self.last_update < self.cache_ttl:
                return self.metrics

            # Collect fresh metrics
            self.metrics = {}
            self.metrics.update(self.collect_queue_metrics())
            self.metrics.update(self.collect_node_metrics())
            self.metrics.update(self.collect_cpu_metrics())
            self.metrics.update(self.collect_scheduler_metrics())
            self.metrics.update(self.collect_job_metrics())

            # Partition metrics
            self.metrics["partitions"] = self.collect_partition_metrics()

            # Add utilization percentage
            if self.metrics.get("slurm_cpus_total", 0) > 0:
                self.metrics["slurm_cpu_utilization"] = (
                    self.metrics["slurm_cpus_allocated"] /
                    self.metrics["slurm_cpus_total"] * 100
                )
            else:
                self.metrics["slurm_cpu_utilization"] = 0

            self.last_update = now
            return self.metrics


def format_prometheus_metrics(metrics: dict[str, Any]) -> str:
    """Format metrics in Prometheus exposition format."""
    lines = []

    # Add help and type comments
    metric_help = {
        "slurm_queue_pending": ("gauge", "Number of pending jobs in the queue"),
        "slurm_queue_running": ("gauge", "Number of running jobs"),
        "slurm_queue_completing": ("gauge", "Number of completing jobs"),
        "slurm_queue_total": ("gauge", "Total number of jobs in the queue"),
        "slurm_nodes_total": ("gauge", "Total number of nodes"),
        "slurm_nodes_idle": ("gauge", "Number of idle nodes"),
        "slurm_nodes_allocated": ("gauge", "Number of allocated nodes"),
        "slurm_nodes_mixed": ("gauge", "Number of mixed state nodes"),
        "slurm_nodes_down": ("gauge", "Number of down nodes"),
        "slurm_nodes_drain": ("gauge", "Number of draining nodes"),
        "slurm_cpus_total": ("gauge", "Total number of CPUs"),
        "slurm_cpus_allocated": ("gauge", "Number of allocated CPUs"),
        "slurm_cpus_idle": ("gauge", "Number of idle CPUs"),
        "slurm_cpus_other": ("gauge", "Number of CPUs in other state"),
        "slurm_cpu_utilization": ("gauge", "CPU utilization percentage"),
        "slurm_scheduler_last_cycle": ("gauge", "Last scheduler cycle time in microseconds"),
        "slurm_scheduler_backfill_depth": ("gauge", "Backfill scheduler depth"),
        "slurm_jobs_completed_1h": ("counter", "Jobs completed in last hour"),
        "slurm_jobs_failed_1h": ("counter", "Jobs failed in last hour"),
        "slurm_jobs_cancelled_1h": ("counter", "Jobs cancelled in last hour"),
        "slurm_jobs_timeout_1h": ("counter", "Jobs timed out in last hour"),
    }

    for name, value in metrics.items():
        if name == "partitions":
            # Handle partition metrics separately
            for partition in value:
                pname = partition["name"]
                lines.append(f'slurm_partition_available{{partition="{pname}"}} {partition["available"]}')
                lines.append(f'slurm_partition_nodes{{partition="{pname}"}} {partition["nodes"]}')
            continue

        if name in metric_help:
            mtype, help_text = metric_help[name]
            lines.append(f"# HELP {name} {help_text}")
            lines.append(f"# TYPE {name} {mtype}")

        if isinstance(value, (int, float)):
            lines.append(f"{name} {value}")

    return "\n".join(lines) + "\n"


class MetricsHandler(http.server.BaseHTTPRequestHandler):
    """HTTP handler for Prometheus metrics endpoint."""

    collector = SlurmMetricsCollector()

    def do_GET(self):
        if self.path == "/metrics":
            try:
                metrics = self.collector.collect_all()
                output = format_prometheus_metrics(metrics)

                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.end_headers()
                self.wfile.write(output.encode())
            except Exception as e:
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"Error: {e}\n".encode())
        elif self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"OK\n")
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        """Suppress default logging."""
        pass


def main():
    """Main entry point."""
    port = 9341
    server = http.server.HTTPServer(("0.0.0.0", port), MetricsHandler)
    print(f"Slurm Exporter listening on port {port}")
    print("Endpoints:")
    print(f"  http://localhost:{port}/metrics - Prometheus metrics")
    print(f"  http://localhost:{port}/health  - Health check")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.shutdown()


if __name__ == "__main__":
    main()
