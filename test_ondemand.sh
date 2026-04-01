#!/bin/bash
set -e

# Test suite for the Open OnDemand profile
# Run with: ./test_ondemand.sh
# Requires: docker compose --profile ondemand up -d

CI_MODE=${CI:-false}

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

test_ondemand_running() {
    print_test "Checking if Open OnDemand container is running..."

    if docker compose --profile ondemand ps ondemand 2>/dev/null | grep -q "Up"; then
        print_pass "Open OnDemand container is running"
    else
        print_fail "Open OnDemand container is not running"
        return 1
    fi
}

test_httpd_responding() {
    print_test "Checking if Apache httpd is responding on port 8080..."

    HTTP_CODE=$(docker exec ondemand curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/ 2>/dev/null || echo "000")

    if [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "302" ] || [ "$HTTP_CODE" = "303" ]; then
        print_pass "Apache httpd is responding (HTTP $HTTP_CODE)"
    else
        print_fail "Apache httpd is not responding (HTTP $HTTP_CODE)"
        return 1
    fi
}

test_dex_responding() {
    print_test "Checking if Dex OIDC provider is responding..."

    HTTP_CODE=$(docker exec ondemand curl -s -o /dev/null -w "%{http_code}" http://localhost:5556/dex/.well-known/openid-configuration 2>/dev/null || echo "000")

    if [ "$HTTP_CODE" = "200" ]; then
        print_pass "Dex OIDC provider is responding"
    else
        print_fail "Dex OIDC provider is not responding (HTTP $HTTP_CODE)"
        return 1
    fi
}

test_ood_dashboard() {
    print_test "Checking if OOD dashboard is accessible..."

    # The dashboard should redirect to auth or return the page
    HTTP_CODE=$(docker exec ondemand curl -s -o /dev/null -w "%{http_code}" -L http://localhost:8080/pun/sys/dashboard 2>/dev/null || echo "000")

    if [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "302" ] || [ "$HTTP_CODE" = "303" ]; then
        print_pass "OOD dashboard is accessible (HTTP $HTTP_CODE)"
    else
        print_fail "OOD dashboard is not accessible (HTTP $HTTP_CODE)"
        return 1
    fi
}

test_slurm_commands() {
    print_test "Checking if Slurm commands work from OOD container..."

    SINFO_OUTPUT=$(docker exec ondemand sinfo --version 2>&1 || echo "FAILED")

    if echo "$SINFO_OUTPUT" | grep -q "slurm"; then
        print_info "  $SINFO_OUTPUT"
        print_pass "Slurm commands available in OOD container"
    else
        print_fail "Slurm commands not working: $SINFO_OUTPUT"
        return 1
    fi
}

test_slurm_connectivity() {
    print_test "Checking Slurm cluster connectivity from OOD container..."

    SINFO_OUTPUT=$(docker exec ondemand sinfo -N 2>&1 || echo "FAILED")

    if echo "$SINFO_OUTPUT" | grep -q "idle\|alloc\|mix"; then
        print_info "  Cluster nodes visible from OOD container"
        print_pass "Slurm cluster reachable from OOD container"
    else
        print_fail "Cannot reach Slurm cluster: $SINFO_OUTPUT"
        return 1
    fi
}

test_ood_user_exists() {
    print_test "Checking if OOD demo user exists on compute nodes..."

    OOD_USER=$(docker exec slurmctld id ood 2>&1 || echo "FAILED")

    if echo "$OOD_USER" | grep -q "uid=1001"; then
        print_pass "OOD user exists on cluster nodes (uid=1001)"
    else
        print_fail "OOD user not found on cluster nodes: $OOD_USER"
        return 1
    fi
}

main() {
    print_header "Open OnDemand Profile Test Suite"

    if [ "$CI_MODE" = "true" ]; then
        print_info "Running in CI mode"
    fi

    echo ""

    test_ondemand_running || true
    test_httpd_responding || true
    test_dex_responding || true
    test_ood_dashboard || true
    test_slurm_commands || true
    test_slurm_connectivity || true
    test_ood_user_exists || true

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
