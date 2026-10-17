#!/bin/bash
set -euo pipefail

# Test suite for the monitoring profile (Elasticsearch + Kibana)
# Run with: ./test_monitoring.sh

CI_MODE=${CI:-false}

# Detect container runtime (docker or podman)
if [ -z "$CONTAINER_RUNTIME" ]; then
    if command -v podman &>/dev/null && podman info &>/dev/null 2>&1; then
        CONTAINER_RUNTIME="podman"
    else
        CONTAINER_RUNTIME="docker"
    fi
fi

COMPOSE_CMD="$CONTAINER_RUNTIME compose"

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
    NC='\033[0m'
fi

TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

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

test_elasticsearch_running() {
    print_test "Checking if Elasticsearch is running..."

    if $COMPOSE_CMD ps elasticsearch 2>/dev/null | grep -q "Up"; then
        print_pass "Elasticsearch is running"
    else
        print_fail "Elasticsearch is not running"
        return 1
    fi
}

test_elasticsearch_healthy() {
    print_test "Checking Elasticsearch cluster health..."

    HEALTH=$(run_container elasticsearch curl -s "http://localhost:9200/_cluster/health" | grep -o '"status":"[^"]*"' | cut -d'"' -f4)

    if [ "$HEALTH" = "green" ] || [ "$HEALTH" = "yellow" ]; then
        print_pass "Elasticsearch cluster is healthy (status: $HEALTH)"
    else
        print_fail "Elasticsearch cluster is unhealthy (status: $HEALTH)"
        return 1
    fi
}

test_kibana_running() {
    print_test "Checking if Kibana is running..."

    if $COMPOSE_CMD ps kibana 2>/dev/null | grep -q "Up"; then
        print_pass "Kibana is running"
    else
        print_fail "Kibana is not running"
        return 1
    fi
}

test_kibana_healthy() {
    print_test "Checking Kibana API status..."

    STATUS=$(run_container kibana curl -s "http://localhost:5601/api/status" | grep -o '"overall":{"level":"[^"]*"' | cut -d'"' -f6)

    if [ "$STATUS" = "available" ]; then
        print_pass "Kibana is available"
    else
        print_fail "Kibana is not available (status: $STATUS)"
        return 1
    fi
}

test_slurm_elasticsearch_config() {
    print_test "Checking Slurm Elasticsearch configuration..."

    JOBCOMP_TYPE=$(run_container slurmctld grep "^JobCompType" /etc/slurm/slurm.conf | cut -d= -f2)
    JOBCOMP_LOC=$(run_container slurmctld grep "^JobCompLoc" /etc/slurm/slurm.conf | cut -d= -f2)

    if [ "$JOBCOMP_TYPE" = "jobcomp/elasticsearch" ]; then
        print_info "  JobCompType=$JOBCOMP_TYPE"
        print_info "  JobCompLoc=$JOBCOMP_LOC"
        print_pass "Slurm configured for Elasticsearch job completion"
    else
        print_fail "Slurm not configured for Elasticsearch (JobCompType=$JOBCOMP_TYPE)"
        return 1
    fi
}

test_job_completion_indexed() {
    print_test "Testing job completion indexing to Elasticsearch..."

    # Submit a job and wait for completion
    JOB_ID=$(run_container slurmctld bash -c "cd /data && sbatch --wrap='echo test' --wait" 2>&1 | sed -n 's/.*Submitted batch job \([0-9][0-9]*\).*/\1/p')

    if [ -z "$JOB_ID" ]; then
        print_fail "Failed to submit test job"
        return 1
    fi

    print_info "  Submitted job $JOB_ID, waiting for indexing..."

    # Wait for job completion to be indexed (max 30 seconds)
    for i in {1..30}; do
        # Search for the job in Elasticsearch (index is "slurm")
        RESULT=$(run_container elasticsearch curl -s "http://localhost:9200/slurm/_search" 2>/dev/null || echo "")

        if echo "$RESULT" | grep -q "\"jobid\":$JOB_ID"; then
            print_pass "Job $JOB_ID indexed in Elasticsearch"
            return 0
        fi
        sleep 1
    done

    # Show debug info on failure
    print_info "  Checking if index exists..."
    run_container elasticsearch curl -s "http://localhost:9200/_cat/indices?v" 2>/dev/null || true
    print_fail "Job $JOB_ID not found in Elasticsearch after 30 seconds"
    return 1
}

main() {
    print_header "Monitoring Profile Test Suite"

    if [ "$CI_MODE" = "true" ]; then
        print_info "Running in CI mode"
    fi

    echo ""

    test_elasticsearch_running || true
    test_elasticsearch_healthy || true
    test_kibana_running || true
    test_kibana_healthy || true
    test_slurm_elasticsearch_config || true
    test_job_completion_indexed || true

    echo ""
    print_header "Test Summary"
    echo -e "Tests Run:    ${BLUE}$TESTS_RUN${NC}"
    echo -e "Tests Passed: ${GREEN}$TESTS_PASSED${NC}"
    echo -e "Tests Failed: ${RED}$TESTS_FAILED${NC}"
    echo ""

    if [ $TESTS_FAILED -eq 0 ]; then
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    else
        echo -e "${RED}Some tests failed!${NC}"
        exit 1
    fi
}

main
