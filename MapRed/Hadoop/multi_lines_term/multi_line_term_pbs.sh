#!/bin/bash
#PBS -N multiLineTermJob
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

# Ensure NameNode is out of Safe Mode
hdfs dfsadmin -safemode leave

# Wait for services to start up (optional, adjust sleep time as necessary)
sleep 10

# Move to the execution directory
work_path=/home/rotem.kendel/projecctA/Hadoop/multi_lines_term
cd "$work_path"

# Define input and output directories in HDFS
INPUT_DIR="/user/rotem.kendel/input"
OUTPUT_DIR="/user/rotem.kendel/output"

# Remove old input and output directories if they exist
hdfs dfs -rm -r -skipTrash $INPUT_DIR
hdfs dfs -rm -r -skipTrash $OUTPUT_DIR

# Create the input directory in HDFS
hdfs dfs -mkdir -p $INPUT_DIR

# Upload local files to the HDFS input directory
hdfs dfs -put $work_path/local_input_files/* $INPUT_DIR

# Remove local output directory if it exists
LOCAL_OUTPUT_DIR="$work_path/local_output"
if [ -d "$LOCAL_OUTPUT_DIR" ]; then
    rm -r "$LOCAL_OUTPUT_DIR"
fi

# Create the local output directory
mkdir -p $LOCAL_OUTPUT_DIR

# Run the WordCount job
hadoop jar "$work_path"/MultiLines.jar MultiLineTermDriver $INPUT_DIR $OUTPUT_DIR

# Copy the output from HDFS to the local filesystem
hdfs dfs -get $OUTPUT_DIR/* $LOCAL_OUTPUT_DIR/

# Optionally stop Hadoop services after job completion
stop-dfs.sh
stop-yarn.sh
