#!/bin/bash
#SBATCH --job-name=gpu_env
#SBATCH --output=/data/gpu_env_%j.out
#SBATCH --time=00:02:00
#SBATCH --ntasks=1
#SBATCH -p gpu
#SBATCH --gres=gpu:1     # land on a GPU node (g1 in 25.05 config)
# If you prefer to hard-pin the node instead, swap the line above for:
# #SBATCH -w g1

# Make CUDA visible to Slurm-launched shells (they don't inherit container ENV)
export CUDA_HOME=/usr/local/cuda-12.6
export PATH="$CUDA_HOME/bin:$PATH"
export LD_LIBRARY_PATH="$CUDA_HOME/lib64:$CUDA_HOME/targets/x86_64-linux/lib:$LD_LIBRARY_PATH"

echo "GPU/CUDA environment on $(hostname):"
printenv | egrep 'CUDA|LD_LIBRARY_PATH' || true

echo ""
echo "nvcc --version:"
nvcc --version 2>/dev/null || echo "nvcc not found"

echo ""
echo "nvidia-smi:"
nvidia-smi 2>/dev/null || echo "nvidia-smi not available (driver/runtime?)"
