#!/bin/bash
set -e

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}Running Slurm Example Jobs${NC}"
echo -e "${BLUE}================================${NC}"
echo ""

# Copy examples to cluster if not already there
echo -e "${YELLOW}[INFO]${NC} Copying example jobs to cluster..."
docker exec slurmctld bash -c "mkdir -p /data/examples"
docker cp examples/jobs/. slurmctld:/data/examples/

echo ""
echo -e "${BLUE}Example jobs available:${NC}"
docker exec slurmctld bash -c "ls -1 /data/examples/*.sh" | sed 's|/data/examples/||'

echo ""
echo -e "${YELLOW}Select mode:${NC}"
echo "  1) Run all examples"
echo "  2) Run specific example"
echo "  3) List examples only"
read -p "Enter choice (1-3): " choice

case $choice in
    1)
        echo ""
        echo -e "${BLUE}Submitting all example jobs...${NC}"

        docker exec slurmctld bash -c "ls -1 /data/examples/*.sh" | while read job_script; do
            job_name=$(basename "$job_script")
            echo -e "${YELLOW}[SUBMIT]${NC} $job_name"

            job_id=$(docker exec slurmctld bash -c "cd /data && sbatch examples/$job_name 2>&1" | sed -n 's/.*Submitted batch job \([0-9][0-9]*\).*/\1/p')

            if [ -n "$job_id" ]; then
                echo -e "${GREEN}  ✓${NC} Job ID: $job_id"
            else
                echo -e "${RED}  ✗${NC} Failed to submit"
            fi
        done

        echo ""
        echo -e "${BLUE}Waiting for jobs to complete...${NC}"
        sleep 5

        # Wait for all jobs to finish
        max_wait=60
        waited=0
        while [ $waited -lt $max_wait ]; do
            running=$(docker exec slurmctld squeue -h 2>/dev/null | wc -l)
            if [ "$running" -eq 0 ]; then
                echo -e "${GREEN}All jobs completed!${NC}"
                break
            fi
            echo -e "${YELLOW}  Waiting...${NC} ($running jobs still running)"
            sleep 5
            waited=$((waited + 5))
        done

        echo ""
        echo -e "${BLUE}Job outputs:${NC}"
        docker exec slurmctld bash -c "cd /data && ls -1 *.out 2>/dev/null | tail -10 | while read f; do echo '---' \$f '---'; head -20 \$f; echo ''; done"
        ;;

    2)
        echo ""
        docker exec slurmctld bash -c "ls -1 /data/examples/*.sh" | sed 's|/data/examples/||' | nl
        read -p "Enter number: " num

        job_script=$(docker exec slurmctld bash -c "ls -1 /data/examples/*.sh" | sed -n "${num}p")
        job_name=$(basename "$job_script")

        echo ""
        echo -e "${YELLOW}[SUBMIT]${NC} $job_name"
        job_id=$(docker exec slurmctld bash -c "cd /data && sbatch examples/$job_name 2>&1" | sed -n 's/.*Submitted batch job \([0-9][0-9]*\).*/\1/p')

        if [ -n "$job_id" ]; then
            echo -e "${GREEN}  ✓${NC} Job ID: $job_id submitted"

            echo ""
            echo -e "${BLUE}Waiting for job to complete...${NC}"

            for i in {1..30}; do
                state=$(docker exec slurmctld squeue -j "$job_id" -h -o "%T" 2>/dev/null || echo "COMPLETED")
                if [ "$state" = "COMPLETED" ] || [ -z "$state" ]; then
                    echo -e "${GREEN}Job completed!${NC}"
                    break
                fi
                echo -e "${YELLOW}  Job state:${NC} $state"
                sleep 2
            done

            echo ""
            echo -e "${BLUE}Job output:${NC}"
            output_file=$(docker exec slurmctld bash -c "ls -t /data/*.out 2>/dev/null | head -1")
            if [ -n "$output_file" ]; then
                docker exec slurmctld cat "$output_file"
            else
                echo -e "${RED}No output file found${NC}"
            fi
        else
            echo -e "${RED}  ✗${NC} Failed to submit job"
        fi
        ;;

    3)
        echo ""
        echo -e "${BLUE}Example jobs:${NC}"
        docker exec slurmctld bash -c "cd /data/examples && for f in *.sh; do echo ''; echo -e '${BLUE}\$f${NC}:'; head -8 \$f | grep -E '^#SBATCH|^# ' | sed 's/^#SBATCH/  SBATCH/; s/^# /  /'; done"
        ;;

    *)
        echo -e "${RED}Invalid choice${NC}"
        exit 1
        ;;
esac

echo ""
echo -e "${BLUE}================================${NC}"
echo -e "${GREEN}Done!${NC}"
echo ""
echo -e "${YELLOW}Tip:${NC} Use 'make jobs' to view job queue"
echo -e "${YELLOW}Tip:${NC} Use 'docker exec slurmctld squeue' to check job status"
echo -e "${YELLOW}Tip:${NC} Use 'docker exec slurmctld ls /data/*.out' to see output files"
