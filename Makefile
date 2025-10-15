.PHONY: help build up start down clean logs test status shell logs-slurmctld logs-slurmdbd update-slurm reload-slurm version set-version build-all test-all test-version

# Default target
.DEFAULT_GOAL := help

# Supported Slurm versions
SUPPORTED_VERSIONS := 24.11.6 25.05.3
DEFAULT_VERSION := 25.05.3

# Colors for help output
CYAN := $(shell tput -Txterm setaf 6)
RESET := $(shell tput -Txterm sgr0)

help:  ## Show this help message
	@echo "Slurm Docker Cluster - Available Commands"
	@echo "=========================================="
	@echo ""
	@echo "Cluster Management:"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "build" "Build Docker images"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "up" "Start containers"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "down" "Stop containers"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "clean" "Remove containers and volumes"
	@printf "  ${CYAN}%-15s${RESET} %s\n" "rebuild" "Clean, rebuild, and start"
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
	@echo "  make set-version VER=24.11.6"
	@echo "  make test-version VER=24.11.6"

build:  ## Build Docker images
	docker compose --progress plain build

up:  ## Start containers
	docker compose up -d

down:  ## Stop containers
	docker compose down

clean:  ## Remove containers and volumes
	docker compose down -v

logs:  ## Show container logs
	docker compose logs -f

test:  ## Run test suite
	./test_cluster.sh

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

# Multi-Version Support Targets

version:  ## Show current Slurm version
	@if [ -f .env ]; then \
		grep SLURM_VERSION .env || echo "SLURM_VERSION not set (default: $(DEFAULT_VERSION))"; \
	else \
		echo "No .env file found (default: $(DEFAULT_VERSION))"; \
	fi

set-version:  ## Set Slurm version (usage: make set-version VER=24.11.6)
	@if [ -z "$(VER)" ]; then \
		echo "Error: VER parameter required. Usage: make set-version VER=24.11.6"; \
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

test-version:  ## Test a specific version (usage: make test-version VER=24.11.6)
	@if [ -z "$(VER)" ]; then \
		echo "Error: VER parameter required. Usage: make test-version VER=24.11.6"; \
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
