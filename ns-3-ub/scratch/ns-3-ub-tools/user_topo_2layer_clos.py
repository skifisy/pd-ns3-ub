import net_sim_builder as netsim
import networkx as nx


def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []

def build_topology():
    graph = netsim.NetworkSimulationGraph()

    host_num = 512
    leaf_sw_num = 16
    spine_sw_num = host_num // leaf_sw_num

    total_node_num = host_num + leaf_sw_num + spine_sw_num
    host_ids = []
    leaf_ids = []
    spine_ids = []

    # step1: 生成节点,要求host在前,node在后,且顺序编号,不符合要求会报错
    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')
        host_ids.append(host_id)

    for leaf_sw_id in range(leaf_sw_num):
        graph.add_netisim_node(leaf_sw_id + host_num, forward_delay='1ns')
        leaf_ids.append(leaf_sw_id + host_num)

    for spine_sw_id in range(spine_sw_num):
        graph.add_netisim_node(spine_sw_id + host_num + leaf_sw_num, forward_delay='1ns')
        spine_ids.append(spine_sw_id + host_num + leaf_sw_num)


    # step2: 生成拓扑连线,按照自定义拓扑,将指定id的节点(host或node皆可)进行连线并指定带宽和时延(发包器会自动连接)
    host_per_leaf = host_num // leaf_sw_num
    for host_id in range(host_num):
        host_id = host_id
        leaf_id = leaf_ids[host_id // host_per_leaf]
        graph.add_netisim_edge(host_id, leaf_id, bandwidth='400Gbps', delay='20ns', edge_count=1)


    for leaf_idx, leaf_id in enumerate(leaf_ids):
        for spine_idx, spine_id in enumerate(spine_ids):
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth='400Gbps', delay='20ns', edge_count=1)

    return graph


def main():
    graph = build_topology()


    # step3: 生成配置文件
    # step3.1: build_graph_config会生成一系列中间数据
    graph.build_graph_config()
    # step3.2: 默认生成通用压缩路由表
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    # step3.3: 使用 on-demand TP，避免预生成 host-pair transport_channel.csv
    graph.write_config(include_transport=False)


if __name__ == '__main__':
    main()
