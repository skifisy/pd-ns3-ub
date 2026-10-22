import argparse
import datetime
import os
import shutil
from pathlib import Path

from algorithms.registry import get_legacy_algorithm, legacy_algorithm_names
from algorithms.writer import write_schedule_csv


def generate_all_rank_table(total_host, n_comm_size, rank_mapping='linear'):
    """
    Generate rank tables for all communication domains.
    
    Args:
        total_host: Total number of hosts
        n_comm_size: Communication domain size (hosts per domain)
        rank_mapping: Strategy for assigning ranks to domains
            - 'linear': [0,1,2,3][4,5,6,7]... (default)
            - 'round-robin': [0,2,4,6][1,3,5,7]...
    """
    if total_host % n_comm_size != 0:
        raise ValueError("total_host % n_comm_size != 0")
    n_comm_num = total_host // n_comm_size
    all_rank_table = []
    
    if rank_mapping == 'linear':
        # Linear: consecutive ranks in each domain [0,1,2,3][4,5,6,7]
        for comm_id in range(n_comm_num):
            rt = list(range(comm_id * n_comm_size, (comm_id + 1) * n_comm_size))
            all_rank_table.append(rt)
    elif rank_mapping == 'round-robin':
        # Round-robin: stride across all ranks [0,2,4,6][1,3,5,7]
        for comm_id in range(n_comm_num):
            rt = [comm_id + i * n_comm_num for i in range(n_comm_size)]
            all_rank_table.append(rt)
    else:
        raise ValueError(f"Unknown rank_mapping strategy: {rank_mapping}. Choose 'linear' or 'round-robin'")
    
    return all_rank_table


def parse_size(size_str):
    """
    解析带有单位的字节数字符串。

    参数:
    - size_str: 字节数字符串，可带有单位（如B、KB、MB、GB）

    返回:
    - size: 以字节为单位的整数

    抛出:
    - ValueError: 如果输入格式不正确
    """
    size_str = size_str.upper()
    if size_str.endswith('GB'):
        size = int(size_str[:-2]) * 1024 * 1024 * 1024
    elif size_str.endswith('MB'):
        size = int(size_str[:-2]) * 1024 * 1024
    elif size_str.endswith('KB'):
        size = int(size_str[:-2]) * 1024
    elif size_str.endswith('B'):
        size = int(size_str[:-1])
    elif size_str.isdigit():
        size = int(size_str)
    else:
        raise ValueError("无效的字节数格式，请使用 B、KB、MB 或 GB 作为单位")
    return size

def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description="XCCL RDMA Maker: Generate traffic.csv from collective algorithms",
        epilog="Examples:\n"
               "  %(prog)s -n 8 -c 4 -b 1MB -a ar_ring\n"
               "  %(prog)s -n 16 -c 8 -b 512KB -a ar_nhr\n"
             "  %(prog)s -n 4 -c 4 -b 256KB -a ar_rhd\n"
             "  %(prog)s -n 8 -c 8 -b 1MB -a a2a_pairwise",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('-n', '--host-count', dest='host_count', type=int, required=True,
                        help='Total number of hosts')
    parser.add_argument('-c', '--comm-domain-size', dest='comm_size', type=int, required=True,
                        help='Communication domain size (hosts per domain)')
    parser.add_argument('-b', '--data-size', dest='comm_byte', type=str, required=True,
                        help='Per-participant total data volume for one collective (B/KB/MB/GB); algo will slice/partition per phase internally')
    parser.add_argument('-a', '--algo', type=str, required=True, choices=legacy_algorithm_names(),
                        help='Collective communication algorithm')
    parser.add_argument('-d', '--phase-delay', dest='phase_delay', type=int, default=0,
                        help='Inter-phase delay between phases (ns)')
    parser.add_argument('-o', '--output-dir', dest='output_dir', type=str, default='./output',
                        help='Output root directory (default: ./output)')
    parser.add_argument('--rank-mapping', dest='rank_mapping', type=str, default='linear',
                        choices=['linear', 'round-robin'],
                        help='Strategy for assigning ranks to communication domains (default: linear)')
    parser.add_argument('--peers-per-phase', type=int,
                        help='Pairwise AllToAll peers grouped into one phase')

    return parser.parse_args()

if __name__ == "__main__":
    args = parse_args()

    total_host = args.host_count
    comm_size = args.comm_size
    comm_byte = parse_size(args.comm_byte)
    algo = args.algo
    phase_delay = args.phase_delay
    rank_mapping = args.rank_mapping

    folder_name = f"{total_host}_{comm_size}_{comm_byte}_{algo}_d{phase_delay}_{rank_mapping}"
    timestamp = datetime.datetime.now().strftime("%Y%m%d%H%M%S")
    output_dir = os.path.join(args.output_dir, f"{timestamp}_{folder_name}")

    all_rank_table = generate_all_rank_table(total_host, comm_size, rank_mapping)
    print(f"rank_table: {all_rank_table}")

    algorithm = get_legacy_algorithm(algo)
    builder_options = dict(algorithm.legacy_options)
    if args.peers_per_phase is not None:
        if algo != 'a2a_pairwise':
            raise ValueError("--peers-per-phase is only valid with a2a_pairwise")
        builder_options['peers_per_phase'] = args.peers_per_phase
    schedule = algorithm.build(int(comm_size), int(comm_byte), **builder_options)
    print(f"phases: {len(schedule.phases)}")

    if os.path.exists(output_dir):
        shutil.rmtree(output_dir)
    write_schedule_csv(
        Path(output_dir) / 'traffic.csv',
        all_rank_table,
        schedule,
        phase_delay=f"{phase_delay}ns",
        include_inactive=True,
        delay_first_phase=True,
    )
    print(f"traffic.csv written to: {output_dir}/traffic.csv")
