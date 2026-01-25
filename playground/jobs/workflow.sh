#!/bin/bash
#SBATCH --job-name=workflow_stage
#SBATCH --ntasks=1
#SBATCH --time=${TIME:-10}
#SBATCH --output=/data/workflow_%j.out
#SBATCH --error=/data/workflow_%j.err

# Workflow Stage Job
# Template for multi-stage pipeline jobs with dependencies
#
# Environment variables:
#   STAGE       - Current stage number (default: 1)
#   TOTAL_STAGES - Total number of stages (default: 3)
#   DURATION    - Stage duration in seconds (default: 30)
#   CHECKPOINT  - Enable checkpoint/restart simulation (default: false)

STAGE=${STAGE:-1}
TOTAL_STAGES=${TOTAL_STAGES:-3}
DURATION=${DURATION:-30}
CHECKPOINT=${CHECKPOINT:-false}

CHECKPOINT_FILE="/data/workflow_${SLURM_JOB_ID}_checkpoint"

echo "=========================================="
echo "Workflow Stage $STAGE of $TOTAL_STAGES"
echo "=========================================="
echo "Hostname:   $(hostname)"
echo "Job ID:     $SLURM_JOB_ID"
echo "Stage:      $STAGE / $TOTAL_STAGES"
echo "Duration:   ${DURATION}s"
echo "Checkpoint: $CHECKPOINT"
echo "Start time: $(date)"
echo "=========================================="

# Check for previous checkpoint
if [ "$CHECKPOINT" = "true" ] && [ -f "$CHECKPOINT_FILE" ]; then
    echo "Found checkpoint file, resuming..."
    source "$CHECKPOINT_FILE"
    echo "  Resumed from: $CHECKPOINT_TIME"
fi

# Simulate stage-specific work
echo ""
echo "Executing stage $STAGE work..."

case $STAGE in
    1)
        echo "  Stage 1: Data preparation"
        echo "  - Validating input data"
        sleep $((DURATION / 3))
        echo "  - Preprocessing data"
        sleep $((DURATION / 3))
        echo "  - Preparing output"
        sleep $((DURATION / 3))
        STAGE_RESULT="prepared"
        ;;
    2)
        echo "  Stage 2: Main processing"
        echo "  - Loading prepared data"
        sleep $((DURATION / 4))
        echo "  - Running computations"
        sleep $((DURATION / 2))
        echo "  - Aggregating results"
        sleep $((DURATION / 4))
        STAGE_RESULT="processed"
        ;;
    3)
        echo "  Stage 3: Finalization"
        echo "  - Validating results"
        sleep $((DURATION / 3))
        echo "  - Generating reports"
        sleep $((DURATION / 3))
        echo "  - Cleanup"
        sleep $((DURATION / 3))
        STAGE_RESULT="finalized"
        ;;
    *)
        echo "  Stage $STAGE: Generic processing"
        sleep $DURATION
        STAGE_RESULT="completed"
        ;;
esac

# Write checkpoint if enabled
if [ "$CHECKPOINT" = "true" ]; then
    echo "CHECKPOINT_TIME='$(date)'" > "$CHECKPOINT_FILE"
    echo "STAGE_RESULT='$STAGE_RESULT'" >> "$CHECKPOINT_FILE"
    echo "Checkpoint saved to: $CHECKPOINT_FILE"
fi

# Stage completion message
echo ""
echo "Stage $STAGE result: $STAGE_RESULT"

if [ $STAGE -lt $TOTAL_STAGES ]; then
    echo "Next stage: $((STAGE + 1))"
else
    echo "Workflow complete!"
    # Cleanup checkpoint file on final stage
    rm -f "$CHECKPOINT_FILE"
fi

echo ""
echo "=========================================="
echo "End time:   $(date)"
echo "Stage $STAGE completed successfully"
echo "=========================================="
