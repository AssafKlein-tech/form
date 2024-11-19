#!/bin/bash
#PBS -N parser_data
#PBS -q zeus_new_q
#PBS -j oe
#PBS -m bae
#PBS -M rotem.kendel@campus.technion.ac.il
#PBS -l select=1:ncpus=128
#PBS -l walltime=72:00:00


PBS_O_WORKDIR=/home/rotem.kendel

date > "start_time"

source /usr/local/intel21/setup.sh
source .bashrc
cd $PBS_O_WORKDIR

./projecctA/inputs/pars_data.sh ./data_copied 64

date > "end_time"
