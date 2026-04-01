#!/bin/bash
set -e

# Detect if running in CI
CI_MODE=${CI:-false}

# Colors for output (disabled in CI for better log readability)
if [ "$CI_MODE" = "true" ]; then
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
else
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m' # No Color
fi

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Print functions
print_header() {
    echo -e "${BLUE}================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}================================${NC}"
}

print_test() {
    echo -e "${YELLOW}[TEST]${NC} $1"
    TESTS_RUN=$((TESTS_RUN + 1))
}

print_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

print_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    TESTS_FAILED=$((TESTS_FAILED + 1))
}

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

# Test functions
test_containers_running() {
    print_test "Checking if all containers are running..."

    EXPECTED_CONTAINERS=("mysql" "slurmdbd" "slurmctld" "slurmrestd")
    ALL_RUNNING=true

    for container in "${EXPECTED_CONTAINERS[@]}"; do
        if docker compose ps "$container" 2>/dev/null | grep -q "Up"; then
            print_info "  ✓ $container is running"
        else
            print_fail "  ✗ $container is not running"
            ALL_RUNNING=false
        fi
    done

    # Check cpu-worker nodes dynamically
    WORKER_COUNT=$(docker compose ps cpu-worker --format '{{.Names}}' 2>/dev/null | wc -l)

    if [ "$WORKER_COUNT" -gt 0 ]; then
        print_info "  ✓ $WORKER_COUNT worker node(s) running"
    else
        print_fail "  ✗ No worker nodes running"
        ALL_RUNNING=false
    fi

    if [ "$ALL_RUNNING" = true ]; then
        print_pass "All containers are running"
    else
        print_fail "Some containers are not running"
        return 1
    fi
}

test_munge_auth() {
    print_test "Testing MUNGE authentication..."

    if docker exec slurmctld bash -c "munge -n | unmunge" >/dev/null 2>&1; then
        print_pass "MUNGE authentication is working"
    else
        print_fail "MUNGE authentication failed"
        return 1
    fi
}

test_mysql_connection() {
    print_test "Testing MySQL database connection..."

    if docker exec slurmdbd bash -c "echo 'SELECT 1' | mysql -h mysql -u\${MYSQL_USER} -p\${MYSQL_PASSWORD} 2>/dev/null" >/dev/null; then
        print_pass "MySQL connection successful"
    else
        print_fail "MySQL connection failed"
        return 1
    fi
}

test_slurmdbd_connection() {
    print_test "Testing slurmdbd daemon..."

    if docker exec slurmctld sacctmgr list cluster -n 2>/dev/null | grep -q "linux"; then
        print_pass "slurmdbd is responding and cluster is registered"
    else
        print_fail "slurmdbd is not responding or cluster not registered"
        return 1
    fi
}

test_slurmctld_status() {
    print_test "Testing slurmctld daemon..."

    if docker exec slurmctld scontrol ping >/dev/null 2>&1; then
        print_pass "slurmctld is responding"
    else
        print_fail "slurmctld is not responding"
        return 1
    fi
}

test_compute_nodes() {
    print_test "Testing compute nodes availability..."

    NODE_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)

    # Count running cpu-worker nodes
    EXPECTED_COUNT=$(docker compose ps cpu-worker --format '{{.Names}}' 2>/dev/null | wc -l)

    if [ "$NODE_COUNT" -eq "$EXPECTED_COUNT" ]; then
        print_pass "$NODE_COUNT compute node(s) are available (matches expected $EXPECTED_COUNT)"
    else
        print_fail "Expected $EXPECTED_COUNT compute nodes, found $NODE_COUNT"
        return 1
    fi
}

test_nodes_state() {
    print_test "Testing compute nodes state..."

    IDLE_NODES=$(docker exec slurmctld sinfo -h -o "%T" | grep -c "idle" || echo "0")

    if [ "$IDLE_NODES" -ge 1 ]; then
        print_pass "Compute nodes are in idle state ($IDLE_NODES nodes)"
    else
        print_fail "No compute nodes in idle state"
        docker exec slurmctld sinfo
        return 1
    fi
}

test_partition() {
    print_test "Testing partition configuration..."

    if docker exec slurmctld sinfo -h | grep -q "cpu"; then
        print_pass "Default partition 'cpu' exists"
    else
        print_fail "Default partition 'cpu' not found"
        return 1
    fi
}

test_config_consistency() {
    print_test "Testing config consistency (slurm.conf vs entrypoint Feature tags)..."

    ALL_CONSISTENT=true

    # Get the active slurm.conf from the running container
    SLURM_CONF=$(docker exec slurmctld cat /etc/slurm/slurm.conf 2>/dev/null)
    ENTRYPOINT=$(docker exec slurmctld cat /usr/local/bin/docker-entrypoint.sh 2>/dev/null)

    if [ -z "$SLURM_CONF" ] || [ -z "$ENTRYPOINT" ]; then
        print_fail "Could not read slurm.conf or docker-entrypoint.sh from container"
        return 1
    fi

    # Check CPU: slurm.conf NodeSet feature must match entrypoint's slurmd-cpu Feature
    CPU_CONF_FEATURE=$(echo "$SLURM_CONF" | grep 'NodeSet=cpu_nodes' | sed -n 's/.*Feature=\([^ ]*\).*/\1/p' || true)
    CPU_ENTRY_FEATURE=$(echo "$ENTRYPOINT" | sed -n '/slurmd-cpu/,/^fi$/p' | sed -n 's/.*Feature=\([a-zA-Z0-9_]*\).*/\1/p' | head -1 || true)

    if [ -z "$CPU_CONF_FEATURE" ]; then
        print_fail "  No cpu_nodes NodeSet Feature found in slurm.conf"
        ALL_CONSISTENT=false
    elif [ -z "$CPU_ENTRY_FEATURE" ]; then
        print_fail "  No Feature found in entrypoint for slurmd-cpu"
        ALL_CONSISTENT=false
    elif [ "$CPU_CONF_FEATURE" = "$CPU_ENTRY_FEATURE" ]; then
        print_info "  ✓ CPU Feature matches: slurm.conf($CPU_CONF_FEATURE) = entrypoint($CPU_ENTRY_FEATURE)"
    else
        print_fail "  CPU Feature mismatch: slurm.conf($CPU_CONF_FEATURE) != entrypoint($CPU_ENTRY_FEATURE)"
        ALL_CONSISTENT=false
    fi

    # Check GPU: slurm.conf NodeSet feature must match entrypoint's slurmd-gpu Feature
    GPU_CONF_FEATURE=$(echo "$SLURM_CONF" | grep 'NodeSet=gpu_nodes' | sed -n 's/.*Feature=\([^ ]*\).*/\1/p' || true)
    GPU_ENTRY_FEATURE=$(echo "$ENTRYPOINT" | sed -n '/slurmd-gpu/,/^fi$/p' | sed -n 's/.*Feature=\([a-zA-Z0-9_]*\).*/\1/p' | head -1 || true)

    if [ -z "$GPU_CONF_FEATURE" ]; then
        print_fail "  No gpu_nodes NodeSet Feature found in slurm.conf"
        ALL_CONSISTENT=false
    elif [ -z "$GPU_ENTRY_FEATURE" ]; then
        print_fail "  No Feature found in entrypoint for slurmd-gpu"
        ALL_CONSISTENT=false
    elif [ "$GPU_CONF_FEATURE" = "$GPU_ENTRY_FEATURE" ]; then
        print_info "  ✓ GPU Feature matches: slurm.conf($GPU_CONF_FEATURE) = entrypoint($GPU_ENTRY_FEATURE)"
    else
        print_fail "  GPU Feature mismatch: slurm.conf($GPU_CONF_FEATURE) != entrypoint($GPU_ENTRY_FEATURE)"
        ALL_CONSISTENT=false
    fi

    # Verify CPU and GPU partitions reference their respective NodeSets
    CPU_PARTITION_NODES=$(echo "$SLURM_CONF" | grep 'PartitionName=cpu ' | sed -n 's/.*Nodes=\([^ ]*\).*/\1/p' || true)
    GPU_PARTITION_NODES=$(echo "$SLURM_CONF" | grep 'PartitionName=gpu ' | sed -n 's/.*Nodes=\([^ ]*\).*/\1/p' || true)

    if [ "$CPU_PARTITION_NODES" = "cpu_nodes" ]; then
        print_info "  ✓ cpu partition references cpu_nodes NodeSet"
    else
        print_fail "  cpu partition references '$CPU_PARTITION_NODES' instead of 'cpu_nodes'"
        ALL_CONSISTENT=false
    fi

    if [ "$GPU_PARTITION_NODES" = "gpu_nodes" ]; then
        print_info "  ✓ gpu partition references gpu_nodes NodeSet"
    else
        print_fail "  gpu partition references '$GPU_PARTITION_NODES' instead of 'gpu_nodes'"
        ALL_CONSISTENT=false
    fi

    if [ "$ALL_CONSISTENT" = true ]; then
        print_pass "Config consistency verified (CPU=Feature=$CPU_CONF_FEATURE, GPU=Feature=$GPU_CONF_FEATURE)"
    else
        print_fail "Config inconsistencies detected"
        return 1
    fi
}

test_job_submission() {
    print_test "Testing job submission..."

    # Submit a simple job
    JOB_ID=$(docker exec slurmctld bash -c "cd /data && sbatch --wrap='hostname' 2>&1" | sed -n 's/.*Submitted batch job \([0-9][0-9]*\).*/\1/p')

    if [ -n "$JOB_ID" ]; then
        print_info "  Job ID: $JOB_ID submitted"

        # Wait for job to complete (max 30 seconds)
        for i in {1..30}; do
            JOB_STATE=$(docker exec slurmctld squeue -j "$JOB_ID" -h -o "%T" 2>/dev/null || echo "COMPLETED")
            if [ "$JOB_STATE" = "COMPLETED" ] || [ -z "$JOB_STATE" ]; then
                break
            fi
            sleep 1
        done

        print_pass "Job submitted successfully (Job ID: $JOB_ID)"
    else
        print_fail "Job submission failed"
        return 1
    fi
}

test_job_execution() {
    print_test "Testing job execution and output..."

    # Submit a job and wait for it
    OUTPUT=$(docker exec slurmctld bash -c "cd /data && sbatch --wrap='echo SUCCESS_TEST_\$SLURM_JOB_ID' --wait 2>&1 && sleep 2 && cat slurm-*.out | grep SUCCESS_TEST" 2>/dev/null || echo "")

    if echo "$OUTPUT" | grep -q "SUCCESS_TEST"; then
        print_pass "Job executed and produced output"
    else
        print_fail "Job execution failed or no output"
        return 1
    fi
}

test_job_accounting() {
    print_test "Testing job accounting..."

    # Check if sacct can retrieve job history
    if docker exec slurmctld sacct -n --format=JobID -X 2>/dev/null | grep -q "[0-9]"; then
        print_pass "Job accounting is working"
    else
        print_fail "Job accounting failed - no jobs recorded"
        return 1
    fi
}

test_multi_node_job() {
    print_test "Testing multi-node job allocation..."

    # Get current node count
    NODE_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)

    # Only run multi-node test if we have 2+ nodes
    if [ "$NODE_COUNT" -lt 2 ]; then
        print_info "  Skipping (only $NODE_COUNT node available)"
        return 0
    fi

    # Try to allocate 2 nodes
    JOB_OUTPUT=$(docker exec slurmctld bash -c "srun -N 2 hostname" 2>&1 || echo "FAILED")

    # Count non-empty lines in output (should be 2 hostnames)
    OUTPUT_LINES=$(echo "$JOB_OUTPUT" | grep -v "^$" | wc -l)

    if [ "$OUTPUT_LINES" -eq 2 ]; then
        print_pass "Multi-node job executed on 2 nodes"
    else
        print_fail "Multi-node job failed"
        print_info "  Output: $JOB_OUTPUT"
        return 1
    fi
}

test_resource_limits() {
    print_test "Testing resource limit configuration..."

    # Check if dynamic nodes have CPU and memory configured
    FIRST_NODE=$(docker exec slurmctld scontrol show nodes 2>/dev/null | grep -o 'NodeName=c[0-9]*' | head -1 | cut -d= -f2)

    if [ -z "$FIRST_NODE" ]; then
        print_fail "No dynamic CPU worker nodes found"
        return 1
    fi

    NODE_INFO=$(docker exec slurmctld scontrol show node "$FIRST_NODE" | grep -E "CPUTot|RealMemory")

    if echo "$NODE_INFO" | grep -q "CPUTot=[0-9]" && echo "$NODE_INFO" | grep -q "RealMemory=[0-9]"; then
        print_pass "Resource limits configured correctly on $FIRST_NODE"
    else
        print_fail "Resource limits not configured as expected on $FIRST_NODE"
        print_info "  Node info: $NODE_INFO"
        return 1
    fi
}

test_scale_up() {
    print_test "Testing dynamic scale-up..."

    # Get initial node count
    INITIAL_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)
    print_info "  Initial node count: $INITIAL_COUNT"

    # Get current worker count and scale up by 1
    WORKER_COUNT=$(docker compose ps cpu-worker --format '{{.Names}}' 2>/dev/null | wc -l)
    TARGET=$((WORKER_COUNT + 1))
    print_info "  Scaling cpu-worker from $WORKER_COUNT to $TARGET"

    docker compose up -d --scale cpu-worker=$TARGET --no-recreate >/dev/null 2>&1

    # Poll until the new node appears in Slurm and is ready (max 60s)
    EXPECTED_COUNT=$((INITIAL_COUNT + 1))
    for i in $(seq 1 60); do
        CURRENT_COUNT=$(docker exec slurmctld sinfo -N -h 2>/dev/null | wc -l)
        if [ "$CURRENT_COUNT" -ge "$EXPECTED_COUNT" ]; then
            break
        fi
        sleep 1
    done

    CURRENT_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)

    if [ "$CURRENT_COUNT" -ne "$EXPECTED_COUNT" ]; then
        print_fail "Expected $EXPECTED_COUNT nodes after scale-up, found $CURRENT_COUNT"
        return 1
    fi

    print_info "  Node count after scale-up: $CURRENT_COUNT"

    # Verify all node names match c<N> pattern (no container ID fallbacks)
    BAD_NAMES=$(docker exec slurmctld scontrol show nodes 2>/dev/null \
        | grep -o 'NodeName=[^ ]*' | cut -d= -f2 | grep -v '^c[0-9]*$' || true)

    if [ -n "$BAD_NAMES" ]; then
        print_fail "Found node names not matching c<N> pattern: $BAD_NAMES"
        return 1
    fi

    # Verify all nodes are idle
    NON_IDLE=$(docker exec slurmctld sinfo -N -h -o "%N %T" 2>/dev/null | grep -v "idle" || true)

    if [ -n "$NON_IDLE" ]; then
        print_fail "Some nodes are not idle after scale-up: $NON_IDLE"
        return 1
    fi

    print_pass "Scale-up successful ($INITIAL_COUNT -> $CURRENT_COUNT nodes, all idle)"
}

test_scale_down() {
    print_test "Testing dynamic scale-down with stale node cleanup..."

    # Get current counts
    CURRENT_NODE_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)
    WORKER_COUNT=$(docker compose ps cpu-worker --format '{{.Names}}' 2>/dev/null | wc -l)

    if [ "$WORKER_COUNT" -lt 2 ]; then
        print_info "  Skipping (only $WORKER_COUNT worker, need at least 2 to scale down)"
        return 0
    fi

    TARGET=$((WORKER_COUNT - 1))
    EXPECTED_COUNT=$((CURRENT_NODE_COUNT - 1))
    print_info "  Scaling cpu-worker from $WORKER_COUNT to $TARGET"

    docker compose up -d --scale cpu-worker=$TARGET --no-recreate >/dev/null 2>&1

    # Wait for the extra container to stop
    for i in $(seq 1 15); do
        LIVE=$(docker compose ps cpu-worker --format '{{.Names}}' 2>/dev/null | wc -l)
        if [ "$LIVE" -le "$TARGET" ]; then
            break
        fi
        sleep 1
    done

    # Clean up stale nodes (mirrors Makefile scale-cpu-workers logic)
    LIVE_NODES=$(docker compose ps cpu-worker -q 2>/dev/null \
        | while read cid; do
            docker exec "$cid" hostname 2>/dev/null
        done | sort)
    SLURM_NODES=$(docker exec slurmctld scontrol show nodes 2>/dev/null \
        | grep -o 'NodeName=c[0-9]*' | cut -d= -f2 | sort)
    STALE_NODES=$(comm -23 <(echo "$SLURM_NODES") <(echo "$LIVE_NODES") | paste -sd, -)

    if [ -n "$STALE_NODES" ]; then
        print_info "  Removing stale nodes: $STALE_NODES"
        docker exec slurmctld scontrol delete nodename=$STALE_NODES 2>/dev/null || true
    fi

    # Verify node count decreased
    FINAL_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)

    if [ "$FINAL_COUNT" -ne "$EXPECTED_COUNT" ]; then
        print_fail "Expected $EXPECTED_COUNT nodes after scale-down, found $FINAL_COUNT"
        return 1
    fi

    # Verify the stale node is gone
    if [ -n "$STALE_NODES" ]; then
        FIRST_STALE=$(echo "$STALE_NODES" | cut -d, -f1)
        if docker exec slurmctld scontrol show node "$FIRST_STALE" 2>/dev/null | grep -q "NodeName"; then
            print_fail "Stale node $FIRST_STALE still present in Slurm"
            return 1
        fi
    fi

    print_pass "Scale-down successful ($CURRENT_NODE_COUNT -> $FINAL_COUNT nodes, stale nodes cleaned)"
}

test_singularity_pull_image() {
    print_test "Testing Singularity image pull..."

    # Check if alpine_3.22.2.sif exists, if yes, remove it
    if docker exec slurmctld test -f alpine_3.22.2.sif >/dev/null 2>&1; then
        docker exec slurmctld rm alpine_3.22.2.sif >/dev/null 2>&1 || true
    fi

    if docker exec slurmctld singularity pull docker://alpine:3.22.2 >/dev/null 2>&1; then
        print_pass "Singularity image pull successful"
    else
        print_fail "Singularity image pull failed"
        return 1
    fi
}

test_singularity_multi_node_job() {
    print_test "Testing Singularity multi-node job..."

    # Get current node count
    NODE_COUNT=$(docker exec slurmctld sinfo -N -h | wc -l)

    # Only run multi-node test if we have 2+ nodes
    if [ "$NODE_COUNT" -lt 2 ]; then
        print_info "  Skipping (only $NODE_COUNT node available)"
        return 0
    fi

    # Check if alpine_3.22.2.sif exists, if not, pull it
    if ! docker exec slurmctld test -f alpine_3.22.2.sif >/dev/null 2>&1; then
        if ! docker exec slurmctld singularity pull docker://alpine:3.22.2 >/dev/null 2>&1; then
            print_fail "Failed to pull Singularity image"
            return 1
        fi
    fi

    # Run the multi-node singularity job
    JOB_OUTPUT=$(docker exec slurmctld bash -c "srun -N 2 singularity exec alpine_3.22.2.sif /bin/sh -c 'cat /etc/alpine-release'" 2>&1 || echo "FAILED")

    # Count non-empty lines in output (should be 2 Alpine release lines)
    OUTPUT_LINES=$(echo "$JOB_OUTPUT" | grep -v "^$" | wc -l)

    # Clean up the image
    docker exec slurmctld rm alpine_3.22.2.sif >/dev/null 2>&1 || true

    if [ "$OUTPUT_LINES" -eq 2 ]; then
        print_pass "Singularity multi-node job executed successfully on 2 nodes"
    else
        print_fail "Singularity multi-node job failed"
        print_info "  Output: $JOB_OUTPUT"
        return 1
    fi
}

test_get_jwt_token() {
    print_test "Testing JWT token generation..."

    # Get JWT token from scontrol
    TOKEN_OUTPUT=$(docker exec slurmctld scontrol token 2>&1)

    # Extract token value (format: SLURM_JWT=eyJhb...)
    if echo "$TOKEN_OUTPUT" | grep -q "SLURM_JWT="; then
        JWT_TOKEN=$(echo "$TOKEN_OUTPUT" | grep "SLURM_JWT=" | cut -d'=' -f2)

        if [ -z "$JWT_TOKEN" ]; then
            print_fail "JWT token is empty"
            return 1
        fi

        print_info "  JWT Token: ${JWT_TOKEN:0:50}..."

        # Validate JWT token format (should have 3 parts separated by dots)
        DOT_COUNT=$(echo "$JWT_TOKEN" | grep -o '\.' | wc -l)

        if [ "$DOT_COUNT" -eq 2 ]; then
            # Verify each part contains valid base64-like characters
            HEADER=$(echo "$JWT_TOKEN" | cut -d'.' -f1)
            PAYLOAD=$(echo "$JWT_TOKEN" | cut -d'.' -f2)
            SIGNATURE=$(echo "$JWT_TOKEN" | cut -d'.' -f3)

            if [ -n "$HEADER" ] && [ -n "$PAYLOAD" ] && [ -n "$SIGNATURE" ]; then
                print_pass "Valid JWT token generated (3 valid parts)"
            else
                print_fail "JWT token has empty parts"
                return 1
            fi
        else
            print_fail "Invalid JWT token format (expected 3 parts separated by dots, got $((DOT_COUNT + 1)))"
            print_info "  Token: $JWT_TOKEN"
            return 1
        fi
    else
        print_fail "Failed to get JWT token or invalid output format"
        print_info "  Output: $TOKEN_OUTPUT"
        return 1
    fi
}

test_validate_jwt_authentication() {
    print_test "Testing JWT authentication via slurmrestd..."

    # Get JWT token from scontrol
    TOKEN_OUTPUT=$(docker exec slurmctld scontrol token 2>&1)

    # Extract token value (format: SLURM_JWT=eyJhb...)
    if ! echo "$TOKEN_OUTPUT" | grep -q "SLURM_JWT="; then
        print_fail "Failed to get JWT token"
        print_info "  Output: $TOKEN_OUTPUT"
        return 1
    fi

    JWT_TOKEN=$(echo "$TOKEN_OUTPUT" | grep "SLURM_JWT=" | cut -d'=' -f2)

    if [ -z "$JWT_TOKEN" ]; then
        print_fail "JWT token is empty"
        return 1
    fi

    print_info "  Using JWT Token: ${JWT_TOKEN:0:50}..."

    API_VERSION=$(get_api_version)

    print_info "  Using data_parser version: $API_VERSION"

    # Execute curl request with JWT token and check HTTP status code
    HTTP_CODE=$(docker exec slurmctld bash -c "curl -s -o /dev/null -w '%{http_code}' -k \
        -H 'X-SLURM-USER-TOKEN: $JWT_TOKEN' \
        -X GET 'http://slurmrestd:6820/slurm/$API_VERSION/diag'" 2>&1)

    print_info "  HTTP Status Code: $HTTP_CODE"

    if [ "$HTTP_CODE" = "200" ]; then
        print_pass "JWT authentication successful (HTTP 200)"
    else
        print_fail "JWT authentication failed (HTTP $HTTP_CODE, expected 200)"
        return 1
    fi
}

test_python_version() {
    print_test "Testing Python version..."

    # Check Python 3.12 is installed and default
    PYTHON_VERSION=$(docker exec slurmctld python3 --version 2>&1 | awk '{print $2}')

    if echo "$PYTHON_VERSION" | grep -q "^3\.12\."; then
        print_pass "Python 3.12 is the default version ($PYTHON_VERSION)"
    else
        print_fail "Python version is not 3.12.x (found: $PYTHON_VERSION)"
        return 1
    fi
}

# Get the latest data_parser version from slurmrestd
get_api_version() {
    local DATA_PARSER_VERSION=$(docker exec slurmctld bash -c "gosu slurmrest slurmrestd -d list 2>&1 | grep 'data_parser/' | tail -1 | sed 's/.*data_parser\///' | tr -d '[:space:]'" 2>&1)

    # Validate format: must be "v" followed by three numbers separated by two dots (e.g., v0.0.44)
    if ! [[ $DATA_PARSER_VERSION =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        print_fail "Invalid data_parser version format: '$DATA_PARSER_VERSION' (expected format: v0.0.44)"
        return 1
    fi

    echo $DATA_PARSER_VERSION
}

test_rest_api_nodes() {
    print_test "Testing REST API /nodes endpoint (README example)..."

    # Detect API version
    API_VERSION=$(get_api_version)
    print_info "  Using API version: $API_VERSION"

    # Get JWT token for authentication
    JWT_TOKEN=$(docker exec slurmctld scontrol token 2>&1 | grep "SLURM_JWT=" | cut -d'=' -f2)
    if [ -z "$JWT_TOKEN" ]; then
        print_fail "Failed to get JWT token for REST API authentication"
        return 1
    fi

    # Test REST API with JWT authentication
    RESPONSE=$(docker exec slurmctld curl -s -H "X-SLURM-USER-TOKEN: $JWT_TOKEN" \
        "http://slurmrestd:6820/slurm/${API_VERSION}/nodes" 2>&1)

    # Check if response is valid JSON
    if echo "$RESPONSE" | jq empty 2>/dev/null; then
        print_info "  ✓ Response is valid JSON"

        # Check for expected fields and node data
        if echo "$RESPONSE" | jq -e '.nodes' >/dev/null 2>&1; then
            NODE_COUNT=$(echo "$RESPONSE" | jq '.nodes | length')
            print_info "  ✓ Found $NODE_COUNT node(s)"
            print_pass "REST API /nodes endpoint works correctly"
        else
            print_fail "REST API /nodes response missing 'nodes' field"
            print_info "  Response: $RESPONSE"
            return 1
        fi
    else
        print_fail "REST API /nodes returned invalid JSON"
        print_info "  Response: $RESPONSE"
        return 1
    fi
}

test_rest_api_partitions() {
    print_test "Testing REST API /partitions endpoint (README example)..."

    # Detect API version
    API_VERSION=$(get_api_version)

    # Get JWT token for authentication
    JWT_TOKEN=$(docker exec slurmctld scontrol token 2>&1 | grep "SLURM_JWT=" | cut -d'=' -f2)
    if [ -z "$JWT_TOKEN" ]; then
        print_fail "Failed to get JWT token for REST API authentication"
        return 1
    fi

    # Test REST API with JWT authentication
    RESPONSE=$(docker exec slurmctld curl -s -H "X-SLURM-USER-TOKEN: $JWT_TOKEN" \
        "http://slurmrestd:6820/slurm/${API_VERSION}/partitions" 2>&1)

    # Check if response is valid JSON
    if echo "$RESPONSE" | jq empty 2>/dev/null; then
        print_info "  ✓ Response is valid JSON"

        # Check for expected fields
        if echo "$RESPONSE" | jq -e '.partitions' >/dev/null 2>&1; then
            PARTITION_COUNT=$(echo "$RESPONSE" | jq '.partitions | length')
            print_info "  ✓ Found $PARTITION_COUNT partition(s)"
            print_pass "REST API /partitions endpoint works correctly"
        else
            print_fail "REST API /partitions response missing 'partitions' field"
            print_info "  Response: $RESPONSE"
            return 1
        fi
    else
        print_fail "REST API /partitions returned invalid JSON"
        print_info "  Response: $RESPONSE"
        return 1
    fi
}

# Main test execution
main() {
    # Ensure .env exists - copy from .env.example if not present
    if [ ! -f .env ]; then
        if [ -f .env.example ]; then
            cp .env.example .env
            echo "Created .env from .env.example"
        else
            echo "ERROR: Neither .env nor .env.example found"
            exit 1
        fi
    fi

    # Read Slurm version from .env file
    SLURM_VERSION=$(grep SLURM_VERSION .env | cut -d= -f2)

    print_header "Slurm Docker Cluster Test Suite (v${SLURM_VERSION})"

    if [ "$CI_MODE" = "true" ]; then
        print_info "Running in CI mode"
    fi

    echo ""

    # Run all tests (continue even if some fail)
    test_containers_running || true
    test_munge_auth || true
    test_mysql_connection || true
    test_slurmdbd_connection || true
    test_slurmctld_status || true
    test_compute_nodes || true
    test_nodes_state || true
    test_partition || true
    test_config_consistency || true
    test_job_submission || true
    test_job_execution || true
    test_job_accounting || true
    test_multi_node_job || true
    test_resource_limits || true
    test_scale_up || true
    test_scale_down || true
    test_python_version || true
    test_singularity_pull_image || true
    test_singularity_multi_node_job || true
    test_rest_api_nodes || true
    test_rest_api_partitions || true
    test_get_jwt_token || true
    test_validate_jwt_authentication || true

    # Print summary
    echo ""
    print_header "Test Summary"
    echo -e "Tests Run:    ${BLUE}$TESTS_RUN${NC}"
    echo -e "Tests Passed: ${GREEN}$TESTS_PASSED${NC}"
    echo -e "Tests Failed: ${RED}$TESTS_FAILED${NC}"
    echo ""

    if [ $TESTS_FAILED -eq 0 ]; then
        echo -e "${GREEN}✓ All tests passed!${NC}"

        # GitHub Actions output
        if [ "$CI_MODE" = "true" ]; then
            echo "::notice title=Test Suite::All $TESTS_RUN tests passed successfully"
        fi

        exit 0
    else
        echo -e "${RED}✗ Some tests failed!${NC}"

        # GitHub Actions output
        if [ "$CI_MODE" = "true" ]; then
            echo "::error title=Test Suite::$TESTS_FAILED out of $TESTS_RUN tests failed"
        fi

        exit 1
    fi
}

# Run main function
main
