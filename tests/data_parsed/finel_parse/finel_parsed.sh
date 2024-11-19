#!/bin/bash

# Check if the correct number of arguments is provided
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 filename"
    exit 1
fi

# Assign the input file to a variable
input_file=$1

# Process the file to remove all * and ( characters from each line
sed 's/[\*\(\)]//g' "$input_file" | sed 's/\[//g' | sed 's/\]//g'
