#!/bin/bash
#SBATCH --job-name=sleep_job
#SBATCH --ntasks=1
#SBATCH --time=${TIME:-5}
#SBATCH --output=/data/sleep_%j.out
#SBATCH --error=/data/sleep_%j.err

# Simple Sleep Job
# Minimal resource usage, useful for testing queue behavior
#
# Environment variables:
#   DURATION - Sleep duration in seconds (default: 30)
#   MESSAGE  - Optional message to display

DURATION=${DURATION:-30}
MESSAGE=${MESSAGE:-"No message"}

echo "=========================================="
echo "Sleep Job"
echo "=========================================="
echo "Hostname:   $(hostname)"
echo "Job ID:     $SLURM_JOB_ID"
echo "Duration:   ${DURATION}s"
echo "Message:    $MESSAGE"
echo "Start time: $(date)"
echo "=========================================="

# Report environment
echo ""
echo "Environment:"
echo "  SLURM_JOB_ID:        $SLURM_JOB_ID"
echo "  SLURM_JOB_NAME:      $SLURM_JOB_NAME"
echo "  SLURM_JOB_NODELIST:  $SLURM_JOB_NODELIST"
echo "  SLURM_JOB_PARTITION: $SLURM_JOB_PARTITION"
echo "  SLURM_SUBMIT_DIR:    $SLURM_SUBMIT_DIR"
echo ""

# Sleep with progress
if [ $DURATION -gt 10 ]; then
    # For longer sleeps, show periodic progress
    elapsed=0
    while [ $elapsed -lt $DURATION ]; do
        remaining=$((DURATION - elapsed))
        if [ $remaining -gt 10 ]; then
            sleep 10
            elapsed=$((elapsed + 10))
            echo "Progress: ${elapsed}s / ${DURATION}s"
        else
            sleep $remaining
            elapsed=$DURATION
        fi
    done
else
    sleep $DURATION
fi

echo ""
echo "=========================================="
echo "End time:   $(date)"
echo "Sleep job completed successfully"
echo "=========================================="
