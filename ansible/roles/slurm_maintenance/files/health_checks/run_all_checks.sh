#!/bin/bash
#
# Run All Health Checks
#
# Executes all health check scripts and aggregates results.
# Designed for use as Ansible validation or standalone monitoring.
#
# Usage: ./run_all_checks.sh [--json] [--fail-on-warning]
#
# Exit codes:
#   0 - All checks passed
#   1 - At least one check failed
#   2 - Warnings present (if --fail-on-warning)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

JSON_OUTPUT=false
FAIL_ON_WARNING=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --json) JSON_OUTPUT=true; shift ;;
        --fail-on-warning) FAIL_ON_WARNING=true; shift ;;
        *) shift ;;
    esac
done

declare -A RESULTS
TOTAL_WARNINGS=0
TOTAL_ERRORS=0

# Run each health check
for script in slurm_health.sh system_health.sh network_health.sh gpu_health.sh; do
    script_path="$SCRIPT_DIR/$script"
    check_name="${script%.sh}"

    if [[ -x "$script_path" ]]; then
        if $JSON_OUTPUT; then
            output=$("$script_path" --json 2>&1)
            exit_code=$?
        else
            output=$("$script_path" 2>&1)
            exit_code=$?
        fi

        RESULTS["$check_name"]="$exit_code"

        case $exit_code in
            0) ;; # pass
            1) ((TOTAL_ERRORS++)) ;;
            2) ((TOTAL_WARNINGS++)) ;;
        esac

        if ! $JSON_OUTPUT; then
            echo "$output"
            echo ""
            echo "----------------------------------------"
            echo ""
        fi
    else
        RESULTS["$check_name"]="skip"
    fi
done

# Aggregate results
if $JSON_OUTPUT; then
    echo "{"
    echo "  \"hostname\": \"$(hostname)\","
    echo "  \"timestamp\": \"$(date -Iseconds)\","
    echo "  \"summary\": {"
    echo "    \"total_checks\": ${#RESULTS[@]},"
    echo "    \"errors\": $TOTAL_ERRORS,"
    echo "    \"warnings\": $TOTAL_WARNINGS"
    echo "  },"
    echo "  \"check_results\": {"
    first=true
    for check in "${!RESULTS[@]}"; do
        $first || echo ","
        first=false
        case "${RESULTS[$check]}" in
            0) status="pass" ;;
            1) status="fail" ;;
            2) status="warn" ;;
            *) status="skip" ;;
        esac
        echo -n "    \"$check\": \"$status\""
    done
    echo ""
    echo "  },"

    if [[ $TOTAL_ERRORS -gt 0 ]]; then
        echo "  \"overall_status\": \"critical\","
        echo "  \"exit_code\": 1"
    elif [[ $TOTAL_WARNINGS -gt 0 ]]; then
        echo "  \"overall_status\": \"warning\","
        if $FAIL_ON_WARNING; then
            echo "  \"exit_code\": 2"
        else
            echo "  \"exit_code\": 0"
        fi
    else
        echo "  \"overall_status\": \"healthy\","
        echo "  \"exit_code\": 0"
    fi
    echo "}"
else
    echo "=========================================="
    echo "HEALTH CHECK SUMMARY: $(hostname)"
    echo "=========================================="
    echo ""
    echo "Results:"
    for check in "${!RESULTS[@]}"; do
        case "${RESULTS[$check]}" in
            0) echo "  [PASS] $check" ;;
            1) echo "  [FAIL] $check" ;;
            2) echo "  [WARN] $check" ;;
            *) echo "  [SKIP] $check" ;;
        esac
    done
    echo ""
    echo "Totals: ${#RESULTS[@]} checks, $TOTAL_ERRORS errors, $TOTAL_WARNINGS warnings"
    echo ""

    if [[ $TOTAL_ERRORS -gt 0 ]]; then
        echo "Overall: CRITICAL - Node NOT ready for service"
    elif [[ $TOTAL_WARNINGS -gt 0 ]]; then
        echo "Overall: WARNING - Node may have issues"
    else
        echo "Overall: HEALTHY - Node ready for service"
    fi
    echo "=========================================="
fi

# Exit code
if [[ $TOTAL_ERRORS -gt 0 ]]; then
    exit 1
elif [[ $TOTAL_WARNINGS -gt 0 ]] && $FAIL_ON_WARNING; then
    exit 2
fi
exit 0
