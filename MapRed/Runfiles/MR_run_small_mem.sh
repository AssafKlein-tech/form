#!/bin/bash
#Running Map-reduce

#check if first variable is a file
if [ -z "$1" ]; then
  echo "Usage: $1 <input file>"
  exit 1
fi

# Save the file provided as the first argument in a variable
export INPUT_FILE="$1"


if [ ! -f "$INPUT_FILE" ]; then
  echo "File '$INPUT_FILE' does not exist."
  exit 1
fi

# Check if an number of seed is provided
if [ -z "$2" ]; then
  echo "Usage: $2 <output_directory> "
  exit 1
fi

# Save the dir provided as the second argument in a variable
export OUTPUT_DIR="$2"

cd ~/form

# Start HDFS and YARN (if not already running)
#start-dfs.sh
#start-yarn.sh

# Ensure NameNode is out of Safe Mode
hdfs dfsadmin -safemode leave

# Wait for services to start up (optional, adjust sleep time as necessary)
sleep 2

# Define input and output directories in HDFS
DFS_INPUT_DIR="/input"
DFS_OUTPUT_DIR="/output"

# Remove old input and output directories if they exist
hdfs dfs -rm -r -skipTrash $DFS_INPUT_DIR
hdfs dfs -rm -r -skipTrash $DFS_OUTPUT_DIR

# Create the input directory in HDFS
sudo hdfs dfs -mkdir -p $DFS_INPUT_DIR

# Upload local files to the HDFS input directory
hdfs dfs -put $INPUT_FILE $DFS_INPUT_DIR

# Remove local output directory if it exists
if [ -d "$OUTPUT_DIR" ]; then
    rm -r "$OUTPUT_DIR"
fi

mkdir -p $OUTPUT_DIR

# Run the WordCount job
time hadoop jar ~/form/MapRed/Hadoop/fraction_num_term/ComplexTermProcessing.jar ComplexTermFractionDriver \
    $DFS_INPUT_DIR $DFS_OUTPUT_DIR #> $OUTPUT_DIR/log.txt 2>&1
    #-D mapreduce.task.io.file.buffer.size=131072  \
    #-D mapreduce.map.memory.mb=1792 \
    #-D mapreduce.map.java.opts=-Xmx1280m\
    #-D mapreduce.task.io.sort.mb=512 \
    #-D mapreduce.map.sort.spill.percent=0.9 \
    #-D mapreduce.task.io.sort.factor=1000 \
    #-D mapreduce.reduce.memory.mb=1560 \
    #-D mapreduce.reduce.java.opts=-Xmx1048m \
    #-D mapreduce.reduce.shuffle.input.buffer.percent=0.9 \
    #-D mapreduce.map.output.compress=false \
    #-D mapreduce.map.speculative=true \
    #-D mapreduce.reduce.speculative=true \
    #-D mapreduce.reduce.shuffle.memory.limit.percent=1.0\
    #-D mapreduce.input.fileinputformat.split.maxsize=67108864 \
    #-D mapreduce.input.fileinputformat.split.minsize=33554432 \
    #-D mapreduce.job.maps=4 \
    #-D mapreduce.job.reduces=4\
    #-D mapreduce.tasktracker.reduce.tasks.maximum=6 \
    #-D mapreduce.tasktracker.map.tasks.maximum=6 \
    #-D mapreduce.map.output.compress.codec=org.apache.hadoop.io.compress.SnappyCodec \
    #-D mapreduce.reduce.shuffle.parallelcopies=12 \
    #-D mapreduce.task.profile=true \
    #-D mapreduce.task.profile.maps=0 \
    #-D mapreduce.task.profile.reduces=0 \
    #-D mapreduce.task.profile.params="-agentlib:hprof=cpu=samples,heap=sites,depth=6" \
    #-D mapreduce.input.fileinputformat.split.minsize=268435456 \

 #&> $OUTPUT_DIR/log.txt
    #-Dmapreduce.map.memory.mb=2048 \
  #  -D mapreduce.job.reduces = 1 \ 
  #  -D mapreduce.map.output.compress=true \
  #  -D mapreduce.map.output.compress.codec=org.apache.hadoop.io.compress.SnappyCodec \
  #  -D mapreduce.task.io.sort.mb = 512 \
  #  -D mapreduce.reduce.shuffle.parallelcopies = 20 \
     

# Copy the output from HDFS to the local filesystem

hdfs dfs -get $DFS_OUTPUT_DIR/* $OUTPUT_DIR/

# Optionally stop Hadoop services after job completion
#stop-dfs.sh
#stop-yarn.sh
