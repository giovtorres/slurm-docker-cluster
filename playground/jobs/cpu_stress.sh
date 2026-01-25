#!/bin/bash
#SBATCH --job-name=cpu_stress
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=${CPUS:-2}
#SBATCH --time=${TIME:-10}
#SBATCH --output=/data/cpu_stress_%j.out
#SBATCH --error=/data/cpu_stress_%j.err

# CPU Stress Test Job
# Generates CPU load using pure bash (no external tools required)
#
# Environment variables:
#   CPUS      - Number of CPUs to use (default: 2)
#   DURATION  - Duration in seconds (default: 60)
#   INTENSITY - Loop iterations: light=1M, medium=10M, heavy=100M (default: medium)

DURATION=${DURATION:-60}
INTENSITY=${INTENSITY:-medium}

case $INTENSITY in
    light)  LOOPS=1000000 ;;
    heavy)  LOOPS=100000000 ;;
    *)      LOOPS=10000000 ;;  # medium
esac

echo "=========================================="
echo "CPU Stress Test"
echo "=========================================="
echo "Hostname:   $(hostname)"
echo "Job ID:     $SLURM_JOB_ID"
echo "CPUs:       $SLURM_CPUS_PER_TASK"
echo "Duration:   ${DURATION}s"
echo "Intensity:  $INTENSITY (${LOOPS} loops)"
echo "Start time: $(date)"
echo "=========================================="

# Function to generate CPU load
cpu_work() {
    local end=$((SECONDS + DURATION))
    while [ $SECONDS -lt $end ]; do
        # Arithmetic operations to keep CPU busy
        for ((j=0; j<LOOPS; j++)); do
            : # No-op but still burns CPU
        done
    done
}

# Launch CPU workers for each allocated CPU
echo "Launching $SLURM_CPUS_PER_TASK CPU workers..."
for i in $(seq 1 $SLURM_CPUS_PER_TASK); do
    cpu_work &
    echo "  Worker $i started (PID: $!)"
done

# Wait for all workers to complete
wait

echo "=========================================="
echo "End time:   $(date)"
echo "CPU stress test completed successfully"
echo "=========================================="
