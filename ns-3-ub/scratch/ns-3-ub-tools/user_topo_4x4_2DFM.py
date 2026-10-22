import net_sim_builder as netsim
import networkx as nx


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

def build_topology(print_groups=False):
    graph = netsim.NetworkSimulationGraph()

    row_num = 4
    col_num = 4
    
    host_num = row_num * col_num

    total_node_num = host_num
    host_ids = []
    leaf_ids = []
    spine_ids = []
    # step1: 生成节点,要求host在前,node在后,且顺序编号,不符合要求会报错
    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')
        host_ids.append(host_id)

    # step2: 生成拓扑连线,按照自定义拓扑,将指定id的节点(host或node皆可)进行连线并指定带宽和时延(发包器会自动连接)
    for x in range(col_num):
        host_in_row = []
        host_in_col = []
        for y in range(row_num):
            host_in_row.append(x * row_num + y)
            host_in_col.append(y * col_num + x)
        if print_groups:
            print(host_in_row)
            print(host_in_col)
        for i in range(row_num):
            for j in range(i + 1, row_num):
                host_id = host_in_row[i]
                another_host_id = host_in_row[j]
                #! 这里没有配置单位，脚本将报错。 
                graph.add_netisim_edge(host_id, another_host_id, bandwidth='400Gbps', delay='10ns', edge_count=1)
        for i in range(col_num):
            for j in range(i + 1, col_num):
                host_id = host_in_col[i]
                another_host_id = host_in_col[j]
                graph.add_netisim_edge(host_id, another_host_id, bandwidth='400Gbps', delay='10ns', edge_count=1)

    return graph


def main():
    graph = build_topology(print_groups=True)
   
    # step3: 生成配置文件,build_graph_config会生成一系列中间数据,最终生成dcn2.0_config.xml文件
    graph.build_graph_config()
    # step3.2: 默认生成通用压缩路由表
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    # step3.3: 使用 on-demand TP，避免预生成 host-pair transport_channel.csv
    graph.write_config(include_transport=False)


if __name__ == '__main__':
    main()
