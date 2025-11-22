iostat -x 1 > iostat_output_mpi2.txt & 
IOSTAT_PID=$!
mpirun -hostfile ./hostfile -np 9 parform ./tests/simple_tests/small_test.frm &> test_mpi.txt
# Kill iostat when done
kill $IOSTAT_PID 2>/dev/null

# Wait for it to finish
wait $IOSTAT_PID 2>/dev/null


echo "Test completed. iostat output saved to iostat_output_mpi2.txt"

iostat -x 1 > iostat_output_reg2.txt & 
IOSTAT_PID=$!
mpirun -hostfile ./hostfile -np 9 parform ./tests/simple_tests/small_test2.frm &> test_reg.txt
# Kill iostat when done
kill $IOSTAT_PID 2>/dev/null

# Wait for it to finish
wait $IOSTAT_PID 2>/dev/null

echo "Test completed. iostat output saved to iostat_output_reg2.txt"