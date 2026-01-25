#!/usr/bin/env python3
# -*- coding: utf-8 -*-

DOCUMENTATION = r"""
---
module: slurm_node_info
short_description: Gather detailed Slurm node information
version_added: "1.0.0"
description:
    - Query comprehensive information about Slurm compute nodes
    - Returns state, resources, jobs, features, and more
    - Useful for pre-maintenance checks and reporting
options:
    name:
        description:
            - Node name or hostlist pattern to query
            - Use '*' or omit for all nodes
        required: false
        type: str
        default: '*'
    state_filter:
        description:
            - Only return nodes in specified state(s)
        required: false
        type: list
        elements: str
        choices: ['idle', 'allocated', 'mixed', 'drain', 'draining', 'drained', 'down', 'unknown']
author:
    - Your Team
"""

EXAMPLES = r"""
- name: Get all node information
  slurm.node.slurm_node_info:
  register: all_nodes

- name: Get specific node info
  slurm.node.slurm_node_info:
    name: compute01
  register: node_info

- name: Get all drained nodes
  slurm.node.slurm_node_info:
    state_filter:
      - drain
      - draining
      - drained
  register: drained_nodes

- name: Check if any nodes need attention
  slurm.node.slurm_node_info:
    state_filter:
      - down
      - drain
  register: problem_nodes

- name: Display nodes needing maintenance
  debug:
    msg: "{{ problem_nodes.nodes | map(attribute='name') | list }}"
"""

RETURN = r"""
nodes:
    description: List of node information dictionaries
    type: list
    returned: always
    elements: dict
    contains:
        name:
            description: Node hostname
            type: str
        state:
            description: Current node state
            type: str
        partition:
            description: Partition(s) the node belongs to
            type: str
        cpus_total:
            description: Total CPUs on node
            type: int
        cpus_alloc:
            description: Currently allocated CPUs
            type: int
        cpus_idle:
            description: Available CPUs
            type: int
        memory_total:
            description: Total memory in MB
            type: int
        memory_alloc:
            description: Allocated memory in MB
            type: int
        memory_free:
            description: Free memory in MB
            type: int
        features:
            description: Node features
            type: list
        gres:
            description: Generic resources (GPUs, etc.)
            type: str
        reason:
            description: Reason for drain/down state
            type: str
        jobs:
            description: Number of jobs running on node
            type: int
        load:
            description: Current CPU load
            type: float
        boot_time:
            description: Last boot timestamp
            type: str
node_count:
    description: Total number of nodes returned
    type: int
    returned: always
state_summary:
    description: Count of nodes by state
    type: dict
    returned: always
"""

import subprocess
from ansible.module_utils.basic import AnsibleModule


def run_command(cmd: list[str]) -> tuple[int, str, str]:
    """Execute command and return results."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return 1, "", str(e)


def parse_node_info(output: str) -> list[dict]:
    """Parse scontrol show node output into list of dicts."""
    nodes = []
    current_node = {}

    for line in output.split("\n"):
        line = line.strip()
        if not line:
            if current_node:
                nodes.append(current_node)
                current_node = {}
            continue

        # Parse key=value pairs
        for item in line.split():
            if "=" in item:
                key, _, value = item.partition("=")
                key = key.lower()

                # Convert known numeric fields
                if key in ["cputot", "cpualloc", "realmemory", "allocmem", "freemem"]:
                    try:
                        value = int(value)
                    except ValueError:
                        pass
                elif key == "cpuload":
                    try:
                        value = float(value)
                    except ValueError:
                        value = 0.0

                current_node[key] = value

    if current_node:
        nodes.append(current_node)

    return nodes


def format_node(raw: dict) -> dict:
    """Format raw node data into clean structure."""
    state = raw.get("state", "unknown").lower()

    return {
        "name": raw.get("nodename", "unknown"),
        "state": state,
        "partition": raw.get("partitions", ""),
        "cpus_total": raw.get("cputot", 0),
        "cpus_alloc": raw.get("cpualloc", 0),
        "cpus_idle": raw.get("cputot", 0) - raw.get("cpualloc", 0),
        "memory_total": raw.get("realmemory", 0),
        "memory_alloc": raw.get("allocmem", 0),
        "memory_free": raw.get("freemem", raw.get("realmemory", 0) - raw.get("allocmem", 0)),
        "features": raw.get("availablefeatures", "").split(",") if raw.get("availablefeatures") else [],
        "gres": raw.get("gres", ""),
        "reason": raw.get("reason", ""),
        "jobs": int(raw.get("runningjobs", 0)) if raw.get("runningjobs", "").isdigit() else 0,
        "load": raw.get("cpuload", 0.0),
        "boot_time": raw.get("boottime", ""),
        "arch": raw.get("arch", ""),
        "os": raw.get("os", ""),
        "weight": raw.get("weight", 1),
    }


def main():
    module = AnsibleModule(
        argument_spec=dict(
            name=dict(type="str", default="*"),
            state_filter=dict(
                type="list",
                elements="str",
                default=[],
                choices=["idle", "allocated", "mixed", "drain", "draining", "drained", "down", "unknown"]
            ),
        ),
        supports_check_mode=True,
    )

    name = module.params["name"]
    state_filter = [s.lower() for s in module.params["state_filter"]]

    # Query node information
    if name == "*":
        cmd = ["scontrol", "show", "node"]
    else:
        cmd = ["scontrol", "show", "node", name]

    rc, stdout, stderr = run_command(cmd)

    if rc != 0:
        module.fail_json(msg=f"Failed to query nodes: {stderr}")

    # Parse and format results
    raw_nodes = parse_node_info(stdout)
    nodes = [format_node(n) for n in raw_nodes]

    # Apply state filter
    if state_filter:
        filtered = []
        for node in nodes:
            node_state = node["state"].lower()
            for filter_state in state_filter:
                if filter_state in node_state:
                    filtered.append(node)
                    break
        nodes = filtered

    # Calculate state summary
    state_summary = {}
    for node in nodes:
        state = node["state"].split("+")[0]  # Handle combined states like "idle+drain"
        state_summary[state] = state_summary.get(state, 0) + 1

    module.exit_json(
        changed=False,
        nodes=nodes,
        node_count=len(nodes),
        state_summary=state_summary,
    )


if __name__ == "__main__":
    main()
