#!/bin/bash
#SBATCH --job-name=multi_node_test
#SBATCH --output=/data/multi_node_%j.out
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --time=00:02:00

# Multi-node job that runs on both compute nodes
echo "Multi-node job starting"
echo "Job ID: $SLURM_JOB_ID"
echo "Number of nodes: $SLURM_JOB_NUM_NODES"
echo "Number of tasks: $SLURM_NTASKS"
echo "Node list: $SLURM_JOB_NODELIST"

# Run hostname on each allocated node
srun hostname

echo "All tasks completed"
