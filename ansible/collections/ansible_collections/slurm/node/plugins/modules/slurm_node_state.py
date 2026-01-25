#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright: (c) 2024
# GNU General Public License v3.0+

DOCUMENTATION = r"""
---
module: slurm_node_state
short_description: Manage Slurm node state
version_added: "1.0.0"
description:
    - Drain, resume, or set state of Slurm compute nodes
    - Supports idempotent operations and check mode
    - Tracks maintenance reasons for audit trails
options:
    name:
        description:
            - Node name or comma-separated list of nodes
            - Supports Slurm hostlist syntax (e.g., node[1-10])
        required: true
        type: str
    state:
        description:
            - Desired state of the node(s)
        required: true
        type: str
        choices:
            - drain
            - resume
            - down
            - idle
    reason:
        description:
            - Reason for state change (required for drain/down)
            - Recorded in Slurm and visible in sinfo
        required: false
        type: str
        default: "Ansible maintenance"
    wait:
        description:
            - Wait for node to reach target state
        required: false
        type: bool
        default: false
    wait_timeout:
        description:
            - Timeout in seconds when waiting for state
        required: false
        type: int
        default: 300
author:
    - Your Team
"""

EXAMPLES = r"""
- name: Drain node for maintenance
  slurm.node.slurm_node_state:
    name: compute01
    state: drain
    reason: "Scheduled patching - ticket INC12345"

- name: Drain multiple nodes
  slurm.node.slurm_node_state:
    name: "compute[01-10]"
    state: drain
    reason: "Rolling OS update"
    wait: true
    wait_timeout: 600

- name: Resume node after maintenance
  slurm.node.slurm_node_state:
    name: compute01
    state: resume

- name: Mark node as down
  slurm.node.slurm_node_state:
    name: compute01
    state: down
    reason: "Hardware failure - bad DIMM"
"""

RETURN = r"""
changed:
    description: Whether the node state was changed
    type: bool
    returned: always
previous_state:
    description: Node state before the operation
    type: str
    returned: always
current_state:
    description: Node state after the operation
    type: str
    returned: always
nodes_affected:
    description: List of nodes that were modified
    type: list
    returned: always
message:
    description: Human-readable result message
    type: str
    returned: always
"""

import time
import subprocess
from ansible.module_utils.basic import AnsibleModule


def run_command(cmd: list[str]) -> tuple[int, str, str]:
    """Execute a command and return rc, stdout, stderr."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return 1, "", str(e)


def get_node_state(node: str) -> dict:
    """Get current state of a node."""
    rc, stdout, stderr = run_command(["scontrol", "show", "node", node])
    if rc != 0:
        return {"state": "unknown", "reason": stderr}

    state_info = {"state": "unknown", "reason": ""}
    for line in stdout.split("\n"):
        for item in line.split():
            if item.startswith("State="):
                state_info["state"] = item.split("=")[1].lower()
            elif item.startswith("Reason="):
                state_info["reason"] = item.split("=", 1)[1]

    return state_info


def expand_hostlist(hostlist: str) -> list[str]:
    """Expand Slurm hostlist notation to individual nodes."""
    rc, stdout, stderr = run_command(["scontrol", "show", "hostnames", hostlist])
    if rc != 0:
        return [hostlist]  # Return as-is if expansion fails
    return [h.strip() for h in stdout.strip().split("\n") if h.strip()]


def set_node_state(node: str, state: str, reason: str = None) -> tuple[bool, str]:
    """Set node to specified state."""
    cmd = ["scontrol", "update", f"NodeName={node}"]

    if state == "drain":
        cmd.append("State=DRAIN")
        if reason:
            cmd.append(f"Reason={reason}")
    elif state == "resume":
        cmd.append("State=RESUME")
    elif state == "down":
        cmd.append("State=DOWN")
        if reason:
            cmd.append(f"Reason={reason}")
    elif state == "idle":
        cmd.append("State=IDLE")

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return False, stderr
    return True, "State updated successfully"


def wait_for_state(node: str, target_state: str, timeout: int) -> tuple[bool, str]:
    """Wait for node to reach target state."""
    # Map desired state to possible actual states
    state_map = {
        "drain": ["drained", "draining", "drain"],
        "resume": ["idle", "mixed", "allocated"],
        "down": ["down"],
        "idle": ["idle"],
    }

    acceptable_states = state_map.get(target_state, [target_state])
    start_time = time.time()

    while time.time() - start_time < timeout:
        current = get_node_state(node)
        current_state = current["state"].lower().rstrip("*")

        # Check if current state matches any acceptable state
        for acceptable in acceptable_states:
            if acceptable in current_state:
                return True, f"Node reached state: {current_state}"

        time.sleep(5)

    return False, f"Timeout waiting for state {target_state}"


def main():
    module = AnsibleModule(
        argument_spec=dict(
            name=dict(type="str", required=True),
            state=dict(type="str", required=True, choices=["drain", "resume", "down", "idle"]),
            reason=dict(type="str", default="Ansible maintenance"),
            wait=dict(type="bool", default=False),
            wait_timeout=dict(type="int", default=300),
        ),
        supports_check_mode=True,
    )

    name = module.params["name"]
    state = module.params["state"]
    reason = module.params["reason"]
    wait = module.params["wait"]
    wait_timeout = module.params["wait_timeout"]

    # Expand hostlist
    nodes = expand_hostlist(name)
    if not nodes:
        module.fail_json(msg=f"No nodes found matching: {name}")

    result = {
        "changed": False,
        "nodes_affected": [],
        "previous_state": {},
        "current_state": {},
        "message": "",
    }

    # Check current state of all nodes
    for node in nodes:
        node_state = get_node_state(node)
        result["previous_state"][node] = node_state["state"]

    # Determine if change is needed
    changes_needed = []
    for node in nodes:
        current = result["previous_state"][node].lower()

        # Check if already in desired state
        if state == "drain" and "drain" in current:
            continue
        elif state == "resume" and current in ["idle", "mixed", "allocated"]:
            continue
        elif state == "down" and "down" in current:
            continue
        elif state == "idle" and current == "idle":
            continue

        changes_needed.append(node)

    if not changes_needed:
        result["message"] = f"All nodes already in desired state: {state}"
        result["current_state"] = result["previous_state"].copy()
        module.exit_json(**result)

    # Check mode - report what would change
    if module.check_mode:
        result["changed"] = True
        result["nodes_affected"] = changes_needed
        result["message"] = f"Would change {len(changes_needed)} node(s) to state: {state}"
        module.exit_json(**result)

    # Apply changes
    errors = []
    for node in changes_needed:
        success, msg = set_node_state(node, state, reason)
        if success:
            result["nodes_affected"].append(node)
            result["changed"] = True
        else:
            errors.append(f"{node}: {msg}")

    # Wait for state if requested
    if wait and result["changed"]:
        for node in result["nodes_affected"]:
            success, msg = wait_for_state(node, state, wait_timeout)
            if not success:
                errors.append(f"{node}: {msg}")

    # Get final state
    for node in nodes:
        node_state = get_node_state(node)
        result["current_state"][node] = node_state["state"]

    if errors:
        result["message"] = f"Completed with errors: {'; '.join(errors)}"
        module.fail_json(**result)
    else:
        result["message"] = f"Successfully changed {len(result['nodes_affected'])} node(s) to state: {state}"
        module.exit_json(**result)


if __name__ == "__main__":
    main()
