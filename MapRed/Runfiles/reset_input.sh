DFS_INPUT_DIR="/input"

# Remove old input and output directories if they exist
hdfs dfs -rm -skipTrash $DFS_INPUT_DIR/*
