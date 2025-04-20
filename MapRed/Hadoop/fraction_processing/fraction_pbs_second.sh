#!/bin/bash
#PBS -N FructionSecondJob
#PBS -q zeus_new_q
#PBS -j oe
#PBS -m bae
#PBS -M rotem.kendel@campus.technion.ac.il
#PBS -l select=2:ncpus=128:mem=256GB
#PBS -l walltime=72:00:00

# Set working directory
PBS_O_WORKDIR=/home/rotem.kendel
cd $PBS_O_WORKDIR

time > FructionSecondJob.log

# Load necessary modules or source environment variables
source /usr/local/intel21/setup.sh
source ~/.bashrc

# Start HDFS and YARN (if not already running)
start-dfs.sh
start-yarn.sh
# Ensure NameNode is out of Safe Mode
hdfs dfsadmin -safemode leave

# Wait for services to start up (optional, adjust sleep time as necessary)
sleep 10

echo "finished start HDFS: " >> FructionSecondJob.log
time >> FructionSecondJob.log

# Move to the execution directory
work_path=/home/rotem.kendel/projecctA/Hadoop/fraction_num_term
cd "$work_path"

# Define input and output directories in HDFS
INPUT_DIR="/user/rotem.kendel/input_second"
OUTPUT_DIR="/user/rotem.kendel/output_second"

# Remove old input and output directories if they exist
hdfs dfs -rm -r -skipTrash $INPUT_DIR
hdfs dfs -rm -r -skipTrash $OUTPUT_DIR

# Create the input directory in HDFS
hdfs dfs -mkdir -p $INPUT_DIR

# Upload local files to the HDFS input directory
hdfs dfs -put $work_path/local_input_files/* $INPUT_DIR

echo "finished upload files: " >> FructionSecondJob.log
time >> FructionSecondJob.log

# Remove local output directory if it exists
LOCAL_OUTPUT_DIR="$work_path/local_output_second"
if [ -d "$LOCAL_OUTPUT_DIR" ]; then
    rm -r "$LOCAL_OUTPUT_DIR"
fi

# Run the WordCount job
hadoop jar "$work_path"/Fraction.jar ComplexTermFractionDriver $INPUT_DIR $OUTPUT_DIR

echo "finished Hadoop run: " >> FructionSecondJob.log
time >> FructionSecondJob.log

# Copy the output from HDFS to the local filesystem
mkdir -p $LOCAL_OUTPUT_DIR
hdfs dfs -get $OUTPUT_DIR/* $LOCAL_OUTPUT_DIR/

# Optionally stop Hadoop services after job completion
stop-dfs.sh
stop-yarn.sh

echo "finished work: " >> FructionSecondJob.log
time >> FructionSecondJob.log