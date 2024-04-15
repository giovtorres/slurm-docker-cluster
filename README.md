# Slurm Docker Cluster

This is a multi-container Slurm cluster using docker-compose.  The compose file
creates named volumes for persistent storage of MySQL data files as well as
Slurm state and log directories.

## Overview: Containers and Volumes

The compose file will run the following containers:

* mysql
* slurmdbd
* slurmctld
* node001 (slurmd)
* node002 (slurmd)
* nodebf2a001 (slurmd)
* nodebf2a002 (slurmd)

The compose file will create the following named volumes:

* etc_munge         ( -> /etc/munge     )
* etc_slurm         ( -> /etc/slurm     )
* slurm_jobdir      ( -> /data          )
* var_lib_mysql     ( -> /var/lib/mysql )
* var_log_slurm     ( -> /var/log/slurm )

## Building the cluster

### Building the Docker Image

Build the image locally:

```console
sudo docker build -t slurm-docker-cluster:21.08.6 .
```

### Starting the Cluster

First update any system specific paths from the `docker-compose.yml` file to reflect your local configuration.
Run `docker-compose` to instantiate the cluster:

```console
sudo IMAGE_TAG=21.08.6 docker-compose up -d --force-recreate
```

### Register the Cluster with SlurmDBD


> Note: You may have to wait a few seconds for the cluster daemons to become
> ready before registering the cluster.  Otherwise, you may get an error such
> as **sacctmgr: error: Problem talking to the database: Connection refused**.
>
> You can check the status of the cluster by viewing the logs: `docker-compose
> logs -f`

To register the cluster to the slurmdbd daemon, run the `register_cluster.sh`
script:

```console
sudo docker exec slurmctld bash -c "/usr/bin/sacctmgr --immediate add cluster name=linux"
```

```console
sudo docker-compose restart slurmdbd slurmctld
```

## Accessing the Cluster

Use `docker exec` to run a bash shell on the controller container:

```console
sudo docker exec -it slurmctld bash
```

From the shell, execute slurm commands, for example:

```console
[root@slurmctld /]# sinfo
PARTITION AVAIL  TIMELIMIT  NODES  STATE NODELIST
normal*      up 5-00:00:00      2   idle c[1-2]
```

## Submitting Jobs

The `slurm_jobdir` named volume is mounted on each Slurm container as `/data`.
Therefore, in order to see job output files while on the controller, change to
the `/data` directory when on the **slurmctld** container and then submit a job:

```console
[root@slurmctld /]# cd /data/
[root@slurmctld data]# sbatch --wrap="hostname"
Submitted batch job 2
[root@slurmctld data]# ls
slurm-2.out
[root@slurmctld data]# cat slurm-2.out
c1
```

## Stopping and Restarting the Cluster

```console
sudo docker-compose stop
sudo docker-compose start
```

## Deleting the Cluster

To remove all containers and volumes, run:

```console
sudo docker-compose stop
sudo docker-compose rm -f
sudo docker volume rm slurm-docker-cluster_etc_munge slurm-docker-cluster_etc_slurm slurm-docker-cluster_slurm_jobdir slurm-docker-cluster_var_lib_mysql slurm-docker-cluster_var_log_slurm
```
## Updating the Cluster

If you want to change the `slurm.conf` or `slurmdbd.conf` file without a rebuilding you can do so by calling
```console
sudo ./update_slurmfiles.sh slurm.conf slurmdbd.conf
```
(or just one of the files).
The Cluster will automatically be restarted afterwards with
```console
sudo docker-compose restart
```
This might come in handy if you add or remove a node to your cluster or want to test a new setting.

# Check the Slurm logs when everything crashes at startup time

If any container exists with an error code, for example because of a bug in a job_submit script, it is possible
to see the Slurm logs from the host. For instance, to see the slurmctld logs, execute the following command:

```console
sudo docker run --rm -i -v=slurm-docker-cluster_var_log_slurm:/tmp/slurm  busybox cat /tmp/slurm/slurmctld.log
```