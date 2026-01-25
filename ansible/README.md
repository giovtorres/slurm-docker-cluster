# Slurm Rolling Maintenance Automation

Ansible-based rolling maintenance system for Slurm HPC clusters. Provides orchestrated patching, health validation, and emergency controls with full visibility through AWX and Grafana.

## Quick Start

```bash
# Test the dynamic inventory
./inventory/slurm_inventory.py --list

# Dry-run rolling maintenance
ansible-playbook playbooks/rolling_maintenance.yml \
  --check \
  -e "target_nodes=partition_normal" \
  -e "batch_size=5"

# Execute rolling maintenance
ansible-playbook playbooks/rolling_maintenance.yml \
  -e "target_nodes=partition_normal" \
  -e "maintenance_reason='Security patches CVE-2024-XXXX'" \
  -e "batch_size=10"
```

## Directory Structure

```
ansible/
├── ansible.cfg                 # Ansible configuration
├── inventory/
│   └── slurm_inventory.py      # Dynamic inventory from sinfo
├── playbooks/
│   ├── rolling_maintenance.yml # Main rolling update playbook
│   ├── emergency_stop.yml      # Halt all maintenance
│   └── emergency_resume.yml    # Resume all drained nodes
├── roles/
│   └── slurm_maintenance/      # Reusable maintenance tasks
│       ├── tasks/
│       │   ├── main.yml
│       │   ├── drain.yml
│       │   ├── patch.yml
│       │   ├── validate.yml
│       │   └── resume.yml
│       └── vars/
│           └── main.yml
├── collections/
│   └── ansible_collections/
│       └── slurm/node/         # Custom Slurm modules
│           ├── galaxy.yml
│           └── plugins/modules/
│               └── slurm_node_state.py
└── callbacks/
    └── maintenance_progress.py # Progress tracking callback
```

## Key Features

### Rolling Maintenance Playbook

- **Serial execution**: Configurable batch sizes (default: 5 nodes)
- **Safety checks**: Cluster capacity, queue depth validation
- **Full lifecycle**: Drain → Patch → Validate → Resume
- **Error handling**: Failed nodes stay drained for investigation

### Custom Slurm Modules

```yaml
# Drain a node
- slurm.node.slurm_node_state:
    name: compute01
    state: drain
    reason: "Scheduled maintenance"

# Resume after patching
- slurm.node.slurm_node_state:
    name: compute01
    state: resume
```

### Dynamic Inventory Groups

The inventory script creates automatic groups:

| Group | Description |
|-------|-------------|
| `slurm_idle` | Nodes with no jobs |
| `slurm_allocated` | Fully allocated nodes |
| `slurm_mixed` | Partially allocated |
| `slurm_drain` / `slurm_draining` | Draining nodes |
| `slurm_down` | Offline nodes |
| `partition_<name>` | Nodes by partition |
| `slurm_available` | Idle + mixed + allocated |
| `slurm_unavailable` | Drain + down |

### Progress Tracking

Set environment variables to enable tracking:

```bash
export PROMETHEUS_PUSHGATEWAY=http://pushgateway:9091
export MAINTENANCE_WEBHOOK=https://hooks.slack.com/...
export MAINTENANCE_ID=$(date +%Y%m%d_%H%M%S)

ansible-playbook playbooks/rolling_maintenance.yml ...
```

## Emergency Procedures

### Stop All Maintenance
```bash
ansible-playbook playbooks/emergency_stop.yml -e "confirm=yes"
```

### Resume All Drained Nodes
```bash
ansible-playbook playbooks/emergency_resume.yml -e "confirm=yes"
```

## AWX Integration

1. **Create Project**: Point to this Git repo
2. **Create Inventory**: Use `inventory/slurm_inventory.py` as source
3. **Create Job Templates**:
   - Rolling Maintenance (with survey for batch_size, target_nodes)
   - Emergency Stop
   - Emergency Resume

### Recommended Survey Fields

| Field | Type | Default |
|-------|------|---------|
| `target_nodes` | Text | `slurm_idle` |
| `batch_size` | Integer | `5` |
| `maintenance_reason` | Text | `Scheduled maintenance` |
| `drain_timeout` | Integer | `600` |

## Grafana Dashboard

Import `monitoring/grafana/dashboards/maintenance_dashboard.json` for:
- Real-time maintenance progress
- Node state distribution
- Historical success rates
- Maintenance duration metrics

## Configuration Variables

### Role: slurm_maintenance

| Variable | Default | Description |
|----------|---------|-------------|
| `drain_timeout` | `600` | Seconds to wait for drain |
| `drain_reason` | `"Scheduled maintenance"` | Reason for audit |
| `security_only` | `false` | Only security updates |
| `allow_reboot` | `true` | Allow reboot if needed |
| `skip_validation` | `false` | Skip health checks |
| `min_free_memory_mb` | `1024` | Validation threshold |
| `check_gpu` | `false` | Run GPU health checks |

## Team Responsibilities

### Infrastructure Lead
- AWX deployment and configuration
- Dynamic inventory integration
- Grafana dashboard setup
- Credential management

### Automation Engineer
- Playbook development
- Rolling logic and safety checks
- Emergency procedures
- Testing and CI/CD

### Integration Engineer
- Custom Ansible modules
- Health check scripts
- Prometheus metrics integration
- Slurm API integration

## Testing

```bash
# Lint playbooks
ansible-lint playbooks/*.yml

# Test inventory
./inventory/slurm_inventory.py --list | jq .

# Dry-run on dev cluster
ansible-playbook playbooks/rolling_maintenance.yml \
  --check -v \
  -e "target_nodes=slurm_idle" \
  -e "batch_size=2"
```
