#!/bin/bash
#SBATCH --job-name=array_test
#SBATCH --output=/data/array_test_%A_%a.out
#SBATCH --array=1-5
#SBATCH --time=00:01:00

# Array job - runs 5 instances with different task IDs
echo "Array job task $SLURM_ARRAY_TASK_ID of array job $SLURM_ARRAY_JOB_ID"
echo "Running on: $(hostname)"
echo "Task started at: $(date)"

# Simulate different processing based on task ID
sleep $((SLURM_ARRAY_TASK_ID))

echo "Task $SLURM_ARRAY_TASK_ID completed at: $(date)"
