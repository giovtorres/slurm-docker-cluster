#!/bin/bash
#SBATCH --job-name=multi_node_singularity_test
#SBATCH --output=/data/multi_node_singularity_%j.out
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --time=00:02:00

# Multi-node job that runs on both compute nodes
echo "Multi-Node-Singularity job starting"
echo "Job ID: $SLURM_JOB_ID"
echo "Number of nodes: $SLURM_JOB_NUM_NODES"
echo "Number of tasks: $SLURM_NTASKS"
echo "Node list: $SLURM_JOB_NODELIST"
echo ""

echo "Pull Alpine v3.22.2 Image"
singularity pull docker://alpine:3.22.2
echo ""

# Run hostname on each allocated node inside Alpine singularity container
# Also show Alpine versiob.
srun singularity exec alpine_3.22.2.sif /bin/sh -c \
    "echo 'Container OS:'; \
     cat /etc/alpine-release; \
     echo 'Actual Hostname:'; \
     hostname; \
     echo '';" 

echo "All tasks completed"
