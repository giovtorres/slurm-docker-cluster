#!/usr/bin/env python3
# -*- coding: utf-8 -*-

DOCUMENTATION = r"""
---
module: slurm_wait_drain
short_description: Wait for Slurm node to fully drain
version_added: "1.0.0"
description:
    - Waits for all jobs to complete on a draining node
    - Returns when node reaches DRAINED state
    - Supports timeout and polling interval configuration
options:
    name:
        description:
            - Node name to wait for
        required: true
        type: str
    timeout:
        description:
            - Maximum seconds to wait for drain
        required: false
        type: int
        default: 600
    poll_interval:
        description:
            - Seconds between state checks
        required: false
        type: int
        default: 10
    fail_on_timeout:
        description:
            - Whether to fail if timeout is reached
            - If false, returns with timed_out=true
        required: false
        type: bool
        default: true
author:
    - Your Team
"""

EXAMPLES = r"""
- name: Wait for node to drain (default 10 min timeout)
  slurm.node.slurm_wait_drain:
    name: compute01
  register: drain_result

- name: Wait with custom timeout
  slurm.node.slurm_wait_drain:
    name: compute01
    timeout: 1800  # 30 minutes
    poll_interval: 30
  register: drain_result

- name: Wait but don't fail on timeout
  slurm.node.slurm_wait_drain:
    name: compute01
    timeout: 300
    fail_on_timeout: false
  register: drain_result

- name: Handle timeout gracefully
  block:
    - slurm.node.slurm_wait_drain:
        name: compute01
        timeout: 600
  rescue:
    - name: Force drain if timeout
      slurm.node.slurm_node_state:
        name: compute01
        state: down
        reason: "Forced down after drain timeout"
"""

RETURN = r"""
drained:
    description: Whether node successfully reached drained state
    type: bool
    returned: always
timed_out:
    description: Whether operation timed out
    type: bool
    returned: always
elapsed:
    description: Seconds elapsed during wait
    type: float
    returned: always
final_state:
    description: Node state when operation completed
    type: str
    returned: always
jobs_remaining:
    description: Jobs still running when operation completed (0 if drained)
    type: int
    returned: always
message:
    description: Human-readable result
    type: str
    returned: always
"""

import time
import subprocess
from ansible.module_utils.basic import AnsibleModule


def run_command(cmd: list[str]) -> tuple[int, str, str]:
    """Execute command and return results."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return 1, "", str(e)


def get_node_state(node: str) -> str:
    """Get current state of a node."""
    rc, stdout, stderr = run_command(["sinfo", "-h", "-n", node, "-o", "%T"])
    if rc != 0:
        return "unknown"
    return stdout.strip().lower()


def get_running_jobs(node: str) -> int:
    """Get count of running jobs on a node."""
    rc, stdout, stderr = run_command([
        "squeue", "-h", "-w", node, "-t", "running", "-o", "%i"
    ])
    if rc != 0:
        return -1
    return len([line for line in stdout.strip().split("\n") if line])


def main():
    module = AnsibleModule(
        argument_spec=dict(
            name=dict(type="str", required=True),
            timeout=dict(type="int", default=600),
            poll_interval=dict(type="int", default=10),
            fail_on_timeout=dict(type="bool", default=True),
        ),
        supports_check_mode=True,
    )

    name = module.params["name"]
    timeout = module.params["timeout"]
    poll_interval = module.params["poll_interval"]
    fail_on_timeout = module.params["fail_on_timeout"]

    # Check mode just verifies node exists
    if module.check_mode:
        state = get_node_state(name)
        if state == "unknown":
            module.fail_json(msg=f"Node {name} not found")
        module.exit_json(
            changed=False,
            drained="drain" in state,
            timed_out=False,
            elapsed=0,
            final_state=state,
            jobs_remaining=get_running_jobs(name),
            message=f"Check mode: node currently in state {state}"
        )

    start_time = time.time()
    last_job_count = -1

    while True:
        elapsed = time.time() - start_time
        state = get_node_state(name)
        jobs = get_running_jobs(name)

        # Log progress if job count changed
        if jobs != last_job_count:
            last_job_count = jobs

        # Check if drained
        if "drained" in state or (state in ["idle", "down"] and jobs == 0):
            module.exit_json(
                changed=False,
                drained=True,
                timed_out=False,
                elapsed=round(elapsed, 2),
                final_state=state,
                jobs_remaining=0,
                message=f"Node {name} drained successfully in {round(elapsed, 1)}s"
            )

        # Check timeout
        if elapsed >= timeout:
            result = dict(
                changed=False,
                drained=False,
                timed_out=True,
                elapsed=round(elapsed, 2),
                final_state=state,
                jobs_remaining=jobs,
                message=f"Timeout after {timeout}s. Node {name} still has {jobs} running jobs"
            )

            if fail_on_timeout:
                module.fail_json(**result)
            else:
                module.exit_json(**result)

        # Check for unexpected states
        if "down" in state and "drain" not in state:
            module.fail_json(
                changed=False,
                drained=False,
                timed_out=False,
                elapsed=round(elapsed, 2),
                final_state=state,
                jobs_remaining=jobs,
                msg=f"Node {name} went DOWN unexpectedly during drain"
            )

        time.sleep(poll_interval)


if __name__ == "__main__":
    main()
