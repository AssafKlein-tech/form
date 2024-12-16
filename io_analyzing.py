import pandas as pd
import matplotlib.pyplot as plt

# Step 1: Load the iostat file
def parse_iostat(file_path,device):
    data = []
    headers = []
    current_device = None

    with open(file_path, 'r') as f:
        found_headers=False
        for line in f:
            # Skip empty or irrelevant lines
            if line.strip() == "" or line.startswith("Linux"):
                continue

            # Capture headers (starts with "Device")
            if line.startswith("Device") and not found_headers:
                found_headers = True
                headers = line.split()
                continue

            # Capture device data
            values = line.split()
            if len(values) == len(headers) and values[0] == device:  # Ensure data matches header length
                device_data = dict(zip(headers, values))
                data.append(device_data)

    return pd.DataFrame(data)

def parse_cpustat(file_path):
    data = []
    headers = []
    cpu_data = []

    with open(file_path, 'r') as f:
        found_headers=False
        for line in f:
            # Skip empty or irrelevant lines
            if line.strip() == "" or line.startswith("Linux"):
                continue

            # Capture headers (starts with "Device")
            if line.startswith("avg-cpu") and not found_headers:
                found_headers = True
                headers = line.split()[1:]
                continue

            # Capture device data
            values = line.split()
            if len(values) == len(headers):  # Ensure data matches header length
                cpu_data = dict(zip(headers, values))
                data.append(cpu_data)

    return pd.DataFrame(data)

# Step 2: Convert metrics to numeric for analysis
def preprocess_data_cpu(df):
    numeric_cols = ["%user", "%iowait", "%idle", "%system"]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')
    return df

def preprocess_data_io(df):
    numeric_cols = ["r/s", "w/s", "rkB/s", "wkB/s", "%util","r_await","w_await"]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')
    return df

# Moving average function for smoothing
def smooth_data(series, window_size=5):
    return series.rolling(window=window_size, center=True).mean()

# Step 3: Plot key metrics
def plot_metrics(io_df, cpu_df, device_name, show):
    device_data = io_df[io_df["Device"] == device_name]
    
    smooth_window = 3
    
    device_data["rkB/s_smooth"] = smooth_data(device_data["rkB/s"], smooth_window)
    device_data["rwait_smooth"] = smooth_data(device_data["r_await"], smooth_window)
    device_data["wwait_smooth"] = smooth_data(device_data["w_await"], smooth_window)
    device_data["wkB/s_smooth"] = smooth_data(device_data["wkB/s"], smooth_window)
    device_data["%util_smooth"] = smooth_data(device_data["%util"], smooth_window)
    cpu_df["%user_smooth"] = smooth_data(cpu_df["%user"], smooth_window)
    
#    
#    # Plot Read/Write Throughput
#    plt.figure(figsize=(10, 5))
#    plt.plot(device_data["wkB/s_smooth"], color="#f39c12",label="Write (wkB/s)")
#    plt.plot(device_data["rkB/s_smooth"], color="#2874a6", label="Read (rkB/s)")
#    plt.title(f"Disk R/W Throughput")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Throughput (kB/s)")
#    plt.legend()
#    plt.grid()
#    plt.savefig("./tests/analysis/form_r_w.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
        
     # Plot Read/Write Throughput
    plt.figure(figsize=(10, 5))
    #plt.plot(device_data["wwait_smooth"], color="#f39c12",label="Write wait sec")
    plt.plot(device_data["rwait_smooth"], color="#2874a6", label="Read Req Time (milisec)")
    plt.title(f"Time For Read Request To Complete")
    plt.xlabel("Time (sec)")
    plt.ylabel("Time To Complete (milisec)")
    plt.legend()
    plt.grid()
    plt.savefig("./tests/analysis/hadoop_r_lat.png")
    plt.show()
#    
#    
#    #Plot Write Throughput
#    plt.figure(figsize=(10, 5))
#    plt.plot(device_data["wkB/s_smooth"], color="#f39c12",label="Write (wkB/s)")
#    plt.title(f"Disk Write Throughput")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Throughput (kB/s)")
#    plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_w.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#    
#    #Plot Read Throuput
#    plt.figure(figsize=(10, 5))
#    plt.plot(device_data["rkB/s_smooth"], color="#2874a6", label="Read (rkB/s)")
#    plt.title(f"Disk Read Throughput")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Throughput (kB/s)")
#    plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_r.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#        
#    #Plot Total Throughput
#    plt.figure(figsize=(10, 5))
#    plt.plot(device_data["rkB/s_smooth"] + device_data["wkB/s_smooth"], color="#239b56",label="IO (kB/s)")
#    plt.title(f"Total Disk Throughput")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Throughput (kB/s)")
#    plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_tot.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#        
#    smooth_window = 20
#    
#    device_data["rkB/s_smooth"] = smooth_data(device_data["rkB/s"], smooth_window)
#    device_data["wkB/s_smooth"] = smooth_data(device_data["wkB/s"], smooth_window)
#    device_data["%util_smooth"] = smooth_data(device_data["%util"], smooth_window)
#    cpu_df["%user_smooth"] = smooth_data(cpu_df["%user"], smooth_window)
#
#    #Print IO Throuput VS Compute
#    fig, ax1 = plt.subplots()
#    fig.set_size_inches(10,5)
#    ax2 = ax1.twinx()
#    line1,  =ax1.plot(device_data["rkB/s_smooth"] + device_data["wkB/s_smooth"], color="#239b56",label="IO (kB/s)")
#    line2,  =ax2.plot(cpu_df["%user_smooth"], color="#ca6f1e", label="CPU utilization (%)")
#    plt.title(f"Total Disk Throughput")
#    ax1.set_xlabel("Time (sec)")
#    ax1.set_ylabel('Throughput (kB/s) ', color='#239b56')
#    ax2.set_ylabel('CPU Utilization (%)', color="#ca6f1e")
#
#    # Combine legends from both axes
#    lines = [line1, line2]
#    labels = [line.get_label() for line in lines]
#    ax1.legend(lines, labels, loc='upper right')
#    #plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_compare.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#    
#    
#    # Plot Utilization
#    plt.figure(figsize=(10, 5))
#    plt.plot(cpu_df["%user_smooth"], color="#ca6f1e", label="CPU utilization (%)")
#    plt.title(f"Disk Utilization")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Utilization (%)")
#    plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_cpu.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#
#    # Plot Utilization
#    plt.figure(figsize=(10, 5))
#    plt.plot(device_data["%util_smooth"], label="Disk Utilization (%)", color="#e74c3c")
#    plt.title(f"Disk Utilization")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Utilization (%)")
#    plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_util.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#        
#    # Plot Utilization
#    plt.figure(figsize=(10, 5))
#    plt.plot(device_data["%util_smooth"], label="Disk Utilization (%)", color="#239b56")
#    plt.plot(cpu_df["%user_smooth"], color="#ca6f1e", label="CPU Utilization (%)")
#    plt.title(f"Disk VS CPU Utilization")
#    plt.xlabel("Time (sec)")
#    plt.ylabel("Utilization (%)")
#    plt.legend()
#    plt.grid()
#    plt.savefig(r"./tests/analysis/hadoop_compare2.png")
#    if show:
#        plt.show()
#    else:
#        plt.close()
#

# Step 4: Summarize Statistics
def summarize_stats(df, device_name):
    device_data = df[df["Device"] == device_name]
    summary = device_data.describe()
    print(f"Summary Statistics for {device_name}:\n{summary}")

# Main Function to Execute Analysis
def analyze_iostat(file_path, device_name, show):
    io_df = parse_iostat(file_path, device_name)
    cpu_df = parse_cpustat(file_path)
    io_df = io_df[:760]
    cpu_df = cpu_df[:760]
    io_df = preprocess_data_io(io_df)
    cpu_df = preprocess_data_cpu(cpu_df)
    summarize_stats(io_df, device_name)
    plot_metrics(io_df,cpu_df, device_name, show)
    

# Replace 'iostat_output.txt' with your file path
#file_path = "./tests/No_combining/iostat_output.txt"
file_path = "./iostat_hadoop2.txt"
show = False
device_name = "sdc"  # Replace with your actual device name
analyze_iostat(file_path, device_name,show)
