#!/bin/bash
#
# System Health Check for HPC Nodes
#
# Checks system resources critical for compute jobs.
# Exit codes:
#   0 - Healthy
#   1 - Critical failure
#   2 - Warning
#
# Usage: ./system_health.sh [--json] [--thresholds mem=90,disk=85,load=2.0]

set -o pipefail

JSON_OUTPUT=false
MEM_THRESHOLD=90      # Max memory usage %
DISK_THRESHOLD=90     # Max disk usage %
LOAD_THRESHOLD=0      # Max load per CPU (0 = auto-detect)
MIN_FREE_MEM_MB=1024  # Minimum free memory in MB

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --json)
            JSON_OUTPUT=true
            shift
            ;;
        --thresholds)
            IFS=',' read -ra THRESH <<< "$2"
            for t in "${THRESH[@]}"; do
                key="${t%%=*}"
                val="${t##*=}"
                case "$key" in
                    mem) MEM_THRESHOLD="$val" ;;
                    disk) DISK_THRESHOLD="$val" ;;
                    load) LOAD_THRESHOLD="$val" ;;
                esac
            done
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

declare -A CHECKS
declare -A METRICS
WARNINGS=()
ERRORS=()

check_pass() { CHECKS["$1"]="pass"; }
check_warn() { CHECKS["$1"]="warn"; WARNINGS+=("$2"); }
check_fail() { CHECKS["$1"]="fail"; ERRORS+=("$2"); }

# Get CPU count for load threshold
NUM_CPUS=$(nproc 2>/dev/null || grep -c ^processor /proc/cpuinfo)
[[ "$LOAD_THRESHOLD" == "0" ]] && LOAD_THRESHOLD=$(echo "$NUM_CPUS * 1.5" | bc)

# Check 1: Memory usage
MEM_INFO=$(free -m | awk '/^Mem:/ {print $2, $3, $7}')
read -r MEM_TOTAL MEM_USED MEM_AVAIL <<< "$MEM_INFO"
MEM_USAGE=$((MEM_USED * 100 / MEM_TOTAL))
METRICS["memory_total_mb"]="$MEM_TOTAL"
METRICS["memory_used_mb"]="$MEM_USED"
METRICS["memory_available_mb"]="$MEM_AVAIL"
METRICS["memory_usage_percent"]="$MEM_USAGE"

if [[ "$MEM_AVAIL" -lt "$MIN_FREE_MEM_MB" ]]; then
    check_fail "memory" "Available memory ${MEM_AVAIL}MB below minimum ${MIN_FREE_MEM_MB}MB"
elif [[ "$MEM_USAGE" -gt "$MEM_THRESHOLD" ]]; then
    check_warn "memory" "Memory usage ${MEM_USAGE}% exceeds threshold ${MEM_THRESHOLD}%"
else
    check_pass "memory"
fi

# Check 2: Disk space on critical paths
DISK_FAIL=false
for path in /tmp /var /home; do
    if [[ -d "$path" ]]; then
        USAGE=$(df -h "$path" 2>/dev/null | awk 'NR==2 {gsub(/%/,""); print $5}')
        if [[ -n "$USAGE" ]]; then
            METRICS["disk_${path//\//_}_percent"]="$USAGE"
            if [[ "$USAGE" -gt "$DISK_THRESHOLD" ]]; then
                check_warn "disk_$path" "Disk $path at ${USAGE}% (threshold: ${DISK_THRESHOLD}%)"
            fi
        fi
    fi
done

# Overall disk check
if [[ ${#WARNINGS[@]} -eq 0 ]] || ! [[ "${WARNINGS[*]}" =~ "Disk" ]]; then
    check_pass "disk"
fi

# Check 3: Load average
LOAD_1MIN=$(cat /proc/loadavg | awk '{print $1}')
METRICS["load_1min"]="$LOAD_1MIN"
METRICS["load_threshold"]="$LOAD_THRESHOLD"

LOAD_OK=$(echo "$LOAD_1MIN < $LOAD_THRESHOLD" | bc -l)
if [[ "$LOAD_OK" -eq 1 ]]; then
    check_pass "load"
else
    check_warn "load" "Load ${LOAD_1MIN} exceeds threshold ${LOAD_THRESHOLD} (${NUM_CPUS} CPUs)"
fi

# Check 4: Swap usage
SWAP_INFO=$(free -m | awk '/^Swap:/ {print $2, $3}')
read -r SWAP_TOTAL SWAP_USED <<< "$SWAP_INFO"
if [[ "$SWAP_TOTAL" -gt 0 ]]; then
    SWAP_USAGE=$((SWAP_USED * 100 / SWAP_TOTAL))
    METRICS["swap_usage_percent"]="$SWAP_USAGE"

    if [[ "$SWAP_USAGE" -gt 50 ]]; then
        check_warn "swap" "Swap usage at ${SWAP_USAGE}%"
    else
        check_pass "swap"
    fi
else
    check_pass "swap"  # No swap configured
    METRICS["swap_usage_percent"]="0"
fi

# Check 5: OOM killer activity (recent)
OOM_COUNT=$(dmesg 2>/dev/null | grep -c "Out of memory" || echo "0")
METRICS["oom_events"]="$OOM_COUNT"
if [[ "$OOM_COUNT" -gt 0 ]]; then
    check_warn "oom" "Found $OOM_COUNT OOM killer events in dmesg"
else
    check_pass "oom"
fi

# Check 6: Zombie processes
ZOMBIE_COUNT=$(ps aux 2>/dev/null | grep -c ' Z ' || echo "0")
METRICS["zombie_processes"]="$ZOMBIE_COUNT"
if [[ "$ZOMBIE_COUNT" -gt 10 ]]; then
    check_warn "zombies" "$ZOMBIE_COUNT zombie processes detected"
else
    check_pass "zombies"
fi

# Check 7: Critical services
for service in sshd crond; do
    if command -v systemctl &> /dev/null; then
        if systemctl is-active --quiet "$service" 2>/dev/null; then
            check_pass "service_$service"
        else
            check_warn "service_$service" "Service $service not running"
        fi
    elif pgrep -x "$service" > /dev/null 2>&1; then
        check_pass "service_$service"
    fi
done

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
    echo "  \"warnings\": $(printf '%s\n' "${WARNINGS[@]}" | jq -R . | jq -s .),"
    echo "  \"errors\": $(printf '%s\n' "${ERRORS[@]}" | jq -R . | jq -s .),"

    if [[ ${#ERRORS[@]} -gt 0 ]]; then
        echo "  \"status\": \"critical\""
    elif [[ ${#WARNINGS[@]} -gt 0 ]]; then
        echo "  \"status\": \"warning\""
    else
        echo "  \"status\": \"healthy\""
    fi
    echo "}"
else
    echo "=== System Health Check: $(hostname) ==="
    echo ""
    echo "Resources:"
    echo "  Memory: ${MEM_USED}/${MEM_TOTAL}MB (${MEM_USAGE}%)"
    echo "  Load:   ${LOAD_1MIN} (threshold: ${LOAD_THRESHOLD})"
    echo "  CPUs:   ${NUM_CPUS}"
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
