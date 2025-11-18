# SLURM TUI Monitor

A modern, real-time terminal user interface for monitoring SLURM clusters built with [Textual](https://textual.textualize.io/).

![Version](https://img.shields.io/badge/version-0.1.0-blue)
![Python](https://img.shields.io/badge/python-3.9+-blue)

## Overview

SLURM TUI Monitor provides an intuitive, real-time dashboard for monitoring SLURM clusters directly in your terminal. It connects to the SLURM REST API (slurmrestd) to display:

- **Cluster Overview**: Real-time stats on nodes, jobs, CPU and memory utilization
- **Node Status**: Detailed view of all compute nodes with their current state
- **Job Queue**: Interactive job list with filtering and state information
- **Auto-Refresh**: Configurable automatic data updates
- **Keyboard Navigation**: Efficient vim-style keybindings

## Features

### Dashboard View

- Cluster health summary
- Node availability statistics
- Job queue overview (pending/running/completed)
- CPU utilization with visual progress bars
- Memory utilization tracking
- Real-time resource allocation visualization

### Nodes View

- Sortable table of all cluster nodes
- State information with color coding (idle/allocated/mixed/down)
- CPU allocation per node (used/total)
- Memory allocation per node (used/total GB)
- Partition membership
- Quick identification of problem nodes

### Jobs View

- Complete job queue with filtering
- Job details: ID, user, name, state, partition
- Node allocation information
- Runtime and wait time tracking
- Color-coded job states (pending/running/completed/failed)
- Automatic filtering of completed jobs (configurable)

### Technical Features

- **Async Architecture**: Non-blocking API calls for smooth UI
- **Smart Caching**: Reduces API load with intelligent caching
- **Error Handling**: Graceful degradation and informative error messages
- **Configurable**: YAML config with environment variable overrides
- **Responsive**: Adapts to any terminal size
- **Low Footprint**: Minimal resource usage (~50-100 MB RAM)

## Quick Start

### Installation

```bash
cd slurm-docker-cluster/slurmtui
pip install -e .
```

### Configuration

```bash
# Copy example configuration
cp config.yaml.example config.yaml

# Edit if needed (defaults work for slurm-docker-cluster)
# Default API URL: http://localhost:6820
# Default refresh interval: 5 seconds
```

### Run

```bash
# Start the monitor
slurmtui

# Or with custom config
slurmtui --config /path/to/config.yaml

# Or with environment variables
SLURM_API_URL=http://slurm:6820 slurmtui
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `d` | Switch to Dashboard view |
| `n` | Switch to Nodes view |
| `j` | Switch to Jobs view |
| `r` | Manual refresh (fetch latest data) |
| `q` | Quit application |
| `↑/k` | Navigate up |
| `↓/j` | Navigate down (in tables) |
| `Tab` | Cycle between UI elements |

## Configuration

### Config File (config.yaml)

```yaml
# SLURM API Settings
slurm:
  api_url: "http://localhost:6820"
  api_version: "v0.0.42"  # v0.0.41 for SLURM 24.11.x
  timeout: 10

# Application Settings
app:
  refresh_interval: 5  # seconds (0 to disable)
  theme: "dark"  # or "light"
  log_level: "info"

# Display Settings
display:
  show_completed_jobs: false
  max_jobs_display: 100
  time_format: "%Y-%m-%d %H:%M:%S"
  memory_in_gb: true
```

### Environment Variables

Override configuration with environment variables:

```bash
# SLURM API
export SLURM_API_URL="http://slurmrestd:6820"
export SLURM_API_VERSION="v0.0.42"

# Application
export SLURMTUI_REFRESH=10  # seconds
export SLURMTUI_THEME="light"
export SLURMTUI_DEBUG=1

# Run
slurmtui
```

## Docker Integration

### Option 1: Run Inside slurmctld Container

```bash
# Copy files to container
docker cp slurmtui/ slurmctld:/opt/slurmtui/

# Enter container and install
docker exec -it slurmctld bash
pip3 install textual httpx pydantic pyyaml python-dateutil rich

# Run the monitor
cd /opt/slurmtui
python3 -m slurmtui
```

### Option 2: Dedicated Monitoring Container

Add to `docker-compose.yml`:

```yaml
  slurmmonitor:
    image: slurm-docker-cluster:${SLURM_VERSION:-25.05.3}
    container_name: slurmmonitor
    hostname: slurmmonitor
    volumes:
      - ./slurmtui:/opt/slurmtui:z
      - etc_munge:/etc/munge
    environment:
      - SLURM_API_URL=http://slurmrestd:6820
      - SLURM_API_VERSION=v0.0.42
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

Start and attach:

```bash
docker-compose up -d slurmmonitor
docker attach slurmmonitor

# Detach: Ctrl+P, Ctrl+Q
```

See [DOCKER.md](slurmtui/DOCKER.md) for complete Docker integration guide.

## API Compatibility

| SLURM Version | API Version | Status |
|---------------|-------------|--------|
| 25.05.x       | v0.0.42     | ✅ Tested |
| 24.11.x       | v0.0.41     | ✅ Supported |

## Architecture

```
slurmtui/
├── __main__.py          # CLI entry point
├── app.py               # Main Textual application
├── config.py            # Configuration management
├── api/
│   ├── client.py        # Async SLURM REST API client
│   └── models.py        # Pydantic data models
├── widgets/
│   ├── dashboard.py     # Dashboard overview widget
│   ├── nodes.py         # Node status table
│   └── jobs.py          # Job queue table
└── utils/
    └── formatters.py    # Data formatting utilities
```

## Troubleshooting

### Connection Issues

```bash
# Test API connectivity
curl http://localhost:6820/slurm/v0.0.42/ping

# Check slurmrestd status
docker ps | grep slurmrestd

# View slurmrestd logs
docker logs slurmrestd
```

### Display Issues

```bash
# Ensure proper terminal support
export TERM=xterm-256color

# Try light theme if dark doesn't work
SLURMTUI_THEME=light slurmtui

# Enable debug logging
slurmtui --debug
```

### Performance

If the monitor is slow:

- Increase refresh interval: `SLURMTUI_REFRESH=10`
- Check API response times: Enable debug logging
- Reduce max jobs displayed in config.yaml

## Development

### Setup Development Environment

```bash
cd slurmtui

# Install with dev dependencies
pip install -e ".[dev]"

# Run tests
pytest

# Format code
black slurmtui/

# Lint
ruff check slurmtui/
```

### Project Structure

- **API Client**: Async HTTP client using httpx
- **Data Models**: Pydantic models with validation
- **UI Framework**: Textual for TUI
- **Config**: YAML + environment variables
- **Utilities**: Formatters for human-readable output

## Future Enhancements

- [ ] Job detail modal with full job information
- [ ] Historical charts and trends (sparklines)
- [ ] Search and advanced filtering
- [ ] Multi-cluster support
- [ ] Alert notifications
- [ ] Export to CSV/JSON
- [ ] Database integration for long-term metrics
- [ ] Web export (Textual supports web output)

## Contributing

Contributions are welcome! Areas for improvement:

1. Additional widgets (partitions, QOS, reservations)
2. Enhanced error handling and recovery
3. Unit and integration tests
4. Performance optimizations
5. Accessibility improvements
6. Documentation and examples

## License

Part of the slurm-docker-cluster project. See main repository for license details.

## Support & Resources

- **SLURM REST API**: https://slurm.schedmd.com/rest_api.html
- **Textual Framework**: https://textual.textualize.io/
- **Issues**: https://github.com/giovtorres/slurm-docker-cluster/issues
- **Documentation**: See [README.md](slurmtui/README.md) and [DOCKER.md](slurmtui/DOCKER.md)

## Credits

- Built with [Textual](https://textual.textualize.io/) by Textualize
- Designed for [slurm-docker-cluster](https://github.com/giovtorres/slurm-docker-cluster)
- Uses the SLURM REST API by SchedMD
