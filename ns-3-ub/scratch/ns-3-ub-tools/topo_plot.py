import pandas as pd
import matplotlib.pyplot as plt
import networkx as nx
import numpy as np
import matplotlib.patches as mpatches
from matplotlib.patches import FancyArrowPatch
from matplotlib.lines import Line2D
import argparse
import os

# 颜色主题
COLORS = {
    'device': '#FFFFFF', 'device_edge': '#000000', 'leaf': '#424141',
    'leaf_edge': '#000000', 'core': "#424141", 'core_edge': '#000000',
    'edge_device': '#000000', 'edge_switch': '#000000', 'background': '#FFFFFF',
    'grid': "#CFCCCC", 'sw_sw_edge': '#3B3C99', 'd_d_edge': '#FF6B6B'
}

def get_range_int(range_int_str: str):
    """
    将范围字符串解析为起始和结束整数
    例: "0..511" -> (0, 511)
    """
    if '..' in range_int_str:
        start, end = range_int_str.split('..')
        return int(start), int(end)
    else:
        return int(range_int_str), int(range_int_str)

def plot_network_topology(input_dir, output_name="network_topology", save_pdf=False):
    # 读取数据
    nodes = pd.read_csv(os.path.join(input_dir, 'node.csv'))  
    edges = pd.read_csv(os.path.join(input_dir, 'topology.csv'))  

    # 创建图
    G = nx.Graph()

    # 添加所有节点
    all_nodes = {}
    for idx, row in nodes.iterrows():
        node_id_start, node_id_end = get_range_int(row['nodeId'])
        node_type = row['nodeType']
        port_num = int(row['portNum'])
        
        for node_id in range(node_id_start, node_id_end + 1):
            all_nodes[node_id] = {
                'type': node_type, 'ports': port_num, 'range_start': node_id_start,
                'range_end': node_id_end, 'layer': idx
            }
            G.add_node(node_id, **all_nodes[node_id])

    # 添加边
    for _, row in edges.iterrows():
        node1, node2 = int(row['nodeId1']), int(row['nodeId2'])
        G.add_edge(node1, node2, bandwidth=row['bandwidth'], delay=row['delay'])

    # 层次分组
    node_layers = {node: info['layer'] for node, info in all_nodes.items()}
    layer_groups = {}
    for node, layer in node_layers.items():
        layer_groups.setdefault(layer, []).append(node)

    # 创建图形
    fig, ax = plt.subplots(figsize=(20, 10), facecolor=COLORS['background'])
    ax.set_facecolor(COLORS['background'])

    # 布局计算
    positions = {}
    fig_width, fig_height = 18, 8

    # 计算有效层数
    valid_layers = [layer for layer in sorted(layer_groups.keys()) if layer_groups[layer]]
    if not valid_layers:
        return
    
    num_layers = len(valid_layers)
    
    # 自适应计算Y位置 - 所有情况统一处理
    if num_layers == 1:
        y_positions = [fig_height / 2]
    else:
        # 使用画布70%的高度，均匀分布并居中
        usable_height = fig_height * 0.85
        y_start = (fig_height - usable_height) / 2
        y_positions = [y_start + i * (usable_height / (num_layers - 1)) for i in range(num_layers)]
    
    # 为每层分配节点位置
    for i, layer in enumerate(valid_layers):
        nodes_in_layer = layer_groups[layer]
        y_pos = y_positions[i]
        
        # X方向自适应布局
        if len(nodes_in_layer) == 1:
            x_positions = [fig_width / 2]
        else:
            node_count = len(nodes_in_layer)
            margin_ratio = max(0.1, min(0.3, 1.0 / node_count))
            margin = fig_width * margin_ratio
            x_positions = np.linspace(margin, fig_width - margin, node_count)
        
        for j, node in enumerate(sorted(nodes_in_layer)):
            positions[node] = (x_positions[j], y_pos)

    # 边分类
    edge_types = {'device_switch': [], 'switch_switch': [], 'device_device': []}
    for edge in G.edges():
        node1, node2 = edge
        layer1, layer2 = node_layers.get(node1, 0), node_layers.get(node2, 0)
        
        if layer1 == 0 and layer2 == 0:
            edge_types['device_device'].append(edge)
        elif (layer1 == 0 and layer2 >= 1) or (layer1 >= 1 and layer2 == 0):
            edge_types['device_switch'].append(edge)
        elif layer1 >= 1 and layer2 >= 1:
            edge_types['switch_switch'].append(edge)

    # 绘制连线 - Device-Device弧线
    for edge in edge_types['device_device']:
        node1, node2 = edge
        x1, y1 = positions[node1]
        x2, y2 = positions[node2]
        
        distance = abs(x2 - x1)
        rad = 0.3 / distance if distance > 1 else 0.3
        
        arrow = FancyArrowPatch((x1, y1), (x2, y2),
                               connectionstyle=f"arc3,rad={rad}",
                               arrowstyle="-", color=COLORS['d_d_edge'],
                               linewidth=0.8, alpha=0.3)
        ax.add_patch(arrow)

    # Device-Switch连线
    nx.draw_networkx_edges(G, positions, edgelist=edge_types['device_switch'], 
                          width=1.2, alpha=0.4, edge_color=COLORS['edge_device'])

    # Switch-Switch连线
    for edge in edge_types['switch_switch']:
        node1, node2 = edge
        x1, y1 = positions[node1]
        x2, y2 = positions[node2]
        
        if abs(y1 - y2) < 0.5:  # 同层弧线
            distance = abs(x2 - x1)
            rad = 0.4 / distance if distance > 1 else 0.4
            
            arrow = FancyArrowPatch((x1, y1), (x2, y2),
                                   connectionstyle=f"arc3,rad={rad}",
                                   arrowstyle="-", color=COLORS['sw_sw_edge'],
                                   linewidth=1.2, alpha=0.4)
            ax.add_patch(arrow)
        else:  # 不同层直线
            ax.plot([x1, x2], [y1, y2], color=COLORS['edge_switch'], 
                    linewidth=1.2, alpha=0.4)

    # 绘制节点 - 自适应大小
    device_nodes = []
    switch_nodes = []
    
    # 按节点类型分组，而不是按layer
    for node, info in all_nodes.items():
        if info['type'].lower() in ['device', 'host', 'end_device']:
            device_nodes.append(node)
        else:
            switch_nodes.append(node)
    
    # 计算自适应节点大小的函数
    def calculate_adaptive_node_size(nodes_list, node_type='device'):
        if not nodes_list:
            return 50, 7  # 返回节点大小和字体大小
        
        # 按层分组统计最大节点数
        layer_node_counts = {}
        for node in nodes_list:
            layer = all_nodes[node]['layer']
            layer_node_counts[layer] = layer_node_counts.get(layer, 0) + 1
        
        max_nodes_in_layer = max(layer_node_counts.values()) if layer_node_counts else 1
        
        # 根据同层最大节点数和画布宽度计算节点大小
        available_width = fig_width * 0.8  # 可用宽度（留20%边距）
        
        if node_type == 'device':
            # 设备节点：圆形，基础大小
            min_spacing = 0.1  # 节点间最小间距
            max_node_width = (available_width - (max_nodes_in_layer - 1) * min_spacing) / max_nodes_in_layer
            # 将宽度转换为matplotlib的node_size（面积）
            base_size = max(3, min(100, (max_node_width * 50) ** 2))
            # 计算对应的字体大小
            font_size = max(6, min(8, max_node_width * 30))
        else:
            # 交换机节点：方形，更大
            min_spacing = 0.2  # 交换机间距更大
            max_node_width = (available_width - (max_nodes_in_layer - 1) * min_spacing) / max_nodes_in_layer
            base_size = max(200, min(1000, (max_node_width * 80) ** 2))
            # 交换机字体大小
            font_size = max(5, min(12, max_node_width * 28))
        
        return int(base_size), font_size
    
    # 绘制设备节点
    if device_nodes:
        device_size, device_font_size = calculate_adaptive_node_size(device_nodes, 'device')
        nx.draw_networkx_nodes(G, positions, nodelist=device_nodes,
                             node_size=device_size, node_color=COLORS['device'], 
                             edgecolors=COLORS['device_edge'], linewidths=1, alpha=0.9)
    else:
        device_font_size = 6  # 默认字体大小
    
    # 绘制交换机节点
    if switch_nodes:
        switch_size, switch_font_size = calculate_adaptive_node_size(switch_nodes, 'switch')
        nx.draw_networkx_nodes(G, positions, nodelist=switch_nodes,
                             node_size=switch_size, node_color=COLORS['leaf'], node_shape='s',
                             edgecolors=COLORS['leaf_edge'], linewidths=1)
    else:
        switch_font_size = 7  # 默认字体大小

    # 添加标签 - 使用自适应字体大小
    if switch_nodes:
        switch_labels = {node: f"SW\n{node}" for node in switch_nodes}
        nx.draw_networkx_labels(G, positions, labels=switch_labels, font_size=int(switch_font_size), 
                               font_color='white', font_weight='bold')

    # 设备标签 - 使用自适应字体大小
    if device_nodes:
        device_labels = {}
        sample_step = max(1, len(device_nodes) // 32)
        for i in range(0, len(device_nodes), sample_step):
            node = sorted(device_nodes)[i]
            device_labels[node] = f"Host\n{node}"
        
        device_label_positions = {node: (positions[node][0], positions[node][1] - 0.4) 
                                 for node in device_labels.keys()}
        nx.draw_networkx_labels(G, device_label_positions, labels=device_labels, 
                               font_size=int(device_font_size), font_color='black', font_weight='bold')

    # 图例和样式
    # 统计所有switch节点（不区分层级）
    all_switch_nodes = []
    for layer in range(1, len(layer_groups)):
        all_switch_nodes.extend(layer_groups.get(layer, []))
    
    legend_elements = [
        mpatches.Rectangle((0, 0), 1, 1, facecolor=COLORS['leaf'], edgecolor=COLORS['leaf_edge'], 
                       label=f'Switches ({len(all_switch_nodes)})'),
        Line2D([0], [0], marker='o', color='w', markerfacecolor=COLORS['device'], 
                   markeredgecolor=COLORS['device_edge'], markersize=8, linewidth=0,
                   label=f'Hosts ({len(layer_groups.get(0, []))})'),
        # plt.Line2D([0], [0], color=COLORS['edge_device'], linewidth=2, label='Host-SW Links'),
        # plt.Line2D([0], [0], color=COLORS['sw_sw_edge'], linewidth=2, label='SW-SW Links'),
        # plt.Line2D([0], [0], color=COLORS['d_d_edge'], linewidth=2, label='Host-Host Links')
    ]

    legend = plt.legend(handles=legend_elements, loc='upper left', fontsize=8, 
                       framealpha=0.9, ncol=5, bbox_to_anchor=(0.05, 0.88))

    ax.set_xlim(0, fig_width)
    ax.set_ylim(0, fig_height + 1)
    ax.axis('off')
    ax.grid(True, alpha=0.05, color=COLORS['grid'])

    plt.subplots_adjust(top=0.88, bottom=0.05, left=0.05, right=0.95)

    # 保存文件 - 输出到输入目录
    output_png = os.path.join(input_dir, f"{output_name}.png")
    plt.savefig(output_png, dpi=300, bbox_inches='tight', 
               facecolor=COLORS['background'], pad_inches=0.1)
    
    saved_files = [output_png]
    
    if save_pdf:
        output_pdf = os.path.join(input_dir, f"{output_name}.pdf")
        plt.savefig(output_pdf, bbox_inches='tight', 
                   facecolor=COLORS['background'], pad_inches=0.1)
        saved_files.append(output_pdf)

    print(f"✅ Network topology visualization completed!")
    if len(saved_files) == 1:
        print(f"📁 Saved as: {saved_files[0]}")
    else:
        print(f"📁 Saved as: {' & '.join(saved_files)}")
    # Only show the plot if running in an interactive backend
    try:
        backend = plt.get_backend().lower()
    except Exception:
        backend = ''

    if 'agg' not in backend:
        plt.show()

def main():
    parser = argparse.ArgumentParser(
        description="Network Topology Visualization Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -i ./data                    # Use ./data directory
  %(prog)s -i /path/to/csv -o my_topo   # Custom output name
  %(prog)s --input-dir ./sim_output     # Long option form

Input Files Required:
  - node.csv: Contains nodeId, nodeType, portNum columns
  - topology.csv: Contains nodeId1, nodeId2, bandwidth, delay columns
        """)
    
    parser.add_argument('-i', '--input-dir', required=True,
                      help='Directory containing node.csv and topology.csv')
    parser.add_argument('-o', '--output', default='network_topology',
                      help='Output filename prefix (default: network_topology)')
    parser.add_argument('--pdf', action='store_true',
                      help='Also save PDF format (PNG is always saved)')
    
    args = parser.parse_args()
    
    # 检查输入目录
    if not os.path.isdir(args.input_dir):
        print(f"❌ Error: Directory '{args.input_dir}' does not exist")
        return
    
    # 检查必需文件
    node_file = os.path.join(args.input_dir, 'node.csv')
    topo_file = os.path.join(args.input_dir, 'topology.csv')
    
    if not os.path.exists(node_file):
        print(f"❌ Error: File '{node_file}' not found")
        return
    
    if not os.path.exists(topo_file):
        print(f"❌ Error: File '{topo_file}' not found")
        return
    
    plot_network_topology(args.input_dir, args.output)

if __name__ == "__main__":
    main()