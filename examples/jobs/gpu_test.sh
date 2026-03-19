#!/bin/bash
#SBATCH --job-name=gpu-test
#SBATCH --partition=gpu
#SBATCH --gres=gpu:1
#SBATCH --output=gpu-test-%j.out
#SBATCH --time=00:05:00

# GPU Test Job
# This job requests 1 GPU and runs basic GPU detection tests
# Requires: GPU_ENABLE=true in .env

echo "=========================================="
echo "GPU Test Job"
echo "=========================================="
echo "Job ID: $SLURM_JOB_ID"
echo "Node: $(hostname)"
echo "Date: $(date)"
echo ""

echo "=========================================="
echo "Environment"
echo "=========================================="
echo "SLURM_JOB_GPUS: ${SLURM_JOB_GPUS:-not set}"
echo "CUDA_VISIBLE_DEVICES: ${CUDA_VISIBLE_DEVICES:-not set}"
echo "SLURM_GPUS_ON_NODE: ${SLURM_GPUS_ON_NODE:-not set}"
echo ""

echo "=========================================="
echo "GPU Detection (nvidia-smi)"
echo "=========================================="
if command -v nvidia-smi &> /dev/null; then
    nvidia-smi
    echo ""
    echo "GPU Details:"
    nvidia-smi --query-gpu=index,name,driver_version,memory.total,memory.free,compute_cap --format=csv,noheader
else
    echo "ERROR: nvidia-smi not found"
    echo "Ensure nvidia-container-toolkit is installed on host"
    exit 1
fi
echo ""

echo "=========================================="
echo "CUDA Device Query"
echo "=========================================="
if [ -f /usr/local/cuda/samples/bin/x86_64/linux/release/deviceQuery ]; then
    /usr/local/cuda/samples/bin/x86_64/linux/release/deviceQuery
elif command -v nvidia-smi &> /dev/null; then
    echo "CUDA samples not available, using nvidia-smi as fallback"
    nvidia-smi -q
else
    echo "No CUDA query tools available"
fi
echo ""

echo "=========================================="
echo "GPU Test Complete"
echo "=========================================="
echo "Job completed successfully on $(hostname)"
