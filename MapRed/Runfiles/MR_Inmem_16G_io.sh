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

cd /home/assaf/Repos/form

# Start HDFS and YARN (if not already running)
stop-dfs.sh
stop-yarn.sh
start-dfs.sh
start-yarn.sh

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
hdfs dfs -mkdir -p $DFS_INPUT_DIR

# Upload local files to the HDFS input directory
hdfs dfs -put $INPUT_FILE $DFS_INPUT_DIR

# Remove local output directory if it exists
if [ -d "$OUTPUT_DIR" ]; then
    rm -r "$OUTPUT_DIR"
fi

mkdir -p $OUTPUT_DIR

iostat -x 1 > iostat_hadoop.txt & 
# Run the WordCount job
time hadoop jar /home/assaf/Repos/form/MapRed/Hadoop/fraction_num_term/ComplexTermProcessing.jar ComplexTermFractionDriver \
    -D mapreduce.task.io.file.buffer.size=524288  \
    -D mapreduce.map.memory.mb=1536 \
    -D mapreduce.map.java.opts=-Xmx1024m\
    -D mapreduce.task.io.sort.mb=576 \
    -D mapreduce.map.sort.spill.percent=0.95 \
    -D mapreduce.task.io.sort.factor=100 \
    -D mapreduce.reduce.memory.mb=2400 \
    -D mapreduce.reduce.java.opts=-Xmx1736m \
    -D mapreduce.map.output.compress=true \
    -D mapreduce.map.output.compress.codec=org.apache.hadoop.io.compress.SnappyCodec \
    -D mapreduce.reduce.shuffle.input.buffer.percent=0.9 \
    -D mapreduce.reduce.shuffle.merge.percent=0.8\
    -D mapred.job.reduce.input.buffer.percent=0.75\
    -D mapreduce.map.speculative=false \
    -D mapreduce.job.reduces=8\
    -D mapreduce.reduce.speculative=false \
    -D mapreduce.input.fileinputformat.split.minsize=536870912\
    $DFS_INPUT_DIR $DFS_OUTPUT_DIR
 #&> $OUTPUT_DIR/log.txt
    #-Dmapreduce.map.memory.mb=2048 \
  #  -D mapreduce.job.reduces = 1 \ 
  #  -D mapreduce.map.output.compress=true \
  #  -D mapreduce.map.output.compress.codec=org.apache.hadoop.io.compress.SnappyCodec \
  #  -D mapreduce.task.io.sort.mb = 512 \
     

# Copy the output from HDFS to the local filesystem

#hdfs dfs -get $DFS_OUTPUT_DIR/* $OUTPUT_DIR/

# Optionally stop Hadoop services after job completion
#stop-dfs.sh
#stop-yarn.sh
