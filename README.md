# Slurm Docker Cluster

**Slurm Docker Cluster** is a multi-container Slurm cluster designed for rapid
deployment using Docker Compose. This repository simplifies the process of
setting up a robust Slurm environment for development, testing, or lightweight
usage.

## 🏁 Quick Start

**Requirements:** [Docker](https://docs.docker.com/get-docker/) and [Docker Compose](https://docs.docker.com/compose/install/)

```bash
git clone https://github.com/giovtorres/slurm-docker-cluster.git
cd slurm-docker-cluster

# Start with defaults (Slurm 25.05.x with no-monitoring)
make up

# Verify cluster is working
make status
make test

# See all available commands
make help
```

**To customize:** Copy `.env.example` to `.env` and modify settings before running `make up`

```bash
cp .env.example .env
# Edit .env to change SLURM_VERSION or enable ELASTICSEARCH_HOST
make up
```

**Supported Slurm versions:** 25.05.x, 24.11.x

**Supported architectures (auto-detected):** AMD64, ARM64

## 📦 What's Included

**Containers:**

- **mysql** - Job and cluster database
- **slurmdbd** - Database daemon for accounting
- **slurmctld** - Controller for job scheduling
- **slurmrestd** - REST API daemon (HTTP/JSON access)
- **c1, c2** - Compute nodes

**Persistent volumes:**

- Configuration (`etc_slurm`)
- Logs (`var_log_slurm`)
- Job files (`slurm_jobdir`)
- Database (`var_lib_mysql`)
- Authentication (`etc_munge`)

## 🖥️ Using the Cluster

```bash
# Access controller
make shell

# Inside controller:
sinfo                          # View cluster status
sbatch --wrap="hostname"       # Submit job
squeue                         # View queue
sacct                          # View accounting

# Or run example jobs
make run-examples
```

## 📊 Monitoring

### REST API

Query cluster via REST API (version auto-detected: v0.0.42 for 25.05.x, v0.0.41 for 24.11.x):

```bash
# Get nodes
docker exec slurmrestd curl -s --unix-socket /var/run/slurmrestd/slurmrestd.socket \
  http://localhost/slurm/v0.0.42/nodes | jq

# Get partitions
docker exec slurmrestd curl -s --unix-socket /var/run/slurmrestd/slurmrestd.socket \
  http://localhost/slurm/v0.0.42/partitions | jq
```

### Elasticsearch and Kibana (Optional)

Enable job completion monitoring and visualization:

```bash
# 1. Setting ELASTICSEARCH_HOST in .env enables the monitoring profile
ELASTICSEARCH_HOST=http://elasticsearch:9200

# 2. Start cluster (monitoring auto-enabled)
make up

# 3. Access Kibana at http://localhost:5601
# After loading, click: Elasticsearch → Index Management → slurm → Discover index

# 4. Query job completions directly
docker exec elasticsearch curl -s "http://localhost:9200/slurm/_search?pretty"

# Test monitoring
make test-monitoring
```

**Indexed data:** Job ID, user, partition, state, times, nodes, exit code

## 🔄 Cluster Management

```bash
make down     # Stop cluster (keeps data)
make clean    # Remove all containers and volumes
make rebuild  # Clean, rebuild, and restart
make logs     # View container logs
```

> **Note:** If `ELASTICSEARCH_HOST` is set in `.env`, monitoring containers are automatically managed.

## ⚙️ Advanced

### Version Management

```bash
make set-version VER=24.11.6   # Switch Slurm version
make version                   # Show current version
make build-all                 # Build all supported versions
make test-all                  # Test all versions
```

### Configuration Updates

```bash
# Live edit (persists across restarts)
docker exec -it slurmctld vi /etc/slurm/slurm.conf
make reload-slurm

# Push local changes
vi config/25.05/slurm.conf
make update-slurm FILES="slurm.conf"

# Permanent changes
make rebuild
```

### Multi-Architecture Builds

```bash
# Cross-platform build (uses QEMU emulation)
docker buildx build --platform linux/arm64 \
  --build-arg SLURM_VERSION=25.05.3 \
  --load -t slurm-docker-cluster:25.05.3 .
```

## 📚 Documentation

- **Commands:** Run `make help` for all available commands
- **Examples:** Job scripts in `examples/` directory

## 🤝 Contributing

Contributions are welcomed! Fork this repo, create a branch, and submit a pull request.

## 📄 License

This project is licensed under the [MIT License](LICENSE).
