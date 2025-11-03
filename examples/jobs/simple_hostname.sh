#!/bin/bash
#SBATCH --job-name=hostname_test
#SBATCH --output=/data/hostname_test_%j.out
#SBATCH --ntasks=1
#SBATCH --time=00:01:00

# Simple job that prints hostname and basic info
echo "Job started at: $(date)"
echo "Running on node: $(hostname)"
echo "Job ID: $SLURM_JOB_ID"
echo "Number of tasks: $SLURM_NTASKS"
echo "Job completed at: $(date)"
