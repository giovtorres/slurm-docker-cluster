#!/bin/bash
#SBATCH --job-name=memory_test
#SBATCH --output=/data/memory_test_%j.out
#SBATCH --ntasks=1
#SBATCH --mem=500M
#SBATCH --time=00:02:00

# Memory allocation test
echo "Memory test job starting on $(hostname)"
echo "Job ID: $SLURM_JOB_ID"
echo "Requested memory: 500M"

# Show available memory
free -h

# Allocate some memory (100MB array)
echo "Allocating memory..."
python3 -c "
import time
# Allocate approximately 100MB
data = bytearray(100 * 1024 * 1024)
print('Allocated 100MB of memory')
time.sleep(2)
print('Memory released')
"

echo "Memory test completed"
