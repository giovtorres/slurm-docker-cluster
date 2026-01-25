#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright: (c) 2024
# GNU General Public License v3.0+

DOCUMENTATION = r"""
---
module: slurm_partition
short_description: Query and manage Slurm partition states
version_added: "1.1.0"
description:
    - Query partition information and statistics
    - Set partition state (UP, DOWN, DRAIN, INACTIVE)
    - Get node counts and resource availability per partition
    - Useful for maintenance planning and capacity management
options:
    name:
        description:
            - Partition name to query or manage
            - Use '*' or omit for all partitions
        required: false
        type: str
        default: '*'
    state:
        description:
            - Desired state of the partition
            - Only used when modifying partition state
            - Query-only if not specified
        required: false
        type: str
        choices:
            - up
            - down
            - drain
            - inactive
    reason:
        description:
            - Reason for state change
            - Recorded for audit purposes
        required: false
        type: str
        default: "Ansible maintenance"
    default:
        description:
            - Set partition as default for job submission
        required: false
        type: bool
    priority:
        description:
            - Scheduling priority for the partition
        required: false
        type: int
    max_time:
        description:
            - Maximum job time limit (in minutes or UNLIMITED)
        required: false
        type: str
    allow_accounts:
        description:
            - Accounts allowed to use this partition
            - Use 'ALL' to allow all accounts
        required: false
        type: str
    deny_accounts:
        description:
            - Accounts denied from using this partition
        required: false
        type: str
author:
    - Your Team
notes:
    - Requires Slurm admin privileges for state changes
    - Query operations work with operator privileges
"""

EXAMPLES = r"""
- name: Get all partition information
  slurm.node.slurm_partition:
  register: all_partitions

- name: Get specific partition info
  slurm.node.slurm_partition:
    name: compute
  register: compute_partition

- name: Drain entire partition for maintenance
  slurm.node.slurm_partition:
    name: gpu
    state: drain
    reason: "GPU firmware update"

- name: Bring partition back online
  slurm.node.slurm_partition:
    name: gpu
    state: up

- name: Disable partition during outage
  slurm.node.slurm_partition:
    name: interactive
    state: down
    reason: "Network maintenance"

- name: Check partition capacity before maintenance
  slurm.node.slurm_partition:
    name: compute
  register: partition_info
- debug:
    msg: >
      Partition {{ partition_info.partitions[0].name }} has
      {{ partition_info.partitions[0].nodes_idle }} idle nodes,
      {{ partition_info.partitions[0].nodes_allocated }} allocated

- name: Set partition properties
  slurm.node.slurm_partition:
    name: lowpriority
    priority: 10
    max_time: "4:00:00"
"""

RETURN = r"""
changed:
    description: Whether the partition was modified
    type: bool
    returned: always
partitions:
    description: List of partition information
    type: list
    returned: always
    elements: dict
    contains:
        name:
            description: Partition name
            type: str
        state:
            description: Partition state (UP, DOWN, DRAIN, INACTIVE)
            type: str
        nodes_total:
            description: Total nodes in partition
            type: int
        nodes_idle:
            description: Idle nodes in partition
            type: int
        nodes_allocated:
            description: Allocated nodes in partition
            type: int
        nodes_mixed:
            description: Partially allocated nodes
            type: int
        nodes_down:
            description: Down or unavailable nodes
            type: int
        nodes_draining:
            description: Nodes in draining state
            type: int
        cpus_total:
            description: Total CPUs in partition
            type: int
        cpus_available:
            description: Available CPUs for scheduling
            type: int
        memory_total_mb:
            description: Total memory in partition (MB)
            type: int
        default:
            description: Whether this is the default partition
            type: bool
        max_time:
            description: Maximum job time limit
            type: str
        node_list:
            description: List of nodes in partition
            type: str
        allow_accounts:
            description: Accounts allowed to use partition
            type: str
        deny_accounts:
            description: Accounts denied from partition
            type: str
        priority:
            description: Partition scheduling priority
            type: int
partition_count:
    description: Number of partitions returned
    type: int
    returned: always
message:
    description: Human-readable result message
    type: str
    returned: always
"""

import subprocess
from ansible.module_utils.basic import AnsibleModule


def run_command(cmd: list[str]) -> tuple[int, str, str]:
    """Execute a command and return rc, stdout, stderr."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return 1, "", str(e)


def get_partition_info(name: str = "*") -> list[dict]:
    """Get detailed partition information."""
    if name == "*":
        cmd = ["scontrol", "show", "partition"]
    else:
        cmd = ["scontrol", "show", "partition", name]

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return []

    partitions = []
    current = {}

    for line in stdout.split("\n"):
        line = line.strip()
        if not line:
            if current:
                partitions.append(current)
                current = {}
            continue

        for item in line.split():
            if "=" in item:
                key, _, value = item.partition("=")
                current[key.lower()] = value

    if current:
        partitions.append(current)

    return partitions


def get_partition_node_stats(partition_name: str) -> dict:
    """Get node statistics for a partition using sinfo."""
    stats = {
        "nodes_total": 0,
        "nodes_idle": 0,
        "nodes_allocated": 0,
        "nodes_mixed": 0,
        "nodes_down": 0,
        "nodes_draining": 0,
        "cpus_total": 0,
        "cpus_available": 0,
    }

    # Get node counts by state
    # Format: STATE NODES CPUS_A/I/O/T
    cmd = ["sinfo", "-h", "-p", partition_name, "-o", "%T %D %C"]
    rc, stdout, stderr = run_command(cmd)

    if rc != 0:
        return stats

    for line in stdout.strip().split("\n"):
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 3:
            state = parts[0].lower().rstrip("*")
            try:
                nodes = int(parts[1])
            except ValueError:
                nodes = 0

            # Parse CPU info (allocated/idle/other/total)
            cpu_parts = parts[2].split("/")
            if len(cpu_parts) >= 4:
                try:
                    cpus_idle = int(cpu_parts[1])
                    cpus_total = int(cpu_parts[3])
                    stats["cpus_available"] += cpus_idle
                    stats["cpus_total"] += cpus_total
                except ValueError:
                    pass

            stats["nodes_total"] += nodes

            if "idle" in state:
                stats["nodes_idle"] += nodes
            elif "alloc" in state:
                stats["nodes_allocated"] += nodes
            elif "mix" in state:
                stats["nodes_mixed"] += nodes
            elif "drain" in state:
                stats["nodes_draining"] += nodes
            elif "down" in state:
                stats["nodes_down"] += nodes

    return stats


def format_partition(raw: dict) -> dict:
    """Format raw partition data into clean structure."""
    name = raw.get("partitionname", "unknown")
    stats = get_partition_node_stats(name)

    # Parse TotalCPUs and TotalNodes
    try:
        total_cpus = int(raw.get("totalcpus", 0))
    except ValueError:
        total_cpus = stats["cpus_total"]

    try:
        total_nodes = int(raw.get("totalnodes", 0))
    except ValueError:
        total_nodes = stats["nodes_total"]

    # Memory (comes as MB usually)
    try:
        # DefMemPerCPU is in MB
        mem_per_cpu = int(raw.get("defmempercpu", 0))
        total_mem = mem_per_cpu * total_cpus
    except ValueError:
        total_mem = 0

    return {
        "name": name,
        "state": raw.get("state", "UNKNOWN").upper(),
        "nodes_total": total_nodes if total_nodes else stats["nodes_total"],
        "nodes_idle": stats["nodes_idle"],
        "nodes_allocated": stats["nodes_allocated"],
        "nodes_mixed": stats["nodes_mixed"],
        "nodes_down": stats["nodes_down"],
        "nodes_draining": stats["nodes_draining"],
        "cpus_total": total_cpus if total_cpus else stats["cpus_total"],
        "cpus_available": stats["cpus_available"],
        "memory_total_mb": total_mem,
        "default": raw.get("default", "NO") == "YES",
        "max_time": raw.get("maxtime", "UNLIMITED"),
        "node_list": raw.get("nodes", ""),
        "allow_accounts": raw.get("allowaccounts", "ALL"),
        "deny_accounts": raw.get("denyaccounts", ""),
        "priority": int(raw.get("priorityjobfactor", 1)),
        "preempt_mode": raw.get("preemptmode", "OFF"),
        "qos": raw.get("qos", ""),
    }


def set_partition_state(name: str, state: str, reason: str = None) -> tuple[bool, str]:
    """Set partition state."""
    state_map = {
        "up": "UP",
        "down": "DOWN",
        "drain": "DRAIN",
        "inactive": "INACTIVE",
    }

    slurm_state = state_map.get(state.lower())
    if not slurm_state:
        return False, f"Invalid state: {state}"

    cmd = ["scontrol", "update", f"PartitionName={name}", f"State={slurm_state}"]

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return False, f"Failed to set partition state: {stderr}"

    return True, f"Partition {name} state set to {slurm_state}"


def update_partition(name: str, params: dict) -> tuple[bool, str]:
    """Update partition configuration."""
    cmd = ["scontrol", "update", f"PartitionName={name}"]
    updates = []

    if params.get("default") is not None:
        val = "YES" if params["default"] else "NO"
        cmd.append(f"Default={val}")
        updates.append(f"Default={val}")

    if params.get("priority") is not None:
        cmd.append(f"PriorityJobFactor={params['priority']}")
        updates.append(f"Priority={params['priority']}")

    if params.get("max_time"):
        cmd.append(f"MaxTime={params['max_time']}")
        updates.append(f"MaxTime={params['max_time']}")

    if params.get("allow_accounts"):
        cmd.append(f"AllowAccounts={params['allow_accounts']}")
        updates.append(f"AllowAccounts={params['allow_accounts']}")

    if params.get("deny_accounts"):
        cmd.append(f"DenyAccounts={params['deny_accounts']}")
        updates.append(f"DenyAccounts={params['deny_accounts']}")

    if not updates:
        return False, "No updates specified"

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return False, f"Failed to update partition: {stderr}"

    return True, f"Updated partition {name}: {', '.join(updates)}"


def main():
    module = AnsibleModule(
        argument_spec=dict(
            name=dict(type="str", default="*"),
            state=dict(type="str", choices=["up", "down", "drain", "inactive"]),
            reason=dict(type="str", default="Ansible maintenance"),
            default=dict(type="bool"),
            priority=dict(type="int"),
            max_time=dict(type="str"),
            allow_accounts=dict(type="str"),
            deny_accounts=dict(type="str"),
        ),
        supports_check_mode=True,
    )

    name = module.params["name"]
    state = module.params["state"]

    result = {
        "changed": False,
        "partitions": [],
        "partition_count": 0,
        "message": "",
    }

    # Get current partition info
    raw_partitions = get_partition_info(name)

    if not raw_partitions:
        if name != "*":
            module.fail_json(msg=f"Partition {name} not found", **result)
        result["message"] = "No partitions found"
        module.exit_json(**result)

    partitions = [format_partition(p) for p in raw_partitions]
    result["partitions"] = partitions
    result["partition_count"] = len(partitions)

    # If no state or updates requested, this is a query
    has_updates = any([
        state,
        module.params.get("default") is not None,
        module.params.get("priority"),
        module.params.get("max_time"),
        module.params.get("allow_accounts"),
        module.params.get("deny_accounts"),
    ])

    if not has_updates:
        result["message"] = f"Retrieved {len(partitions)} partition(s)"
        module.exit_json(**result)

    # Modification requested - need specific partition
    if name == "*":
        module.fail_json(msg="Must specify partition name for state changes", **result)

    # Check mode
    if module.check_mode:
        result["changed"] = True
        result["message"] = f"Would update partition {name}"
        module.exit_json(**result)

    # Apply state change if requested
    if state:
        success, msg = set_partition_state(name, state, module.params["reason"])
        if not success:
            module.fail_json(msg=msg, **result)
        result["changed"] = True
        result["message"] = msg

    # Apply other updates
    update_params = {
        "default": module.params.get("default"),
        "priority": module.params.get("priority"),
        "max_time": module.params.get("max_time"),
        "allow_accounts": module.params.get("allow_accounts"),
        "deny_accounts": module.params.get("deny_accounts"),
    }

    if any(v is not None for v in update_params.values()):
        success, msg = update_partition(name, update_params)
        if success:
            result["changed"] = True
            if result["message"]:
                result["message"] += "; " + msg
            else:
                result["message"] = msg
        elif "No updates" not in msg:
            module.fail_json(msg=msg, **result)

    # Refresh partition info after changes
    raw_partitions = get_partition_info(name)
    result["partitions"] = [format_partition(p) for p in raw_partitions]

    if not result["message"]:
        result["message"] = "No changes made"

    module.exit_json(**result)


if __name__ == "__main__":
    main()
