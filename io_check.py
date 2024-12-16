import os
import time

# Path for testing
file_path = "test_file.txt"
file_size = 1024 * 1024  # 1 MB

# Write test
write_data = b"x" * file_size

write_time = 0 
read_time = 0

for i in range(10):
    start_write = time.time()
    with open(file_path, "wb") as f:
        f.write(write_data)
        f.flush()  # Flush the OS cache for this file
        os.fsync(f.fileno())  # Force write to disk
    end_write = time.time()

    # Read test
    start_read = time.time()
    with open(file_path, "rb") as f:
        read_data = f.read()
    end_read = time.time()

    # Ensure data integrity
    assert write_data == read_data, "Read data does not match written data!"
    write_time += end_write - start_write
    read_time += end_read - start_read
    os.remove(file_path)

# Output timings
print(f"Write latency: {write_time/10:.6f} seconds")
print(f"Read latency: {read_time/10:.6f} seconds")

# Cleanup

