#!/bin/bash
#
# Slurm Configuration Generator using Gomplate
#
# Renders version-specific Slurm configuration files from templates.
# Usage: ./generate_configs.sh [templates_dir] [output_dir]
# Example: ./generate_configs.sh ./configs /etc/slurm
#

set -e

# Parse arguments
TEMPLATES_DIR="$1"
OUTPUT_DIR="$2"

# Set defaults if not provided via arguments
if [[ -z "$TEMPLATES_DIR" ]]; then
    # Detect if we're in container build context (legacy compatibility)
    if [[ -d "/tmp/configs" ]]; then
        TEMPLATES_DIR="/tmp/configs"
    else
        TEMPLATES_DIR="./configs"
    fi
fi

if [[ -z "$OUTPUT_DIR" ]]; then
    OUTPUT_DIR="/etc/slurm"
fi

# Read SLURM_TAG from .env if not set in environment
if [[ -z "$SLURM_TAG" ]]; then
    if [[ -f ".env" ]]; then
        source .env
    fi
fi

# Validate SLURM_TAG is set
if [[ -z "$SLURM_TAG" ]]; then
    echo "Error: SLURM_TAG not set in environment or .env file" >&2
    exit 1
fi

# Calculate SLURM_VERSION (major.minor) from IMAGE_TAG (e.g., 24.05.8 -> 24.05)
# IMAGE_TAG should be provided from .env or environment
SLURM_VERSION=$(echo "$IMAGE_TAG" | cut -d. -f1,2)

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "Generating Slurm $IMAGE_TAG configuration files..."

# Render each template
for template in slurm.conf slurmdbd.conf; do
    template_file="$TEMPLATES_DIR/$template.tpl"
    output_file="$OUTPUT_DIR/$template"

    if [[ -f "$template_file" ]]; then
        SLURM_VERSION="$SLURM_VERSION" gomplate \
            -f "$template_file" \
            -o "$output_file"


        echo "Generated: $output_file"
    else
        echo "Warning: Template not found: $template_file" >&2
    fi
done

echo "Configuration generation completed for Slurm $IMAGE_TAG"
