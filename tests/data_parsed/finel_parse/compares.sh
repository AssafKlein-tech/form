#!/bin/bash

# Check if the correct number of arguments is provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 file1 file2"
    exit 1
fi

# Assign the input files to variables
file1=$1
file2=$2
output_file="output.txt"

# Clear the output file if it already exists
> "$output_file"

# Process the first file line by line
while IFS= read -r line1; do
    # Extract the first and second fields from the first file
    field1_file1=$(echo "$line1" | awk '{print $1}')
    field2_file1=$(echo "$line1" | awk '{print $2}')
    
    # Initialize the line with the second field from the first file
    output_line="$field2_file1"

    # Search for a match in the second file
    match=$(grep -m 1 "$field2_file1" "$file2")
    
    if [ -n "$match" ]; then
        # If a match is found, extract the first field from the matching line in the second file
        field1_file2=$(echo "$match" | awk '{print $1}')
        output_line="$output_line - $field1_file2"
    fi
    
    # Add the first field from the first file at the end of the line
    output_line="$output_line - $field1_file1"
    
    # Append the line to the output file
    echo "$output_line" >> "$output_file"
done < "$file1"

# Print the output file
cat "$output_file"
