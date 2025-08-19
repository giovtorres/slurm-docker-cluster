# Ray Setup

This `README.md` file documents additiona steps/configs needed to run
Ray workloads on the slurm docker cluster.

## Running RayJobs
```bash
docker exec -it slurmctld bash
cd data
sbatch slurm-basic.sh
```
