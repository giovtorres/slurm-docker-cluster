# Podman Support for Slurm Docker Cluster

This document explains how to use Podman as a drop-in replacement for Docker when running the Slurm Docker Cluster.

## Overview

Podman is a daemonless container engine that provides a Docker-compatible CLI. This project supports Podman through its `podman compose` command, which offers Docker Compose compatibility mode.

## Prerequisites

### Install Podman

#### macOS (via Homebrew)

```bash
brew install podman
podman machine init
podman machine start
```

#### Linux (RHEL, Fedora, CentOS, etc.)

```bash
# RHEL/CentOS Stream
sudo dnf install podman podman-compose

# Fedora
sudo dnf install podman podman-compose

# Ubuntu/Debian
sudo apt install podman podman-compose
```

#### Verify Installation

```bash
podman info
podman --version
```

## Quick Start

### Using Podman

If Podman is installed and accessible, the Makefile will automatically detect it and use `podman compose` instead of `docker compose`.

```bash
# Check which runtime is being used
make podman-check

# Build images (uses Podman if available)
make build

# Start the cluster
make up

# Stop the cluster
make down

# Clean everything
make clean
```

### Manual Override

To explicitly use Docker or Podman, set the `CONTAINER_RUNTIME` environment variable:

```bash
# Force Docker
CONTAINER_RUNTIME=docker make up

# Force Podman
CONTAINER_RUNTIME=podman make up
```

## Key Differences and Workarounds

### 1. Device Access (privileged containers)

Podman handles device access differently than Docker. The cluster uses `privileged: true` for worker containers to access cgroups.

**Docker:**
```yaml
privileged: true
```

**Podman equivalent:**
```yaml
privileged: true  # Still works in Podman compatibility mode
```

When running natively (without `--compatibility`), replace with:
```yaml
cap_add:
  - SYS_ADMIN
  - NET_ADMIN
security_opt:
  - unconfined
```

### 2. Volume Mounts

Podman may require additional flags for volume mounts in rootless mode:

```bash
# For rootless Podman, use --mount type=bind,...
podman run --mount type=bind,src=/path,dst=/path ...
```

The `--compatibility` flag in `podman compose` handles this automatically.

### 3. GPU Support

For NVIDIA GPU support with Podman:

1. Install `nvidia-container-toolkit` (same as Docker)
2. Configure Podman to use NVIDIA runtime:

```bash
# Edit /etc/containers/containers.conf
[network]
default_network = "podman"

[engine]
cgroup_manager = "cgroupfs"
runtime = "crun"

[runtimes]
crun = "/usr/bin/crun"
runc = "/usr/bin/runc"
nvidia = "/usr/bin/nvidia-container-runtime"
```

3. Enable GPU support in `.env`:

```bash
GPU_ENABLE=true
```

### 4. Network Configuration

Podman uses a different default network driver. The project's bridge network configuration works with both:

```yaml
networks:
  slurm-network:
    driver: bridge
```

## Troubleshooting

### Podman Not Detected

If Podman is installed but not being used:

```bash
# Verify Podman works
podman info

# Check the Makefile runtime detection
make podman-check

# Force Podman usage
CONTAINER_RUNTIME=podman make up
```

### Volume Permission Issues (Rootless)

When using rootless Podman, volume permissions may need adjustment:

```bash
# For shared volumes, use rootful Podman or adjust permissions
podman volume create --opt type=none --opt device=/path --opt o=bind volume_name
```

### Compatibility Mode

The Makefile automatically adds `--compatibility` when using Podman, so Docker Compose v2 features work correctly.

## Testing with Podman

Run the full test suite:

```bash
make test
```

Or run specific tests:

```bash
make test-monitoring
make test-gpu
make test-ondemand
```

## Benefits of Using Podman

1. **Daemonless**: No background daemon required
2. **Rootless by default**: Improved security for development
3. **Docker-compatible**: Drop-in replacement with `podman compose`
4. **Systemd integration**: Better service management on Linux

## Supported Commands

All `make` targets work with Podman:

| Command | Description |
|---------|-------------|
| `make build` | Build Docker images |
| `make up` | Start containers |
| `make down` | Stop containers |
| `make clean` | Remove containers and volumes |
| `make status` | Show cluster status |
| `make shell` | Open shell in slurmctld |
| `make scale-cpu-workers N=3` | Scale CPU workers |
| `make scale-gpu-workers N=2` | Scale GPU workers |
| `make test` | Run test suite |
| `make rebuild` | Clean, build, and start |

## Example: Complete Workflow with Podman

```bash
# 1. Clone and navigate to the repository
git clone https://github.com/giovtorres/slurm-docker-cluster.git
cd slurm-docker-cluster

# 2. Verify Podman is available
make podman-check

# 3. Copy environment template (optional)
cp .env.example .env

# 4. Build the images
make build

# 5. Start the cluster
make up

# 6. Check status
make status

# 7. Access the controller
make shell

# Inside slurmctld:
sinfo
sbatch --wrap="hostname"
squeue

# 8. Stop when done
make down
```

## Additional Resources

- [Podman Documentation](https://podman.io/getting-started/)
- [Podman Compose](https://github.com/containers/podman-compose)
- [Podman GPU Support](https://podman.io/getting-started/gpu)
