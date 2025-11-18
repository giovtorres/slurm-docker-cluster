# SLURM TUI Monitor

A modern, real-time terminal-based monitoring dashboard for SLURM clusters built with [Textual](https://textual.textualize.io/).

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Python](https://img.shields.io/badge/python-3.9+-blue.svg)

## Features

- **Real-time Monitoring**: Auto-refreshing dashboard with configurable intervals
- **Node Status**: Live view of cluster nodes, CPU/memory allocation
- **Job Queue**: Interactive job list with filtering and sorting
- **Job Details**: Detailed information for any job with a single click
- **Historical Charts**: Visualize trends and cluster performance
- **Keyboard Navigation**: Vim-style bindings for efficient navigation
- **Responsive Design**: Adapts to any terminal size
- **Easy Configuration**: YAML-based configuration with environment variable support

## Screenshots

*(Screenshots will be added once the UI is implemented)*

## Installation

### From Source

```bash
cd slurm-docker-cluster/slurmtui
pip install -e .
```

### Requirements

- Python 3.9 or higher
- Access to SLURM REST API (slurmrestd)

## Quick Start

1. **Copy the example configuration:**
   ```bash
   cp config.yaml.example config.yaml
   ```

2. **Edit configuration** (optional):
   ```yaml
   slurm:
     api_url: "http://localhost:6820"
     api_version: "v0.0.42"

   app:
     refresh_interval: 5
   ```

3. **Run the monitor:**
   ```bash
   slurmtui
   # or
   python -m slurmtui
   ```

## Configuration

### Configuration File

The default configuration file is `config.yaml` in the current directory. You can specify a custom location:

```bash
slurmtui --config /path/to/config.yaml
```

### Environment Variables

Override configuration with environment variables:

- `SLURM_API_URL` - SLURM REST API URL
- `SLURM_API_VERSION` - API version (v0.0.41 or v0.0.42)
- `SLURMTUI_REFRESH` - Refresh interval in seconds
- `SLURMTUI_CONFIG` - Path to configuration file

### Example Configuration

See `config.yaml.example` for all available options.

## Usage

### Keyboard Shortcuts

- **d** - Dashboard view
- **j** - Jobs view
- **n** - Nodes view
- **h** - History view
- **r** - Manual refresh
- **/** - Search/Filter
- **q** - Quit
- **?** - Help

### Navigation

- **↑/k** - Move up
- **↓/j** - Move down
- **Enter** - Select/View details
- **Esc** - Close modal/Go back
- **Tab** - Switch between panels

## Docker Integration

### Running Inside SLURM Container

```bash
# Enter the slurmctld container
docker exec -it slurmctld bash

# Install slurmtui
cd /opt/slurmtui
pip install -e .

# Run the monitor
slurmtui
```

### Standalone Monitoring Container

Add to your `docker-compose.yml`:

```yaml
slurmmonitor:
  image: slurm-docker-cluster:${SLURM_VERSION:-25.05.3}
  container_name: slurmmonitor
  hostname: slurmmonitor
  volumes:
    - ./slurmtui:/opt/slurmtui
  environment:
    - SLURM_API_URL=http://slurmrestd:6820
  command: ["bash", "-c", "cd /opt/slurmtui && pip install -e . && slurmtui"]
  depends_on:
    slurmrestd:
      condition: service_healthy
  networks:
    - slurm-network
  stdin_open: true
  tty: true
```

## Development

### Setup Development Environment

```bash
# Install with dev dependencies
pip install -e ".[dev]"

# Run tests
pytest

# Format code
black slurmtui/

# Lint code
ruff check slurmtui/
```

### Project Structure

```
slurmtui/
├── __init__.py
├── __main__.py              # Entry point
├── app.py                   # Main Textual application
├── config.py                # Configuration management
├── api/
│   ├── client.py            # SLURM REST API client
│   └── models.py            # Pydantic data models
├── widgets/
│   ├── dashboard.py         # Dashboard view
│   ├── nodes.py             # Node status panel
│   ├── jobs.py              # Job queue table
│   ├── job_detail.py        # Job detail modal
│   └── charts.py            # Historical charts
└── utils/
    └── formatters.py        # Formatting utilities
```

## API Compatibility

| SLURM Version | API Version | Status |
|---------------|-------------|--------|
| 25.05.x       | v0.0.42     | ✅ Supported |
| 24.11.x       | v0.0.41     | ✅ Supported |

## Troubleshooting

### Connection Issues

```bash
# Test API connectivity
curl http://localhost:6820/slurm/v0.0.42/ping

# Check slurmrestd is running
docker ps | grep slurmrestd
```

### Permission Issues

Ensure the REST API is accessible. By default, SLURM uses MUNGE for authentication. The monitor may need to run inside a container with MUNGE access.

## Contributing

Contributions are welcome! Please see the main repository for contribution guidelines.

## License

This project is part of the slurm-docker-cluster repository and follows the same licensing terms.

## Acknowledgments

- Built with [Textual](https://textual.textualize.io/) by Textualize
- Designed for [slurm-docker-cluster](https://github.com/giovtorres/slurm-docker-cluster)
- SLURM REST API by SchedMD

## Support

For issues and questions:
- Open an issue on GitHub
- Check the [SLURM documentation](https://slurm.schedmd.com/)
- Review the [Textual documentation](https://textual.textualize.io/)
