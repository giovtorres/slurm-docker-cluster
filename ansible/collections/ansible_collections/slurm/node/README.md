# Ansible Collection: slurm.node

Ansible collection for managing Slurm HPC cluster nodes. Provides modules for node state management, information gathering, and maintenance operations.

## Installation

```bash
# From local path
ansible-galaxy collection install ./collections/ansible_collections/slurm/node

# Or add to requirements.yml
collections:
  - name: slurm.node
    type: dir
    source: ./collections/ansible_collections/slurm/node
```

## Modules

### slurm_node_state

Manage Slurm node state (drain, resume, down, idle).

```yaml
- name: Drain node for maintenance
  slurm.node.slurm_node_state:
    name: compute01
    state: drain
    reason: "Scheduled patching"

- name: Resume node after maintenance
  slurm.node.slurm_node_state:
    name: compute01
    state: resume

- name: Drain multiple nodes
  slurm.node.slurm_node_state:
    name: "compute[01-10]"
    state: drain
    reason: "Rolling update batch 1"
    wait: true
    wait_timeout: 600
```

**Parameters:**
| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | yes | - | Node name or hostlist pattern |
| `state` | yes | - | Target state: drain, resume, down, idle |
| `reason` | no | "Ansible maintenance" | Reason for state change |
| `wait` | no | false | Wait for state change to complete |
| `wait_timeout` | no | 300 | Timeout when waiting |

### slurm_node_info

Query detailed node information.

```yaml
- name: Get all node info
  slurm.node.slurm_node_info:
  register: nodes

- name: Get drained nodes only
  slurm.node.slurm_node_info:
    state_filter:
      - drain
      - draining
  register: drained

- name: Report problem nodes
  debug:
    msg: "{{ drained.nodes | map(attribute='name') | list }}"
```

**Parameters:**
| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | no | "*" | Node name or pattern |
| `state_filter` | no | [] | Filter by state(s) |

**Returns:**
- `nodes`: List of node details
- `node_count`: Number of nodes
- `state_summary`: Count by state

### slurm_wait_drain

Wait for node to fully drain (all jobs complete).

```yaml
- name: Wait for drain with timeout
  slurm.node.slurm_wait_drain:
    name: compute01
    timeout: 1800
    poll_interval: 30
  register: drain_result

- name: Handle timeout gracefully
  slurm.node.slurm_wait_drain:
    name: compute01
    timeout: 600
    fail_on_timeout: false
  register: result

- name: Force down if timeout
  slurm.node.slurm_node_state:
    name: compute01
    state: down
    reason: "Forced after drain timeout"
  when: result.timed_out
```

**Parameters:**
| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | yes | - | Node name |
| `timeout` | no | 600 | Max seconds to wait |
| `poll_interval` | no | 10 | Seconds between checks |
| `fail_on_timeout` | no | true | Fail task on timeout |

**Returns:**
- `drained`: Whether drain completed
- `timed_out`: Whether timeout occurred
- `elapsed`: Seconds waited
- `jobs_remaining`: Jobs still running

### slurm_job_query

Query job queue information.

```yaml
- name: Get jobs on node
  slurm.node.slurm_job_query:
    node: compute01
    state:
      - running
  register: jobs

- name: Check affected users
  debug:
    msg: "{{ jobs.affected_users | length }} users affected"
```

**Parameters:**
| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `job_id` | no | - | Specific job ID |
| `state` | no | - | Filter by state(s) |
| `user` | no | - | Filter by user |
| `partition` | no | - | Filter by partition |
| `node` | no | - | Filter by node |

## Example Playbook

```yaml
- name: Rolling maintenance
  hosts: slurm_nodes
  serial: 5

  tasks:
    - name: Check jobs before drain
      slurm.node.slurm_job_query:
        node: "{{ inventory_hostname }}"
        state: [running]
      delegate_to: localhost
      register: running_jobs

    - name: Drain node
      slurm.node.slurm_node_state:
        name: "{{ inventory_hostname }}"
        state: drain
        reason: "Maintenance - {{ maintenance_ticket }}"
      delegate_to: localhost

    - name: Wait for jobs to complete
      slurm.node.slurm_wait_drain:
        name: "{{ inventory_hostname }}"
        timeout: "{{ drain_timeout | default(600) }}"
      delegate_to: localhost

    - name: Apply patches
      yum:
        name: "*"
        state: latest

    - name: Resume node
      slurm.node.slurm_node_state:
        name: "{{ inventory_hostname }}"
        state: resume
      delegate_to: localhost
```

## License

GPL-3.0-or-later
