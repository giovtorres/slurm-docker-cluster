#!/usr/bin/env python3
"""
Dynamic Ansible inventory for Slurm clusters.

Queries sinfo to build inventory grouped by node state and partition.
For AWX: Configure as "Sourced from a Project" inventory source.

Usage:
    ./slurm_inventory.py --list
    ./slurm_inventory.py --host <hostname>
"""

import argparse
import json
import subprocess
import sys
from collections import defaultdict


def run_sinfo(fmt: str) -> list[str]:
    """Run sinfo with specified format and return lines."""
    cmd = ["sinfo", "-h", "-N", "-o", fmt]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return [line.strip() for line in result.stdout.strip().split("\n") if line.strip()]
    except subprocess.CalledProcessError as e:
        print(f"Error running sinfo: {e.stderr}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print("sinfo not found. Is Slurm installed?", file=sys.stderr)
        sys.exit(1)


def get_node_details(hostname: str) -> dict:
    """Get detailed info for a specific node."""
    cmd = ["scontrol", "show", "node", hostname]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        details = {}
        for item in result.stdout.replace("\n", " ").split():
            if "=" in item:
                key, _, value = item.partition("=")
                details[key.lower()] = value
        return details
    except subprocess.CalledProcessError:
        return {}


def build_inventory() -> dict:
    """Build complete Ansible inventory from Slurm."""
    inventory = {
        "_meta": {"hostvars": {}},
        "all": {"children": ["slurm_nodes", "ungrouped"]},
        "slurm_nodes": {"children": []},
    }

    # State-based groups
    state_groups = defaultdict(list)
    partition_groups = defaultdict(list)

    # Query: NodeName State Partition CPUs Memory Features
    lines = run_sinfo("%N|%T|%P|%c|%m|%f")

    for line in lines:
        parts = line.split("|")
        if len(parts) < 6:
            continue

        node, state, partition, cpus, memory, features = parts[:6]

        # Normalize state (remove * suffix, lowercase)
        state_clean = state.rstrip("*").lower()
        partition_clean = partition.rstrip("*")

        # Add to state group
        state_group = f"slurm_{state_clean}"
        state_groups[state_group].append(node)

        # Add to partition group
        part_group = f"partition_{partition_clean}"
        partition_groups[part_group].append(node)

        # Host variables
        inventory["_meta"]["hostvars"][node] = {
            "slurm_state": state_clean,
            "slurm_partition": partition_clean,
            "slurm_cpus": int(cpus) if cpus.isdigit() else cpus,
            "slurm_memory_mb": int(memory) if memory.isdigit() else memory,
            "slurm_features": features.split(",") if features else [],
            "ansible_host": node,  # Can be overridden if DNS differs
        }

    # Add state groups
    for group, hosts in state_groups.items():
        inventory[group] = {"hosts": list(set(hosts))}
        if group not in inventory["slurm_nodes"]["children"]:
            inventory["slurm_nodes"]["children"].append(group)

    # Add partition groups
    for group, hosts in partition_groups.items():
        inventory[group] = {"hosts": list(set(hosts))}
        if group not in inventory["slurm_nodes"]["children"]:
            inventory["slurm_nodes"]["children"].append(group)

    # Convenience groups for maintenance operations
    inventory["slurm_available"] = {
        "children": ["slurm_idle", "slurm_mixed", "slurm_allocated"]
    }
    inventory["slurm_unavailable"] = {
        "children": ["slurm_down", "slurm_drain", "slurm_draining", "slurm_drained"]
    }
    inventory["slurm_needs_attention"] = {
        "children": ["slurm_down", "slurm_fail", "slurm_error"]
    }

    return inventory


def get_host(hostname: str) -> dict:
    """Get variables for a specific host."""
    details = get_node_details(hostname)
    if not details:
        return {}

    return {
        "slurm_state": details.get("state", "unknown").lower(),
        "slurm_partition": details.get("partitions", ""),
        "slurm_cpus": details.get("cputot", 0),
        "slurm_memory_mb": details.get("realmemory", 0),
        "slurm_features": details.get("availablefeatures", "").split(","),
        "slurm_reason": details.get("reason", ""),
        "slurm_alloc_cpus": details.get("cpualloc", 0),
        "slurm_alloc_mem": details.get("allocmem", 0),
        "ansible_host": hostname,
    }


def main():
    parser = argparse.ArgumentParser(description="Slurm dynamic inventory for Ansible")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list", action="store_true", help="List all hosts")
    group.add_argument("--host", help="Get variables for specific host")

    args = parser.parse_args()

    if args.list:
        print(json.dumps(build_inventory(), indent=2))
    elif args.host:
        print(json.dumps(get_host(args.host), indent=2))


if __name__ == "__main__":
    main()
