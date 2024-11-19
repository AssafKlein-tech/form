#!/bin/bash

echo "Written by rkendel, start running"

# Function to process the file or input
process_input() {
    path="./projecctA/before_data_with_del"
    worker_nums="$1"
    curr_worker_num=0

    # Ensure the directory exists
    mkdir -p "$path"

    # Initialize an associative array to buffer output for each worker
    declare -A worker_buffers

    # Initialize empty worker buffers
    for ((i = 1; i <= worker_nums; i++)); do
        worker_buffers["worker_$i"]=""
    done

    while IFS= read -r line; do
        # Check if it is a "Time =" print
        if echo "$line" | grep -q "Time ="; then
            curr_worker_num=0
        fi

        # Check if it is a new worker print
        if echo "$line" | grep -q "rkendel"; then
            curr_worker_num=$(echo "$line" | awk -F'[:(]' '{print $2}' | grep -o '[0-9]\+')
        fi

        # Check if the current worker number exceeds the declared number
        if [ "$curr_worker_num" -gt "$worker_nums" ]; then
            echo "More workers than declared"
            exit 1
        fi

        worker_name="worker_$curr_worker_num"
        worker_buffers["$worker_name"]+="$line"$'\n'

        # Remove the first line from the file
        sed -i '1d' "$input_file"
    done

    # Write buffered output to files
    for worker in "${!worker_buffers[@]}"; do
        echo -n "${worker_buffers[$worker]}" > "$path/$worker.txt"
    done
}

# Check if the number of workers is provided as an argument
if [ -z "$1" ]; then
    echo "Usage: $0 workers_num"
    exit 1
fi

# Process the input
process_input "$1"
