
#!/bin/bash
#SBATCH --job-name=mpi_job_test      # Job name
#SBATCH --mail-type=ALL              # Mail events (NONE, BEGIN, END, FAIL, ALL)
#SBATCH --mail-user=<email_address>  # Where to send mail	
#SBATCH --ntasks=24                  # Number of MPI ranks
#SBATCH --cpus-per-task=1            # Number of cores per MPI rank 
#SBATCH --nodes=2                    # Number of nodes
#SBATCH --ntasks-per-node=12         # How many tasks on each node
#SBATCH --ntasks-per-socket=6        # How many tasks on each CPU or socket
#SBATCH --distribution=cyclic:cyclic # Distribute tasks cyclically on nodes and sockets
#SBATCH --mem-per-cpu=600mb          # Memory per processor
#SBATCH --time=00:05:00              # Time limit hrs:min:sec
#SBATCH --output=mpi_test_%j.out     # Standard output and error log
pwd; hostname; date

echo "Running prime number generator program on $SLURM_JOB_NUM_NODES nodes with $SLURM_NTASKS tasks, each with $SLURM_CPUS_PER_TASK cores."

module load intel/2016.0.109 openmpi/1.10.2

srun --mpi=pmi2 /ufrc/data/training/SLURM/prime/prime_mpi

date
