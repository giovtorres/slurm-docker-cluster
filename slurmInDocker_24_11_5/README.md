# Slurm Docker Cluster

This is a multi-container Slurm cluster using docker compose.  

The compose file creates named volumes for persistent storage of MySQL data files as well as Slurm state and log directories.

# Intro

## 1. Containers and Volumes

The compose file will run the following containers:

* mysql
* slurmdbd
* slurmctld
* compute1 (slurmd)
* compute2 (slurmd)
* grafana
* elasticsearch
* base(for test)

The compose file will create the following named volumes:

* etc_munge         ( -> /etc/munge     )
* etc_slurm         ( -> /etc/slurm     )
* slurm_jobdir      ( -> /data          )
* var_lib_mysql     ( -> /var/lib/mysql )
* var_log_slurm     ( -> /var/log/slurm )

## 2. Default settings 

Maria DB : 
  - Default user : `slurm`
  - Default password : `password`

RockyLinux : `9`

Slurm default version : `24.11.5`

UID : `990`
GID : `990`


## 3. Building the Docker Image

Build the image locally:

```bash
docker build -t slurm-docker-cluster:24-11-5 .
```

Or equivalently using `docker compose`:

```bash
docker compose up -d
```

Slurm Tags : https://github.com/SchedMD/slurm/tags

## 4. Starting the Cluster

Run `docker compose` to instantiate the cluster:

```console
docker compose up -d
```

## 5. Register the Cluster with SlurmDBD

To register the cluster to the slurmdbd daemon, run the `register_cluster.sh`
script:

```console
./register_cluster.sh
```

> Note: You may have to wait a few seconds for the cluster daemons to become
> ready before registering the cluster.  Otherwise, you may get an error such
> as **sacctmgr: error: Problem talking to the database: Connection refused**.
>
> You can check the status of the cluster by viewing the logs: `docker compose
> logs -f`

## 6. Accessing the Cluster

Use `docker exec` to run a bash shell on the controller container:

```console
docker exec -it slurmctld bash
```

From the shell, execute slurm commands, for example:

```console
[root@slurmctld /]# sinfo
PARTITION AVAIL  TIMELIMIT  NODES  STATE NODELIST
normal*      up 5-00:00:00      2   idle c[1-2]
```

## 7. Submitting Jobs

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

## 8. Stopping and Restarting the Cluster

```console
docker compose stop
docker compose start
```

## 9.Deleting the Cluster

To remove all containers and volumes, run:

```console
docker compose stop
docker compose rm -f
docker volume rm slurm-docker-cluster_etc_munge slurm-docker-cluster_etc_slurm slurm-docker-cluster_slurm_jobdir slurm-docker-cluster_var_lib_mysql slurm-docker-cluster_var_log_slurm
```
## 10. Updating the Cluster

If you want to change the `slurm.conf` or `slurmdbd.conf` file without a rebuilding you can do so by calling
```console
./update_slurmfiles.sh slurm.conf slurmdbd.conf
```
(or just one of the files).
The Cluster will automatically be restarted afterwards with
```console
docker compose restart
```
This might come in handy if you add or remove a node to your cluster or want to test a new setting.

----------

# Settings during manual compilation of slurm programs

## 1.  cgroup

The requirements for cgroups vary depending on the version of slurm.  So the cgroup option is turned off by default.

**NOTE**: The cgroup/v1 plugin is deprecated and will not be supported in future Slurm versions. Newer GNU/Linux distributions are dropping, or have dropped, support for cgroup v1 and may even not provide kernel support for the required cgroup v1 interfaces. Systemd also deprecated cgroup v1. Starting with Slurm version 25.05, no new features will be added to cgroup v1. Support for critical bugs will be provided until its final removal. 

quote from https://slurm.schedmd.com/cgroups.html

You can choose a cgroup with the same version as the host machine at compile time. 

see which version of cgroup on your host-machine:

``` grep cgroup /proc/filesystems``` 

if v1: 

```bash
./configure --enable-debug --prefix=/usr --sysconfdir=/etc/slurm  --with-mysql_config=/usr/bin  --libdir=/usr/lib64 --with-cgroup=v1\
```

if v2: 

```bash
./configure --enable-debug --prefix=/usr --sysconfdir=/etc/slurm  --with-mysql_config=/usr/bin  --libdir=/usr/lib64 --with-cgroup=v2\
# To make sure this code is in your Dockerfile
dnf install -y dbus-devel libbpf-devel hwloc-devel gcc make
cp src/plugins/cgroup/v2/.libs/cgroup_v2.so /usr/lib64/slurm/

```

Don't forget to modify slurm.conf and cgroup.conf files, see more: https://slurm.schedmd.com/cgroups.html

## 2.  RESTAPI (slurmrestd)

This daemon is designed to allow clients to communicate with Slurm via a REST API, quote from https://slurm.schedmd.com/rest.html

Verify that all installation dependencies are complete：

``` bash
dnf install epel-release -y
dnf config-manager --set-enabled epel
dnf install libjwt libjwt-devel -y
./configure --prefix=/usr --sysconfdir=/etc/slurm --with-jwt --enable-slurmrestd
cd src/plugins/auth/jwt
make
```

UID  990  slurm            GID  990  slurm         
UID  995  slurmrestd   GID  995  restd

slurmrestd should not be run as SlurmUser or with SlurmUser’s group. see chown and chmod in Dockerfile

!!! Be sure to find the correct version of dataparser, this is not mentioned in the official manual, but if the version is wrong you will not be able to submit the rest request correctly, Default: Latest data_parser plugin version with no flags in **slurm.conf** selected:

```bash
# operation in container
ls /usr/lib64/slurm/data_parser_*
#------------------------------------------------------------------------------------------------------------------
#/usr/lib64/slurm/data_parser_v0_0_40.a	 /usr/lib64/slurm/data_parser_v0_0_40.so  /usr/lib64/slurm/data_parser_v0_0_41.la  /usr/lib64/slurm/data_parser_v0_0_42.a   /usr/lib64/slurm/data_parser_v0_0_42.so
#/usr/lib64/slurm/data_parser_v0_0_40.la  /usr/lib64/slurm/data_parser_v0_0_41.a   /usr/lib64/slurm/data_parser_v0_0_41.so  /usr/lib64/slurm/data_parser_v0_0_42.la
```

TEST:

entry slurmctld:

```	bash
scontrol token username=<name>
# generate a key SLURM_JWT=thisisakey.yekasisiht
```

in hostmachine:

```sh
$ VER=v0.0.42                        
curl -H "X-SLURM-USER-TOKEN:thisisakey.yekasisiht\" \
     $API/slurm/$VER/diag | jq
```

then should see:

```

        "cycle_last": 0,
        "cycle_max": 0,
        "total_time": 137,
        "average_time": {
          "set": true,
          "infinite": false,
          "number": 137
        }
      },
      {
        "type_id": 5039,
        "message_type": "REQUEST_AUTH_TOKEN",
        "count": 2,
        "queued": 0,
        "dropped": 0,
        "cycle_last": 0,
        "cycle_max": 0,
        "total_time": 1350,
        "average_time": {
          "set": true,
          "infinite": false,
          "number": 675
        }
      }
    ],
    "rpcs_by_user": [
      {
        "user_id": 0,
        "user": "root",
        "count": 3,
        "total_time": 1487,
        "average_time": {
          "set": true,
          "infinite": false,
          "number": 495
        }
      }
    ],
    "pending_rpcs": [],
    "pending_rpcs_by_hostlist": []
  },
  "meta": {
    "plugin": {
      "type": "openapi/slurmctld",
      "name": "Slurm OpenAPI slurmctld",
      "data_parser": "data_parser/v0.0.42",
      "accounting_storage": "accounting_storage/slurmdbd"
    },
    "client": {
      "source": "[slurmrestd]:42010(fd:9)",
      "user": "root",
      "group": "root"
    },
    "command": [],
    "slurm": {
      "version": {
        "major": "24",
        "micro": "5",
        "minor": "11"
      },
      "release": "24.11.5",
      "cluster": "cluster"
    }
  },
  "errors": [],
  "warnings": []
}
```

## 3. Elasticsearch

By default, elasticsearch is enabled, and the corresponding plugin is compiled. See Dockerfile

``` shell
RUN dnf install -y \
        libcurl-devel \
        pam-devel \
        numactl-devel \
        openssl-devel\ 
        readline-devel\ 
        gtk2-devel\ 
        perl-ExtUtils-MakeMaker

WORKDIR /home/software/slurm
RUN ./configure \
    --prefix=/usr \
    --sysconfdir=/etc/slurm \
    --with-json \
  && cd src/plugins/jobcomp/elasticsearch \
  && make -j$(nproc) \
  && cp .libs/jobcomp_elasticsearch.so /usr/lib64/slurm/

```

If you don't need this feature, modify the dockerfile and yml.

Note: slurm's plugin does not provide timestamps, http://localhost:3000/, Click on the logo in the upper left -> Data Source -> select Elasticsearch -> Time field name, select any one except @timestamp. 



## Credit

This is a fork of Giovanni Torres' github repo : https://github.com/giovtorres/slurm-docker-cluster

and of SckyzO's github repo: https://github.com/SckyzO/slurm-docker-cluster

I upgraded the slurm level to 24.11.5 and added slurmrestd, elasticsearch, grafana.

Many of the error-prone steps I compiled completely manually to make the build clearer.
