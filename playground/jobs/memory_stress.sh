#!/bin/bash
#SBATCH --job-name=memory_stress
#SBATCH --ntasks=1
#SBATCH --mem=${MEMORY:-1G}
#SBATCH --time=${TIME:-10}
#SBATCH --output=/data/memory_stress_%j.out
#SBATCH --error=/data/memory_stress_%j.err

# Memory Stress Test Job
# Allocates and accesses memory to test memory constraints
#
# Environment variables:
#   MEMORY    - Memory to allocate (default: 1G)
#   DURATION  - Duration in seconds (default: 60)
#   PATTERN   - Access pattern: hold, sequential, random (default: hold)

DURATION=${DURATION:-60}
PATTERN=${PATTERN:-hold}

# Parse memory specification
MEMORY_SPEC=${SLURM_MEM_PER_NODE:-1000}

echo "=========================================="
echo "Memory Stress Test"
echo "=========================================="
echo "Hostname:   $(hostname)"
echo "Job ID:     $SLURM_JOB_ID"
echo "Memory:     ${MEMORY_SPEC}MB"
echo "Duration:   ${DURATION}s"
echo "Pattern:    $PATTERN"
echo "Start time: $(date)"
echo "=========================================="

# Use Python for reliable memory allocation
python3 << PYTHON
import time
import sys
import random

memory_mb = $MEMORY_SPEC
duration = $DURATION
pattern = "$PATTERN"

# Allocate ~80% of requested memory (leave room for overhead)
chunk_size = 1024 * 1024  # 1MB per chunk
num_chunks = int(memory_mb * 0.8)

print(f"Allocating {num_chunks} MB of memory...")
sys.stdout.flush()

data = []
try:
    for i in range(num_chunks):
        # Allocate 1MB string
        data.append('x' * chunk_size)
        if (i + 1) % 100 == 0:
            print(f"  Allocated {i + 1} MB")
            sys.stdout.flush()

    print(f"Memory allocation complete: {len(data)} MB")
    sys.stdout.flush()

    print(f"Holding memory for {duration} seconds (pattern: {pattern})...")
    sys.stdout.flush()

    start = time.time()
    access_count = 0

    while time.time() - start < duration:
        if pattern == "random":
            # Random access pattern
            for _ in range(1000):
                idx = random.randint(0, len(data) - 1)
                _ = data[idx][0]
                access_count += 1
        elif pattern == "sequential":
            # Sequential access pattern
            for chunk in data:
                _ = chunk[0]
                access_count += 1
        # For "hold" pattern, just sleep
        time.sleep(1)

    print(f"Access count: {access_count}")

except MemoryError as e:
    print(f"Warning: Could not allocate all memory: {e}")
    print(f"Allocated: {len(data)} MB")
    time.sleep(duration)

print("Memory stress test completed")
PYTHON

echo "=========================================="
echo "End time:   $(date)"
echo "Memory stress test completed successfully"
echo "=========================================="
