# -*- coding: utf-8 -*-
import os
import re
import csv
import sys

def parse_file(file_path):
    total_packet_size = 0
    first_timestamp = None
    last_timestamp = None
    first_packet_size = 0

    with open(file_path, 'r') as file:
        for line in file:
            pattern = r'\[(\d+\.?\d*)us\].*Rx.*PacketSize:\s(\d+)'
            match = re.search(pattern, line)
            if match:
                timestamp = float(match.group(1))
                packet_size = int(match.group(2))

                if first_timestamp is None:
                    first_timestamp = timestamp
                    first_packet_size = packet_size
                last_timestamp = timestamp
                total_packet_size += packet_size

    # Subtract the size of the first packet to align with the time interval (T_last - T_first)
    # which represents the transmission time for (N-1) packets.
    if total_packet_size > 0:
        total_packet_size -= first_packet_size

    return first_timestamp, last_timestamp, total_packet_size

def parse_file_tx(file_path):
    total_packet_size = 0
    first_timestamp = None
    last_timestamp = None
    last_packet_size = 0

    with open(file_path, 'r') as file:
        for line in file:
            pattern = r'\[(\d+\.?\d*)us\].*Tx.*PacketSize:\s(\d+)'
            match = re.search(pattern, line)
            if match:
                timestamp = float(match.group(1))
                packet_size = int(match.group(2))

                if first_timestamp is None:
                    first_timestamp = timestamp
                last_timestamp = timestamp
                last_packet_size = packet_size
                total_packet_size += packet_size

    # Subtract the size of the last packet to align with the time interval (T_last - T_first)
    # Since Tx timestamp is "Start of Transmission", the interval covers P_1 to P_{N-1}.
    if total_packet_size > 0:
        total_packet_size -= last_packet_size

    return first_timestamp, last_timestamp, total_packet_size

def parse_file_name(file_name):
    match = re.search(r'node_(\d+)_port_(\d+)', file_name)
    if match:
        node_num = int(match.group(1))
        port_num = int(match.group(2))
        return node_num, port_num
    else:
        return None

def process_files(directory, output_csv):
    results = []
    typename_tx = "Tx"
    typename_rx = "Rx"
    
    for filename in os.listdir(directory):
        if re.match(r'PortTrace_node_\d+_port_\d+\.tr', filename):
            file_path = os.path.join(directory, filename)
            node_id, port_id = parse_file_name(filename)
            first_timestamp, last_timestamp, total_packet_size = parse_file(file_path)
            if first_timestamp is not None and last_timestamp is not None:
                time_diff = last_timestamp - first_timestamp
                if time_diff > 0:
                    average_rate = round((((total_packet_size*8)/ (1000 ** 3)) / (time_diff/1000000)), 2)
                else:
                    average_rate = 0
                results.append((node_id, port_id, typename_rx, first_timestamp, last_timestamp, total_packet_size, average_rate))
            first_timestamp_tx, last_timestamp_tx, total_packet_size_tx = parse_file_tx(file_path)
            if first_timestamp_tx is not None and last_timestamp_tx is not None:
                time_diff = last_timestamp_tx - first_timestamp_tx
                if time_diff > 0:
                    average_rate = round((((total_packet_size_tx*8)/ (1000 ** 3)) / (time_diff/1000000)), 2)
                else:
                    average_rate = 0
                results.append((node_id, port_id, typename_tx, first_timestamp_tx, last_timestamp_tx, total_packet_size_tx, average_rate))

    # Sort results by nodeId and portId
    results.sort(key=lambda x: (x[0], x[1]))

    with open(output_csv, 'w', newline='') as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(['nodeId', 'portId', 'type', 'startTimestamp (us)', 'completesTimestamp (us)', 'TotalBytes', 'throughput(Gbps)'])
        writer.writerows(results)

    print(f"Stats: Generated throughput metrics for {len(results)} port-directions.")
    if len(results) == 0:
        print("Note: No port throughput data found. This is expected if UB_PORT_TRACE_ENABLE is set to 'false'.")
    else:
        print(f"Saved per-port throughput results to {output_csv}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("python3 cal_throughput.py output_dir")
        sys.exit(1)
    case_dir = sys.argv[1]
    if len(sys.argv[2]) > 0 and sys.argv[2].lower() == 'true' :
        output_dir = os.path.join(case_dir, "test")  # 输入文件夹路径
    else :
        output_dir = os.path.join(case_dir, "output")  # 输入文件夹路径
    os.makedirs(output_dir, exist_ok=True)
    directory = os.path.join(case_dir, "runlog")  # 替换为您的文件夹路径
    if len(sys.argv[2]) > 0 and sys.argv[2].lower() == 'true' :
        output_csv = os.path.join(case_dir, "test", "throughput.csv")
    else :
        output_csv = os.path.join(case_dir, "output", "throughput.csv")
    process_files(directory, output_csv)
