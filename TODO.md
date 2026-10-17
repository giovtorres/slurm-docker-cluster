# TODO / Improvement Tracker

This document tracks ongoing and planned improvements to the slurm-docker-cluster project.

## High Priority (Next)

### ✅ Completed

- **Support Podman** ([Issue #172](https://github.com/giovtorres/slurm-docker-cluster/issues/172)) - **COMPLETED**
  - Added Podman compatibility for local development and testing
  - Updated documentation with Podman equivalents of `make` commands
  - Implemented runtime auto-detection (Docker/Podman)
  - Created README-Podman.md with detailed Podman setup instructions
  - Added `podman-check` Make target

### ⚡ Quick Wins (In Progress / Ready to PR)
- **Bump to latest patch versions**
  - Update all Slurm patch versions (25.11.x → 25.11.latest, 26.05.x → 26.05.latest)
  - Update Lmod to latest stable release
  - Update Spack to latest stable release
  - Update MariaDB to latest 12.x
  - Update Elasticsearch/Kibana to latest 9.x
  - Update Open OnDemand to latest 4.2.x patch

---

## 🔴 High Priority — Security

### 1. Hardcoded / weak default database credentials
- **File**: `.env.example`, `docker-compose.yml` lines 9–10
- **Issue**: Default MySQL password is `password`. While documented as "suitable for local development only", this encourages unsafe defaults.
- **Fix**: Generate a random password on first run and persist it, or at minimum use a much less guessable default and warn more prominently.

### 2. Password passed in cleartext to MySQL CLI
- **File**: `docker-entrypoint.sh` line 52
```bash
until echo "SELECT 1" | mysql -h mysql -u${MYSQL_USER} -p${MYSQL_PASSWORD} 2>/dev/null
```
- **Issue**: `-p${MYSQL_PASSWORD}` exposes the password in the process table. Also, MySQL client may warn about this.
- **Fix**: Use `MYSQL_PWD` environment variable or a `.my.cnf` file with restricted permissions.

### 3. JWT key generated once and never rotated
- **File**: `docker-entrypoint.sh` lines 45–49
- **Issue**: The JWT HS256 key is created on first run but lives forever (in the named volume). If an attacker gets in, they can forge tokens indefinitely.
- **Fix**: Document rotation procedure, or store the key in a Docker secret / env var instead of generating it once at runtime.

### 4. `privileged: true` on worker containers
- **File**: `docker-compose.yml` lines 67, 102, 128, 162
- **Issue**: slurmctld, slurmrestd, cpu-worker, and gpu-worker all run with `privileged: true`. Workers only need cgroup access, not full privileges.
- **Fix**: Replace with specific capabilities:
  ```yaml
  cap_add:
    - SYS_ADMIN
  security_opt:
    - apparmor:unconfined
  ```

### 5. Elasticsearch running without TLS (in monitoring profile)
- **File**: `docker-compose.yml` lines 236–239
- **Issue**: `xpack.security.enabled=false` in a container that's reachable on the Docker network. If someone gains access to the network, they can read job data.
- **Fix**: For production use, enable X-Pack security. For dev, this is acceptable but should be clearly documented.

---

## 🟠 High-Medium Priority — Reliability & Resilience

### 6. No Docker image layer cache strategy
- **File**: `.dockerignore`
- **Issue**: The `.dockerignore` does not exclude `venv/`, which can bloat the Docker build context significantly.
- **Fix**: Add `venv/` and any other non-essential directories to `.dockerignore`.

### 7. `detect_replica_number()` fallback uses container ID as node name
- **File**: `docker-entrypoint.sh` lines 9–28
- **Issue**: When replica detection fails, the fallback returns the raw container hostname (a long hex string like `abc123def456`), which breaks Slurm's expected naming convention.
- **Fix**: Either fail explicitly with a clear error message, or use a counter-based approach that persists across restarts via an env var.

### 8. No healthcheck for cpu-worker / gpu-worker on `slurmd` port
- **File**: `docker-compose.yml` lines 146–151 (workers)
- **Issue**: The worker healthcheck uses `pidof slurmd`, which will pass as soon as the process starts, not when it's actually registered and ready.
- **Fix**: Add a readiness probe that checks Slurm registration:
  ```yaml
  test: ["CMD-SHELL", "scontrol show node $(hostname) >/dev/null 2>&1"]
  ```

### 9. MySQL healthcheck uses image-specific script
- **File**: `docker-compose.yml` lines 15–20
- **Issue**: The MySQL healthcheck `healthcheck.sh` is from the MariaDB image — but this isn't explicitly vendored or documented.
- **Fix**: Replace with a self-contained command:
  ```yaml
  test: ["CMD", "mysqladmin", "ping", "-h", "localhost"]
  ```

### 10. No restart policy on containers
- **File**: `docker-compose.yml` — no `restart:` directives anywhere
- **Issue**: If a container crashes, it stays down. There's no automatic recovery.
- **Fix**: Add `restart: unless-stopped` to all services.

---

## 🟡 Medium Priority — Code Quality & Maintainability

### 11. Duplicate test helper functions across scripts
- **Files**: `test_cluster.sh`, `test_ondemand.sh`, `test_monitoring.sh`, `test_gpu.sh`
- **Issue**: All test scripts define their own color variables, counters, and print functions.
- **Fix**: Extract shared helpers into a `lib/test_helpers.sh` source file that all test scripts `source`.

### 12. No argument validation / error handling in shell scripts
- **Files**: All `.sh` scripts
- **Issue**: Scripts use `set -e` but many don't validate prerequisites before doing work.
- **Fix**: Add a `check_cluster_ready()` helper function and call it at the start of scripts that need it.

### 13. Inconsistent shebangs
- **Files**: `test_gpu.sh` uses `#!/usr/bin/env bash`, `test_cluster.sh` uses `#!/bin/bash`
- **Fix**: Standardize on `#!/usr/bin/env bash` and add appropriate options consistently.

### 14. Makefile auto-detection reads from runtime `.env`, not `.env.example`
- **File**: `Makefile` lines 12–14
- **Issue**: If `.env` doesn't exist, profile detection variables are empty. The `build` target won't auto-detect profiles since it happens before any `.env` is set.
- **Fix**: Fall back to `.env.example` when `.env` is missing.

### 15. Singularity → Apptainer migration not reflected
- **File**: `test_cluster.sh` lines 446–489, `Dockerfile` line 207
- **Issue**: The Dockerfile installs `apptainer`, but the test script uses `singularity pull`.
- **Fix**: Either install `singularity-ce` package or update tests to use `apptainer` commands.

### 16. `set -e` combined with `|| true` pattern masks real errors
- **Files**: Multiple test scripts
- **Issue**: Test suite swallows individual failures and only counts them at the end.
- **Fix**: Consider using an explicit "continue on failure" flag or separate pre-check tests from functional tests.

---

## 🟡 Medium Priority — Testing

### 17. `test_spack` installs gmake every time — slow CI path
- **File**: `test_cluster.sh` lines 732–751
- **Issue**: Installing `gmake` via Spack takes several minutes and is run on every test invocation.
- **Fix**: Cache the Spack install output (check if module already exists before installing), or make this an optional/slow test that's skipped in CI unless a flag is set.

### 18. Scale-up/scale-down tests mutate global state
- **Files**: `test_cluster.sh` lines 337–443
- **Issue**: These tests scale the cluster up by 1 and then down, which interacts with any other concurrently running workloads.
- **Fix**: Consider adding an isolation flag or running these in a dedicated test environment.

### 19. No negative tests
- **Issue**: The test suite only validates happy paths. There are no tests for job failure handling, container restart scenarios, or network partitions.
- **Fix**: Add at least a few adversarial tests.

---

## 🟢 Low-Medium Priority — Configuration & Documentation

### 20. No Docker Compose version field
- **File**: `docker-compose.yml` line 1
- **Issue**: Adding `version: "3.8"` helps clarify the format for users who expect it.
- **Fix**: Add explicit version or document v2 format usage.

### 21. `.env.example` has commented-out GPU vars that require editing
- **File**: `.env.example` lines 60–64
- **Issue**: To enable GPU, users must uncomment multiple lines and ensure `nvidia-container-toolkit` is installed.
- **Fix**: Add a dedicated "GPU Setup" section with step-by-step commands.

### 22. No documented minimum Docker / Docker Compose version
- **Issue**: The project uses features that require relatively recent Docker Compose.
- **Fix**: Add a `Minimum requirements` section to the README.

### 23. Missing README update for multi-version CI matrix
- **File**: `.github/workflows/test.yml`
- **Issue**: The README may not reflect that CI now runs on both amd64 and arm64.
- **Fix**: Update README to mention the CI matrix and ARM support.

### 24. No contribution guidelines (CONTRIBUTING.md)
- **Issue**: First-time contributors don't know how to add a new Slurm version, a new config, or run tests locally.
- **Fix**: Add `CONTRIBUTING.md` covering testing, version updates, and CI expectations.

---

## 🟢 Low Priority — Performance & Polish

### 25. `dnf clean all` runs in every Docker layer
- **File**: `Dockerfile`
- **Issue**: Running `dnf clean all && rm -rf /var/cache/dnf` within the same RUN instruction prevents cache from being reused.
- **Fix**: Combine all package operations into fewer layers.

### 26. `detect_replica_number()` uses a loop up to 64
- **File**: `docker-entrypoint.sh` line 11
- **Issue**: If there are 64 replicas and the match is at position 64, this performs 64 DNS lookups sequentially.
- **Fix**: Pass max replicas as a build-time default based on expected scale.

### 27. OOD entrypoint not yet reviewed in detail
- **File**: `ood/ondemand-entrypoint.sh`
- **Note**: Likely follows the same pattern and may have similar concerns.

---

## How to Use This Document

1. **Priority tags** indicate urgency: 🔴 High, 🟠 Medium-High, 🟡 Medium, 🟢 Low
2. **Issue numbers** link to GitHub issues when available
3. **Files listed** show where changes need to be made
4. **Fix suggestions** provide a starting point for implementation

---

*Last updated: 2026-10-17*

## Summary

**Podman Support (Issue #172) - COMPLETED**
- Runtime auto-detection: Docker or Podman
- All `make` targets work with both runtimes
- Test suite updated for both runtimes
- Documentation in README-Podman.md
