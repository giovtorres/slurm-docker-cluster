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

# Start with defaults (Slurm 25.11.x, no monitoring)
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
# Edit .env to change SLURM_VERSION, enable ELASTICSEARCH_HOST, or enable GPU_ENABLE
make up
```

**Supported Slurm versions:** 25.11.x, 25.05.x, 24.11.x

**Supported architectures (auto-detected):** AMD64, ARM64

## 📦 What's Included

**Containers:**

- **mysql** - Job and cluster database
- **slurmdbd** - Database daemon for accounting
- **slurmctld** - Controller for job scheduling
- **slurmrestd** - REST API daemon (HTTP/JSON access)
- **c1, c2** - Compute nodes
- **g1** - (optional) GPU compute node with NVIDIA support
- **elasticsearch** - (optional) indexing jobs
- **kibana** - (optional) visualization for elasticsearch

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

Query cluster via REST API (version auto-detected: v0.0.44 for 25.11.x, v0.0.42 for 25.05.x, v0.0.41 for 24.11.x):

```bash
# Get JWT Token
JWT_TOKEN=$(docker exec slurmctld scontrol token 2>&1 | grep "SLURM_JWT=" | cut -d'=' -f2)

# Get nodes
docker exec slurmrestd curl -s -H "X-SLURM-USER-TOKEN: $JWT_TOKEN" \
  http://localhost:6820/slurm/v0.0.42/nodes | jq .nodes

# Get partitions
docker exec slurmrestd curl -s -H "X-SLURM-USER-TOKEN: $JWT_TOKEN" \
  http://localhost:6820/slurm/v0.0.42/partitions | jq .partitions
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

## 🎮 GPU Support (NVIDIA)

Enable optional NVIDIA GPU support with dedicated GPU node:

```bash
# 1. One-time host setup (add NVIDIA repo and install nvidia-container-toolkit)
curl -s -L https://nvidia.github.io/libnvidia-container/stable/rpm/nvidia-container-toolkit.repo \
  | sudo tee /etc/yum.repos.d/nvidia-container-toolkit.repo
sudo dnf install -y nvidia-container-toolkit
sudo nvidia-ctk runtime configure --runtime=docker
sudo systemctl restart docker

# 2. Enable GPU in .env (CUDA toolkit installed in container automatically)
GPU_ENABLE=true
CUDA_VERSION=12.6  # Optional, defaults to 12.6

# 3. Build with GPU support
make rebuild

# 4. Verify GPU detection
docker exec g1 nvidia-smi

# Test GPU functionality
make test-gpu
```

> **Note:** GPU testing is not included in CI (GitHub-hosted runners have no GPUs). Run `make test-gpu` manually on a host with an NVIDIA GPU and `nvidia-container-toolkit` installed.

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
  --build-arg SLURM_VERSION=25.05.6 \
  --load -t slurm-docker-cluster:25.05.6 .
```

## 📚 Documentation

- **Commands:** Run `make help` for all available commands
- **Examples:** Job scripts in `examples/` directory

## 🤝 Contributing

Contributions are welcomed! Fork this repo, create a branch, and submit a pull request.

## 📄 License

This project is licensed under the [MIT License](LICENSE).
