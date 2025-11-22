#!/bin/bash

# Function to monitor swap activity
monitor_swap() {
    local output_file=$1
    vmstat 1 > "$output_file" &
    echo $!
}

# Test 1: MPI with 9 processes
echo "=== Test 1: MPI with 9 processes ==="
echo "Starting iostat and vmstat monitoring..."

iostat -x 1 > iostat_output_mpi_9s.txt & 
IOSTAT_PID=$!

vmstat 1 > vmstat_output_mpi_9s.txt &
VMSTAT_PID=$!

echo "Running parform..."
mpirun -hostfile ./hostfile -np 9 parform ./tests/simple_tests/small_test.frm &> test_mpi_9s_io.txt

# Kill monitoring processes
kill $IOSTAT_PID 2>/dev/null
kill $VMSTAT_PID 2>/dev/null

wait $IOSTAT_PID 2>/dev/null
wait $VMSTAT_PID 2>/dev/null

echo "Test 1 completed."
echo "  iostat output: iostat_output_mpi2.txt"
echo "  vmstat output: vmstat_output_mpi2.txt"
echo ""

# Test 2: MPI with 5 processes
echo "=== Test 2: MPI with 5 processes ==="
echo "Starting iostat and vmstat monitoring..."

iostat -x 1 > iostat_output_reg_5s.txt & 
IOSTAT_PID=$!

vmstat 1 > vmstat_output_reg_5s.txt &
VMSTAT_PID=$!

echo "Running parform..."
mpirun -hostfile ./hostfile -np 5 parform ./tests/simple_tests/small_test2.frm &> test_reg_5s.txt

# Kill monitoring processes
kill $IOSTAT_PID 2>/dev/null
kill $VMSTAT_PID 2>/dev/null

wait $IOSTAT_PID 2>/dev/null
wait $VMSTAT_PID 2>/dev/null

echo "Test 2 completed."
echo "  iostat output: iostat_output_reg_5s.txt"
echo "  vmstat output: vmstat_output_reg_5s.txt"
echo ""

# Summary analysis
echo "=== Swap Analysis ==="
echo "Test 1 swap activity (first 10 lines with si/so):"
grep -v "r  b" vmstat_output_mpi_9s.txt | head -10 | awk '{print "si:", $6, "so:", $7}'

echo ""
echo "Test 2 swap activity (first 10 lines with si/so):"
grep -v "r  b" vmstat_output_reg_5s.txt | head -10 | awk '{print "si:", $6, "so:", $7}'

echo ""
echo "All tests completed!"