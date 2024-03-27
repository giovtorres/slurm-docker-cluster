#!/usr/bin/env bash

file="$1"
path="$2"

set -e

export CONTENT_TMP=$(cat $file)
sudo docker exec slurmctld bash -c "echo \"$CONTENT_TMP\" >$path/\"$file\""
sudo docker-compose restart
