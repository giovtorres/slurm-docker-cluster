#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

# Flag to determine if a restart is needed
restart=false

# Define the Docker container name
CONTAINER_NAME="slurmctld"

# Function to update configuration file inside the Docker container
update_config_file() {
    local file_name=$1
    echo "---> Updating ${file_name} inside the ${CONTAINER_NAME} container..."
    
    # Read the content of the configuration file into a variable
    if [ -f "$file_name" ]; then
        SLURM_TMP=$(cat "$file_name")
        
        # Update the configuration file inside the Docker container
        docker exec "${CONTAINER_NAME}" bash -c "echo \"$SLURM_TMP\" > /etc/slurm/$file_name"
        echo "---> ${file_name} updated successfully."
        
        # Set the restart flag to true
        restart=true
    else
        echo "Error: File ${file_name} does not exist."
        exit 1
    fi
}

# Loop through all the arguments passed to the script
for var in "$@"; do
    # Check if the argument is either slurmdbd.conf or slurm.conf
    if [ "$var" = "slurmdbd.conf" ] || [ "$var" = "slurm.conf" ]; then
        update_config_file "$var"
    else
        echo "Warning: Unsupported file ${var}. Only slurmdbd.conf and slurm.conf are supported."
    fi
done

# Restart the Docker Compose services if any configuration file was updated
if $restart; then
    echo "---> Restarting slurmdbd and slurmctld services..."
    docker-compose restart slurmdbd slurmctld
    echo "---> Services restarted successfully."
fi

