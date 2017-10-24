## MPI Examples

Some simple examples of using slurm on the docker cluster.

### ping_pong

This code comes from mpitutorials (see the header in ping_pong.c) The code has been modified slightly to show which servers are running the code.

To use:

* Start the container as described in the main README
* docker cp ping_pong slurmctld:/data/ping_pong
* docker cp ping.sh slurmctld:/data/ping.sh
* On the slurmctld:
*   cd data
*   sbatch ping.sh
