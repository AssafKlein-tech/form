#!/bin/bash

# Number of tests to run
NUM_TESTS=20  # Change this to run more/fewer tests
POWER=${1:-13}

# Create results directory with proper permissions
mkdir -p test_results
chmod 755 test_results

# Loop through test indicess
for idx in $(seq 1 $NUM_TESTS); do
    echo "Running test $idx..."
    
    # Create the output file with proper permissions first
    touch "test_results/test15_${idx}.txt"
    chmod 644 "test_results/test15_${idx}.txt"

    # Run the MPI program and save output
    mpirun -hostfile ./hostfile -np 9 \
        parform ./tests/simple_tests/small_test11.frm\
         &> ./test_results/test15_${idx}.txt
    
    # Check if the run was successful
    if [ $? -eq 0 ]; then
        echo "Test $idx completed successfully"
        rm "test_results/test15_${idx}.txt"
    else
        echo "Test $idx failed with exit code $?"
    fi
done
echo "All tests completed"