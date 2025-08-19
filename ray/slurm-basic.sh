#!/bin/bash
# shellcheck disable=SC2206

#SBATCH --job-name=test
#SBATCH --cpus-per-task=1
#SBATCH --mem-per-cpu=1GB
#SBATCH --nodes=2
#SBATCH --tasks-per-node=1
#SBATCH --time=01:00:00

# Script inspired by:
# https://docs.ray.io/en/latest/cluster/vms/user-guides/community/slurm-basic.html#slurm-basic-sh

set -x

# ============================================================================
# CONSTANTS
# ============================================================================
RAY_PORT=6379
RAY_VERSION="2.43.0"
PYTHON_VERSION="python3.11"
VENV_DIR=".venv"
HEAD_NODE_WAIT_TIME=10
MAIN_SCRIPT="simple-trainer.py"

# ============================================================================
# ENVIRONMENT SETUP
# ============================================================================
rm -rf "$VENV_DIR" && mkdir "$VENV_DIR"
$PYTHON_VERSION -m venv "$VENV_DIR"
source "$VENV_DIR/bin/activate"
pip install \
    ray=="$RAY_VERSION"

# ============================================================================
# NODE CONFIGURATION
# ============================================================================
nodes=$(scontrol show hostnames "$SLURM_JOB_NODELIST")
nodes_array=($nodes)

head_node=${nodes_array[0]}
head_node_ip=$(srun \
    --nodes=1 \
    --ntasks=1 \
    -w "$head_node" \
    hostname --ip-address)

# If we detect a space character in the head node IP, we'll convert it to an ipv4 address. This step is optional.
if [[ "$head_node_ip" == *" "* ]]; then
    IFS=' ' read -ra ADDR <<<"$head_node_ip"
    if [[ ${#ADDR[0]} -gt 16 ]]; then
        head_node_ip=${ADDR[1]}
    else
        head_node_ip=${ADDR[0]}
    fi
    echo "IPV6 address detected. We split the IPV4 address as $head_node_ip"
fi

# ============================================================================
# RAY HEAD NODE SETUP
# ============================================================================
ip_head=$head_node_ip:$RAY_PORT
export ip_head
echo "IP Head: $ip_head"

echo "Starting HEAD at $head_node"
srun \
    --nodes=1 \
    --ntasks=1 \
    -w "$head_node" \
    ray start \
        --head \
        --node-ip-address="$head_node_ip" \
        --port=$RAY_PORT \
        --num-cpus "${SLURM_CPUS_PER_TASK}" \
        --block &

# ============================================================================
# RAY WORKER NODES SETUP
# ============================================================================

# Number of nodes other than the head node
worker_num=$((SLURM_JOB_NUM_NODES - 1))

for ((i = 1; i <= worker_num; i++)); do
    node_i=${nodes_array[$i]}
    echo "Starting WORKER $i at $node_i"
    srun \
        --nodes=1 \
        --ntasks=1 \
        -w "$node_i" \
        ray start \
            --address "$ip_head" \
            --num-cpus "${SLURM_CPUS_PER_TASK}" \
            --block &
    sleep $WORKER_START_DELAY
done

# ============================================================================
# RUN MAIN SCRIPT
# ============================================================================
python -u "$MAIN_SCRIPT" "$SLURM_CPUS_PER_TASK"

echo "Main script completed"