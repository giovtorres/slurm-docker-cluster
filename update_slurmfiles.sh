#!/usr/bin/env bash

set -e

restart=false

if [ $# -eq 0 ]; then
    echo "Usage: $0 configs/slurm.conf.tpl [configs/slurmdbd.conf.tpl ...]"
    echo "Example: $0 configs/slurm.conf.tpl configs/slurmdbd.conf.tpl"
    exit 1
fi

for var in "$@"; do
    if [[ "$var" == *.tpl ]]; then
        # Get base name without .tpl extension
        base_name=$(basename "$var" .tpl)
        
        echo "Updating $base_name from template $var..."
        
        # Copy template to container
        docker cp "$var" slurmctld:/usr/local/share/slurm/templates/
        
        # Get current IMAGE_TAG from .env file and calculate SLURM_VERSION
        source .env
        SLURM_VERSION=$(echo "$IMAGE_TAG" | cut -d. -f1,2)
        
        # Regenerate config from template
        docker exec slurmctld bash -c "
            SLURM_VERSION='$SLURM_VERSION' gomplate \
                -f /usr/local/share/slurm/templates/$(basename "$var") \
                -o /etc/slurm/$base_name
        "
        restart=true
    else
        echo "Error: Only .tpl template files are supported"
        echo "Usage: $0 configs/slurm.conf.tpl [configs/slurmdbd.conf.tpl ...]"
        exit 1
    fi
done

if $restart; then 
    echo "Restarting containers to apply configuration changes..."
    docker compose restart
    echo "Configuration update complete!"
fi
