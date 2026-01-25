# AWX Setup for Slurm Maintenance

AWX provides scheduling, visibility, and audit logging for Slurm rolling maintenance operations.

## Quick Start

```bash
cd awx

# Full setup (generates credentials, starts services, configures AWX)
./setup_awx.sh

# Or step by step:
./setup_awx.sh --start   # Start services only
./setup_awx.sh --config  # Configure AWX resources
./setup_awx.sh --status  # Show status and credentials
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  AWX Web UI (port 8052)                                     │
│  - Job Templates (Rolling Maintenance, Emergency Stop/Resume)│
│  - Schedules (maintenance windows)                          │
│  - Activity Stream (audit trail)                            │
├─────────────────────────────────────────────────────────────┤
│  AWX Task Workers                                           │
│  - Execute Ansible playbooks                                │
│  - Push metrics to Prometheus                               │
│  - Send webhooks for notifications                          │
├─────────────────────────────────────────────────────────────┤
│  Supporting Services                                        │
│  - PostgreSQL (job history, audit logs)                     │
│  - Redis (task queue, caching)                              │
│  - Pushgateway (Prometheus metrics)                         │
└─────────────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────────────┐
│  Slurm Cluster                                              │
│  - Dynamic inventory from sinfo                             │
│  - SSH access to compute nodes                              │
└─────────────────────────────────────────────────────────────┘
```

## Services

| Service | Port | Description |
|---------|------|-------------|
| AWX Web | 8052 | Web UI and REST API |
| Pushgateway | 9091 | Prometheus metrics endpoint |
| PostgreSQL | 5432 (internal) | Database |
| Redis | 6379 (internal) | Cache/Queue |

## Initial Configuration

The setup script creates:

### Organization
- **Slurm HPC** - Container for all resources

### Project
- **Slurm Maintenance** - Linked to `../ansible` directory

### Inventory
- **Slurm Cluster** - Dynamic inventory from `slurm_inventory.py`

### Job Templates

| Template | Description |
|----------|-------------|
| Rolling Maintenance | Main maintenance workflow with survey |
| EMERGENCY STOP | Halt all maintenance immediately |
| Emergency Resume All | Resume all drained nodes |

## Scheduling Maintenance

### Via Web UI

1. Navigate to **Templates** → **Rolling Maintenance**
2. Click **Schedule** button
3. Configure:
   - Name: "Weekly Security Patches"
   - Start: Select date/time
   - Repeat: Weekly (or as needed)
   - Survey values: batch_size=10, target_nodes=partition_normal

### Via API

```bash
# Get auth token
TOKEN=$(curl -s -X POST "http://localhost:8052/api/v2/tokens/" \
  -u "admin:password" \
  -H "Content-Type: application/json" | jq -r '.token')

# Launch job
curl -X POST "http://localhost:8052/api/v2/job_templates/1/launch/" \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "extra_vars": {
      "target_nodes": "partition_normal",
      "batch_size": 10,
      "maintenance_reason": "Security patches"
    }
  }'
```

## SSH Credentials

For AWX to connect to Slurm nodes:

1. **Option A: Existing key**
   ```bash
   cp ~/.ssh/id_rsa ./credentials/ssh/
   chmod 600 ./credentials/ssh/id_rsa
   ```

2. **Option B: Generate new key**
   ```bash
   ssh-keygen -t rsa -b 4096 -f ./credentials/ssh/id_rsa -N ''
   # Then distribute public key to nodes
   ```

3. **Configure in AWX UI**
   - Go to **Credentials** → **Slurm Nodes SSH**
   - Paste your private key

## Notifications

### Slack Integration

1. Create Slack webhook URL
2. Add to `.env`:
   ```
   MAINTENANCE_WEBHOOK=https://hooks.slack.com/services/XXX/YYY/ZZZ
   ```
3. Restart AWX task container

### Custom Notifications

AWX supports multiple notification types:
- Email
- Slack
- Microsoft Teams
- PagerDuty
- Webhook

Configure via **Administration** → **Notifications** in AWX UI.

## Monitoring Integration

### Prometheus Scraping

Add to your `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'awx-pushgateway'
    static_configs:
      - targets: ['pushgateway:9091']
    honor_labels: true
```

### Grafana

Import the dashboard from:
```
../monitoring/grafana/dashboards/maintenance_dashboard.json
```

## Approval Workflows

For production, set up approval workflows:

1. **Create Approval Template**
   - Job Type: "Check" (dry-run)
   - Enable "Require Approval"

2. **Create Workflow Template**
   ```
   [Approval Request] → [Approval] → [Rolling Maintenance]
   ```

3. **Assign Approvers**
   - Configure in **Access** tab of workflow

## Troubleshooting

### Check Logs
```bash
# All services
docker compose -f docker-compose.awx.yml logs -f

# Specific service
docker compose -f docker-compose.awx.yml logs -f awx-web
docker compose -f docker-compose.awx.yml logs -f awx-task
```

### Reset Admin Password
```bash
docker compose -f docker-compose.awx.yml exec awx-web \
  awx-manage changepassword admin
```

### Database Issues
```bash
# Check PostgreSQL
docker compose -f docker-compose.awx.yml exec awx-postgres \
  psql -U awx -c "SELECT 1"

# Rebuild database (WARNING: destroys data)
docker compose -f docker-compose.awx.yml down -v
./setup_awx.sh
```

### Inventory Not Updating
```bash
# Test inventory script manually
docker compose -f docker-compose.awx.yml exec awx-task \
  /var/lib/awx/projects/slurm-maintenance/inventory/slurm_inventory.py --list
```

## Backup and Restore

### Backup
```bash
# Database
docker compose -f docker-compose.awx.yml exec awx-postgres \
  pg_dump -U awx awx > awx_backup_$(date +%Y%m%d).sql

# Credentials (encrypted)
docker compose -f docker-compose.awx.yml exec awx-web \
  awx-manage dumpdata main.credential > credentials_backup.json
```

### Restore
```bash
# Database
docker compose -f docker-compose.awx.yml exec -T awx-postgres \
  psql -U awx awx < awx_backup_YYYYMMDD.sql
```

## Security Hardening

For production deployments:

1. **Change default credentials immediately**
2. **Enable HTTPS** (add nginx/traefik in front)
3. **Restrict ALLOWED_HOSTS** in settings.py
4. **Configure LDAP/SAML** for authentication
5. **Set up RBAC** for team access control
6. **Enable audit logging** retention policies

## Commands Reference

```bash
# Start
./setup_awx.sh --start

# Stop
./setup_awx.sh --stop

# Status
./setup_awx.sh --status

# Clean (removes all data!)
./setup_awx.sh --clean

# Reconfigure
./setup_awx.sh --config
```
