# Docker Integration Guide

This guide explains how to run SLURM TUI Monitor with the slurm-docker-cluster.

## Option 1: Run Inside Existing Container

The simplest way is to run the monitor inside the `slurmctld` container.

### Step 1: Enter the Container

```bash
docker exec -it slurmctld bash
```

### Step 2: Install Dependencies

```bash
# Install Python dependencies
pip3 install textual httpx pydantic pyyaml python-dateutil rich
```

### Step 3: Copy and Configure

```bash
# Create directory for the monitor
mkdir -p /opt/slurmtui
cd /opt/slurmtui

# Copy the slurmtui package from the host
# (Assumes you're in the slurm-docker-cluster directory on the host)
exit  # Exit container first

# From the host, copy files into container
docker cp slurmtui/ slurmctld:/opt/slurmtui/

# Re-enter container
docker exec -it slurmctld bash
cd /opt/slurmtui

# Create config file
cp config.yaml.example config.yaml
```

### Step 4: Run the Monitor

```bash
cd /opt/slurmtui
python3 -m slurmtui
```

The monitor should connect to `http://localhost:6820` automatically since it's running in the same container as slurmrestd.

## Option 2: Run from Host Machine

If you prefer to run the monitor from your host machine, you can connect to the exposed REST API port.

### Step 1: Install on Host

```bash
cd slurm-docker-cluster/slurmtui
pip install -e .
```

### Step 2: Configure

```bash
cp config.yaml.example config.yaml

# Edit config.yaml to use localhost:6820
# The slurmrestd port is already exposed in docker-compose.yml
```

### Step 3: Run

```bash
slurmtui
```

## Option 3: Add a Dedicated Monitoring Container

Add a new service to your `docker-compose.yml`:

```yaml
  slurmmonitor:
    image: slurm-docker-cluster:${SLURM_VERSION:-25.05.3}
    container_name: slurmmonitor
    hostname: slurmmonitor
    volumes:
      - ./slurmtui:/opt/slurmtui:z
      - etc_munge:/etc/munge
      - etc_slurm:/etc/slurm
    environment:
      - SLURM_API_URL=http://slurmrestd:6820
      - SLURM_API_VERSION=v0.0.42
      - SLURMTUI_REFRESH=5
    working_dir: /opt/slurmtui
    command: >
      bash -c "
      pip3 install -q textual httpx pydantic pyyaml python-dateutil rich &&
      python3 -m slurmtui
      "
    depends_on:
      slurmrestd:
        condition: service_healthy
    networks:
      - slurm-network
    stdin_open: true
    tty: true
```

### Using the Dedicated Container

```bash
# Start all services including the monitor
docker-compose up -d

# Attach to the monitor to view the UI
docker attach slurmmonitor

# Detach without stopping (Ctrl+P, Ctrl+Q)

# To stop the monitor
docker-compose stop slurmmonitor
```

## Configuration for Docker

### Environment Variables

When running inside Docker, use environment variables for configuration:

```yaml
environment:
  # SLURM API Configuration
  - SLURM_API_URL=http://slurmrestd:6820
  - SLURM_API_VERSION=v0.0.42

  # Application Settings
  - SLURMTUI_REFRESH=5
  - SLURMTUI_THEME=dark

  # Debug Mode
  - SLURMTUI_DEBUG=0
```

### Network Connectivity

The monitor needs network access to the `slurmrestd` service:

- **Same network**: Ensure the monitor container is on the `slurm-network`
- **Service name**: Use service name `slurmrestd` instead of `localhost`
- **Port**: Default is 6820

### Authentication

By default, slurmrestd uses MUNGE authentication:

- **Option 1**: Run monitor in a container with MUNGE access (mount `/etc/munge`)
- **Option 2**: Configure slurmrestd for token-based authentication
- **Option 3**: Run monitor in a container that's part of the SLURM network

## Troubleshooting

### Connection Refused

**Problem**: `Connection refused` error when connecting to API

**Solutions**:
1. Ensure slurmrestd is running: `docker ps | grep slurmrestd`
2. Check network connectivity: `docker exec slurmmonitor curl http://slurmrestd:6820/slurm/v0.0.42/ping`
3. Verify port mapping: Port 6820 should be exposed in docker-compose.yml

### MUNGE Authentication Errors

**Problem**: Authentication failures with MUNGE

**Solutions**:
1. Mount the munge volume: `- etc_munge:/etc/munge`
2. Ensure munge is running: `docker exec slurmmonitor systemctl status munge`
3. Check munge permissions

### Textual Display Issues

**Problem**: UI looks broken or colors are wrong

**Solutions**:
1. Ensure container has TTY: `tty: true` in docker-compose.yml
2. Use `stdin_open: true` for interactive sessions
3. Set terminal emulator: `export TERM=xterm-256color`

### Auto-Refresh Not Working

**Problem**: Data doesn't refresh automatically

**Solutions**:
1. Check configuration: `SLURMTUI_REFRESH` should be > 0
2. Check logs for errors
3. Verify API connectivity

## Performance Considerations

### Resource Usage

The monitor is lightweight but consider:

- **CPU**: Minimal (< 1% most of the time)
- **Memory**: ~50-100 MB
- **Network**: API calls every N seconds (configurable)

### Optimization

To minimize resource usage:

```yaml
environment:
  # Increase refresh interval for less frequent API calls
  - SLURMTUI_REFRESH=10

  # Disable completed jobs display
  # (Configure in config.yaml: display.show_completed_jobs: false)
```

## Example Makefile Integration

Add monitoring commands to your Makefile:

```makefile
.PHONY: monitor monitor-install monitor-start monitor-stop

monitor-install:
	docker exec -it slurmctld bash -c "pip3 install -q textual httpx pydantic pyyaml python-dateutil rich"

monitor-start:
	docker exec -it slurmctld bash -c "cd /opt && python3 -m slurmtui"

monitor:
	@$(MAKE) monitor-install
	@$(MAKE) monitor-start
```

Usage:
```bash
# Copy slurmtui to container first
docker cp slurmtui/ slurmctld:/opt/slurmtui/

# Run the monitor
make monitor
```

## Best Practices

1. **Use environment variables** for configuration in Docker environments
2. **Mount the slurmtui directory** as a volume for easy updates
3. **Run in a dedicated container** for isolation
4. **Use health checks** to ensure the API is ready before starting the monitor
5. **Log to stdout** for Docker log integration

## Next Steps

- Customize the configuration in `config.yaml`
- Explore keyboard shortcuts (press `?` in the app)
- Set up auto-start with your cluster
- Integrate with monitoring/alerting systems
