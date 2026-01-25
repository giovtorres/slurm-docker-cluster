#!/bin/bash
#
# Slurm Node Health Check
#
# Verifies slurmd is running and can communicate with controller.
# Exit codes:
#   0 - Healthy
#   1 - Critical failure
#   2 - Warning (degraded but operational)
#
# Usage: ./slurm_health.sh [--json]

set -o pipefail

JSON_OUTPUT=false
[[ "$1" == "--json" ]] && JSON_OUTPUT=true

declare -A CHECKS
WARNINGS=()
ERRORS=()

# Helper functions
check_pass() {
    CHECKS["$1"]="pass"
}

check_warn() {
    CHECKS["$1"]="warn"
    WARNINGS+=("$2")
}

check_fail() {
    CHECKS["$1"]="fail"
    ERRORS+=("$2")
}

# Check 1: slurmd process running
if pidof slurmd > /dev/null 2>&1; then
    check_pass "slurmd_running"
else
    check_fail "slurmd_running" "slurmd process not running"
fi

# Check 2: slurmd service status (if systemd)
if command -v systemctl &> /dev/null; then
    if systemctl is-active --quiet slurmd 2>/dev/null; then
        check_pass "slurmd_service"
    else
        check_warn "slurmd_service" "slurmd systemd service not active (may be running standalone)"
    fi
else
    check_pass "slurmd_service"  # Skip on non-systemd systems
fi

# Check 3: Controller communication
if scontrol ping 2>/dev/null | grep -q "UP"; then
    check_pass "controller_comm"
else
    check_fail "controller_comm" "Cannot communicate with slurmctld"
fi

# Check 4: MUNGE authentication
if munge -n 2>/dev/null | unmunge > /dev/null 2>&1; then
    check_pass "munge_auth"
else
    check_fail "munge_auth" "MUNGE authentication failed"
fi

# Check 5: Node registration
HOSTNAME=$(hostname -s)
NODE_STATE=$(sinfo -h -n "$HOSTNAME" -o "%T" 2>/dev/null)
if [[ -n "$NODE_STATE" ]]; then
    check_pass "node_registered"

    # Check if in problematic state
    case "$NODE_STATE" in
        *down*|*fail*)
            check_fail "node_state" "Node is in $NODE_STATE state"
            ;;
        *drain*)
            check_warn "node_state" "Node is draining/drained: $NODE_STATE"
            ;;
        *)
            check_pass "node_state"
            ;;
    esac
else
    check_fail "node_registered" "Node $HOSTNAME not found in Slurm configuration"
fi

# Check 6: Slurm configuration accessible
if [[ -r /etc/slurm/slurm.conf ]] || [[ -r /etc/slurm-llnl/slurm.conf ]]; then
    check_pass "config_readable"
else
    check_warn "config_readable" "Cannot read slurm.conf"
fi

# Check 7: Spool directory writable
SPOOL_DIR="/var/spool/slurmd"
if [[ -d "$SPOOL_DIR" ]] && [[ -w "$SPOOL_DIR" ]]; then
    check_pass "spool_writable"
else
    check_fail "spool_writable" "Spool directory $SPOOL_DIR not writable"
fi

# Output results
if $JSON_OUTPUT; then
    echo "{"
    echo "  \"hostname\": \"$(hostname)\","
    echo "  \"timestamp\": \"$(date -Iseconds)\","
    echo "  \"checks\": {"
    first=true
    for check in "${!CHECKS[@]}"; do
        $first || echo ","
        first=false
        echo -n "    \"$check\": \"${CHECKS[$check]}\""
    done
    echo ""
    echo "  },"
    echo "  \"warnings\": ["
    for i in "${!WARNINGS[@]}"; do
        [[ $i -gt 0 ]] && echo ","
        echo -n "    \"${WARNINGS[$i]}\""
    done
    echo ""
    echo "  ],"
    echo "  \"errors\": ["
    for i in "${!ERRORS[@]}"; do
        [[ $i -gt 0 ]] && echo ","
        echo -n "    \"${ERRORS[$i]}\""
    done
    echo ""
    echo "  ],"

    if [[ ${#ERRORS[@]} -gt 0 ]]; then
        echo "  \"status\": \"critical\","
        echo "  \"exit_code\": 1"
    elif [[ ${#WARNINGS[@]} -gt 0 ]]; then
        echo "  \"status\": \"warning\","
        echo "  \"exit_code\": 2"
    else
        echo "  \"status\": \"healthy\","
        echo "  \"exit_code\": 0"
    fi
    echo "}"
else
    echo "=== Slurm Health Check: $(hostname) ==="
    echo ""

    for check in "${!CHECKS[@]}"; do
        case "${CHECKS[$check]}" in
            pass) echo "[PASS] $check" ;;
            warn) echo "[WARN] $check" ;;
            fail) echo "[FAIL] $check" ;;
        esac
    done

    if [[ ${#WARNINGS[@]} -gt 0 ]]; then
        echo ""
        echo "Warnings:"
        for w in "${WARNINGS[@]}"; do
            echo "  - $w"
        done
    fi

    if [[ ${#ERRORS[@]} -gt 0 ]]; then
        echo ""
        echo "Errors:"
        for e in "${ERRORS[@]}"; do
            echo "  - $e"
        done
    fi

    echo ""
    if [[ ${#ERRORS[@]} -gt 0 ]]; then
        echo "Status: CRITICAL"
    elif [[ ${#WARNINGS[@]} -gt 0 ]]; then
        echo "Status: WARNING"
    else
        echo "Status: HEALTHY"
    fi
fi

# Exit with appropriate code
if [[ ${#ERRORS[@]} -gt 0 ]]; then
    exit 1
elif [[ ${#WARNINGS[@]} -gt 0 ]]; then
    exit 2
else
    exit 0
fi
