#!/bin/bash
set -e

# Detect replica number by matching own IP against Docker Compose DNS entries.
# Compose names containers as <project>-<service>-<N>, and Docker's embedded DNS
# resolves these names within the network. We iterate N=1..max and find which
# one resolves to our IP.
# Falls back to the container ID (hostname) if detection fails.
detect_replica_number() {
    local service_name="$1"
    local max_replicas="${2:-64}"
    local my_ip
    my_ip=$(hostname -i 2>/dev/null | awk '{print $1}')

    for i in $(seq 1 "$max_replicas"); do
        local resolved
        resolved=$(getent hosts "${COMPOSE_PROJECT_NAME}-${service_name}-${i}" 2>/dev/null | awk '{print $1}')
        if [ "$resolved" = "$my_ip" ]; then
            echo "$i"
            return 0
        elif [ -z "$resolved" ]; then
            break
        fi
    done

    # Fallback: use container ID
    hostname
    return 1
}

echo "---> Starting the MUNGE Authentication service (munged) ..."
gosu munge /usr/sbin/munged

if [ "$1" = "slurmdbd" ]
then
    echo "---> Starting the Slurm Database Daemon (slurmdbd) ..."

    # Substitute environment variables in slurmdbd.conf
    envsubst < /etc/slurm/slurmdbd.conf > /etc/slurm/slurmdbd.conf.tmp
    mv /etc/slurm/slurmdbd.conf.tmp /etc/slurm/slurmdbd.conf
    chown slurm:slurm /etc/slurm/slurmdbd.conf
    chmod 600 /etc/slurm/slurmdbd.conf

    # create jwt key for jwt/auth
    if [ ! -f /etc/slurm/jwt_hs256.key ]; then
        dd if=/dev/random of=/etc/slurm/jwt_hs256.key bs=32 count=1
        chown slurm:slurm /etc/slurm/jwt_hs256.key
        chmod 0600 /etc/slurm/jwt_hs256.key
    fi

    # Wait for MySQL using environment variables directly
    until echo "SELECT 1" | mysql -h mysql -u${MYSQL_USER} -p${MYSQL_PASSWORD} 2>&1 > /dev/null
    do
        echo "-- Waiting for database to become active ..."
        sleep 2
    done
    echo "-- Database is now active ..."

    exec gosu slurm /usr/sbin/slurmdbd -Dvvv
    # exec tail -f /dev/null
fi

if [ "$1" = "slurmctld" ]
then

    if [ "$SSH_ENABLE" = "true" ]
    then
        echo "---> Configuring SSH ..."
        # Require authorized_keys from host mount to exist and be a regular file
        if [ ! -f /tmp/authorized_keys_host ]; then
            echo "---> ERROR: host 'authorized_keys' not correctly mounted to /tmp/authorized_keys_host (file missing or SSH_AUTHORIZED_KEYS variable has wrong filename )." >&2
            exit 1
        fi

        mkdir -p /root/.ssh
        cp /tmp/authorized_keys_host /root/.ssh/authorized_keys
        chown root:root /root/.ssh/authorized_keys
        chmod 600 /root/.ssh/authorized_keys
        chown root:root /root/.ssh
        chmod 700 /root/.ssh
        echo "---> Copied and set permissions for authorized_keys"

        echo "---> Start SSHD ..."
        /usr/sbin/sshd -D -e &
    fi

    echo "---> Waiting for slurmdbd to become active before starting slurmctld ..."

    until 2>/dev/null >/dev/tcp/slurmdbd/6819
    do
        echo "-- slurmdbd is not available.  Sleeping ..."
        sleep 2
    done
    echo "-- slurmdbd is now active ..."

    # Configure Elasticsearch for job completion if ELASTICSEARCH_HOST is set
    if [ -n "${ELASTICSEARCH_HOST}" ]; then
        echo "---> Configuring Elasticsearch job completion logging..."
        echo "---> Elasticsearch host: ${ELASTICSEARCH_HOST}"

        # Wait for Elasticsearch to be available
        until curl -s "${ELASTICSEARCH_HOST}/_cluster/health" >/dev/null 2>&1; do
            echo "-- Elasticsearch is not available. Sleeping ..."
            sleep 2
        done
        echo "-- Elasticsearch is now active ..."

        # Update slurm.conf to use Elasticsearch for job completion
        # Format: http://host:port/index/_doc (ES 8.x+ typeless mode)
        sed -i "s|^JobCompType=.*|JobCompType=jobcomp/elasticsearch|" /etc/slurm/slurm.conf
        sed -i "s|^JobCompLoc=.*|JobCompLoc=${ELASTICSEARCH_HOST}/slurm/_doc|" /etc/slurm/slurm.conf

        echo "---> Job completion configured for Elasticsearch"
    fi

    echo "---> Starting the Slurm Controller Daemon (slurmctld) ..."
    exec gosu slurm /usr/sbin/slurmctld -i -Dvvv
fi

if [ "$1" = "slurmrestd" ]
then
    echo "---> Waiting for slurmctld to become active before starting slurmrestd ..."

    until 2>/dev/null >/dev/tcp/slurmctld/6817
    do
        echo "-- slurmctld is not available.  Sleeping ..."
        sleep 2
    done
    echo "-- slurmctld is now active ..."

    echo "---> Starting the Slurm REST API Daemon (slurmrestd) ..."
    # Run slurmrestd on both Unix socket and network port
    # Unix socket provides passwordless local access
    # Note: slurmrestd should NOT be run as SlurmUser or root (security requirement)
    mkdir -p /var/run/slurmrestd
    chown slurmrest:slurmrest /var/run/slurmrestd

    # Export the SLURM_JWT=daemon environment variable before starting the slurmrestd daemon
    # to activate AuthAltTypes=auth/jwt as the primary authentication mechanism
    export SLURM_JWT=daemon; exec gosu slurmrest /usr/sbin/slurmrestd -vvv unix:/var/run/slurmrestd/slurmrestd.socket 0.0.0.0:6820
fi

if [ "$1" = "slurmd-cpu" ]
then
    echo "---> Waiting for slurmctld to become active before starting dynamic slurmd..."

    until 2>/dev/null >/dev/tcp/slurmctld/6817
    do
        echo "-- slurmctld is not available.  Sleeping ..."
        sleep 2
    done
    echo "-- slurmctld is now active ..."

    # Derive a sequential node name from the Docker Compose replica number.
    # e.g., slurm-cpu-worker-1 -> c1, slurm-cpu-worker-2 -> c2
    # Falls back to container ID if replica detection fails.
    REPLICA=$(detect_replica_number "cpu-worker")
    NODE_NAME="c${REPLICA}"
    hostname "${NODE_NAME}"

    echo "---> Dynamic CPU worker registering as: ${NODE_NAME}"
    echo "---> Starting slurmd in dynamic registration mode (-Z)..."

    # -Z: dynamic node self-registration with slurmctld
    # Feature=cpu: tag for cpu partition NodeSet matching
    exec /usr/sbin/slurmd -Z -Dvvv \
        --conf "Feature=cpu"
fi

if [ "$1" = "slurmd-gpu" ]
then
    echo "---> Waiting for slurmctld to become active before starting GPU slurmd..."

    until 2>/dev/null >/dev/tcp/slurmctld/6817
    do
        echo "-- slurmctld is not available.  Sleeping ..."
        sleep 2
    done
    echo "-- slurmctld is now active ..."

    # Derive a sequential node name from the Docker Compose replica number.
    # e.g., slurm-gpu-worker-1 -> g1, slurm-gpu-worker-2 -> g2
    # Falls back to container ID if replica detection fails.
    REPLICA=$(detect_replica_number "gpu-worker")
    NODE_NAME="g${REPLICA}"
    hostname "${NODE_NAME}"

    echo "---> Dynamic GPU worker registering as: ${NODE_NAME}"
    echo "---> Starting slurmd in dynamic GPU registration mode (-Z)..."

    GPU_COUNT=$(ls /dev/nvidia[0-9] 2>/dev/null | wc -l)
    exec /usr/sbin/slurmd -Z -Dvvv \
        --conf "Feature=gpu Gres=gpu:nvidia:${GPU_COUNT}"
fi

exec "$@"
