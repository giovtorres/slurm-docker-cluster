#!/bin/bash
#SBATCH --ntasks=8                   # Number of MPI ranks
#SBATCH --cpus-per-task=1            # Number of cores per MPI rank
#SBATCH --nodes=4                    # Number of nodes
#SBATCH --ntasks-per-node=2          # How many tasks on each node
#SBATCH --ntasks-per-socket=2        # How many tasks on each CPU or socket
#SBATCH --distribution=cyclic:cyclic # Distribute tasks cyclically on nodes and sockets
#SBATCH --mem-per-cpu=100mb          # Memory per processor
#SBATCH --time=00:05:00              # Time limit hrs:min:sec
#SBATCH --output=ring_%j.out         # Standard output and error log
pwd; hostname; date

echo "Running ring on $SLURM_JOB_NUM_NODES nodes with $SLURM_NTASKS tasks, each with $SLURM_CPUS_PER_TASK cores."

srun --mpi=pmi2 /data/ring
