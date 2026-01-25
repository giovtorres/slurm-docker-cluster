# Slurm Playground

A comprehensive environment for learning Slurm, capacity planning, and configuration testing. Generate workloads, scale clusters from 2-20+ nodes, and observe behavior with full observability.

## Quick Start

```bash
# Initialize the playground (install CLI)
make playground-init

# Start with default 2 nodes
make playground-start

# Or start with more nodes
make playground-start NODES=10

# Or use a preset configuration
make playground-start PROFILE=medium
```

## CLI Tool

The `playground` CLI provides commands for workload generation, scaling, and metrics:

```bash
# Show help
playground --help

# Check cluster status
playground status

# Submit workloads
playground workload cpu --count=10 --duration=60
playground workload memory --count=5 --memory=2G
playground workload burst --jobs=100 --interval=0.5
playground workload workflow --stages=3

# Scale the cluster
playground scale set 10           # Set to 10 standard nodes
playground scale preset medium    # Apply preset configuration
playground scale status          # Show current topology

# Monitor metrics
playground metrics live          # Real-time dashboard
playground metrics report        # Summary report
playground metrics export        # Export to JSON

# Run experiments
playground experiment list       # List available experiments
playground experiment run burst_workload
playground experiment results burst_workload
```

## Directory Structure

```
playground/
├── cli/                    # Python CLI tool
│   ├── slurm_playground/   # Main package
│   └── pyproject.toml      # Package configuration
│
├── jobs/                   # Bash job templates
│   ├── cpu_stress.sh       # CPU-bound workload
│   ├── memory_stress.sh    # Memory-bound workload
│   ├── io_stress.sh        # I/O-bound workload
│   ├── sleep_job.sh        # Simple queue testing
│   └── workflow.sh         # Multi-stage pipeline
│
├── profiles/               # Workload profiles (JSON)
│   ├── burst.json          # Sudden burst workload
│   ├── steady.json         # Steady-state workload
│   └── stress.json         # High-stress workload
│
├── experiments/            # Experiment scenarios
│   ├── burst_workload/     # Burst submission testing
│   ├── queue_pressure/     # Sustained backlog testing
│   └── fair_share/         # Fairness testing
│
├── configs/                # Configuration files
│   ├── slurm.conf.j2       # Jinja2 template
│   └── node_profiles.yml   # Node type definitions
│
└── scale.sh                # Scaling helper script
```

## Scaling Options

### Node Counts
Scale from 2 to 20+ nodes:
```bash
playground scale set 5       # 5 standard nodes
playground scale set 10      # 10 standard nodes
playground scale set 20      # 20 standard nodes
```

### Presets
Use predefined configurations:
```bash
playground scale preset minimal   # 2 standard nodes
playground scale preset small     # 5 standard nodes
playground scale preset medium    # 10 standard + 2 highmem + 2 highcpu
playground scale preset large     # 20 standard + 2 highmem + 2 highcpu + 2 gpu
playground scale preset gpu       # 4 standard + 4 GPU nodes
```

### Node Profiles
Available node types:
- **standard**: 2 CPUs, 1GB RAM
- **highmem**: 2 CPUs, 4GB RAM (highmem feature)
- **highcpu**: 4 CPUs, 2GB RAM (highcpu feature)
- **gpu**: 2 CPUs, 2GB RAM, 1 GPU (simulated)

## Monitoring Stack

Start the full monitoring stack with Prometheus and Grafana:

```bash
# Start monitoring
make playground-metrics

# Access dashboards
# Grafana:    http://localhost:3000 (admin/admin)
# Prometheus: http://localhost:9090
```

The Grafana dashboard includes:
- Queue depth over time
- CPU utilization
- Node state distribution
- Job completion status
- Per-partition metrics

## Workload Generation

### CPU Stress
```bash
playground workload cpu \
    --count=10 \        # Number of jobs
    --cpus=2 \          # CPUs per job
    --duration=60 \     # Duration in seconds
    --intensity=medium  # light, medium, or heavy
```

### Memory Stress
```bash
playground workload memory \
    --count=5 \
    --memory=2G \       # Memory per job
    --duration=120 \
    --pattern=hold      # hold, sequential, or random
```

### Burst Workload
```bash
playground workload burst \
    --jobs=100 \        # Total jobs
    --interval=0.5 \    # Seconds between submissions
    --type=mixed        # sleep, cpu, or mixed
```

### Workflow (Dependencies)
```bash
playground workload workflow \
    --stages=3 \        # Number of stages
    --duration=30       # Duration per stage
```

### Job Arrays
```bash
playground workload array "1-100" --duration=30
playground workload array "1-50%10"  # Max 10 concurrent
```

## Experiments

Run predefined experiments to test cluster behavior:

```bash
# List experiments
playground experiment list

# Run an experiment
playground experiment run burst_workload

# View results
playground experiment results burst_workload

# Compare two runs
playground experiment compare burst_workload queue_pressure

# Create custom experiment
playground experiment create my_experiment
```

### Available Experiments
- **burst_workload**: Tests response to sudden job bursts
- **queue_pressure**: Tests sustained queue backlog handling
- **fair_share**: Tests fair share scheduling between users

## Makefile Targets

```bash
make playground-init      # Install CLI, check dependencies
make playground-start     # Start cluster (NODES=N or PROFILE=name)
make playground-stop      # Stop all containers
make playground-reset     # Reset to default, clear jobs
make playground-shell     # Shell into slurmctld
make playground-logs      # Tail all container logs
make playground-metrics   # Start Prometheus + Grafana
make playground-status    # Show comprehensive status
make playground-scale     # Scale nodes (NODES=N)
make playground-workload  # Submit sample workload
```

## Examples

### Learning Slurm Basics
```bash
# Start minimal cluster
make playground-start

# Submit a simple job
playground workload sleep --count=1 --duration=10

# Watch it run
watch -n1 playground status
```

### Capacity Planning
```bash
# Start large cluster
make playground-start PROFILE=large

# Submit realistic workload
playground workload profile steady

# Monitor performance
playground metrics live
```

### Testing Scheduling Policies
```bash
# Configure fair share
playground scale preset medium

# Run fair share experiment
playground experiment run fair_share

# Analyze results
playground experiment results fair_share
```

### Stress Testing
```bash
# Maximum cluster
make playground-start NODES=20

# Submit stress workload
playground workload profile stress

# Monitor for failures
playground metrics live
```

## Troubleshooting

### Cluster won't start
```bash
# Check logs
make playground-logs

# Reset and try again
make playground-reset
make playground-start
```

### Jobs stuck pending
```bash
# Check node states
playground info

# Check job details
playground jobs --all

# Reset nodes if needed
docker exec slurmctld scontrol update nodename=c[1-10] state=idle
```

### Metrics not showing
```bash
# Restart monitoring stack
docker compose -f docker-compose.yml -f monitoring/docker-compose.monitoring.yml restart

# Check exporter
curl http://localhost:9341/metrics
```
