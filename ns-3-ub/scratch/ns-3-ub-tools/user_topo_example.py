import net_sim_builder as netsim
import networkx as nx

# =============================
# 寻路算法定义
# =============================
def all_simple_paths(G, source, target):
    try:
        # 这里你可以在networkx库中寻找适合的寻路函数。
        # 调用networkx库的all_simple_paths函数，可以获得跳数<=cutoff值的所有不成环路径。
        paths = nx.all_simple_paths(G, source, target, cutoff=2)
    except nx.NetworkXNoPath:
        paths = []
    return paths

def all_shortest_paths(G, source, target):
    try:
        # 这里你可以在networkx库中寻找适合的寻路函数。
        # 调用networkx库的all_shortest_path函数，可以获得所有最短路径。
        paths = nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        paths = []
    return paths

def build_topology():
    graph = netsim.NetworkSimulationGraph()

    host_num = 4
    leaf_sw_num = 2
    spine_sw_num = 2
    host_ids = []
    leaf_ids = []
    spine_ids = []
    
    # =============================
    # 步骤1: 添加网络节点
    # 注意: 必须先添加主机节点，后添加交换机节点，且节点ID必须按顺序编号
    # =============================

    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')
        host_ids.append(host_id)

    for leaf_sw_id in range(leaf_sw_num):
        graph.add_netisim_node(leaf_sw_id + host_num, forward_delay='1ns')
        leaf_ids.append(leaf_sw_id + host_num)

    for spine_sw_id in range(spine_sw_num):
        graph.add_netisim_node(spine_sw_id + host_num + leaf_sw_num, forward_delay='2ns')
        spine_ids.append(spine_sw_id + host_num + leaf_sw_num)

    # =============================
    # 步骤2: 构建网络连接
    # 根据数据中心网络拓扑结构建立连接:
    # 1. 主机连接到叶子交换机
    # 2. 叶子交换机与脊椎交换机全连接
    # Fiber propagation is approximately 2e8 m/s, or 5 ns per meter.
    # A 20m one-way fiber therefore corresponds to about 100ns; set each
    # link delay from the physical topology when the length is known.
    # =============================
    for host_id in range(host_num):
        host_id = host_id
        leaf_id = leaf_ids[host_id//leaf_sw_num]
        graph.add_netisim_edge(host_id, leaf_id, bandwidth='400Gbps', delay='21ns', edge_count=1)

    for spine_id in spine_ids:
        for leaf_id in leaf_ids:
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth='400Gbps', delay='22ns', edge_count=1)

    return graph


def main():
    graph = build_topology()

    # =============================
    # 步骤3: 生成仿真配置文件
    # =============================
    # step3.1: build_graph_config会生成一系列中间数据
    graph.build_graph_config()
    # step3.2: 默认生成通用压缩路由表
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    # step3.3: 使用 on-demand TP，避免预生成 host-pair transport_channel.csv
    graph.write_config(include_transport=False)


if __name__ == '__main__':
    main()
