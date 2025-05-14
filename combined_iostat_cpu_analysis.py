
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

def parse_iostat(file_path, device):
    data = []
    headers = []
    cpu_data = []
    with open(file_path, 'r') as f:
        found_io_headers = False
        found_cpu_headers = False
        for line in f:
            if line.strip() == "" or line.startswith("Linux"):
                continue
            if line.startswith("avg-cpu") and not found_cpu_headers:
                found_cpu_headers = True
                cpu_headers = line.split()[1:]
                continue
            elif found_cpu_headers:
                values = line.split()
                if len(values) == len(cpu_headers):
                    cpu_data.append(dict(zip(cpu_headers, values)))
                found_cpu_headers = False  # Reset after capturing one block
            if line.startswith("Device") and not found_io_headers:
                found_io_headers = True
                headers = line.split()
                continue
            values = line.split()
            if len(values) == len(headers) and values[0] == device:
                device_data = dict(zip(headers, values))
                device_data["source"] = Path(file_path).stem
                data.append(device_data)
    return pd.DataFrame(data), pd.DataFrame(cpu_data)

def preprocess_data_io(df):
    numeric_cols = ["r/s", "w/s", "rkB/s", "wkB/s", "%util", "r_await", "w_await"]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')
    return df

def preprocess_data_cpu(df):
    numeric_cols = ["%user", "%system", "%idle", "%iowait"]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')
    return df

def smooth_data(series, window_size=5):
    return series.rolling(window=window_size, center=True).mean()

def plot_combined_with_cpu(io_dfs, cpu_dfs, device_name):
    fig, axs = plt.subplots(3, 1, figsize=(12, 15), sharex=True)

    for io_df, cpu_df in zip(io_dfs, cpu_dfs):
        label = io_df["source"].iloc[0]
        rkB_s = smooth_data(io_df["rkB/s"])
        wkB_s = smooth_data(io_df["wkB/s"])
        util = smooth_data(io_df["%util"])
        cpu_user = smooth_data(cpu_df["%user"])

        axs[0].plot(rkB_s + wkB_s, label=f"{label} Total IO (rkB/s + wkB/s)")
        axs[1].plot(util, label=f"{label} Disk Utilization (%)")
        axs[2].plot(cpu_user, label=f"{label} CPU %user")

    axs[0].set_title(f"Combined Disk Throughput for Device: {device_name}")
    axs[1].set_title(f"Combined Disk Utilization for Device: {device_name}")
    axs[2].set_title("CPU User Utilization")

    for ax in axs:
        ax.set_ylabel("Metric Value")
        ax.legend()
        ax.grid()

    axs[-1].set_xlabel("Time (seconds)")
    plt.tight_layout()
    plt.savefig(f"combined_disk_cpu_{device_name}.png")
    plt.show()

def analyze_multiple_nodes(file_paths, device_name):
    io_dfs = []
    cpu_dfs = []
    for path in file_paths:
        io_df, cpu_df = parse_iostat(path, device_name)
        io_df = preprocess_data_io(io_df)
        cpu_df = preprocess_data_cpu(cpu_df)
        io_dfs.append(io_df)
        cpu_dfs.append(cpu_df)
    plot_combined_with_cpu(io_dfs, cpu_dfs, device_name)

# Example usage
file_paths = ["./iostat_node1.log", "./iostat_node2.log"]  # Replace with your actual paths
device_name = "sda"  # Or "sda", etc.
analyze_multiple_nodes(file_paths, device_name)
