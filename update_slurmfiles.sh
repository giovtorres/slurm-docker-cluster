#!/usr/bin/env bash

set -e

restart=false

for var in "$@"
do
    if [ "$var" = "slurmdbd.conf" ] || [ "$var" = "slurm.conf" ]
    then
        export SLURM_TMP=$(cat $var)
        docker exec slurmctld bash -c "echo \"$SLURM_TMP\" >/etc/slurm/\"$var\""
        restart=true
    fi
done
if $restart; then docker-compose restart; fi
