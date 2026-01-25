#!/bin/bash
#
# GPU Health Check for HPC Nodes
#
# Checks NVIDIA GPU health including:
# - GPU detection and driver status
# - Memory errors (ECC)
# - Temperature thresholds
# - Utilization anomalies
# - CUDA availability
#
# Exit codes:
#   0 - Healthy
#   1 - Critical failure
#   2 - Warning
#   3 - No GPU detected (informational)
#
# Usage: ./gpu_health.sh [--json] [--temp-warn 80] [--temp-crit 90]

set -o pipefail

JSON_OUTPUT=false
TEMP_WARN=80
TEMP_CRIT=90
MEM_UTIL_WARN=95

while [[ $# -gt 0 ]]; do
    case "$1" in
        --json) JSON_OUTPUT=true; shift ;;
        --temp-warn) TEMP_WARN="$2"; shift 2 ;;
        --temp-crit) TEMP_CRIT="$2"; shift 2 ;;
        *) shift ;;
    esac
done

declare -A CHECKS
declare -A METRICS
declare -a GPU_INFO
WARNINGS=()
ERRORS=()

check_pass() { CHECKS["$1"]="pass"; }
check_warn() { CHECKS["$1"]="warn"; WARNINGS+=("$2"); }
check_fail() { CHECKS["$1"]="fail"; ERRORS+=("$2"); }

# Check if nvidia-smi is available
if ! command -v nvidia-smi &> /dev/null; then
    if $JSON_OUTPUT; then
        echo '{"status": "no_gpu", "message": "nvidia-smi not found - no NVIDIA GPU or driver not installed", "exit_code": 3}'
    else
        echo "No NVIDIA GPU detected (nvidia-smi not found)"
    fi
    exit 3
fi

# Check driver loaded
if ! lsmod 2>/dev/null | grep -q nvidia; then
    check_fail "driver" "NVIDIA kernel module not loaded"
fi

# Get GPU count
GPU_COUNT=$(nvidia-smi --query-gpu=count --format=csv,noheader,nounits 2>/dev/null | head -1)
if [[ -z "$GPU_COUNT" ]] || [[ "$GPU_COUNT" -eq 0 ]]; then
    if $JSON_OUTPUT; then
        echo '{"status": "no_gpu", "message": "No GPUs detected by nvidia-smi", "exit_code": 3}'
    else
        echo "No GPUs detected by nvidia-smi"
    fi
    exit 3
fi

METRICS["gpu_count"]="$GPU_COUNT"
check_pass "gpu_detected"

# Query all GPU information at once
GPU_DATA=$(nvidia-smi --query-gpu=index,name,temperature.gpu,memory.used,memory.total,utilization.gpu,utilization.memory,ecc.errors.corrected.volatile.total,ecc.errors.uncorrected.volatile.total,power.draw,power.limit,pstate,fan.speed --format=csv,noheader,nounits 2>/dev/null)

# Process each GPU
gpu_idx=0
while IFS=',' read -r idx name temp mem_used mem_total gpu_util mem_util ecc_corr ecc_uncorr power power_limit pstate fan; do
    # Trim whitespace
    idx=$(echo "$idx" | xargs)
    name=$(echo "$name" | xargs)
    temp=$(echo "$temp" | xargs)
    mem_used=$(echo "$mem_used" | xargs)
    mem_total=$(echo "$mem_total" | xargs)
    gpu_util=$(echo "$gpu_util" | xargs)
    mem_util=$(echo "$mem_util" | xargs)
    ecc_corr=$(echo "$ecc_corr" | xargs)
    ecc_uncorr=$(echo "$ecc_uncorr" | xargs)
    power=$(echo "$power" | xargs)
    pstate=$(echo "$pstate" | xargs)
    fan=$(echo "$fan" | xargs)

    # Store metrics
    METRICS["gpu${idx}_name"]="\"$name\""
    METRICS["gpu${idx}_temp_c"]="${temp:-0}"
    METRICS["gpu${idx}_mem_used_mb"]="${mem_used:-0}"
    METRICS["gpu${idx}_mem_total_mb"]="${mem_total:-0}"
    METRICS["gpu${idx}_gpu_util_pct"]="${gpu_util:-0}"
    METRICS["gpu${idx}_mem_util_pct"]="${mem_util:-0}"
    METRICS["gpu${idx}_power_w"]="${power:-0}"
    METRICS["gpu${idx}_pstate"]="\"${pstate:-unknown}\""

    # Temperature checks
    if [[ -n "$temp" ]] && [[ "$temp" != "[N/A]" ]]; then
        if [[ "$temp" -ge "$TEMP_CRIT" ]]; then
            check_fail "gpu${idx}_temp" "GPU $idx temperature ${temp}C exceeds critical threshold ${TEMP_CRIT}C"
        elif [[ "$temp" -ge "$TEMP_WARN" ]]; then
            check_warn "gpu${idx}_temp" "GPU $idx temperature ${temp}C exceeds warning threshold ${TEMP_WARN}C"
        else
            check_pass "gpu${idx}_temp"
        fi
    fi

    # Memory utilization check
    if [[ -n "$mem_util" ]] && [[ "$mem_util" != "[N/A]" ]]; then
        if [[ "$mem_util" -ge "$MEM_UTIL_WARN" ]]; then
            check_warn "gpu${idx}_memory" "GPU $idx memory utilization at ${mem_util}%"
        else
            check_pass "gpu${idx}_memory"
        fi
    fi

    # ECC error checks
    if [[ -n "$ecc_uncorr" ]] && [[ "$ecc_uncorr" != "[N/A]" ]] && [[ "$ecc_uncorr" -gt 0 ]]; then
        check_fail "gpu${idx}_ecc" "GPU $idx has $ecc_uncorr uncorrectable ECC errors"
        METRICS["gpu${idx}_ecc_uncorrected"]="$ecc_uncorr"
    elif [[ -n "$ecc_corr" ]] && [[ "$ecc_corr" != "[N/A]" ]] && [[ "$ecc_corr" -gt 100 ]]; then
        check_warn "gpu${idx}_ecc" "GPU $idx has $ecc_corr correctable ECC errors"
        METRICS["gpu${idx}_ecc_corrected"]="$ecc_corr"
    else
        check_pass "gpu${idx}_ecc"
    fi

    # Store GPU info for summary
    GPU_INFO+=("GPU $idx: $name, ${temp}C, ${mem_used}/${mem_total}MB, ${gpu_util}% util")

    ((gpu_idx++))
done <<< "$GPU_DATA"

# Check CUDA availability
if command -v nvcc &> /dev/null; then
    CUDA_VERSION=$(nvcc --version 2>/dev/null | grep "release" | awk '{print $6}' | cut -d',' -f1)
    check_pass "cuda"
    METRICS["cuda_version"]="\"$CUDA_VERSION\""
else
    # Check for CUDA runtime even without nvcc
    if [[ -f /usr/local/cuda/version.txt ]] || [[ -d /usr/local/cuda ]]; then
        check_pass "cuda"
    else
        check_warn "cuda" "CUDA toolkit not found (nvcc not in PATH)"
    fi
fi

# Check nvidia-persistenced (recommended for HPC)
if pgrep -x nvidia-persist > /dev/null 2>&1; then
    check_pass "persistence"
else
    check_warn "persistence" "nvidia-persistenced not running (recommended for HPC)"
fi

# Check for GPU reset capability (useful for recovery)
if nvidia-smi --query-gpu=gpu_reset_status --format=csv,noheader 2>/dev/null | grep -q "Ready"; then
    check_pass "reset_capable"
fi

# Output
if $JSON_OUTPUT; then
    echo "{"
    echo "  \"hostname\": \"$(hostname)\","
    echo "  \"timestamp\": \"$(date -Iseconds)\","
    echo "  \"gpu_count\": $GPU_COUNT,"
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
    echo "=== GPU Health Check: $(hostname) ==="
    echo ""
    echo "GPUs Detected: $GPU_COUNT"
    echo ""
    for info in "${GPU_INFO[@]}"; do
        echo "  $info"
    done
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
