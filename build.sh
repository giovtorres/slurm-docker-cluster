
set -ex
IMAGE="ghcr.io/pharchive/phare/slurm-docker-cluster:21.08"
docker build --build-arg -t ${IMAGE} .
