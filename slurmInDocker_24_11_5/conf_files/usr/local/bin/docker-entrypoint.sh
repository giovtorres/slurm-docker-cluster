#!/bin/bash
set -e

# Function to wait for the database to be ready
wait_for_database() {
    echo "---> Waiting for the database to become active ..."
    # Loop until the database is ready to accept connections
    until echo "SELECT 1" | mysql -h $StorageHost -u$StorageUser -p$StoragePass 2>&1 > /dev/null
    do
        echo "-- Waiting for database to become active ..."
        sleep 2
    done
    echo "-- Database is now active ..."
}

# Function to wait for a service to be ready on a given port
wait_for_service() {
    local service_name=$1
    local port=$2
    echo "---> Waiting for $service_name to become active on port $port ..."
    # Loop until the service is available on the specified port
    until 2>/dev/null >/dev/tcp/$service_name/$port
    do
        echo "-- $service_name is not available on port $port. Sleeping ..."
        sleep 2
    done
    echo "-- $service_name is now active on port $port ..."
}

# Main entrypoint logic
# Check the first argument passed to the script and start the appropriate service
if [ "$1" = "slurmdbd" ]; then
    echo "---> Starting the MUNGE Authentication service (munged) ..."
    # Start the MUNGE service using gosu to switch to the munge user
    gosu munge /usr/sbin/munged

    echo "---> Starting the Slurm Database Daemon (slurmdbd) ..."

    # Source the slurmdbd configuration file to get the database credentials
    . /etc/slurm/slurmdbd.conf

    # Wait for the database to be ready before starting slurmdbd
    wait_for_database
    # Start slurmdbd with gosu to switch to the slurm user
    exec gosu slurm /usr/sbin/slurmdbd -Dvvv

elif [ "$1" = "slurmctld" ]; then
    echo "---> Starting the MUNGE Authentication service (munged) ..."
    # Start the MUNGE service using gosu to switch to the munge user
    gosu munge /usr/sbin/munged

    echo "---> Waiting for slurmdbd to become active before starting slurmctld ..."
    # Wait for slurmdbd to be ready before starting slurmctld
    wait_for_service "slurmdbd" 6819

    echo "---> Starting the Slurm Controller Daemon (slurmctld) ..."
    # Check the version of slurmctld and start it with the appropriate options
    if /usr/sbin/slurmctld -V | grep -q '17.02' ; then
        exec gosu slurm /usr/sbin/slurmctld -Dvvv
    else
        exec gosu slurm /usr/sbin/slurmctld -i -Dvvv
    fi

elif [ "$1" = "slurmd" ]; then
    echo "---> Starting the MUNGE Authentication service (munged) ..."
    # Start the MUNGE service using gosu to switch to the munge user
    gosu munge /usr/sbin/munged

    echo "---> Waiting for slurmctld to become active before starting slurmd ..."
    # Wait for slurmctld to be ready before starting slurmd
    wait_for_service "slurmctld" 6817

    echo "---> Starting the Slurm Node Daemon (slurmd) ..."
    # Start slurmd
    exec /usr/sbin/slurmd -Dvvv

elif [ "$1" = "slurmrestd" ]; then
    echo "---> Starting the MUNGE Authentication service (munged) ..."
    gosu munge /usr/sbin/munged        

    echo "---> Waiting for slurmctld to become active ..."
    wait_for_service "slurmctld" 6817    
    # var key for identify, scontrol token generate key for commuinication between components, scontrol token username=<user> for user submit jobs.
    export SLURM_JWT=daemon
    echo "---> Starting slurmrestd ..."
    exec gosu slurmrestd /usr/sbin/slurmrestd \
        -vvv \
        -a jwt \
        0.0.0.0:6820

else
    exec "$@"

fi