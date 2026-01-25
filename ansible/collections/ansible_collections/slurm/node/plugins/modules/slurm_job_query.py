#!/usr/bin/env python3
# -*- coding: utf-8 -*-

DOCUMENTATION = r"""
---
module: slurm_job_query
short_description: Query Slurm job information
version_added: "1.0.0"
description:
    - Query job queue and job details from Slurm
    - Filter by state, user, partition, or node
    - Useful for pre-maintenance checks
options:
    job_id:
        description:
            - Specific job ID to query
        required: false
        type: str
    state:
        description:
            - Filter by job state(s)
        required: false
        type: list
        elements: str
        choices: ['pending', 'running', 'suspended', 'completing', 'completed', 'failed', 'timeout', 'cancelled']
    user:
        description:
            - Filter by username
        required: false
        type: str
    partition:
        description:
            - Filter by partition
        required: false
        type: str
    node:
        description:
            - Filter by node (jobs running on this node)
        required: false
        type: str
author:
    - Your Team
"""

EXAMPLES = r"""
- name: Get all pending jobs
  slurm.node.slurm_job_query:
    state:
      - pending
  register: pending_jobs

- name: Get jobs on specific node
  slurm.node.slurm_job_query:
    node: compute01
  register: node_jobs

- name: Check if safe to drain (no long-running jobs)
  slurm.node.slurm_job_query:
    node: compute01
    state:
      - running
  register: running_jobs

- name: Warn users if jobs will be affected
  debug:
    msg: "{{ running_jobs.job_count }} jobs will need to complete before maintenance"
  when: running_jobs.job_count > 0
"""

RETURN = r"""
jobs:
    description: List of job information
    type: list
    returned: always
    elements: dict
job_count:
    description: Number of jobs matching query
    type: int
    returned: always
affected_users:
    description: Unique users with matching jobs
    type: list
    returned: always
total_cpus:
    description: Total CPUs used by matching jobs
    type: int
    returned: always
"""

import subprocess
from ansible.module_utils.basic import AnsibleModule


def run_command(cmd: list[str]) -> tuple[int, str, str]:
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return 1, "", str(e)


def query_jobs(job_id=None, states=None, user=None, partition=None, node=None) -> list[dict]:
    """Query jobs using squeue."""
    # Build squeue command
    # Format: JobID|User|State|Partition|Nodes|NumCPUs|TimeUsed|TimeLimit|Name
    cmd = ["squeue", "-h", "-o", "%i|%u|%T|%P|%N|%C|%M|%l|%j"]

    if job_id:
        cmd.extend(["-j", job_id])
    if states:
        cmd.extend(["-t", ",".join(states)])
    if user:
        cmd.extend(["-u", user])
    if partition:
        cmd.extend(["-p", partition])
    if node:
        cmd.extend(["-w", node])

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return []

    jobs = []
    for line in stdout.strip().split("\n"):
        if not line:
            continue
        parts = line.split("|")
        if len(parts) >= 9:
            jobs.append({
                "job_id": parts[0],
                "user": parts[1],
                "state": parts[2],
                "partition": parts[3],
                "nodes": parts[4],
                "cpus": int(parts[5]) if parts[5].isdigit() else 0,
                "time_used": parts[6],
                "time_limit": parts[7],
                "name": parts[8],
            })

    return jobs


def main():
    module = AnsibleModule(
        argument_spec=dict(
            job_id=dict(type="str"),
            state=dict(
                type="list",
                elements="str",
                choices=["pending", "running", "suspended", "completing",
                         "completed", "failed", "timeout", "cancelled"]
            ),
            user=dict(type="str"),
            partition=dict(type="str"),
            node=dict(type="str"),
        ),
        supports_check_mode=True,
    )

    jobs = query_jobs(
        job_id=module.params["job_id"],
        states=module.params["state"],
        user=module.params["user"],
        partition=module.params["partition"],
        node=module.params["node"],
    )

    # Calculate summaries
    users = list(set(j["user"] for j in jobs))
    total_cpus = sum(j["cpus"] for j in jobs)

    module.exit_json(
        changed=False,
        jobs=jobs,
        job_count=len(jobs),
        affected_users=users,
        total_cpus=total_cpus,
    )


if __name__ == "__main__":
    main()
