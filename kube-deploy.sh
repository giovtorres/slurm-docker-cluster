kubectl apply -f 'kube-manifests/*.yaml'
kubectl create configmap slurm-conf-configmap --from-file=slurm.conf
kubectl create configmap slurmdbd-conf-configmap --from-file=slurmdbd.conf