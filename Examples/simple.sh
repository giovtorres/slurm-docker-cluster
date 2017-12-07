#!/usr/bin/env bash

#SBATCH -o slurm.sh.out

echo "In the directory: `pwd`"
echo "As the user: `whoami`"
echo "write this is a file" > analysis.output
sleep 60
