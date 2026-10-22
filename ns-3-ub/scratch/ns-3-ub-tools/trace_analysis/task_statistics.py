# -*- coding: utf-8 -*-
import os
import csv
from glob import glob
import re
import sys

# 定义字段提取函数
def extract_task_fields(line):
    match = re.match(r'\[(.*?)\] (.*?),.*?taskId: (\d+)', line)
    if match:
        timestamp = match.group(1)[:-2]
        event = match.group(2)
        task_id = match.group(3)
        if "WQE Starts" in event:
            return {'task_id': task_id, 'timestamp': timestamp, 'event': 'Starts'}
        elif "WQE Completes" in event:
            return {'task_id': task_id, 'timestamp': timestamp, 'event': 'Completes'}
        elif "MEM Task Starts" in event:
            return {'task_id': task_id, 'timestamp': timestamp, 'event': 'Starts'}
        elif "MEM Task Completes" in event:
            return {'task_id': task_id, 'timestamp': timestamp, 'event': 'Completes'}
    return None

def extract_packet_fields(line):
    match = re.match(r'\[(.*?)\] (.*?),.*?taskId: (\d+)', line)
    if match:
        timestamp = match.group(1)[:-2]
        event = match.group(2)
        task_id = match.group(3)
        if "First Packet Sends" in event or "Last Packet ACKs" in event:
            return {'task_id': task_id, 'timestamp': timestamp, 'event': event}
    return None

def filterRedundantData(data):
    # 初始化结果字典
    result = {}
    # 遍历每个任务ID及其对应的事件列表
    for task_id, events in data.items():
        first_packet_sends = None
        last_packet_acks = None

        # 遍历每个事件
        for event in events:
            if event['event'] == 'First Packet Sends':
                timestamp = float(event['timestamp'])
                if first_packet_sends is None or timestamp < first_packet_sends:
                    first_packet_sends = timestamp
            elif event['event'] == 'Last Packet ACKs':
                timestamp = float(event['timestamp'])
                if last_packet_acks is None or timestamp > last_packet_acks:
                    last_packet_acks = timestamp

        # 将结果存储在结果字典中
        result[task_id] = {
            'First Packet Sends': first_packet_sends,
            'Last Packet ACKs': last_packet_acks
        }
    return result

def process_files_to_csv(input_folder,csv_file, output_csv, pattern="*.tr"):
    # 获取所有匹配的文件路径
    file_paths = glob(os.path.join(input_folder, pattern))
    task_data = {}
    packet_data = {}
    # 遍历处理每个文件
    for file_path in file_paths:
        try:
            filename = os.path.basename(file_path)

            if "PacketTrace_node_" in filename:
                # 读取PacketTrace文件
                with open(file_path, 'r') as f:
                    for line in f:
                        data = extract_packet_fields(line)
                        if data:
                            task_id = data['task_id']
                            if task_id not in packet_data:
                                packet_data[task_id] = []
                            packet_data[task_id].append(data)

            elif "TaskTrace_node_" in filename:
                # 读取TaskTrace文件           
                with open(file_path, 'r') as f:
                    for line in f:
                        data = extract_task_fields(line)
                        if data:
                            task_id = data['task_id']
                            if task_id not in task_data:
                                task_data[task_id] = []
                            task_data[task_id].append(data)
    
            #else :
                #print("不解析该文件：",filename)

        except Exception as e:
            print(f"处理文件 {file_path} 时出错: {e}")

    #print("packet_data:",packet_data)

    filterData = filterRedundantData(packet_data)

    # # 匹配数据并生成CSV
    merged_data = []
    for task_id in task_data:
        task_Data = task_data[task_id]
        packe_Data= filterData.get(task_id, {'First Packet Sends': None, 'Last Packet ACKs': None})
        # 使用字典推导式获取特定事件的timestamp
        timestamps = {
            item['event']: item['timestamp']
            for item in task_Data
            if item['event'] in ['Starts', 'Completes']
        }

        merged_entry = {
            'Task ID': task_id,
            'Task Start Time': timestamps.get('Starts', None),
            'Task End Time': timestamps.get('Completes', None),
            'First Packet Sends':packe_Data['First Packet Sends'],
            'Last Packet ACKs':packe_Data['Last Packet ACKs']
        }
        merged_data.append(merged_entry)

    with open(csv_file, newline='') as f:
        reader = csv.DictReader(f)
        fieldnames = list(reader.fieldnames or [])
        rows = list(reader)

    added_columns = [
        'taskStartTime(us)',
        'taskCompletesTime(us)',
        'firstPacketSends(us)',
        'lastPacketACKs(us)',
        'taskThroughput(Gbps)',
    ]
    for column in added_columns:
        if column not in fieldnames:
            fieldnames.append(column)

    rows_by_task = {}
    for row in rows:
        try:
            rows_by_task[int(row['taskId'])] = row
        except (KeyError, ValueError):
            continue

    # 遍历任务详细信息列表
    missing_packet_info_count = 0
    total_tasks = len(merged_data)
    
    for task in merged_data:
        task_id = int(task['Task ID'])
        task_start_time = float(task['Task Start Time'])
        task_end_time = float(task['Task End Time'])
        
        if task['First Packet Sends'] is None:
            first_packet_sends = 0.0
            missing_packet_info_count += 1
        else:
            first_packet_sends = float(task['First Packet Sends'])

        if task['Last Packet ACKs'] is None:
            last_packet_ACKs = 0.0
        else:
            last_packet_ACKs = float(task['Last Packet ACKs'])

        row = rows_by_task.get(task_id)
        if row is None:
            continue
        duration_us = task_end_time - task_start_time
        data_size = int(row['dataSize(Byte)'])
        task_throughput = round((data_size * 8 / 1000) / duration_us, 4) if duration_us > 0 else 0

        row['taskStartTime(us)'] = task_start_time
        row['taskCompletesTime(us)'] = task_end_time
        row['firstPacketSends(us)'] = first_packet_sends
        row['lastPacketACKs(us)'] = last_packet_ACKs
        row['taskThroughput(Gbps)'] = task_throughput
    
    # 保存修改后的DataFrame到新的CSV文件
    with open(output_csv, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    
    print(f"Stats: Processed {total_tasks} tasks.")
    if missing_packet_info_count > 0:
        print(f"Note: {missing_packet_info_count}/{total_tasks} tasks missing packet timestamps (First Packet Sends / Last Packet ACKs).")
        print("      This is expected if UB_PACKET_TRACE_ENABLE is set to 'false'.")
    else:
        print("Stats: All tasks have complete packet timestamp info.")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("python3 task_statistics.py traffic.csv")
        sys.exit(1)

    case_dir = sys.argv[1]
    input_dir = os.path.join(case_dir, "runlog")  # 输入文件夹路径
    if len(sys.argv[2]) > 0 and sys.argv[2].lower() == 'true' :
        output_file = os.path.join(case_dir, "test", "task_statistics.csv")  # 输出CSV路径
    else:
        output_file = os.path.join(case_dir, "output", "task_statistics.csv")  # 输出CSV路径
    traffic_file = os.path.join(case_dir, "traffic.csv")
    # 创建输入文件夹（如果不存在）
    os.makedirs(input_dir, exist_ok=True)
    if len(sys.argv[2]) > 0 and sys.argv[2].lower() == 'true' :
        output_dir = os.path.join(case_dir, "test")  # 输入文件夹路径
    else:
        output_dir = os.path.join(case_dir, "output")  # 输入文件夹路径
    os.makedirs(output_dir, exist_ok=True)
    
    process_files_to_csv(input_dir, traffic_file,output_file)
    print(f"Saved traffic tasks throughput results to {output_file}")
