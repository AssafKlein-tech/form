#!/bin/bash

# Check if the correct number of arguments is provided
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 filename"
    exit 1
fi

# Assign the input file to a variable
input_file=$1

# Process the file to remove all spaces from each line and store in a temporary file
temp_file=$(mktemp)
sed 's/ //g' "$input_file" > "$temp_file"

# Replace the first * in every line with a space
sed 's/\*/ /' "$temp_file"

# Clean up the temporary file
rm "$temp_file"
