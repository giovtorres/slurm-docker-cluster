.PHONY: help build build-no-cache up start down clean logs test test-monitoring test-gpu status shell logs-slurmctld logs-slurmdbd update-slurm reload-slurm version set-version build-all test-all test-version rebuild jobs quick-test run-examples scale-cpu-workers scale-gpu-workers

# Default target
.DEFAULT_GOAL := help

# Supported Slurm versions
SUPPORTED_VERSIONS := 25.05.6 25.11.2
# Read default version from .env.example (source of truth)
DEFAULT_VERSION := $(shell grep '^SLURM_VERSION=' .env.example | cut -d= -f2)

# Auto-detect profiles based on .env configuration
ELASTICSEARCH_HOST := $(shell grep -E '^ELASTICSEARCH_HOST=' .env 2>/dev/null | cut -d= -f2)
GPU_ENABLE := $(shell grep -E '^GPU_ENABLE=' .env 2>/dev/null | cut -d= -f2)

# Build profile flags
PROFILES :=
ifdef ELASTICSEARCH_HOST
    PROFILES += --profile monitoring
endif
ifeq ($(GPU_ENABLE),true)
    PROFILES += --profile gpu
endif
PROFILE_FLAG := $(PROFILES)

# Colors for help output
CYAN := $(shell tput -Txterm setaf 6)
RESET := $(shell tput -Txterm sgr0)

help:  ## Show this help message
	@echo "Slurm Docker Cluster - Available Commands"
	@echo "=========================================="
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
	@echo "Playground (Learning & Testing):"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-init" "Install CLI and prepare playground"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-start" "Start playground cluster (NODES=N, PROFILE=...)"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-stop" "Stop playground cluster"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-reset" "Reset to default 2-node cluster"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-shell" "Shell into slurmctld"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-logs" "Tail playground logs"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-metrics" "Open Grafana dashboard"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "playground-status" "Show playground status"
	@echo ""
	@echo "AWX (Maintenance Automation):"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "awx-setup" "Full AWX setup (first time)"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "awx-up" "Start AWX services"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "awx-down" "Stop AWX services"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "awx-status" "Show AWX status and credentials"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "awx-logs" "Tail AWX logs"
	@printf "  ${CYAN}%-18s${RESET} %s\n" "awx-clean" "Remove AWX and all data"
	@echo ""
	@echo "Examples:"
	@echo "  make update-slurm FILES=\"slurm.conf slurmdbd.conf\""
	@echo "  make set-version VER=24.11.6"
	@echo "  make test-version VER=24.11.6"
	@echo "  make playground-start NODES=10"
	@echo "  make playground-start PROFILE=medium"
	@echo "  make set-version VER=25.05.6"
	@echo "  make scale-cpu-workers N=3"
	@echo "  make scale-gpu-workers N=2"
	@echo "  make test-version VER=25.05.6"
	@echo ""
	@echo "Monitoring:"
	@echo "  Enable:  Set ELASTICSEARCH_HOST=http://elasticsearch:9200 in .env"
	@echo "  Disable: Comment out or remove ELASTICSEARCH_HOST from .env"
	@echo ""
	@echo "GPU Support (NVIDIA):"
	@echo "  Enable:  Set GPU_ENABLE=true in .env (requires nvidia-container-toolkit on host)"
	@echo "  Disable: Set GPU_ENABLE=false or remove GPU_ENABLE from .env"

build:  ## Build Docker images
	docker compose --progress plain build

build-no-cache:  ## Build Docker images without cache
	docker compose --progress plain build --no-cache

up:  ## Start containers (auto-enables monitoring if ELASTICSEARCH_HOST is set in .env)
	docker compose $(PROFILE_FLAG) up -d

down:  ## Stop containers
	docker compose $(PROFILE_FLAG) down

clean:  ## Remove containers and volumes
	docker compose $(PROFILE_FLAG) down -v

logs:  ## Show container logs
	docker compose logs -f

test:  ## Run test suite
	./test_cluster.sh

test-monitoring:  ## Run monitoring profile test suite
	./test_monitoring.sh

test-gpu:  ## Run GPU profile test suite
	./test_gpu.sh

status:  ## Show cluster status
	@echo "=== Containers ==="
	@docker compose ps
	@echo ""
	@echo "=== Cluster ==="
	@docker exec slurmctld sinfo 2>/dev/null || echo "Not ready"

shell:  ## Open shell in slurmctld
	docker exec -it slurmctld bash

logs-slurmctld:  ## Show slurmctld logs
	docker compose logs -f slurmctld

logs-slurmdbd:  ## Show slurmdbd logs
	docker compose logs -f slurmdbd

quick-test:  ## Submit a quick test job
	docker exec slurmctld bash -c "cd /data && sbatch --wrap='hostname' && sleep 3 && squeue && cat slurm-*.out 2>/dev/null | tail -5"

run-examples:  ## Run example jobs
	./run_examples.sh

jobs:  ## View job queue
	docker exec slurmctld squeue

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
	docker exec slurmctld scontrol reconfigure
	@echo "✓ Configuration reloaded"

scale-cpu-workers:  ## Scale CPU workers (usage: make scale-cpu-workers N=3)
	@if [ -z "$(N)" ]; then \
		echo "Error: N parameter required. Usage: make scale-cpu-workers N=3"; \
		exit 1; \
	fi
	docker compose $(PROFILE_FLAG) up -d --scale cpu-worker=$(N) --no-recreate
	@echo "Waiting for dynamic workers to register..."; \
	sleep 10; \
	LIVE_NODES=$$(docker compose $(PROFILE_FLAG) ps cpu-worker -q 2>/dev/null \
		| while read cid; do \
			docker exec "$$cid" hostname 2>/dev/null; \
		done | sort); \
	SLURM_NODES=$$(docker exec slurmctld scontrol show nodes 2>/dev/null \
		| grep -oP 'NodeName=c\d+' | cut -d= -f2 | sort); \
	STALE_NODES=$$(comm -23 <(echo "$$SLURM_NODES") <(echo "$$LIVE_NODES") | paste -sd, -); \
	if [ -n "$$STALE_NODES" ]; then \
		echo "Removing stale dynamic nodes: $$STALE_NODES"; \
		docker exec slurmctld scontrol delete nodename=$$STALE_NODES; \
	fi; \
	docker exec slurmctld sinfo

scale-gpu-workers:  ## Scale GPU workers (usage: make scale-gpu-workers N=2)
	@if [ -z "$(N)" ]; then \
		echo "Error: N parameter required. Usage: make scale-gpu-workers N=2"; \
		exit 1; \
	fi
	docker compose --profile gpu $(PROFILE_FLAG) up -d --scale gpu-worker=$(N) --no-recreate
	@echo "Waiting for dynamic GPU workers to register..."; \
	sleep 10; \
	LIVE_NODES=$$(docker compose --profile gpu $(PROFILE_FLAG) ps gpu-worker -q 2>/dev/null \
		| while read cid; do \
			docker exec "$$cid" hostname 2>/dev/null; \
		done | sort); \
	SLURM_NODES=$$(docker exec slurmctld scontrol show nodes 2>/dev/null \
		| grep -oP 'NodeName=g\d+' | cut -d= -f2 | sort); \
	STALE_NODES=$$(comm -23 <(echo "$$SLURM_NODES") <(echo "$$LIVE_NODES") | paste -sd, -); \
	if [ -n "$$STALE_NODES" ]; then \
		echo "Removing stale GPU nodes: $$STALE_NODES"; \
		docker exec slurmctld scontrol delete nodename=$$STALE_NODES; \
	fi; \
	docker exec slurmctld sinfo

# Multi-Version Support Targets

version:  ## Show current Slurm version
	@if [ -f .env ]; then \
		grep SLURM_VERSION .env || echo "SLURM_VERSION not set (default: $(DEFAULT_VERSION))"; \
	else \
		echo "No .env file found (default: $(DEFAULT_VERSION))"; \
	fi

set-version:  ## Set Slurm version (usage: make set-version VER=25.05.6)
	@if [ -z "$(VER)" ]; then \
		echo "Error: VER parameter required. Usage: make set-version VER=25.05.6"; \
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
		docker compose build || exit 1; \
		echo "✓ Built slurm-docker-cluster:$$version"; \
	done
	@echo ""
	@echo "========================================"; \
	echo "✓ All versions built successfully"; \
	echo "========================================"; \
	docker images | grep slurm-docker-cluster

test-version:  ## Test a specific version (usage: make test-version VER=25.05.6)
	@if [ -z "$(VER)" ]; then \
		echo "Error: VER parameter required. Usage: make test-version VER=25.05.6"; \
		echo "Supported versions: $(SUPPORTED_VERSIONS)"; \
		exit 1; \
	fi
	@echo "========================================"; \
	echo "Testing Slurm $(VER)"; \
	echo "========================================"; \
	echo "SLURM_VERSION=$(VER)" > .env
	@$(MAKE) clean
	@echo "Starting cluster with Slurm $(VER)..."
	@docker compose up -d
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

rebuild: clean build up

# =============================================================================
# Playground Targets
# =============================================================================

# Default playground settings
NODES ?= 2
PROFILE ?=

playground-init:  ## Initialize playground - install CLI and check dependencies
	@echo "Initializing Slurm Playground..."
	@echo ""
	@echo "Checking Python environment..."
	@python3 --version || (echo "Error: Python 3 required" && exit 1)
	@echo ""
	@echo "Installing playground CLI..."
	@pip3 install -e playground/cli --quiet || pip3 install -e playground/cli
	@echo ""
	@echo "Verifying installation..."
	@playground --version || echo "Note: You may need to add ~/.local/bin to PATH"
	@echo ""
	@echo "✓ Playground initialized successfully!"
	@echo ""
	@echo "Quick start:"
	@echo "  make playground-start        # Start with 2 nodes"
	@echo "  make playground-start NODES=10  # Start with 10 nodes"
	@echo "  playground --help            # CLI help"

playground-start:  ## Start playground cluster (use NODES=N or PROFILE=name)
	@echo "Starting Slurm Playground..."
ifdef PROFILE
	@echo "Applying profile: $(PROFILE)"
	@./playground/scale.sh preset $(PROFILE)
else
	@echo "Scaling to $(NODES) nodes..."
	@./playground/scale.sh set $(NODES)
endif
	@docker compose up -d
	@echo ""
	@echo "Waiting for cluster to be ready..."
	@sleep 15
	@echo ""
	@docker exec slurmctld sinfo 2>/dev/null || echo "Cluster starting up..."
	@echo ""
	@echo "✓ Playground started!"
	@echo ""
	@echo "Next steps:"
	@echo "  playground status            # Check cluster status"
	@echo "  playground workload cpu -c 5 # Submit CPU jobs"
	@echo "  playground metrics live      # View live metrics"

playground-stop:  ## Stop playground cluster
	@echo "Stopping Slurm Playground..."
	docker compose down
	@echo "✓ Playground stopped"

playground-reset:  ## Reset playground to default 2-node configuration
	@echo "Resetting playground to default configuration..."
	@./playground/scale.sh reset
	@echo ""
	@echo "Cancelling any running jobs..."
	@docker exec slurmctld scancel -u root 2>/dev/null || true
	@echo ""
	@echo "Cleaning up job output files..."
	@docker exec slurmctld bash -c "rm -f /data/*.out /data/*.err" 2>/dev/null || true
	@echo ""
	@echo "✓ Playground reset complete"

playground-shell:  ## Open interactive shell in slurmctld container
	docker exec -it slurmctld bash

playground-logs:  ## Tail logs from all playground containers
	docker compose logs -f --tail=50

playground-metrics:  ## Start monitoring stack and open Grafana
	@echo "Starting monitoring stack..."
	@docker compose -f docker-compose.yml -f monitoring/docker-compose.monitoring.yml up -d prometheus grafana slurm-exporter 2>/dev/null || \
		(echo "Starting monitoring requires the base cluster to be running" && exit 1)
	@echo ""
	@echo "Waiting for Grafana to start..."
	@sleep 5
	@echo ""
	@echo "✓ Monitoring stack started!"
	@echo ""
	@echo "Access URLs:"
	@echo "  Grafana:    http://localhost:3000  (admin/admin)"
	@echo "  Prometheus: http://localhost:9090"
	@echo ""
	@which open >/dev/null 2>&1 && open http://localhost:3000 || echo "Open http://localhost:3000 in your browser"

playground-status:  ## Show comprehensive playground status
	@echo "=== Slurm Playground Status ==="
	@echo ""
	@echo "Containers:"
	@docker compose ps --format "table {{.Name}}\t{{.Status}}\t{{.Ports}}" 2>/dev/null || echo "  Not running"
	@echo ""
	@echo "Cluster Info:"
	@docker exec slurmctld sinfo 2>/dev/null || echo "  Not available"
	@echo ""
	@echo "Queue:"
	@docker exec slurmctld squeue 2>/dev/null || echo "  Not available"
	@echo ""
	@echo "Resource Usage:"
	@docker exec slurmctld sinfo -o "%C" 2>/dev/null | head -2 || echo "  Not available"

playground-scale:  ## Scale cluster (use NODES=N)
	@echo "Scaling cluster to $(NODES) nodes..."
	@./playground/scale.sh set $(NODES)
	@echo ""
	@echo "Verifying configuration..."
	@docker exec slurmctld sinfo 2>/dev/null || echo "Run 'make playground-start' to apply changes"

playground-workload:  ## Submit sample workload (use TYPE=cpu|memory|sleep COUNT=N)
	@echo "Submitting workload..."
	@playground workload $(or $(TYPE),sleep) --count=$(or $(COUNT),5)

# =============================================================================
# AWX Targets (Maintenance Automation)
# =============================================================================

awx-setup:  ## Full AWX setup - generates credentials, starts services, configures resources
	@echo "Setting up AWX for Slurm maintenance automation..."
	@cd awx && ./setup_awx.sh

awx-up:  ## Start AWX services
	@echo "Starting AWX services..."
	@cd awx && ./setup_awx.sh --start

awx-down:  ## Stop AWX services
	@echo "Stopping AWX services..."
	@cd awx && docker compose -f docker-compose.awx.yml down

awx-status:  ## Show AWX status and access credentials
	@cd awx && ./setup_awx.sh --status

awx-logs:  ## Tail AWX logs
	@cd awx && docker compose -f docker-compose.awx.yml logs -f

awx-clean:  ## Remove AWX containers, volumes, and credentials (DESTRUCTIVE!)
	@echo "WARNING: This will delete all AWX data including job history!"
	@read -p "Are you sure? [y/N] " confirm && [ "$$confirm" = "y" ] || exit 1
	@cd awx && ./setup_awx.sh --clean
	@echo "✓ AWX cleaned"
rebuild: clean build up status
