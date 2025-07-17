#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Define variables for clarity and maintainability
CONTAINER_NAME="slurmctld"
CLUSTER_NAME="cluster"
SACCTMGR_CMD="/usr/bin/sacctmgr --immediate add cluster name=$CLUSTER_NAME"

# Check if the Docker container is running
if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "---> Docker container ${CONTAINER_NAME} is running. Proceeding with the command execution."

    # Execute the command inside the slurmctld container to add the cluster
    docker exec "${CONTAINER_NAME}" bash -c "${SACCTMGR_CMD}"

    echo "---> Cluster ${CLUSTER_NAME} added successfully in ${CONTAINER_NAME}."

    # Restart the slurmdbd and slurmctld services using Docker Compose
    echo "---> Restarting slurmdbd and slurmctld services..."
    docker-compose restart slurmdbd slurmctld

    echo "---> Services restarted successfully."
else
    echo "Error: Docker container ${CONTAINER_NAME} is not running."
    exit 1
fi

