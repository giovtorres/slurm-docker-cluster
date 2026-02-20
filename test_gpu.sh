#!/usr/bin/env bash
# GPU test suite for slurm-docker-cluster
# Tests GPU node availability and GRES functionality

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counter
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Helper functions
log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((++TESTS_PASSED))
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((++TESTS_FAILED))
}

run_test() {
    ((++TESTS_RUN))
    echo ""
    echo "=========================================="
    echo "Test $TESTS_RUN: $1"
    echo "=========================================="
}

# Check if GPU profile is enabled
check_gpu_enabled() {
    run_test "Check GPU profile enabled"

    if ! grep -q "^GPU_ENABLE=true" .env 2>/dev/null; then
        log_error "GPU_ENABLE not set to true in .env"
        echo "To enable GPU support:"
        echo "  1. Add 'GPU_ENABLE=true' to .env"
        echo "  2. Ensure nvidia-container-toolkit is installed on host"
        echo "  3. Run: make rebuild"
        return 1
    fi

    log_success "GPU_ENABLE=true found in .env"
    return 0
}

# Check g1 container is running
check_g1_running() {
    run_test "Check g1 GPU node container running"

    if ! docker ps --format '{{.Names}}' | grep -q '^g1$'; then
        log_error "g1 container not running"
        echo "Start GPU node with: make up"
        return 1
    fi

    log_success "g1 container is running"
    return 0
}

# Check g1 node registered in Slurm
check_g1_registered() {
    run_test "Check g1 node registered in Slurm"

    if ! docker exec slurmctld scontrol show node g1 &>/dev/null; then
        log_error "g1 node not registered in Slurm"
        return 1
    fi

    log_success "g1 node registered in Slurm"
    return 0
}

# Check g1 node has GPU GRES
check_gres_configured() {
    run_test "Check GPU GRES configured on g1"

    local gres_output
    gres_output=$(docker exec slurmctld scontrol show node g1 | grep "Gres=" || true)

    if [[ ! "$gres_output" =~ gpu:nvidia:1 ]]; then
        log_error "GPU GRES not configured correctly on g1"
        echo "Expected: Gres=gpu:nvidia:1"
        echo "Got: $gres_output"
        return 1
    fi

    log_success "GPU GRES configured: $gres_output"
    return 0
}

# Check GPU partition exists
check_gpu_partition() {
    run_test "Check 'gpu' partition exists"

    if ! docker exec slurmctld scontrol show partition gpu &>/dev/null; then
        log_error "'gpu' partition not found"
        return 1
    fi

    local partition_nodes
    # Anchor to lines that start with whitespace + "Nodes=" to avoid matching
    # AllocNodes=, MaxNodes=, TotalNodes= which also contain "Nodes="
    partition_nodes=$(docker exec slurmctld scontrol show partition gpu | grep -E "^\s+Nodes=" | cut -d= -f2)

    if [[ "$partition_nodes" != "g1" ]]; then
        log_error "GPU partition doesn't contain g1 node"
        echo "Expected: Nodes=g1"
        echo "Got: Nodes=$partition_nodes"
        return 1
    fi

    log_success "'gpu' partition configured with node g1"
    return 0
}

# Check nvidia-smi works in g1 container
check_nvidia_smi() {
    run_test "Check nvidia-smi works in g1 container"

    if ! docker exec g1 nvidia-smi &>/dev/null; then
        log_error "nvidia-smi not available in g1 container"
        echo "Ensure nvidia-container-toolkit is installed on host"
        return 1
    fi

    local gpu_count
    gpu_count=$(docker exec g1 nvidia-smi --query-gpu=count --format=csv,noheader | head -n1)

    log_success "nvidia-smi detected $gpu_count GPU(s)"
    docker exec g1 nvidia-smi
    return 0
}

# Test GPU job submission
test_gpu_job_submission() {
    run_test "Submit test job requesting GPU GRES"

    # Clean up old job outputs
    docker exec slurmctld bash -c "cd /data && rm -f slurm-*.out gpu-test-*.out" || true

    # Submit job requesting gpu:1
    local job_script='#!/bin/bash
#SBATCH --job-name=gpu-test
#SBATCH --partition=gpu
#SBATCH --gres=gpu:1
#SBATCH --output=gpu-test-%j.out

echo "=== GPU Test Job ==="
echo "Hostname: $(hostname)"
echo "CUDA_VISIBLE_DEVICES: ${CUDA_VISIBLE_DEVICES:-not set}"
echo ""
echo "=== nvidia-smi output ==="
nvidia-smi
echo ""
echo "=== GPU Detection ==="
if command -v nvidia-smi &> /dev/null; then
    nvidia-smi --query-gpu=index,name,driver_version,memory.total --format=csv
else
    echo "nvidia-smi not found"
fi
'

    local jobid
    jobid=$(docker exec slurmctld bash -c "cd /data && cat > gpu-job.sh <<'EOF'
$job_script
EOF
chmod +x gpu-job.sh && sbatch gpu-job.sh | grep -oP '\\d+'")

    if [[ -z "$jobid" ]]; then
        log_error "Failed to submit GPU job"
        return 1
    fi

    log_info "Job $jobid submitted, waiting for completion..."

    # Wait for job to complete (max 60 seconds)
    local elapsed=0
    while [[ $elapsed -lt 60 ]]; do
        local state
        state=$(docker exec slurmctld scontrol show job "$jobid" | grep "JobState=" | grep -oP 'JobState=\K\w+' || echo "UNKNOWN")

        if [[ "$state" == "COMPLETED" ]]; then
            log_success "GPU job $jobid completed successfully"

            # Show job output
            echo ""
            echo "========== Job Output =========="
            docker exec slurmctld bash -c "cd /data && cat gpu-test-${jobid}.out"
            echo "================================"

            return 0
        elif [[ "$state" == "FAILED" ]] || [[ "$state" == "CANCELLED" ]] || [[ "$state" == "TIMEOUT" ]]; then
            log_error "GPU job $jobid failed with state: $state"
            docker exec slurmctld bash -c "cd /data && cat gpu-test-${jobid}.out" || echo "No output file"
            return 1
        fi

        sleep 2
        ((elapsed+=2))
    done

    log_error "GPU job $jobid timed out after 60 seconds"
    docker exec slurmctld scontrol show job "$jobid"
    return 1
}

# Test GPU allocation in squeue
test_gpu_allocation() {
    run_test "Verify GPU allocation shows in job"

    # Submit a held job to check allocation
    local jobid
    jobid=$(docker exec slurmctld bash -c "cd /data && sbatch --hold --partition=gpu --gres=gpu:1 --wrap='sleep 10' | grep -oP '\\d+'")

    if [[ -z "$jobid" ]]; then
        log_error "Failed to submit held GPU job"
        return 1
    fi

    local gres_alloc
    gres_alloc=$(docker exec slurmctld scontrol show job "$jobid" | grep "TresPerNode=" || echo "")

    # Cancel the held job
    docker exec slurmctld scancel "$jobid" || true

    if [[ "$gres_alloc" =~ gres/gpu ]]; then
        log_success "GPU allocation visible in job: $gres_alloc"
        return 0
    else
        log_error "GPU allocation not visible in job"
        echo "Expected TresPerNode to contain gres/gpu"
        echo "Got: $gres_alloc"
        return 1
    fi
}

# Main test execution
main() {
    echo ""
    echo "=========================================="
    echo "GPU Test Suite for slurm-docker-cluster"
    echo "=========================================="
    echo ""

    # Check prerequisites
    if ! check_gpu_enabled; then
        echo ""
        echo "GPU profile not enabled. Exiting."
        exit 1
    fi

    if ! check_g1_running; then
        echo ""
        echo "g1 container not running. Exiting."
        exit 1
    fi

    # Run tests (|| true so set -e doesn't abort on first failure; failures are tracked via TESTS_FAILED)
    check_g1_registered || true
    check_gres_configured || true
    check_gpu_partition || true
    check_nvidia_smi || true
    test_gpu_job_submission || true
    test_gpu_allocation || true

    # Summary
    echo ""
    echo "=========================================="
    echo "Test Summary"
    echo "=========================================="
    echo "Total tests: $TESTS_RUN"
    echo -e "${GREEN}Passed: $TESTS_PASSED${NC}"
    if [[ $TESTS_FAILED -gt 0 ]]; then
        echo -e "${RED}Failed: $TESTS_FAILED${NC}"
        exit 1
    else
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    fi
}

main "$@"
