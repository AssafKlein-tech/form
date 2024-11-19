#!/bin/bash
#PBS -N parser_data
#PBS -q zeus_long_q
#PBS -j oe
#PBS -m bae
#PBS -M rotem.kendel@campus.technion.ac.il
#PBS -l select=1:ncpus=128
#PBS -l walltime=168:00:00

# Set the working directory
PBS_O_WORKDIR=/home/rotem.kendel
cd $PBS_O_WORKDIR

# Load necessary modules or environment settings
source /usr/local/intel21/setup.sh
source .bashrc

# Define the worker function
pars_worker() {
    local INDEX=$1
    date > "start_time_worker_$INDEX"
    ./projecctA/inputs/pars_data.sh ./data.o443924 "$INDEX"
    date > "end_time_worker_$INDEX"
}

# Run the worker function in parallel
for i in {5..64}
do
    pars_worker "$i" &
done

# Wait for all background jobs to finish
wait

echo "All jobs have completed."
date > "end_time"