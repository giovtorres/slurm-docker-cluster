#!/bin/bash
#SBATCH --job-name=cpu_test
#SBATCH --output=/data/cpu_test_%j.out
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --time=00:05:00

# CPU-intensive job (calculate prime numbers)
echo "Starting CPU-intensive task on $(hostname)"
echo "Job ID: $SLURM_JOB_ID"
echo "Allocated CPUs: $SLURM_CPUS_PER_TASK"

# Calculate primes up to 100000
calculate_primes() {
    local max=$1
    local count=0

    for ((num=2; num<=max; num++)); do
        is_prime=1
        for ((i=2; i*i<=num; i++)); do
            if [ $((num % i)) -eq 0 ]; then
                is_prime=0
                break
            fi
        done
        if [ $is_prime -eq 1 ]; then
            ((count++))
        fi
    done

    echo "Found $count prime numbers up to $max"
}

start_time=$(date +%s)
calculate_primes 100000
end_time=$(date +%s)

elapsed=$((end_time - start_time))
echo "Task completed in $elapsed seconds"
