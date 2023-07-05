# Slurm Docker Cluster

This is a multi-container Slurm cluster using Kubernetes.  The Helm chart
creates a named volume for persistent storage of MySQL data files as well as
an NFS volume for shared storage.

## Dependencies

Requires:

* A Kubernetes cluster
* Local installations of
  * Helm
  * kubectl

## Containers and Volumes

The Helm chart will run the following containers:

* login
* mysql
* nfs-server
* slurmdbd
* slurmctld
* slurmd (2 replicas by default)

The compose file will create the following named volumes:

* nfs-server-volume ( -> /home          )
* var_lib_mysql     ( -> /var/lib/mysql )

## Configuring the Cluster

All config files in `slurm-cluster-chart/files` will be mounted into the container to configure their respective services on startup. The `authorized_keys` file contains authorised public keys for the user `rocky`, add your public key to access the cluster. Note that changes to these files will not be propagated to existing deployments (see Reconfiguring the Cluster).
Additional parameters can be found in the `values.yaml` file, which will be applied on a Helm chart deployment. Note that some of these values, such as `encodedMungeKey` will also not propagate until the cluster is restarted (see Reconfiguring the Cluster).

## Deploying the Cluster

After configuring `kubectl` with the appropriate `kubeconfig` file, deploy the cluster using the Helm chart:
```console
helm install <deployment-name> slurm-cluster-chart
```

The cluster will not automatically be configured with the correct IP address for the NFS server. To configure this, run
```console
kubectl get pod -l app=nfs-server -o jsonpath="{.items[0].status.podIP}"
```
to retrieve the pod IP of the NFS server and replace the `nfs.server` value in the `values.yaml` file with this new IP. You can then redeploy the cluster using
```console
helm upgrade <deployment-name> slurm-cluster-chart
```
Use this command for subsequent deployments of the cluster

Note: when redeploying the cluster with the correct NFS IP, you may need to use `kubectl delete pod --force` on the existing `slurmctld`, `slurmd` and `login` pods

## Accessing the Cluster

Retrieve the external IP address of the login node using:
```console
LOGIN=$(kubectl get service login -o jsonpath="{.status.loadBalancer.ingress[0].ip}")
```
and connect to the cluster as the `rocky` user with
```console
ssh rocky@$LOGIN
```

From the shell, execute slurm commands, for example:

```console
[root@slurmctld /]# sinfo
PARTITION AVAIL  TIMELIMIT  NODES  STATE NODELIST
normal*      up 5-00:00:00      2   idle c[1-2]
```

## Running MPI Benchmarks

The Intel MPI Benchmarks are included in the containers. These can be run both with mpirun and srun.
Example job scripts:
* srun:
```console
#!/usr/bin/env bash

#SBATCH -N 2
#SBATCH --ntasks-per-node=1

echo $SLURM_JOB_ID: $SLURM_JOB_NODELIST
srun /usr/lib64/openmpi/bin/mpitests-IMB-MPI1 pingpong
```
* mpirun
```console
#!/usr/bin/env bash

#SBATCH -N 2
#SBATCH --ntasks-per-node=1

echo $SLURM_JOB_ID: $SLURM_JOB_NODELIST
/usr/lib64/openmpi/bin/mpirun --prefix /usr/lib64/openmpi mpitests-IMB-MPI1 pingpong
```
Note: The mpirun script assumes you are running as user 'rocky'. If you are running as root, you will need to include the --allow-run-as-root argument
## Reconfiguring the Cluster

To guarantee changes to config files are propagated to the cluster, use
```console
kubectl rollout restart deployment <deployment-names>
```
Generally restarts to `slurmd`, `slurmctld`, `login` and `slurmdbd` will be required