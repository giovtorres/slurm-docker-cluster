.PHONY: help build build-no-cache up start down clean logs test test-monitoring test-gpu test-ondemand status shell logs-slurmctld logs-slurmdbd update-slurm reload-slurm version set-version build-all test-all test-version rebuild jobs quick-test run-examples scale-cpu-workers scale-gpu-workers podman-check

# Default target
.DEFAULT_GOAL := help

# Supported Slurm versions
SUPPORTED_VERSIONS := 25.11.7 26.05.2
# Read default version from .env.example (source of truth)
DEFAULT_VERSION := $(shell grep '^SLURM_VERSION=' .env.example | cut -d= -f2)

# Auto-detect profiles based on .env configuration
ELASTICSEARCH_HOST := $(shell grep -E '^ELASTICSEARCH_HOST=' .env 2>/dev/null | cut -d= -f2)
GPU_ENABLE := $(shell grep -E '^GPU_ENABLE=' .env 2>/dev/null | cut -d= -f2)
OOD_ENABLE := $(shell grep -E '^OOD_ENABLE=' .env 2>/dev/null | cut -d= -f2)

# Detect container runtime (docker or podman)
CONTAINER_RUNTIME := $(shell if command -v podman &>/dev/null && podman info &>/dev/null 2>&1; then echo "podman"; else echo "docker"; fi)

# Add --compatibility flag for Podman to ensure Docker Compose file compatibility
COMPOSE_COMPAT := $(if $(findstring podman,$(CONTAINER_RUNTIME)),--compatibility,)
COMPOSE_CMD := $(CONTAINER_RUNTIME) compose $(COMPOSE_COMPAT)

# Build profile flags
PROFILES :=
ifdef ELASTICSEARCH_HOST
    PROFILES += --profile monitoring
endif
ifeq ($(GPU_ENABLE),true)
    PROFILES += --profile gpu
endif
ifeq ($(OOD_ENABLE),true)
    PROFILES += --profile ondemand
endif
PROFILE_FLAG := $(PROFILES)

# Validate CONTAINER_RUNTIME value
VALID_RUNTIME := $(filter docker podman,$(CONTAINER_RUNTIME))
ifeq ($(VALID_RUNTIME),)
    $(error CONTAINER_RUNTIME must be 'docker' or 'podman', got '$(CONTAINER_RUNTIME)')
endif

# Colors for help output
CYAN := $(shell tput -Txterm setaf 6)
RESET := $(shell tput -Txterm sgr0)

help:  ## Show this help message
	@echo "Slurm Docker Cluster - Available Commands"
	@echo "=========================================="
	@echo ""
	@echo "Container Runtime: $(CONTAINER_RUNTIME)"
	@echo "COMPOSE_CMD: $(COMPOSE_CMD)"
	@echo ""
	@echo "Cluster Management:"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "build" "Build Docker images"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "build-no-cache" "Build Docker images without cache"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "up" "Start containers"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "down" "Stop containers"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "clean" "Remove containers and volumes"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "scale-cpu-workers" "Scale CPU workers (requires N=...)"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "scale-gpu-workers" "Scale GPU workers (requires N=...)"
	@printf "  ${CYAN}%-20s${RESET} %s\n" "rebuild" "Clean, rebuild, and start"
	@echo ""
	@echo "Quick Commands:"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "jobs" "View job queue"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "status" "Show cluster status"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "logs" "Show all container logs"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "logs-slurmctld" "Show slurmctld logs"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "logs-slurmdbd" "Show slurmdbd logs"
	@echo ""
	@echo "Configuration Management:"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "update-slurm" "Update config files (requires FILES=\"...\")"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "reload-slurm" "Reload Slurm config without restart"
	@echo ""
	@echo "Development & Testing:"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "shell" "Open shell in slurmctld"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "test" "Run test suite"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "test-monitoring" "Run monitoring profile tests"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "test-gpu" "Run GPU profile tests"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "test-ondemand" "Run Open OnDemand profile tests"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "quick-test" "Submit a quick test job"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "run-examples" "Run example jobs"
	@echo ""
	@echo "Multi-Version Support:"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "version" "Show current Slurm version"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "set-version" "Set Slurm version (requires VER=...)"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "build-all" "Build all supported versions"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "test-version" "Test a specific version (requires VER=...)"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "test-all" "Test all supported versions"
	@echo ""
	@echo "Examples:"
	@echo "  make update-slurm FILES=\"slurm.conf slurmdbd.conf\""
	@echo "  make set-version VER=25.11.7"
	@echo "  make scale-cpu-workers N=3"
	@echo "  make scale-gpu-workers N=2"
	@echo "  make test-version VER=25.11.7"
	@echo ""
	@echo "Monitoring:"
	@echo "  Enable:  Set ELASTICSEARCH_HOST=http://elasticsearch:9200 in .env"
	@echo "  Disable: Comment out or remove ELASTICSEARCH_HOST from .env"
	@echo ""
	@echo "GPU Support (NVIDIA):"
	@echo "  Enable:  Set GPU_ENABLE=true in .env (requires nvidia-container-toolkit on host)"
	@echo "  Disable: Set GPU_ENABLE=false or remove GPU_ENABLE from .env"
	@echo ""
	@echo "Open OnDemand:"
	@echo "  Enable:  Set OOD_ENABLE=true in .env"
	@echo "  Disable: Comment out or remove OOD_ENABLE from .env"
	@echo "  Access:  http://localhost:8080 (login: ood@localhost / password)"
	@echo ""
	@echo "Container Runtimes:"
	@echo "  The cluster works with Docker or Podman as drop-in replacements."
	@echo "  Run 'make podman-check' to see current runtime."
	@echo "  Run 'make docker-check' to verify Docker is available."
	@echo "  See README.md 'Container Runtimes' section for setup details."

build:  ## Build Docker images
	$(COMPOSE_CMD) --progress plain build

build-no-cache:  ## Build Docker images without cache
	$(COMPOSE_CMD) --progress plain build --no-cache

up:  ## Start containers (auto-enables monitoring if ELASTICSEARCH_HOST is set in .env)
	$(COMPOSE_CMD) $(PROFILE_FLAG) up -d

down:  ## Stop containers
	$(COMPOSE_CMD) $(PROFILE_FLAG) down

clean:  ## Remove containers and volumes
	$(COMPOSE_CMD) $(PROFILE_FLAG) down -v

logs:  ## Show container logs
	$(COMPOSE_CMD) logs -f

test:  ## Run test suite
	./test_cluster.sh

test-monitoring:  ## Run monitoring profile test suite
	./test_monitoring.sh

test-gpu:  ## Run GPU profile test suite
	./test_gpu.sh

test-ondemand:  ## Run Open OnDemand profile test suite
	./test_ondemand.sh

status:  ## Show cluster status
	@echo "=== Containers ==="
	@$(COMPOSE_CMD) ps
	@echo ""
	@echo "=== Cluster ==="
	@$(COMPOSE_CMD) exec -T slurmctld sinfo 2>/dev/null || echo "Not ready"

shell:  ## Open shell in slurmctld
	$(COMPOSE_CMD) exec -it slurmctld bash

logs-slurmctld:  ## Show slurmctld logs
	$(COMPOSE_CMD) logs -f slurmctld

logs-slurmdbd:  ## Show slurmdbd logs
	$(COMPOSE_CMD) logs -f slurmdbd

quick-test:  ## Submit a quick test job
	$(COMPOSE_CMD) exec slurmctld bash -c "cd /data && sbatch --wrap='hostname' && sleep 3 && squeue && cat slurm-*.out 2>/dev/null | tail -5"

run-examples:  ## Run example jobs
	./run_examples.sh

jobs:  ## View job queue
	$(COMPOSE_CMD) exec slurmctld squeue

update-slurm:  ## Update Slurm config files (usage: make update-slurm FILES="slurm.conf slurmdbd.conf")
	@if [ -z "$(FILES)" ]; then \
		echo "Error: FILES parameter required"; \
		echo "Usage: make update-slurm FILES=\"slurm.conf slurmdbd.conf\""; \
		echo "Available: slurm.conf, slurmdbd.conf, cgroup.conf"; \
		exit 1; \
	fi
	./update_slurmfiles.sh $(FILES)

reload-slurm:  ## Reload Slurm config without restart (after live editing)
	@echo "Reloading Slurm configuration..."
	$(COMPOSE_CMD) exec slurmctld scontrol reconfigure
	@echo "✓ Configuration reloaded"

scale-cpu-workers:  ## Scale CPU workers (usage: make scale-cpu-workers N=3)
	@if [ -z "$(N)" ]; then \
		echo "Error: N parameter required. Usage: make scale-cpu-workers N=3"; \
		exit 1; \
	fi
	$(COMPOSE_CMD) $(PROFILE_FLAG) up -d --scale cpu-worker=$(N) --no-recreate
	@echo "Waiting for dynamic workers to register..."; \
	sleep 10; \
	LIVE_NODES=$$($(COMPOSE_CMD) $(PROFILE_FLAG) ps cpu-worker -q 2>/dev/null \
		| while read cid; do \
			$(COMPOSE_CMD) exec "$$cid" hostname 2>/dev/null; \
		done | sort); \
	SLURM_NODES=$$($(COMPOSE_CMD) exec slurmctld scontrol show nodes 2>/dev/null \
		| grep -o 'NodeName=c[0-9]*' | cut -d= -f2 | sort); \
	STALE_NODES=$$(comm -23 <(echo "$$SLURM_NODES") <(echo "$$LIVE_NODES") | paste -sd, -); \
	if [ -n "$$STALE_NODES" ]; then \
		echo "Removing stale dynamic nodes: $$STALE_NODES"; \
		$(COMPOSE_CMD) exec slurmctld scontrol delete nodename=$$STALE_NODES; \
	fi; \
	$(COMPOSE_CMD) exec slurmctld sinfo

scale-gpu-workers:  ## Scale GPU workers (usage: make scale-gpu-workers N=2)
	@if [ -z "$(N)" ]; then \
		echo "Error: N parameter required. Usage: make scale-gpu-workers N=2"; \
		exit 1; \
	fi
	$(COMPOSE_CMD) --profile gpu $(PROFILE_FLAG) up -d --scale gpu-worker=$(N) --no-recreate
	@echo "Waiting for dynamic GPU workers to register..."; \
	sleep 10; \
	LIVE_NODES=$$($(COMPOSE_CMD) --profile gpu $(PROFILE_FLAG) ps gpu-worker -q 2>/dev/null \
		| while read cid; do \
			$(COMPOSE_CMD) exec "$$cid" hostname 2>/dev/null; \
		done | sort); \
	SLURM_NODES=$$($(COMPOSE_CMD) exec slurmctld scontrol show nodes 2>/dev/null \
		| grep -o 'NodeName=g[0-9]*' | cut -d= -f2 | sort); \
	STALE_NODES=$$(comm -23 <(echo "$$SLURM_NODES") <(echo "$$LIVE_NODES") | paste -sd, -); \
	if [ -n "$$STALE_NODES" ]; then \
		echo "Removing stale GPU nodes: $$STALE_NODES"; \
		$(COMPOSE_CMD) exec slurmctld scontrol delete nodename=$$STALE_NODES; \
	fi; \
	$(COMPOSE_CMD) exec slurmctld sinfo

# Multi-Version Support Targets

version:  ## Show current Slurm version
	@if [ -f .env ]; then \
		grep SLURM_VERSION .env || echo "SLURM_VERSION not set (default: $(DEFAULT_VERSION))"; \
	else \
		echo "No .env file found (default: $(DEFAULT_VERSION))"; \
	fi

set-version:  ## Set Slurm version (usage: make set-version VER=25.11.7)
	@if [ -z "$(VER)" ]; then \
		echo "Error: VER parameter required. Usage: make set-version VER=25.11.7"; \
		echo "Supported versions: $(SUPPORTED_VERSIONS)"; \
		exit 1; \
	fi
	@echo "SLURM_VERSION=$(VER)" > .env
	@echo "✓ Set SLURM_VERSION=$(VER) in .env"
	@echo "Run 'make rebuild' to rebuild with this version"

build-all:  ## Build Docker images for all supported versions
	@echo "Building all supported Slurm versions..."
	@for version in $(SUPPORTED_VERSIONS); do \
		echo ""; \
		echo "========================================"; \
		echo "Building Slurm $$version"; \
		echo "========================================"; \
		echo "SLURM_VERSION=$$version" > .env; \
		$(COMPOSE_CMD) build || exit 1; \
		echo "✓ Built slurm-docker-cluster:$$version"; \
	done
	@echo ""
	@echo "========================================"; \
	echo "✓ All versions built successfully"; \
	echo "========================================"; \
	$(COMPOSE_CMD) images | grep slurm-docker-cluster

test-version:  ## Test a specific version (usage: make test-version VER=25.11.7)
	@if [ -z "$(VER)" ]; then \
		echo "Error: VER parameter required. Usage: make test-version VER=25.11.7"; \
		echo "Supported versions: $(SUPPORTED_VERSIONS)"; \
		exit 1; \
	fi
	@echo "========================================"; \
	echo "Testing Slurm $(VER)"; \
	echo "========================================"; \
	echo "SLURM_VERSION=$(VER)" > .env
	@$(MAKE) clean
	@echo "Starting cluster with Slurm $(VER)..."
	@$(COMPOSE_CMD) up -d
	@echo "Waiting for services to start and auto-register..."
	@sleep 20
	@echo "Running test suite..."
	@./test_cluster.sh
	@echo ""
	@echo "✓ Slurm $(VER) tests completed"
	@$(MAKE) clean

test-all:  ## Run test suite against all supported versions
	@echo "Testing all supported Slurm versions..."
	@echo "Supported versions: $(SUPPORTED_VERSIONS)"
	@echo ""
	@for version in $(SUPPORTED_VERSIONS); do \
		echo ""; \
		echo "========================================"; \
		echo "Testing Slurm $$version"; \
		echo "========================================"; \
		$(MAKE) test-version VER=$$version || exit 1; \
	done
	@echo ""
	@echo "========================================"; \
	echo "✓ All version tests passed!"; \
	echo "========================================";

podman-check:  ## Check Podman compatibility
	@if [ "$(CONTAINER_RUNTIME)" = "podman" ]; then \
		echo "✓ Podman is the active container runtime"; \
		echo "Podman version: $$(podman --version)"; \
		echo "Docker Compose compatibility: enabled"; \
	else \
		echo "ℹ Docker is the active container runtime (or Podman not available)"; \
	fi

docker-check:  ## Check Docker compatibility
	@if command -v docker &>/dev/null; then \
		echo "✓ Docker is installed and accessible"; \
		echo "Docker version: $$(docker --version)"; \
		echo "Docker Compose version: $$(docker compose version 2>/dev/null || echo 'Docker Compose not found')"; \
	else \
		echo "✗ Docker is not installed or not in PATH"; \
	fi

rebuild: clean build up status
