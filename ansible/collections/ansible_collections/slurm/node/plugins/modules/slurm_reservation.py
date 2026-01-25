#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright: (c) 2024
# GNU General Public License v3.0+

DOCUMENTATION = r"""
---
module: slurm_reservation
short_description: Create, modify, or delete Slurm maintenance reservations
version_added: "1.1.0"
description:
    - Manage Slurm reservations for maintenance windows
    - Create reservations to block job scheduling on nodes
    - Modify existing reservations (extend time, add/remove nodes)
    - Delete reservations when maintenance is complete
    - Supports idempotent operations and check mode
options:
    name:
        description:
            - Unique name for the reservation
            - Used to identify the reservation for updates/deletes
        required: true
        type: str
    state:
        description:
            - Desired state of the reservation
        required: false
        type: str
        choices:
            - present
            - absent
        default: present
    nodes:
        description:
            - Node name or hostlist pattern to include
            - Supports Slurm hostlist syntax (e.g., node[1-10])
        required: false
        type: str
    partition:
        description:
            - Reserve entire partition(s) instead of specific nodes
        required: false
        type: str
    start_time:
        description:
            - Reservation start time
            - Format: YYYY-MM-DDTHH:MM:SS or 'now'
        required: false
        type: str
        default: 'now'
    duration:
        description:
            - Reservation duration in minutes
            - Alternative to end_time
        required: false
        type: int
    end_time:
        description:
            - Reservation end time
            - Format: YYYY-MM-DDTHH:MM:SS
            - Alternative to duration
        required: false
        type: str
    users:
        description:
            - Comma-separated list of users allowed to use reservation
            - Use 'root' or 'operator' for maintenance
        required: false
        type: str
        default: 'root'
    accounts:
        description:
            - Comma-separated list of accounts allowed to use reservation
        required: false
        type: str
    flags:
        description:
            - Comma-separated reservation flags
            - Common flags: MAINT, IGNORE_JOBS, OVERLAP, FLEX
        required: false
        type: str
        default: 'MAINT,IGNORE_JOBS'
    features:
        description:
            - Required node features
        required: false
        type: str
    core_count:
        description:
            - Number of cores to reserve (alternative to nodes)
        required: false
        type: int
author:
    - Your Team
notes:
    - Requires Slurm operator or admin privileges
    - MAINT flag prevents jobs from starting on reserved nodes
    - IGNORE_JOBS allows reservation even if jobs are running
"""

EXAMPLES = r"""
- name: Create 2-hour maintenance reservation
  slurm.node.slurm_reservation:
    name: maint_compute01
    nodes: compute01
    duration: 120
    flags: MAINT,IGNORE_JOBS

- name: Reserve multiple nodes for patching
  slurm.node.slurm_reservation:
    name: patch_wave1
    nodes: "compute[01-10]"
    start_time: "2024-01-15T22:00:00"
    duration: 240
    flags: MAINT

- name: Reserve entire partition overnight
  slurm.node.slurm_reservation:
    name: gpu_maintenance
    partition: gpu
    start_time: "2024-01-15T20:00:00"
    end_time: "2024-01-16T06:00:00"
    flags: MAINT,IGNORE_JOBS

- name: Extend existing reservation
  slurm.node.slurm_reservation:
    name: maint_compute01
    duration: 180
    state: present

- name: Delete reservation after maintenance
  slurm.node.slurm_reservation:
    name: maint_compute01
    state: absent

- name: Create reservation allowing specific user
  slurm.node.slurm_reservation:
    name: debug_session
    nodes: debug01
    duration: 60
    users: admin,developer
    flags: FLEX
"""

RETURN = r"""
changed:
    description: Whether the reservation was created/modified/deleted
    type: bool
    returned: always
reservation:
    description: Reservation details
    type: dict
    returned: when state=present
    contains:
        name:
            description: Reservation name
            type: str
        nodes:
            description: Reserved nodes
            type: str
        start_time:
            description: Start time
            type: str
        end_time:
            description: End time
            type: str
        state:
            description: Reservation state (ACTIVE, INACTIVE)
            type: str
        users:
            description: Allowed users
            type: str
        flags:
            description: Reservation flags
            type: str
message:
    description: Human-readable result message
    type: str
    returned: always
previous_state:
    description: Reservation state before operation (exists/absent)
    type: str
    returned: always
"""

import subprocess
from datetime import datetime, timedelta
from ansible.module_utils.basic import AnsibleModule


def run_command(cmd: list[str]) -> tuple[int, str, str]:
    """Execute a command and return rc, stdout, stderr."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr
    except Exception as e:
        return 1, "", str(e)


def get_reservation(name: str) -> dict | None:
    """Get details of an existing reservation."""
    rc, stdout, stderr = run_command(["scontrol", "show", "reservation", name])
    if rc != 0 or "not found" in stderr.lower() or "not found" in stdout.lower():
        return None

    reservation = {}
    for line in stdout.split("\n"):
        for item in line.split():
            if "=" in item:
                key, _, value = item.partition("=")
                reservation[key.lower()] = value

    if not reservation:
        return None

    return {
        "name": reservation.get("reservationname", name),
        "nodes": reservation.get("nodes", ""),
        "partition": reservation.get("partitionname", ""),
        "start_time": reservation.get("starttime", ""),
        "end_time": reservation.get("endtime", ""),
        "duration": reservation.get("duration", ""),
        "state": reservation.get("state", ""),
        "users": reservation.get("users", ""),
        "accounts": reservation.get("accounts", ""),
        "flags": reservation.get("flags", ""),
        "features": reservation.get("features", ""),
        "core_cnt": reservation.get("corecnt", ""),
    }


def create_reservation(params: dict) -> tuple[bool, str]:
    """Create a new reservation."""
    cmd = ["scontrol", "create", "reservation", f"ReservationName={params['name']}"]

    # Add nodes or partition
    if params.get("nodes"):
        cmd.append(f"Nodes={params['nodes']}")
    elif params.get("partition"):
        cmd.append(f"PartitionName={params['partition']}")
    elif params.get("core_count"):
        cmd.append(f"CoreCnt={params['core_count']}")
    else:
        return False, "Must specify nodes, partition, or core_count"

    # Start time
    start = params.get("start_time", "now")
    cmd.append(f"StartTime={start}")

    # Duration or end time
    if params.get("duration"):
        cmd.append(f"Duration={params['duration']}")
    elif params.get("end_time"):
        cmd.append(f"EndTime={params['end_time']}")
    else:
        return False, "Must specify duration or end_time"

    # Users and accounts
    if params.get("users"):
        cmd.append(f"Users={params['users']}")
    if params.get("accounts"):
        cmd.append(f"Accounts={params['accounts']}")

    # Flags
    if params.get("flags"):
        cmd.append(f"Flags={params['flags']}")

    # Features
    if params.get("features"):
        cmd.append(f"Features={params['features']}")

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return False, f"Failed to create reservation: {stderr}"

    return True, "Reservation created successfully"


def update_reservation(name: str, params: dict) -> tuple[bool, str]:
    """Update an existing reservation."""
    cmd = ["scontrol", "update", f"ReservationName={name}"]

    # Only include fields that were explicitly set
    if params.get("nodes"):
        cmd.append(f"Nodes={params['nodes']}")
    if params.get("duration"):
        cmd.append(f"Duration={params['duration']}")
    if params.get("end_time"):
        cmd.append(f"EndTime={params['end_time']}")
    if params.get("users"):
        cmd.append(f"Users={params['users']}")
    if params.get("accounts"):
        cmd.append(f"Accounts={params['accounts']}")
    if params.get("flags"):
        cmd.append(f"Flags={params['flags']}")

    # Don't update if no changes
    if len(cmd) <= 2:
        return False, "No updates specified"

    rc, stdout, stderr = run_command(cmd)
    if rc != 0:
        return False, f"Failed to update reservation: {stderr}"

    return True, "Reservation updated successfully"


def delete_reservation(name: str) -> tuple[bool, str]:
    """Delete an existing reservation."""
    rc, stdout, stderr = run_command(["scontrol", "delete", f"ReservationName={name}"])
    if rc != 0:
        return False, f"Failed to delete reservation: {stderr}"
    return True, "Reservation deleted successfully"


def main():
    module = AnsibleModule(
        argument_spec=dict(
            name=dict(type="str", required=True),
            state=dict(type="str", default="present", choices=["present", "absent"]),
            nodes=dict(type="str"),
            partition=dict(type="str"),
            start_time=dict(type="str", default="now"),
            duration=dict(type="int"),
            end_time=dict(type="str"),
            users=dict(type="str", default="root"),
            accounts=dict(type="str"),
            flags=dict(type="str", default="MAINT,IGNORE_JOBS"),
            features=dict(type="str"),
            core_count=dict(type="int"),
        ),
        supports_check_mode=True,
        mutually_exclusive=[
            ["duration", "end_time"],
            ["nodes", "partition"],
        ],
        required_if=[
            ["state", "present", ["duration", "end_time"], True],  # One of these required
        ],
    )

    name = module.params["name"]
    state = module.params["state"]

    # Get current state
    existing = get_reservation(name)
    previous_state = "exists" if existing else "absent"

    result = {
        "changed": False,
        "message": "",
        "previous_state": previous_state,
    }

    # Handle absent state
    if state == "absent":
        if not existing:
            result["message"] = f"Reservation {name} does not exist"
            module.exit_json(**result)

        if module.check_mode:
            result["changed"] = True
            result["message"] = f"Would delete reservation {name}"
            module.exit_json(**result)

        success, msg = delete_reservation(name)
        if success:
            result["changed"] = True
            result["message"] = msg
            module.exit_json(**result)
        else:
            module.fail_json(msg=msg, **result)

    # Handle present state
    params = {
        "name": name,
        "nodes": module.params["nodes"],
        "partition": module.params["partition"],
        "start_time": module.params["start_time"],
        "duration": module.params["duration"],
        "end_time": module.params["end_time"],
        "users": module.params["users"],
        "accounts": module.params["accounts"],
        "flags": module.params["flags"],
        "features": module.params["features"],
        "core_count": module.params["core_count"],
    }

    if existing:
        # Update existing reservation
        if module.check_mode:
            result["changed"] = True
            result["message"] = f"Would update reservation {name}"
            result["reservation"] = existing
            module.exit_json(**result)

        success, msg = update_reservation(name, params)
        result["changed"] = success
        result["message"] = msg
        result["reservation"] = get_reservation(name) or existing

        if not success and "No updates" not in msg:
            module.fail_json(msg=msg, **result)
        module.exit_json(**result)
    else:
        # Create new reservation
        if module.check_mode:
            result["changed"] = True
            result["message"] = f"Would create reservation {name}"
            module.exit_json(**result)

        success, msg = create_reservation(params)
        if success:
            result["changed"] = True
            result["message"] = msg
            result["reservation"] = get_reservation(name)
            module.exit_json(**result)
        else:
            module.fail_json(msg=msg, **result)


if __name__ == "__main__":
    main()
