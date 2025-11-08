# Slurm Docker Cluster

<p align="center">
    <b> English | <a href="./readme/README_CN.md">简体中文</a> </b>
</p>

**Slurm Docker Cluster** is a multi-container Slurm cluster designed for rapid
deployment using Docker Compose. This repository simplifies the process of
setting up a robust Slurm environment for development, testing, or lightweight
usage.

## 🏁 Getting Started

To get up and running with Slurm in Docker, make sure you have the following tools installed:

- **[Docker](https://docs.docker.com/get-docker/)**
- **[Docker Compose](https://docs.docker.com/compose/install/)**

Clone the repository:

```bash
git clone https://github.com/giovtorres/slurm-docker-cluster.git
cd slurm-docker-cluster
```

## 🔢 Choosing Your Slurm Version

This project supports multiple Slurm versions. To select your version, copy `.env.example` to `.env` and set `SLURM_VERSION`:

```bash
cp .env.example .env
# Edit .env and set:
SLURM_VERSION=25.05.3   # Latest stable (default)
# Or:
SLURM_VERSION=24.11.6   # Previous stable release
```

**Supported versions:** 25.05.x, 24.11.x

## 🏗️ Architecture Support

This project supports both **AMD64 (x86_64)** and **ARM64 (aarch64)**
architectures. The build system automatically detects your architecture. No
special configuration is needed - simply build and run:

```bash
make build
make up
```

## 🚀 Quick Start (Using Make)

The easiest way to get started is using the provided Makefile:

```bash
# Build and start the cluster
make up

# Run tests to verify everything works
make test

# View cluster status
make status
```

See all available commands:
```bash
make help
```

## 📦 Containers and Volumes

This setup consists of the following containers:

- **mysql**: Stores job and cluster data.
- **slurmdbd**: Manages the Slurm database.
- **slurmctld**: The Slurm controller responsible for job and resource management.
- **slurmrestd**: REST API daemon for HTTP/JSON access to the cluster.
- **c1, c2**: Compute nodes (running `slurmd`).

### Persistent Volumes:

- `etc_munge`: Mounted to `/etc/munge` - Authentication keys
- `etc_slurm`: Mounted to `/etc/slurm` - Configuration files (allows live editing)
- `slurm_jobdir`: Mounted to `/data` - Job files shared across all nodes
- `var_lib_mysql`: Mounted to `/var/lib/mysql` - Database persistence
- `var_log_slurm`: Mounted to `/var/log/slurm` - Log files

## 🛠️ Building and Starting the Cluster

### Building

The easiest way to build and start the cluster is using Make:

```bash
# Build images with default version (25.05.3)
make build

# Or build and start in one command
make up
```

To build a different version, update `SLURM_VERSION` in `.env`:

```bash
make set-version VER=24.11.6

# Build
make build
```

Alternatively, use Docker Compose directly:

```bash
docker compose build
```

### Starting

Start the cluster in detached mode:

```bash
make up
```

Check cluster status:

```bash
make status
```

View logs:

```bash
make logs
```

> **Note**: The cluster automatically registers itself with SlurmDBD on first startup. Wait about 15-20 seconds after starting for all services to become healthy and auto-register.

## 🖥️ Using the Cluster

### Accessing the Controller

Open a shell in the Slurm controller:

```bash
make shell
# Or: docker exec -it slurmctld bash
```

Check cluster status:

```bash
[root@slurmctld /]# sinfo
PARTITION AVAIL  TIMELIMIT  NODES  STATE NODELIST
normal*      up   infinite      2   idle c[1-2]
```

### Submitting Jobs

The `/data` directory is shared across all nodes for job files:

```bash
[root@slurmctld /]# cd /data/
[root@slurmctld data]# sbatch --wrap="hostname"
Submitted batch job 2
[root@slurmctld data]# cat slurm-2.out
c1
```

### Running Example Jobs

Use the included example scripts:

```bash
make run-examples
```

This runs sample jobs including simple hostname tests, CPU-intensive workloads, multi-node jobs, and more.

## 🔄 Cluster Management

Stop the cluster (keeps data):

```bash
make down
```

Restart the cluster:

```bash
make up
```

Complete cleanup (removes all data and volumes):

```bash
make clean
```

For more workflows including configuration updates, version switching, and testing, see the **Common Workflows** section below.

## ⚙️ Advanced Configuration

### Multi-Architecture Builds

For cross-platform builds or explicit architecture selection (`arm64` or
`amd64`), use Docker Buildx:

```bash
docker buildx build \
  --platform linux/arm64 \
  --build-arg SLURM_VERSION=25.05.3 \
  --build-arg TARGETARCH=arm64 \
  --load \
  -t slurm-docker-cluster:25.05.3 \
  .
```

**Note**: Cross-platform builds use QEMU emulation and may be slower than native builds.

### Live Configuration Updates

With the `etc_slurm` volume mounted, you can modify configurations without rebuilding:

**Method 1 - Direct editing (persists across restarts):**
```bash
docker exec -it slurmctld vi /etc/slurm/slurm.conf
make reload-slurm
```

**Method 2 - Push changes from config/ directory:**
```bash
# Edit config files locally in config/25.05/ or config/common/
vi config/25.05/slurm.conf

# Push to containers (automatically detects version from .env)
make update-slurm FILES="slurm.conf"

# Or update multiple files
make update-slurm FILES="slurm.conf slurmdbd.conf"
```

**Method 3 - Rebuild image with new configs:**
```bash
# For permanent changes
vi config/25.05/slurm.conf
make rebuild
```

This makes it easy to add/remove nodes or test new configuration settings dynamically.

## 📖 Common Workflows

### Using Make (Recommended)

#### First-time Setup:
```bash
# Build and start cluster
make up

# Verify everything is working
make test

# Check cluster status
make status
```

#### Daily Development:
```bash
# View logs
make logs

# Open shell in controller
make shell

# Inside shell:
cd /data
sbatch --wrap="hostname"
squeue
```

#### Testing Changes:
```bash
# After editing config files
make down
make start
make test
```

#### Cleanup:
```bash
# Stop cluster (keeps data)
make down

# Complete cleanup (removes all data)
make clean
```

### Example: Running Test Jobs

```bash
# Start cluster
make start

# Copy example jobs to cluster
docker cp examples/jobs slurmctld:/data/

# Submit a simple job
docker exec slurmctld bash -c "cd /data/jobs && sbatch simple_hostname.sh"

# Submit a multi-node job
docker exec slurmctld bash -c "cd /data/jobs && sbatch multi_node.sh"

# Watch job queue
docker exec slurmctld squeue

# View job outputs
docker exec slurmctld bash -c "ls -lh /data/jobs/*.out"
docker exec slurmctld bash -c "cat /data/jobs/hostname_test_*.out"
```

### Example: Testing Different Slurm Versions

```bash
# Check current version
make version

# Build all supported versions
make build-all

# Test a specific version
make test-version VER=24.11.6

# Test all versions (comprehensive)
make test-all

# Switch to a different version and use it
make set-version VER=24.11.6
make rebuild
make test
```

### Example: Development Workflow

```bash
# Morning: Start cluster
make start

# Work on features, test locally
make test

# Check logs if issues arise
make logs

# Evening: Stop cluster
make down

# Next day: Quick restart
make start
```

### Makefile Commands Reference

| Command | Description |
|---------|-------------|
| `make help` | Show all available commands |
| `make build` | Build Docker images |
| `make up` | Start containers |
| `make down` | Stop containers |
| `make clean` | Remove containers and volumes |
| `make logs` | Show container logs |
| `make test` | Run test suite |
| `make status` | Show cluster status |
| `make shell` | Open shell in slurmctld |
| `make update-slurm FILES="..."` | Update config files from config/ directory |
| `make reload-slurm` | Reload Slurm config without restart |
| **Multi-Version Commands** | |
| `make version` | Show current Slurm version |
| `make set-version VER=24.11.6` | Set Slurm version in .env |
| `make build-all` | Build all supported versions |
| `make test-version VER=24.11.6` | Test a specific version |
| `make test-all` | Test all supported versions |

## 🤝 Contributing

Contributions are welcomed from the community! If you want to add features, fix bugs, or improve documentation:

1. Fork this repo.
2. Create a new branch: `git checkout -b feature/your-feature`.
3. Submit a pull request.

## 📄 License

This project is licensed under the [MIT License](LICENSE).
