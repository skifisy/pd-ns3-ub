import datetime
import os
from random import sample
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import argparse
from pathlib import Path

from algorithms.all_to_all.pairwisev import build_a2a_pairwisev_from_byte_matrix
from algorithms.writer import write_schedule_csv


def rdma_writer(phase_send_list, args):
    byte_matrix = [
        [0 for _ in range(args.all_host_num)]
        for _ in range(args.all_host_num)
    ]
    for send_dict in phase_send_list:
        for sender, recvers in send_dict.items():
            assert sender < 320
            for recver in recvers:
                assert recver < 320
                byte_matrix[sender][recver] += args.token_size
    schedule = build_a2a_pairwisev_from_byte_matrix(byte_matrix)
    write_schedule_csv(
        Path(args.output_dir) / "traffic.csv",
        [list(range(args.all_host_num))],
        schedule,
        phase_delay="10ns",
    )


def plot_heatmap(matrix, args):
    plt.figure(figsize=(10, 8))
    sns.heatmap(matrix, annot=False, fmt="d", cmap="YlGnBu")
    plt.title("Traffic Matrix Heatmap")
    plt.xlabel("Receiver Host ID")
    plt.ylabel("Sender Host ID")
    # plt.show()
    plt.savefig(os.path.join(args.output_dir, "traffic_matrix.png"))


def save_matrix_to_csv(matrix, args, filename="traffic_matrix.csv"):
    df = pd.DataFrame(matrix)
    df.to_csv(os.path.join(args.output_dir, filename), index=False, header=False)


def route_expert( route_expert_host_ids, topK_route):
    return sample(route_expert_host_ids, topK_route)


def route_shared_expert(shared_expert_host_ids, topK_shared):
    return sample(shared_expert_host_ids, topK_shared)


def alltoallv_random(args):
    phase_send_dict = []  # phase_send_dict[phase_id][sender_id] = [recver_ids]
    # 假设初始状态，一张卡处理一个bs的token；bs中的一个token路由到1个共享专家 + 8个路由专家
    for phase in range(1):
        traffic_matrix = [[0 for _ in range(args.all_host_num)] for _ in range(args.all_host_num)]
        send_dict = {}
        for host_id in range(args.all_host_num):
            send_list_t = []  # host_id将要发送给send_list中所有host，可能出现重复的目的host，此时应该将发送数据量x2
            for token_id in range(args.batch_size):
                # 该token路由到一个共享专家上
                send_list_t += route_shared_expert(args.shared_expert_host_ids, args.topK_shared)
                # 该token路由到topK_route个路由专家上
                send_list_t += route_expert(args.route_expert_host_ids, args.topK_route)

            # 统计信息
            for recver in send_list_t:
                traffic_matrix[host_id][recver] += 1
            # 记录信息
            send_dict[host_id] = send_list_t.copy()

        plot_heatmap(traffic_matrix, args)
        save_matrix_to_csv(traffic_matrix, args)
        phase_send_dict.append(send_dict)

    rdma_writer(phase_send_dict, args)


def alltoallv_allofyou(args):
    # 所有节点发送1个token给其它所有节点
    token_sent = 2
    phase_send_dict = []  # phase_send_dict[phase_id][sender_id] = [recver_ids]
    # 假设初始状态，一张卡处理一个bs的token；bs中的一个token路由到1个共享专家 + 8个路由专家
    for phase in range(1):
        traffic_matrix = [[0 for _ in range(args.all_host_num)] for _ in range(args.all_host_num)]
        send_dict = {}
        for host_id in range(args.all_host_num):
            send_list_t = []
            for _ in range(token_sent):
                send_list_t += args.all_host_ids  # host_id将要发送给send_list中所有host，可能出现重复的目的host，此时应该将发送数据量x2
            # 统计信息
            for recver in send_list_t:
                traffic_matrix[host_id][recver] += 1
            # 记录信息
            send_dict[host_id] = send_list_t.copy()

        plot_heatmap(traffic_matrix, args)
        save_matrix_to_csv(traffic_matrix, args)
        phase_send_dict.append(send_dict)

    rdma_writer(phase_send_dict, args)

def parse_args():
    """
    解析命令行参数。
    """
    parser = argparse.ArgumentParser(description="all2allv RDMA Maker")
    parser.add_argument('-bs', '--batch_size', type=int, required=False, help='batch_size')
    parser.add_argument('-ts', '--token_size', type=int, required=False, help='token_size')
    parser.add_argument('-ks', '--topK_shared', type=int, required=False, help='topK_shared')
    parser.add_argument('-kr', '--topK_route', type=int, required=False, help='topK_route')
    parser.add_argument('-ren', '--route_expert_num', type=int, required=False, help='route_expert_num')
    parser.add_argument('-sen', '--shared_expert_num', type=int, required=False, help='shared_expert_num')
    parser.add_argument('-rehn', '--route_expert_host_num', type=int, required=False, help='route_expert_host_num')
    parser.add_argument('-sehn', '--shared_expert_host_num', type=int, required=False, help='shared_expert_host_num')
    available_algo = ['all2allv', 'all2allv_random']
    parser.add_argument('-a', '--algo', type=str, required=False, choices=available_algo, help='算法类型')

    # 生成默认输出目录名称
    timestamp = datetime.datetime.now().strftime("%Y%m%d%H%M%S")
    default_output_dir = os.path.join("output", timestamp)
    os.makedirs(default_output_dir, exist_ok=True)
    parser.add_argument('-o', '--output_dir', type=str, default=default_output_dir, help='输出目录')

    return parser.parse_args()

if __name__ == "__main__":

    args = parse_args()
    args.batch_size = 32
    args.token_size = 7 * 1024
    args.topK_shared = 1
    args.topK_route = 8
    args.route_expert_num = 256
    args.shared_expert_num = 1
    args.route_expert_host_num = 280  # 256 experts in route_expert_host_num npus
    args.shared_expert_host_num = 40  # 1 shared experts in shared_expert_host_num npus
    args.all_host_num = args.route_expert_host_num + args.shared_expert_host_num
    args.algo = 'all2allv'

    os.makedirs(args.output_dir, exist_ok=True)

    args.all_host_ids = [i for i in range(args.route_expert_host_num + args.shared_expert_host_num)]
    args.route_expert_host_ids = []
    args.shared_expert_host_ids = []
    for id in args.all_host_ids:
        if id % 8 == 0:
            args.shared_expert_host_ids.append(id)
        else:
            args.route_expert_host_ids.append(id)

    if args.algo == 'all2allv':
        alltoallv_allofyou(args)
    if args.algo == 'all2allv_random':
        alltoallv_random(args)
