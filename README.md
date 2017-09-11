# Slurm Docker Cluster

This is a multi-container Slurm cluster using docker-compose.  The compose file
creates named volumes for persistent storage of MySQL data files as well as
Slurm state and log directories.

## Containers and Volumes

The compose file will run the following containers:

* mysql
* slurmdbd
* slurmctld
* c1 (slurmd)
* c2 (slurmd)

The compose file will create the following named volumes:

* etc_munge         ( -> /etc/munge     )
* etc_slurm         ( -> /etc/slurm     )
* slurm_jobdir      ( -> /data          )
* var_lib_mysql     ( -> /var/lib/mysql )
* var_log_slurm     ( -> /var/log/slurm )

## Building the Docker Image

Build the image locally:

```console
$ docker build -t slurm-docker-cluster .
```

## Starting the Cluster

Run `docker-compose` to instantiate the cluster:

```console
$ docker-compose up -d
```

## Register the Cluster with SlurmDBD

To register the cluster to the slurmdbd daemon, run the `register_cluster.sh`
script:

```console
$ ./register_cluster.sh
```

## Accessing the Cluster

Use `docker exec` to run a bash shell on the controller container:

```console
$ docker exec -it slurmctld bash
```

From the shell, execute slurm commands.

## Submitting Jobs

The `slurm_jobdir` named volume is mounted on each Slurm container as `/data`.
Therefore, in order to see job output files while on the controller, change to
the `/data` directory when on the **slurmctld** container and then submit a job:

```console
[root@slurmctld /]# cd /data/
[root@slurmctld data]# sbatch --wrap="uptime"
Submitted batch job 2
[root@slurmctld data]# ls
slurm-2.out
```

## Stopping and Restarting the Cluster

```console
$ docker-compose stop
```

```console
$ docker-compose start
```

## Deleting the Cluster

To remove all containers and volumes, run:

```console
$ docker-compose rm -sf
$ docker volume rm slurmcluster_etc_munge slurmcluster_etc_slurm slurmcluster_slurm_jobdir slurmcluster_var_lib_mysql slurmcluster_var_log_slurm
```
