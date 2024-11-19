#!/bin/bash
#PBS -N HadoopJob
#PBS -q zeus_new_q
#PBS -j oe
#PBS -m bae
#PBS -M rotem.kendel@campus.technion.ac.il
#PBS -l select=1:ncpus=128
#PBS -l walltime=72:00:00

# Set working directory
PBS_O_WORKDIR=/home/rotem.kendel
cd $PBS_O_WORKDIR

# Load necessary modules or source environment variables
source /usr/local/intel21/setup.sh
source ~/.bashrc

# Start HDFS and YARN (if not already running)
start-dfs.sh
start-yarn.sh

hdfs dfsadmin -safemode leave

# Wait for services to start up (optional, adjust sleep time as necessary)
sleep 10

# Run a Hadoop example job (calculate Pi)
hadoop jar $HADOOP_HOME/share/hadoop/mapreduce/hadoop-mapreduce-examples-3.3.6.jar pi 2 5

# Optionally stop Hadoop services after job completion
stop-dfs.sh
stop-yarn.sh

# Log completion time
date > end_time
