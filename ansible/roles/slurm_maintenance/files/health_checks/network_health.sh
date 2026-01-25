#!/bin/bash
#
# Network Health Check for HPC Nodes
#
# Verifies network connectivity and shared storage mounts.
# Exit codes:
#   0 - Healthy
#   1 - Critical failure
#   2 - Warning
#
# Usage: ./network_health.sh [--json] [--controller HOSTNAME] [--mounts /path1,/path2]

set -o pipefail

JSON_OUTPUT=false
CONTROLLER=${SLURM_CONTROLLER:-slurmctld}
SHARED_MOUNTS="/home,/data,/scratch"
DNS_TEST="google.com"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --json) JSON_OUTPUT=true; shift ;;
        --controller) CONTROLLER="$2"; shift 2 ;;
        --mounts) SHARED_MOUNTS="$2"; shift 2 ;;
        *) shift ;;
    esac
done

declare -A CHECKS
declare -A METRICS
WARNINGS=()
ERRORS=()

check_pass() { CHECKS["$1"]="pass"; }
check_warn() { CHECKS["$1"]="warn"; WARNINGS+=("$2"); }
check_fail() { CHECKS["$1"]="fail"; ERRORS+=("$2"); }

# Check 1: Controller reachability
if ping -c 1 -W 2 "$CONTROLLER" > /dev/null 2>&1; then
    check_pass "controller_ping"

    # Measure latency
    LATENCY=$(ping -c 3 -W 2 "$CONTROLLER" 2>/dev/null | tail -1 | awk -F'/' '{print $5}')
    METRICS["controller_latency_ms"]="${LATENCY:-0}"

    if [[ -n "$LATENCY" ]] && (( $(echo "$LATENCY > 10" | bc -l) )); then
        check_warn "controller_latency" "High latency to controller: ${LATENCY}ms"
    fi
else
    check_fail "controller_ping" "Cannot reach controller $CONTROLLER"
fi

# Check 2: Slurm ports
for port in 6817 6818; do
    if timeout 2 bash -c "echo >/dev/tcp/$CONTROLLER/$port" 2>/dev/null; then
        check_pass "port_$port"
    else
        check_fail "port_$port" "Cannot connect to $CONTROLLER:$port"
    fi
done

# Check 3: DNS resolution
if host "$DNS_TEST" > /dev/null 2>&1; then
    check_pass "dns"
else
    check_warn "dns" "DNS resolution failed for $DNS_TEST"
fi

# Check 4: Shared filesystem mounts
IFS=',' read -ra MOUNTS <<< "$SHARED_MOUNTS"
for mount in "${MOUNTS[@]}"; do
    mount=$(echo "$mount" | xargs)  # Trim whitespace
    [[ -z "$mount" ]] && continue

    if [[ -d "$mount" ]]; then
        # Check if it's a mountpoint
        if mountpoint -q "$mount" 2>/dev/null; then
            # Test read/write
            TEST_FILE="$mount/.health_check_$$"
            if touch "$TEST_FILE" 2>/dev/null && rm -f "$TEST_FILE" 2>/dev/null; then
                check_pass "mount_${mount//\//_}"
            else
                check_fail "mount_${mount//\//_}" "Mount $mount not writable"
            fi
        else
            # Not a mountpoint - might be local directory
            if [[ -w "$mount" ]]; then
                check_pass "mount_${mount//\//_}"
            else
                check_warn "mount_${mount//\//_}" "$mount exists but is not a mountpoint"
            fi
        fi
    else
        # Directory doesn't exist - might be optional
        check_warn "mount_${mount//\//_}" "Mount point $mount does not exist"
    fi
done

# Check 5: NFS/Lustre health (if applicable)
if command -v nfsstat &> /dev/null; then
    NFS_RETRANS=$(nfsstat -c 2>/dev/null | awk '/retrans/ {print $2}')
    if [[ -n "$NFS_RETRANS" ]] && [[ "$NFS_RETRANS" -gt 100 ]]; then
        check_warn "nfs_health" "High NFS retransmits: $NFS_RETRANS"
        METRICS["nfs_retransmits"]="$NFS_RETRANS"
    else
        check_pass "nfs_health"
    fi
fi

# Check 6: Network interface health
PRIMARY_IF=$(ip route | awk '/default/ {print $5; exit}')
if [[ -n "$PRIMARY_IF" ]]; then
    # Check for errors
    ERRORS_COUNT=$(cat "/sys/class/net/$PRIMARY_IF/statistics/rx_errors" 2>/dev/null || echo "0")
    DROPS_COUNT=$(cat "/sys/class/net/$PRIMARY_IF/statistics/rx_dropped" 2>/dev/null || echo "0")

    METRICS["net_rx_errors"]="$ERRORS_COUNT"
    METRICS["net_rx_dropped"]="$DROPS_COUNT"

    if [[ "$ERRORS_COUNT" -gt 1000 ]] || [[ "$DROPS_COUNT" -gt 1000 ]]; then
        check_warn "network_errors" "Network errors: $ERRORS_COUNT errors, $DROPS_COUNT drops on $PRIMARY_IF"
    else
        check_pass "network_errors"
    fi

    # Check link status
    LINK_STATE=$(cat "/sys/class/net/$PRIMARY_IF/operstate" 2>/dev/null)
    if [[ "$LINK_STATE" == "up" ]]; then
        check_pass "link_state"
    else
        check_fail "link_state" "Network interface $PRIMARY_IF is $LINK_STATE"
    fi
fi

# Check 7: Firewall not blocking Slurm (basic check)
if command -v iptables &> /dev/null; then
    if iptables -L INPUT -n 2>/dev/null | grep -q "DROP.*dpt:681[78]"; then
        check_warn "firewall" "Firewall may be blocking Slurm ports"
    else
        check_pass "firewall"
    fi
fi

# Output
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
    echo "  \"metrics\": {"
    first=true
    for metric in "${!METRICS[@]}"; do
        $first || echo ","
        first=false
        echo -n "    \"$metric\": ${METRICS[$metric]}"
    done
    echo ""
    echo "  },"
    echo "  \"warnings\": $(printf '%s\n' "${WARNINGS[@]}" | jq -R . 2>/dev/null | jq -s . 2>/dev/null || echo "[]"),"
    echo "  \"errors\": $(printf '%s\n' "${ERRORS[@]}" | jq -R . 2>/dev/null | jq -s . 2>/dev/null || echo "[]"),"

    if [[ ${#ERRORS[@]} -gt 0 ]]; then
        echo "  \"status\": \"critical\""
    elif [[ ${#WARNINGS[@]} -gt 0 ]]; then
        echo "  \"status\": \"warning\""
    else
        echo "  \"status\": \"healthy\""
    fi
    echo "}"
else
    echo "=== Network Health Check: $(hostname) ==="
    echo ""
    echo "Controller: $CONTROLLER"
    echo "Mounts:     ${SHARED_MOUNTS}"
    echo ""

    for check in "${!CHECKS[@]}"; do
        case "${CHECKS[$check]}" in
            pass) echo "[PASS] $check" ;;
            warn) echo "[WARN] $check" ;;
            fail) echo "[FAIL] $check" ;;
        esac
    done

    [[ ${#WARNINGS[@]} -gt 0 ]] && { echo ""; echo "Warnings:"; printf '  - %s\n' "${WARNINGS[@]}"; }
    [[ ${#ERRORS[@]} -gt 0 ]] && { echo ""; echo "Errors:"; printf '  - %s\n' "${ERRORS[@]}"; }

    echo ""
    if [[ ${#ERRORS[@]} -gt 0 ]]; then echo "Status: CRITICAL"
    elif [[ ${#WARNINGS[@]} -gt 0 ]]; then echo "Status: WARNING"
    else echo "Status: HEALTHY"; fi
fi

[[ ${#ERRORS[@]} -gt 0 ]] && exit 1
[[ ${#WARNINGS[@]} -gt 0 ]] && exit 2
exit 0
