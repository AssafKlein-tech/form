#!/bin/bash
#PBS -N data
#PBS -q zeus_new_q
#PBS -j oe
#PBS -m bae
#PBS -M rotem.kendel@campus.technion.ac.il
#PBS -l select=1:ncpus=128
#PBS -l walltime=72:00:00

PBS_O_WORKDIR=/home/rotem.kendel
date > start_time
source /usr/local/intel21/setup.sh
cd $PBS_O_WORKDIR
source .bashrc
mkdir -p workers_data
tform -w63 ./projecctA/tests/simpleTest/print_data.frm
date > end_time
