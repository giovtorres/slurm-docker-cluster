#!/bin/bash
#SBATCH --job-name=dependent_job
#SBATCH --output=/data/dependent_%j.out
#SBATCH --time=00:01:00

# This job demonstrates job dependencies
# Submit with: sbatch --dependency=afterok:<job_id> job_dependency.sh

echo "Dependent job starting on $(hostname)"
echo "Job ID: $SLURM_JOB_ID"
echo "This job ran after its dependency completed successfully"
echo "Current time: $(date)"
