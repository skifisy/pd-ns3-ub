import os
import re
import time
import csv
from collections import defaultdict
from itertools import groupby
from concurrent.futures import ProcessPoolExecutor, as_completed

import networkx as nx
from tqdm import tqdm
from typing import List

from physical_topology import build_physical_topology
from route_split import (
    build_direct_shortest_route_config,
    build_route_lookups,
    build_rule_route_lookups,
    build_topology_route_config,
    compile_route_config,
    is_default_shortest_path_finder,
    split_route_lookups,
)
from topology_recognition import discover_topology_structures
from topology_structure import verify_topology_structures


def _process_path_chunk(graph, path_finding_algo, node_pairs):
    # 子进程中串行处理一段节点对，避免 IO 干扰
    paths = []
    for src, dst in tqdm(node_pairs, desc="Processing paths", leave=False):
        try:
            for p in path_finding_algo(graph, src, dst):
                paths.append(p)
        except nx.exception.NetworkXNoPath:
            pass
    return paths

_SPARSE_ROUTE_WORKER_GRAPH = None
_SPARSE_ROUTE_WORKER_ALGO = None

def _init_sparse_route_worker(graph, path_finding_algo):
    global _SPARSE_ROUTE_WORKER_GRAPH
    global _SPARSE_ROUTE_WORKER_ALGO
    _SPARSE_ROUTE_WORKER_GRAPH = graph
    _SPARSE_ROUTE_WORKER_ALGO = path_finding_algo

def _process_sparse_route_chunk(node_pairs):
    graph = _SPARSE_ROUTE_WORKER_GRAPH
    path_finding_algo = _SPARSE_ROUTE_WORKER_ALGO
    if graph is None or path_finding_algo is None:
        raise RuntimeError("sparse route worker is not initialized")

    route_table = graph.init_sparse_routing_table()
    for src, dst in node_pairs:
        try:
            for path in path_finding_algo(graph, src, dst):
                graph.add_path_to_sparse_route_table(route_table, path)
        except nx.exception.NetworkXNoPath:
            pass
    return {key: dict(value) for key, value in route_table.items()}

VALID_BANDWIDTH_UNIT = {"bps", "Kbps", "Mbps", "Gbps", "Tbps"}
VALID_TIME_UNIT = {"ns", "us", "ms", "s"}
_UNSET = object()

def parse_id_range(value):
    value = str(value).strip()
    if not value:
        raise ValueError("range field must not be empty")
    if ".." not in value:
        parsed = int(value)
        return parsed, parsed
    start, end = value.split("..", 1)
    start = int(start)
    end = int(end)
    if start > end:
        raise ValueError(f"range start must not exceed end: {value}")
    return start, end

def parse_range_start(value):
    return parse_id_range(value)[0]

def _format_range_value(start, end):
    return str(start) if start == end else f"{start}..{end}"

def _ranges_overlap(left_start, left_end, right_start, right_end):
    return left_start <= right_end and right_start <= left_end

def _normalize_route_value(outports_text, metrics_text, row_num):
    outports = tuple(int(value) for value in outports_text.split())
    metrics = tuple(int(value) for value in metrics_text.split())
    if len(outports) != len(metrics):
        raise ValueError(f"第{row_num}行 outPorts 与 metrics 数量不一致")
    if not outports:
        raise ValueError(f"第{row_num}行必须包含至少一个 outPort/metric")

    metric_by_port = {}
    for outport, metric in zip(outports, metrics):
        old_metric = metric_by_port.get(outport)
        if old_metric is not None and old_metric != metric:
            raise ValueError(f"第{row_num}行 outPort {outport} 同时配置了多个 metric")
        metric_by_port[outport] = metric
    return tuple(sorted(metric_by_port.items()))

def _read_route_table_rows(route_table_csv_path):
    with open(route_table_csv_path, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        expected_headers = {"nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"}
        if reader.fieldnames is None or not expected_headers.issubset(set(reader.fieldnames)):
            missing = expected_headers - set(reader.fieldnames or [])
            raise ValueError(f"CSV文件缺少必要的列: {missing}")

        for row_num, row in enumerate(reader, start=2):
            node_start, node_end = parse_id_range(row["nodeId"])
            dst_start, dst_end = parse_id_range(row["dstNodeId"])
            dst_port = int(str(row["dstPortId"]).strip())
            value = _normalize_route_value(row["outPorts"], row["metrics"], row_num)
            yield {
                "row_num": row_num,
                "node_start": node_start,
                "node_end": node_end,
                "dst_start": dst_start,
                "dst_end": dst_end,
                "dst_port": dst_port,
                "value": value,
                "is_range": node_start != node_end or dst_start != dst_end,
            }

def validate_route_table_no_conflicts(route_table_csv_path):
    rows_by_port = defaultdict(list)
    for row in _read_route_table_rows(route_table_csv_path):
        if not row["is_range"]:
            continue
        rows_by_port[row["dst_port"]].append(row)

    for dst_port, rows in rows_by_port.items():
        active = []
        for row in sorted(rows, key=lambda item: (item["node_start"], item["node_end"], item["dst_start"])):
            active = [item for item in active if item["node_end"] >= row["node_start"]]
            for other in active:
                if other["value"] == row["value"]:
                    continue
                if _ranges_overlap(other["dst_start"], other["dst_end"], row["dst_start"], row["dst_end"]):
                    node_overlap = (
                        max(other["node_start"], row["node_start"]),
                        min(other["node_end"], row["node_end"]),
                    )
                    dst_overlap = (
                        max(other["dst_start"], row["dst_start"]),
                        min(other["dst_end"], row["dst_end"]),
                    )
                    raise ValueError(
                        "压缩路由存在冲突覆盖: "
                        f"第{other['row_num']}行与第{row['row_num']}行 "
                        f"在 nodeId={_format_range_value(*node_overlap)}, "
                        f"dstNodeId={_format_range_value(*dst_overlap)}, "
                        f"dstPortId={dst_port} 上配置了不同的 outPorts/metrics"
                    )
            active.append(row)
    return True

def _merge_intervals(intervals):
    intervals = sorted(intervals)
    if not intervals:
        return []

    merged = []
    start, end = intervals[0]
    for next_start, next_end in intervals[1:]:
        if next_start <= end + 1:
            end = max(end, next_end)
            continue
        merged.append((start, end))
        start, end = next_start, next_end
    merged.append((start, end))
    return merged

def _remove_single_value_from_intervals(intervals, value):
    result = []
    for start, end in intervals:
        if value < start or value > end:
            result.append((start, end))
            continue
        if start <= value - 1:
            result.append((start, value - 1))
        if value + 1 <= end:
            result.append((value + 1, end))
    return result

def _missing_intervals(expected, covered):
    covered = _merge_intervals(covered)
    missing = []
    covered_index = 0

    for expected_start, expected_end in expected:
        cursor = expected_start
        while covered_index < len(covered) and covered[covered_index][1] < cursor:
            covered_index += 1

        probe = covered_index
        while probe < len(covered) and covered[probe][0] <= expected_end:
            covered_start, covered_end = covered[probe]
            if covered_start > cursor:
                missing.append((cursor, min(expected_end, covered_start - 1)))
            cursor = max(cursor, covered_end + 1)
            if cursor > expected_end:
                break
            probe += 1

        if cursor <= expected_end:
            missing.append((cursor, expected_end))

    return missing

def validate_with_unit(name, value, allowed_units, example):
        ok = isinstance(value, str) and re.match(r'^\s*\d+(\.\d+)?\s*[A-Za-z]+\s*$', value or "")
        if not ok:
            print(f"\033[91m[Error] add_netisim_edge: 参数 {name}='{value}' 缺少单位，示例: {example}\033[0m")
            raise ValueError(f"{name} must include unit, e.g. {example}")
        unit = re.findall(r'[A-Za-z]+', value.strip())[-1]
        if unit not in allowed_units:
            print(f"\033[91m[Error] add_netisim_edge: 参数 {name}='{value}' 单位不被支持，可用单位: {', '.join(allowed_units)}\033[0m")
            raise ValueError(f"{name} unit must be one of: {', '.join(allowed_units)}")

class NetworkSimulationGraph(nx.Graph):
    node_type = ["host", "node"]

    def __init__(self):
        super().__init__()
        self.link_port = {}            # (u, v, u_port) -> [v_port]
        self.port_list = []            # [(is_switch, port_num)] 按节点顺序
        self.link_infos = []           # [(u,u_port,v,v_port,bw,delay)]，bw/delay 为带单位字符串
        self.route_dict4tp = defaultdict(list)  # (host_a,host_b) -> [(src_port,metric,dst_port),...]
        self.output_dir = "./output/" + time.strftime("%Y-%m-%d_%H-%M-%S", time.localtime()) + "/"

        self.host_ids = []
        self.node_ids = []

        # 下一跳端口表: next_hop_ports[(this_node, neighbor)] = [port1, port2, ...]
        self.next_hop_ports = []

        # 路由表: route_table_2port[node_id][dst_id] = set((out_port, metric, dst_port))
        self.route_table_2port = None
        self.physical_topology = None

    @staticmethod
    def _resolve_node_delays(forward_delay, allocation_delay):
        if forward_delay is _UNSET and allocation_delay is _UNSET:
            return "1ns", "0ns"
        if allocation_delay is _UNSET:
            # Existing callers used forward_delay for the legacy four-column CSV field,
            # which the simulator interprets as allocationDelay.
            return forward_delay, "0ns"
        if forward_delay is _UNSET:
            return allocation_delay, "0ns"
        return allocation_delay, forward_delay

    # 基础节点/边 API（时延属性保留带单位字符串，不做单位转换）
    def add_netisim_host(self,
                         node_id,
                         forward_delay=_UNSET,
                         allocation_delay=_UNSET,
                         **attr):
        self.host_ids.append(node_id)
        allocation_delay, forward_delay = self._resolve_node_delays(
            forward_delay,
            allocation_delay,
        )
        validate_with_unit("allocation_delay", allocation_delay, VALID_TIME_UNIT, "1ns")
        validate_with_unit("forward_delay", forward_delay, VALID_TIME_UNIT, "1ns")
        return super().add_node(
            node_id,
            type="host",
            allocation_delay=allocation_delay,
            forward_delay=forward_delay,
            **attr,
        )

    def add_netisim_node(self,
                         node_id,
                         forward_delay=_UNSET,
                         allocation_delay=_UNSET,
                         **attr):
        self.node_ids.append(node_id)
        allocation_delay, forward_delay = self._resolve_node_delays(
            forward_delay,
            allocation_delay,
        )
        validate_with_unit("allocation_delay", allocation_delay, VALID_TIME_UNIT, "1ns")
        validate_with_unit("forward_delay", forward_delay, VALID_TIME_UNIT, "1ns")
        return super().add_node(
            node_id,
            type="node",
            allocation_delay=allocation_delay,
            forward_delay=forward_delay,
            **attr,
        )

    def add_netisim_edge(self, u, v, bandwidth="200Gbps", delay="20ns", edge_count=1, **attr):
        # 注意：bandwidth、delay 以字符串形式存储，原样写入输出文件
        validate_with_unit("bandwidth", bandwidth, VALID_BANDWIDTH_UNIT, "400Gbps")
        validate_with_unit("delay", delay, VALID_TIME_UNIT, "10ns")
        return super().add_edge(
            u, v,
            bandwidth=bandwidth,
            delay=delay,
            edge_count=int(edge_count),
            **attr
        )

    # 统计/校验
    def check_graph_index_valid(self):
        for i in range(len(self.nodes)):
            assert self.has_node(i), f"编号{i}:必须从0开始顺序编号!"
            assert self.nodes[i]["type"] in self.node_type, f"编号{i}类型非法!"
        assert len(self.host_ids) + len(self.node_ids) == len(self.nodes), "host+node数应等于总节点数!"
        return True

    def get_host_num(self):
        return len(self.host_ids)

    def get_node_num(self):
        return len(self.node_ids)

    def get_total_num(self):
        return len(self.host_ids) + len(self.node_ids)

    def node_is_switch(self, node_id):
        return self.nodes[node_id]["type"] == "node"

    def node_is_host(self, node_id):
        return self.nodes[node_id]["type"] == "host"

    def get_physical_topology(self):
        if self.physical_topology is None:
            raise RuntimeError("build_graph_config() must be called before get_physical_topology()")
        return self.physical_topology

    # 拓扑到端口映射（供后续路由/导出使用）
    def build_graph_config(self):
        assert self.check_graph_index_valid()

        self.physical_topology = None

        total_node_num = self.get_total_num()

        # 确保节点编号连续且与插入顺序一致
        x = list(self.nodes)
        if x != list(range(len(x))):
            raise ValueError(f"节点编号应为 0..{len(x)-1} 的连续整数，实际得到 {x}")

        port_conn_count = [0] * total_node_num
        self.port_list.clear()
        self.link_port.clear()
        self.link_infos.clear()

        # 统计每个节点的端口总数（包含 edge_count）
        for node_id in self.nodes:
            t_port_num = 0
            for edge in self.edges(node_id):
                t_port_num += int(self.get_edge_data(edge[0], edge[1])["edge_count"])
            # is_switch: host=0, switch=1
            self.port_list.append((1 - self.node_is_host(node_id), t_port_num))

        # 链路展开为端口映射、link_infos 与稀疏邻居端口索引。
        self.next_hop_ports = defaultdict(list)
        for u, v in self.edges:
            edge_count = int(self.edges[u, v]["edge_count"])
            bw = self.edges[u, v]["bandwidth"]  # 字符串，如 '400Gbps'
            dl = self.edges[u, v]["delay"]      # 字符串，如 '20ns'
            for _ in range(edge_count):
                u_port = port_conn_count[u]
                v_port = port_conn_count[v]

                self.link_port[(u, v, u_port)] = [v_port]
                self.link_port[(v, u, v_port)] = [u_port]
                self.next_hop_ports[(u, v)].append(u_port)
                self.next_hop_ports[(v, u)].append(v_port)

                port_conn_count[u] += 1
                port_conn_count[v] += 1
                self.link_infos.append((u, u_port, v, v_port, bw, dl))

        self.physical_topology = build_physical_topology(self, self.link_infos)

    def build_graph_config_from_links(self, link_infos, port_counts=None):
        """Install physical links whose endpoint port IDs are already assigned."""
        assert self.check_graph_index_valid()

        total_node_num = self.get_total_num()
        if list(self.nodes) != list(range(total_node_num)):
            raise ValueError(f"节点编号应为 0..{total_node_num - 1} 的连续整数")

        explicit_links = []
        used_ports = defaultdict(set)
        edge_counts = defaultdict(int)
        edge_properties = {}
        for row_num, link_info in enumerate(link_infos, start=1):
            if len(link_info) != 6:
                raise ValueError(f"第{row_num}条链路必须包含6个字段")
            left, left_port, right, right_port, bandwidth, delay = link_info
            left = int(left)
            left_port = int(left_port)
            right = int(right)
            right_port = int(right_port)
            if left not in self or right not in self:
                raise ValueError(f"第{row_num}条链路引用了不存在的节点")
            if left == right:
                raise ValueError(f"第{row_num}条链路不能连接节点自身")
            if left_port < 0 or right_port < 0:
                raise ValueError(f"第{row_num}条链路端口号不能为负数")
            for node_id, port_id in ((left, left_port), (right, right_port)):
                if port_id in used_ports[node_id]:
                    raise ValueError(f"节点{node_id}的端口{port_id}被多条链路使用")
                used_ports[node_id].add(port_id)
            validate_with_unit("bandwidth", bandwidth, VALID_BANDWIDTH_UNIT, "400Gbps")
            validate_with_unit("delay", delay, VALID_TIME_UNIT, "10ns")

            edge = tuple(sorted((left, right)))
            properties = (bandwidth, delay)
            if edge in edge_properties and edge_properties[edge] != properties:
                raise ValueError(
                    f"节点{edge[0]}和{edge[1]}之间的并行链路必须使用相同带宽和时延"
                )
            edge_properties[edge] = properties
            edge_counts[edge] += 1
            explicit_links.append((left, left_port, right, right_port, bandwidth, delay))

        self.clear_edges()
        for (left, right), edge_count in sorted(edge_counts.items()):
            bandwidth, delay = edge_properties[(left, right)]
            self.add_netisim_edge(
                left,
                right,
                bandwidth=bandwidth,
                delay=delay,
                edge_count=edge_count,
            )

        resolved_port_counts = []
        for node_id in range(total_node_num):
            minimum_count = max(used_ports[node_id], default=-1) + 1
            declared_value = None if port_counts is None else port_counts[node_id]
            declared_count = minimum_count if declared_value is None else int(declared_value)
            if declared_count < minimum_count:
                raise ValueError(
                    f"节点{node_id}声明{declared_count}个端口，但链路使用了端口{minimum_count - 1}"
                )
            resolved_port_counts.append(declared_count)

        self.port_list = [
            (1 - self.node_is_host(node_id), resolved_port_counts[node_id])
            for node_id in range(total_node_num)
        ]
        self.link_port.clear()
        self.next_hop_ports = defaultdict(list)
        self.link_infos = explicit_links
        for left, left_port, right, right_port, _bandwidth, _delay in explicit_links:
            self.link_port[(left, right, left_port)] = [right_port]
            self.link_port[(right, left, right_port)] = [left_port]
            self.next_hop_ports[(left, right)].append(left_port)
            self.next_hop_ports[(right, left)].append(right_port)

        self.physical_topology = build_physical_topology(self, self.link_infos)

    # 路由表基础结构
    def init_routing_tables(self):
        total_node_num = self.get_total_num()
        self.route_table_2port = [[set() for _ in range(total_node_num)] for _ in range(total_node_num)]

    def init_sparse_routing_table(self):
        return defaultdict(dict)

    # 查询端口
    def get_link_ports(self, this_node, next_hop):
        return self.next_hop_ports.get((this_node, next_hop), [])

    def _format_range(self, start, end):
        return str(start) if start == end else f"{start}..{end}"

    def _iter_consecutive_ranges(self, values):
        values = sorted(set(values))
        if not values:
            return
        start = prev = values[0]
        for value in values[1:]:
            if value != prev + 1:
                yield start, prev
                start = value
            prev = value
        yield start, prev

    def _iter_host_pairs(self, nodes):
        for index, src in enumerate(nodes):
            for next_index in range(index + 1, len(nodes)):
                yield src, nodes[next_index]

    def _iter_host_pair_chunks(self, nodes, chunk_size):
        chunk = []
        for pair in self._iter_host_pairs(nodes):
            chunk.append(pair)
            if len(chunk) >= chunk_size:
                yield chunk
                chunk = []
        if chunk:
            yield chunk

    # 写入一条路由表项（带 ECMP 与最短度量合并）
    def set_route_table(self, curr_node, next_hop_node, node_before_dst, dst, metric):
        """
        手动添加一条路由表项到指定节点的路由表中
        
        此方法允许用户手动配置单条路由表项，支持ECMP(等价多路径)负载均衡。
        当存在相同出端口和目标端口的路由时，会自动保留更优(更小)的metric值。
        
        Parameters
        ----------
        curr_node : int
            当前节点ID，即要添加路由表项的节点
            
        next_hop_node : int
            下一跳节点ID，数据包从curr_node发出后的下一个节点
            
        node_before_dst : int
            目标节点的前一跳节点ID，用于确定数据包到达目标节点时的入端口
            当next_hop_node等于dst时，node_before_dst用于计算dst的入端口
            
        dst : int
            目标节点ID，数据包的最终目的地
            
        metric : int
            路由度量值，数值越小表示路径越优
            用于路径选择和ECMP负载均衡
            
        Notes
        -----
        - 路由表项格式为: (out_port, metric, dst_port)
        - 支持ECMP: 相同metric的多条路径会被同时保留
        - 自动去重: 相同出端口和目标端口的路由只保留最优metric
        - 端口计算: 基于build_graph_config()生成的端口映射关系
        
        Examples
        --------
        >>> # 添加从节点0到节点3的路由，下一跳为节点1
        >>> graph.set_route_table(curr_node=0, next_hop_node=1, 
        ...                       node_before_dst=2, dst=3, metric=2)
        
        >>> # 在路径 0->1->2->3 中，为节点1添加到节点3的路由
        >>> graph.set_route_table(curr_node=1, next_hop_node=2, 
        ...                       node_before_dst=2, dst=3, metric=1)
        
        See Also
        --------
        gen_route_table : 自动生成全网路由表
        get_link_ports : 获取节点间的端口连接信息
        """
        # node_before_dst 是 dst 的前一跳节点，用于确定 dst 侧的入端口号
        dst_ports = self.get_link_ports(dst, node_before_dst)
        for dst_port in dst_ports:
            next_hop_ports = (
                self.link_port[(dst, node_before_dst, dst_port)]
                if next_hop_node == dst
                else self.get_link_ports(curr_node, next_hop_node)
            )
            
            route_set = self.route_table_2port[curr_node][dst]
            
            for out_port in next_hop_ports:
                route_key = (out_port, dst_port)
                
                # 查找是否存在相同(out_port, dst_port)的路由
                conflicting_routes = [r for r in route_set if (r[0], r[-1]) == route_key]
                
                if not conflicting_routes:
                    # 无冲突，直接添加
                    route_set.add((out_port, metric, dst_port))
                else:
                    # 处理冲突路由（通常只有一个）
                    existing_route = conflicting_routes[0]
                    if existing_route[1] > metric:
                        route_set.remove(existing_route)
                        route_set.add((out_port, metric, dst_port))

    def set_sparse_route_table(self, route_table, curr_node, next_hop_node, node_before_dst, dst, metric):
        dst_ports = self.get_link_ports(dst, node_before_dst)
        for dst_port in dst_ports:
            next_hop_ports = (
                self.link_port[(dst, node_before_dst, dst_port)]
                if next_hop_node == dst
                else self.get_link_ports(curr_node, next_hop_node)
            )

            route_key = (curr_node, dst, dst_port)
            outport_metrics = route_table[route_key]
            for out_port in next_hop_ports:
                old_metric = outport_metrics.get(out_port)
                if old_metric is None or old_metric > metric:
                    outport_metrics[out_port] = metric

    def add_path_to_sparse_route_table(self, route_table, path):
        if not path:
            return
        src, dst = path[0], path[-1]
        for i, node_id in enumerate(path):
            if i >= 1:
                self.set_sparse_route_table(curr_node=node_id,
                                            route_table=route_table,
                                            next_hop_node=path[i - 1],
                                            node_before_dst=path[1],
                                            dst=src,
                                            metric=i)
            if i < len(path) - 1:
                self.set_sparse_route_table(curr_node=node_id,
                                            route_table=route_table,
                                            next_hop_node=path[i + 1],
                                            node_before_dst=path[-2],
                                            dst=dst,
                                            metric=len(path) - i - 1)

    def merge_sparse_route_table(self, route_table, route_delta):
        for route_key, outport_metrics in route_delta.items():
            target = route_table[route_key]
            for out_port, metric in outport_metrics.items():
                old_metric = target.get(out_port)
                if old_metric is None or old_metric > metric:
                    target[out_port] = metric

    def compressed_route_rows(self, route_table, host_router=True):
        by_node_signature = defaultdict(list)

        for (node_id, dst_id, dst_port), outport_metrics in route_table.items():
            if self.node_is_host(node_id) and not host_router:
                continue
            sorted_ports = sorted(outport_metrics.items())
            outports = tuple(port for port, _ in sorted_ports)
            metrics = tuple(metric for _, metric in sorted_ports)
            by_node_signature[(node_id, dst_port, outports, metrics)].append(dst_id)

        dst_range_rows = []
        for (node_id, dst_port, outports, metrics), dst_ids in by_node_signature.items():
            outports_text = " ".join(map(str, outports))
            metrics_text = " ".join(map(str, metrics))
            for dst_start, dst_end in self._iter_consecutive_ranges(dst_ids):
                dst_range_rows.append((node_id,
                                       self._format_range(dst_start, dst_end),
                                       dst_port,
                                       outports_text,
                                       metrics_text))

        by_row_signature = defaultdict(list)
        for node_id, dst_range, dst_port, outports_text, metrics_text in dst_range_rows:
            by_row_signature[(dst_range, dst_port, outports_text, metrics_text)].append(node_id)

        rows = []
        for (dst_range, dst_port, outports_text, metrics_text), node_ids in by_row_signature.items():
            for node_start, node_end in self._iter_consecutive_ranges(node_ids):
                rows.append((self._format_range(node_start, node_end),
                             dst_range,
                             dst_port,
                             outports_text,
                             metrics_text))
        return sorted(rows, key=lambda row: (parse_range_start(row[0]), parse_range_start(row[1]), int(row[2])))

    def write_compressed_route_table_to_csv(self, route_table, host_router=True):
        rows = self.compressed_route_rows(route_table, host_router=host_router)
        os.makedirs(self.output_dir, exist_ok=True)
        filename = os.path.join(self.output_dir, "routing_table.csv")
        with open(filename, "w", encoding="utf-8") as f:
            f.write("nodeId,dstNodeId,dstPortId,outPorts,metrics\n")
            for row in tqdm(rows, desc="写入压缩路由CSV"):
                f.write(",".join(map(str, row)) + "\n")
        print(f"压缩路由表文件: {os.path.abspath(filename)}")
        validate_route_table_no_conflicts(os.path.abspath(filename))
        check_route_table_connectivity(os.path.abspath(filename))
        return rows

    def _should_write_transport_channel(self):
        if getattr(self, "_route_table_requires_on_demand_tp", False):
            raise ValueError("compressed route generation requires write_config(include_transport=False)")
        return True

    # 生成全网路由（按 host-host 最短路径，支持并行）
    def gen_route_table(self, write_file=True, host_router=True, path_finding_algo=nx.all_shortest_paths, multiple_workers=1):
        """
        使用寻路算法生成全网路由表
        
        此方法会自动寻找每一对host节点间的可用路径，并基于这些路径生成完整的路由表。
        如果你需要手动添加特定的路由表项，请使用 `set_route_table` 方法。
        
        Parameters
        ----------
        write_file : bool, optional
            是否将生成的路由表写入CSV文件，默认为True
            
        host_router : bool, optional
            是否为host节点生成路由文件，默认为True
            当设置为False时，仅为交换机节点生成路由表
            
        path_finding_algo : callable, optional
            自定义寻路算法函数，默认为 nx.all_shortest_paths
            函数签名必须为: path_finding_algo(graph, src, dst)
            - graph: NetworkSimulationGraph 实例
            - src: 源host节点ID (int)
            - dst: 目标host节点ID (int)
            返回值: 从src到dst的所有路径的迭代器，每条路径为节点ID的有序列表
  
        multiple_workers : int, optional
            寻路计算使用的并行进程数，默认为1
            设置大于1时启用多进程加速，适用于大型网络拓扑
            
        Notes
        -----
        - 方法会自动调用 build_graph_config() 来构建图配置
        - 生成的路由表支持ECMP(等价多路径)负载均衡
        - 路由表文件将输出到 self.output_dir 目录下的 route_table.csv
        
        Examples
        --------
        >>> # 使用默认最短路径算法
        >>> graph.gen_route_table()
        
        >>> # 使用自定义寻路算法和多进程加速
        >>> def custom_path_finder(graph, src, dst):
        ...     return nx.all_simple_paths(graph, src, dst, cutoff=5)
        >>> graph.gen_route_table(path_finding_algo=custom_path_finder, multiple_workers=4)
        """
        if self.next_hop_ports == []:
            self.build_graph_config()

        self.init_routing_tables()

        nodes = list(self.host_ids)
        node_pairs = [(nodes[i], nodes[j]) for i in range(len(nodes)) for j in range(i + 1, len(nodes))]

        paths = []
        if multiple_workers and multiple_workers > 1 and len(node_pairs) > 0:
            print(f"使用多进程加速，worker数量 {multiple_workers}:")
            chunk_size = max(1, len(node_pairs) // multiple_workers + 1)
            with ProcessPoolExecutor(max_workers=multiple_workers) as executor:
                futures = []
                # 传递一个浅拷贝的无属性 Graph 更轻量
                gcopy = nx.Graph(self)
                for i in range(0, len(node_pairs), chunk_size):
                    chunk = node_pairs[i:i + chunk_size]
                    futures.append(executor.submit(_process_path_chunk, gcopy, path_finding_algo, chunk))
                
                # 添加进度条显示多进程完成情况
                for fut in tqdm(as_completed(futures), total=len(futures), desc="计算路径中"):
                    try:
                        paths.extend(fut.result())
                    except Exception as e:
                        print(f"Error in path finding: {e}")
        else:
            # 串行回退
            for src, dst in tqdm(node_pairs, desc="计算路径中"):
                try:
                    for p in path_finding_algo(self, src, dst):
                        paths.append(p)
                except nx.exception.NetworkXNoPath:
                    pass

        # 将路径写入路由表（双向写入，覆盖所有中间节点到两端 host 的路由）
        for path in tqdm(paths, desc="写入路由表中"):
            if not path:
                continue
            src, dst = path[0], path[-1]
            for i in range(len(path)):
                node_id = path[i]
                # 指向 src 的反向路由
                if i >= 1:
                    metric = i
                    self.set_route_table(node_id, path[i - 1], path[1], src, metric)
                # 指向 dst 的正向路由
                if i < len(path) - 1:
                    metric = len(path) - i - 1
                    self.set_route_table(node_id, path[i + 1], path[-2], dst, metric)

        if write_file:
            self.write_route_table_to_csv(host_router=host_router)
        self._route_table_requires_on_demand_tp = False

    def gen_compressed_route_table(self,
                                   write_file=True,
                                   host_router=True,
                                   path_finding_algo=nx.all_shortest_paths,
                                   multiple_workers=1,
                                   chunk_size=4096):
        """
        使用 streaming 寻路生成通用压缩路由表。

        该方法不保存全量 path 列表，也不创建 N x N dense 路由矩阵。
        它先写入稀疏 route table，再按连续 nodeId/dstNodeId range 合并相同规则。
        当 multiple_workers > 1 时，path_finding_algo 必须是多进程可 pickle 的顶层函数。
        """
        if self.next_hop_ports == []:
            self.build_graph_config()

        route_table = self.init_sparse_routing_table()
        nodes = list(self.host_ids)
        total_pairs = len(nodes) * (len(nodes) - 1) // 2

        if multiple_workers and multiple_workers > 1 and total_pairs > 0:
            chunk_size = max(1, int(chunk_size))
            max_pending = max(1, multiple_workers * 2)
            pending = {}
            chunk_iter = self._iter_host_pair_chunks(nodes, chunk_size)
            gcopy = self

            print(f"使用多进程压缩路由生成，worker数量 {multiple_workers}, chunk_size {chunk_size}:")
            with ProcessPoolExecutor(max_workers=multiple_workers,
                                     initializer=_init_sparse_route_worker,
                                     initargs=(gcopy, path_finding_algo)) as executor:
                with tqdm(total=total_pairs, desc="计算并写入压缩路由") as progress:
                    exhausted = False
                    while pending or not exhausted:
                        while not exhausted and len(pending) < max_pending:
                            try:
                                chunk = next(chunk_iter)
                            except StopIteration:
                                exhausted = True
                                break
                            pending[executor.submit(_process_sparse_route_chunk, chunk)] = len(chunk)

                        for fut in as_completed(pending):
                            processed_pairs = pending.pop(fut)
                            route_delta = fut.result()
                            self.merge_sparse_route_table(route_table, route_delta)
                            progress.update(processed_pairs)
                            break
        else:
            for src, dst in tqdm(self._iter_host_pairs(nodes), total=total_pairs, desc="计算并写入压缩路由"):
                try:
                    for path in path_finding_algo(self, src, dst):
                        self.add_path_to_sparse_route_table(route_table, path)
                except nx.exception.NetworkXNoPath:
                    pass

        if write_file:
            rows = self.write_compressed_route_table_to_csv(route_table, host_router=host_router)
            self._route_table_requires_on_demand_tp = True
            return rows
        self._route_table_requires_on_demand_tp = True
        return self.compressed_route_rows(route_table, host_router=host_router)

    def gen_structured_route_config(self,
                                    routing_mode="mixed",
                                    write_file=True,
                                    host_router=True,
                                    path_finding_algo=nx.all_shortest_paths):
        """Discover verified structure and compile table, rule, or mixed routing."""
        if routing_mode not in {"table", "rule", "mixed"}:
            raise ValueError("routing_mode must be one of: table, rule, mixed")
        if self.next_hop_ports == []:
            self.build_graph_config()

        use_direct_shortest = (
            routing_mode != "table"
            and is_default_shortest_path_finder(path_finding_algo)
        )
        if use_direct_shortest:
            topology = self.get_physical_topology()
            structures = discover_topology_structures(topology)
            verification = verify_topology_structures(topology, structures)
            compiled = build_topology_route_config(self, verification, routing_mode)
            if compiled is None:
                compiled = build_direct_shortest_route_config(
                    self,
                    verification,
                    routing_mode,
                )
        else:
            baseline = build_route_lookups(self, path_finding_algo=path_finding_algo)

        if routing_mode == "table":
            split = split_route_lookups(baseline, {})
            compiled = compile_route_config(split)
        elif not use_direct_shortest:
            topology = self.get_physical_topology()
            structures = discover_topology_structures(topology)
            verification = verify_topology_structures(topology, structures)
            rule_candidates = build_rule_route_lookups(
                self,
                verification,
                path_finding_algo=path_finding_algo,
            )
            split = split_route_lookups(baseline, rule_candidates)
            if routing_mode == "rule" and split.table_lookups:
                raise ValueError(
                    "rule routing requires every route lookup to pass equivalence verification"
                )
            compiled = compile_route_config(split)
        if write_file:
            os.makedirs(self.output_dir, exist_ok=True)
            table_path = os.path.join(self.output_dir, "routing_table.csv")
            table_rows = self.compressed_route_rows(
                compiled.table_lookups,
                host_router=host_router,
            )
            with open(table_path, "w", encoding="utf-8", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(["nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"])
                writer.writerows(table_rows)

            port_group_path = os.path.join(self.output_dir, "routing_port_groups.csv")
            with open(port_group_path, "w", encoding="utf-8", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(["nodeId", "groupId", "ports"])
                for group in compiled.port_groups:
                    writer.writerow([
                        group.node_id,
                        group.group_id,
                        " ".join(map(str, group.ports)),
                    ])

            rule_path = os.path.join(self.output_dir, "routing_rules.csv")
            with open(rule_path, "w", encoding="utf-8", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "nodeId",
                    "dstNodeId",
                    "dstPortId",
                    "shortestPortGroups",
                    "nonShortestPortGroups",
                ])
                for rule in compiled.routing_rules:
                    writer.writerow([
                        rule.current_node_id,
                        rule.destination_node_id,
                        rule.destination_port_id,
                        " ".join(map(str, rule.shortest_port_groups)),
                        " ".join(map(str, rule.non_shortest_port_groups)),
                    ])

            range_rule_path = os.path.join(self.output_dir, "routing_rule_ranges.csv")
            with open(range_rule_path, "w", encoding="utf-8", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "nodeId",
                    "dstStartNodeId",
                    "dstEndNodeId",
                    "dstPortId",
                    "shortestPortGroups",
                    "nonShortestPortGroups",
                ])
                for rule in compiled.routing_range_rules:
                    writer.writerow([
                        rule.current_node_id,
                        rule.destination_start_node_id,
                        rule.destination_end_node_id,
                        rule.destination_port_id,
                        " ".join(map(str, rule.shortest_port_groups)),
                        " ".join(map(str, rule.non_shortest_port_groups)),
                    ])

            topology_rule_path = os.path.join(self.output_dir, "routing_topology_rules.csv")
            with open(topology_rule_path, "w", encoding="utf-8", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "ruleId",
                    "ruleType",
                    "shape",
                    "connections",
                    "nodeId",
                    "nodeType",
                    "coordinates",
                ])
                for rule_id, rule in enumerate(compiled.topology_rules):
                    for node in rule.nodes:
                        writer.writerow([
                            rule_id,
                            rule.rule_type,
                            " ".join(map(str, rule.shape)),
                            " ".join(rule.connections),
                            node.node_id,
                            node.node_type,
                            " ".join(map(str, node.coordinates)),
                        ])

        self._route_table_requires_on_demand_tp = True
        return compiled

    def prepare_structured_case(self, case_path, **kwargs):
        """Compile verified routing into a reusable structured simulation directory."""
        from structured_case import prepare_structured_case

        return prepare_structured_case(self, case_path, **kwargs)

    # 实际写出 CSV，并填充 route_dict4tp 供 ns3_config 使用
    def write_route_table_to_csv(self, host_router=False):
        route_table = self.route_table_2port
        route_dict = defaultdict(list)  # (node_id, dst_id, dst_port) -> [(out_port, metric), ...]

        # 添加进度条显示路由表处理进度
        for node_id in tqdm(self.nodes, desc="处理路由表中"):
            if self.node_is_host(node_id) and not host_router:
                continue
            for dst_id in range(len(route_table[node_id])):
                next_ports_metric_sets = sorted(route_table[node_id][dst_id])
                if not next_ports_metric_sets:
                    continue
                for p_m in next_ports_metric_sets:
                    # 写 CSV 需要聚合到 (node_id,dst_id,dst_port)
                    route_dict[(node_id, dst_id, p_m[-1])].append((p_m[0], p_m[1]))
                    # 同时保留给 ns3 tpn 映射
                    self.route_dict4tp[(node_id, dst_id)].append(p_m)

        os.makedirs(self.output_dir, exist_ok=True)
        filename = os.path.join(self.output_dir, "routing_table.csv")
        with open(filename, "w", encoding="utf-8") as f:
            f.write("nodeId,dstNodeId,dstPortId,outPorts,metrics\n")
            for key in tqdm(route_dict, desc="写入路由CSV"):
                outports = " ".join(str(x[0]) for x in route_dict[key])
                metrics = " ".join(str(x[1]) for x in route_dict[key])
                f.write(",".join(map(str, list(key) + [outports, metrics])) + "\n")
                
        print(f"路由表文件: {os.path.abspath(filename)}")
        
        check_route_table_connectivity(os.path.abspath(filename))

    def gen_2layer_clos_compressed_route_table(self, host_num, leaf_sw_num, write_file=True):
        """
        生成两层 Clos 的压缩路由表。

        节点编号必须符合仓库模板：
        - host: 0..host_num-1
        - leaf: host_num..host_num+leaf_sw_num-1
        - spine: host_num+leaf_sw_num..host_num+leaf_sw_num+spine_sw_num-1

        输出格式复用 routing_table.csv，但 dstNodeId 支持 a..b 范围。
        该路径避免枚举所有 host-pair shortest paths。
        """
        if host_num <= 0 or leaf_sw_num <= 0:
            raise ValueError("host_num and leaf_sw_num must be positive")
        if host_num % leaf_sw_num != 0:
            raise ValueError("host_num must be divisible by leaf_sw_num for 2-layer Clos")
        if self.next_hop_ports == []:
            self.build_graph_config()

        spine_sw_num = host_num // leaf_sw_num
        host_per_leaf = host_num // leaf_sw_num
        leaf_base = host_num
        spine_base = host_num + leaf_sw_num
        expected_total = host_num + leaf_sw_num + spine_sw_num
        if self.get_total_num() != expected_total:
            raise ValueError(
                f"graph node count {self.get_total_num()} does not match 2-layer Clos expected {expected_total}"
            )

        def ports_to_space(port_values):
            return " ".join(str(port) for port in port_values)

        def metrics_to_space(metric_values):
            return " ".join(str(metric) for metric in metric_values)

        rows = []

        # Host: any non-self destination leaves via its single uplink to leaf.
        for host_id in range(host_num):
            leaf_idx = host_id // host_per_leaf
            leaf_id = leaf_base + leaf_idx
            local_start = leaf_idx * host_per_leaf
            local_end = local_start + host_per_leaf - 1
            uplink_ports = self.get_link_ports(host_id, leaf_id)
            if local_start > 0:
                rows.append((host_id, f"0..{local_start - 1}", 0, ports_to_space(uplink_ports), metrics_to_space([4] * len(uplink_ports))))
            if local_start < host_id:
                rows.append((host_id, f"{local_start}..{host_id - 1}", 0, ports_to_space(uplink_ports), metrics_to_space([2] * len(uplink_ports))))
            if host_id < local_end:
                rows.append((host_id, f"{host_id + 1}..{local_end}", 0, ports_to_space(uplink_ports), metrics_to_space([2] * len(uplink_ports))))
            if local_end + 1 < host_num:
                rows.append((host_id, f"{local_end + 1}..{host_num - 1}", 0, ports_to_space(uplink_ports), metrics_to_space([4] * len(uplink_ports))))

        # Leaf: local hosts direct, remote hosts through all spines.
        for leaf_idx in range(leaf_sw_num):
            leaf_id = leaf_base + leaf_idx
            local_start = leaf_idx * host_per_leaf
            local_end = local_start + host_per_leaf - 1
            for host_id in range(local_start, local_end + 1):
                local_ports = self.get_link_ports(leaf_id, host_id)
                rows.append((leaf_id, host_id, 0, ports_to_space(local_ports), metrics_to_space([1] * len(local_ports))))

            spine_ports = []
            for spine_idx in range(spine_sw_num):
                spine_id = spine_base + spine_idx
                spine_ports.extend(self.get_link_ports(leaf_id, spine_id))
            if local_start > 0:
                rows.append((leaf_id, f"0..{local_start - 1}", 0, ports_to_space(spine_ports), metrics_to_space([3] * len(spine_ports))))
            if local_end + 1 < host_num:
                rows.append((leaf_id, f"{local_end + 1}..{host_num - 1}", 0, ports_to_space(spine_ports), metrics_to_space([3] * len(spine_ports))))

        # Spine: each host range descends to its owning leaf.
        for spine_idx in range(spine_sw_num):
            spine_id = spine_base + spine_idx
            for leaf_idx in range(leaf_sw_num):
                leaf_id = leaf_base + leaf_idx
                host_start = leaf_idx * host_per_leaf
                host_end = host_start + host_per_leaf - 1
                down_ports = self.get_link_ports(spine_id, leaf_id)
                rows.append((spine_id, f"{host_start}..{host_end}", 0, ports_to_space(down_ports), metrics_to_space([2] * len(down_ports))))

        if write_file:
            os.makedirs(self.output_dir, exist_ok=True)
            filename = os.path.join(self.output_dir, "routing_table.csv")
            with open(filename, "w", encoding="utf-8") as f:
                f.write("nodeId,dstNodeId,dstPortId,outPorts,metrics\n")
                for row in rows:
                    f.write(",".join(map(str, row)) + "\n")
            print(f"压缩路由表文件: {os.path.abspath(filename)}")
            validate_route_table_no_conflicts(os.path.abspath(filename))
            check_route_table_connectivity(os.path.abspath(filename))

        self._route_table_requires_on_demand_tp = True
        return rows

    def write_config(self, include_transport=True):
        print("开始生成配置文件...")
        os.makedirs(self.output_dir, exist_ok=True)
        
        # 一次性构建所有文件路径
        files = {
            'node': os.path.join(self.output_dir, "node.csv"),
            'transport': os.path.join(self.output_dir, "transport_channel.csv"),
            'topology': os.path.join(self.output_dir, "topology.csv")
        }

        # 生成 transport_channel.csv
        if include_transport:
            self._should_write_transport_channel()
            print("生成TP Channel配置...")
            self._write_transport_channel_csv(files['transport'])
        
        # 生成 node.csv - 优化范围合并逻辑
        print("生成节点配置...")
        self._write_node_csv(files['node'])
        
        # 生成 topology.csv
        print("生成拓扑配置...")
        self._write_topology_csv(files['topology'])
        
        print("\n配置文件生成完成!")
        print(f"输出目录: {os.path.abspath(self.output_dir)}")

    def config_transport_channel(self, priority_list: List[int]):
        self.priority_list = priority_list
    
    def _write_transport_channel_csv(self, filepath):
        """
        生成TP Channel配置文件
    
        此方法为每对host节点之间的每条路由和每个优先级组合生成transport channel配置。
        每个transport channel代表一条具体的通信路径，包含源端口、目标端口、优先级和度量信息。
    
        Parameters
        ----------
        filepath : str
            输出CSV文件的完整路径
            生成的文件包含transport channel的详细配置信息
        
        Notes
        -----
        - 文件格式: nodeId1,portId1,tpn1,nodeId2,portId2,tpn2,priority,metric
        - 只为host节点之间生成transport channel（不包括交换机节点）
        - 每对host节点的每条路由会为每个配置的优先级生成一个channel
        - TPN(Transport Point Number)会为每个节点自动递增分配
        - 使用 `self.route_dict4tp` 中存储的路由信息
        - 优先级列表通过 `config_transport_channel()` 方法配置，默认为[7, 8]
    
        数据来源
        --------
        - route_dict4tp : 包含 (host_a, host_b) -> [(src_port, metric, dst_port)] 映射
        - port_list : 用于识别host节点数量
        - priority_list : 配置的优先级列表
    
        输出文件结构
        -----------
        - nodeId1: 源host节点ID
        - portId1: 源节点的出端口ID
        - tpn1: 源节点的transport point number
        - nodeId2: 目标host节点ID  
        - portId2: 目标节点的入端口ID
        - tpn2: 目标节点的transport point number
        - priority: 传输优先级
        - metric: 路由度量值
    
        Examples
        --------
        >>> # 配置优先级并生成transport channel文件
        >>> graph.config_transport_channel([7, 8, 9])
        >>> graph._write_transport_channel_csv("./output/transport_channel.csv")
        写入TP Channel: 100%|██████████| 12/12 [00:00<00:00, 1234.56it/s]
        TP Channel: /path/to/output/transport_channel.csv
    
        See Also
        --------
        config_transport_channel : 配置优先级列表
        gen_route_table : 生成路由表数据
        ns3_config : 完整的ns-3配置文件生成流程
        """
        host_num = len([x for x in self.port_list if x[0] == 0])
        tpn_counters = defaultdict(int)
        # 使用配置的优先级列表，如果没有配置则使用默认值
        priority_list = getattr(self, 'priority_list', [7, 8])
        
        with open(filepath, "w", newline="", encoding="utf-8") as f:
            f.write("nodeId1,portId1,tpn1,nodeId2,portId2,tpn2,priority,metric\n")
            
            # 计算总的写入行数用于进度条
            total_combinations = (host_num * (host_num - 1) // 2) * len(priority_list)
            with tqdm(total=total_combinations, desc="写入TP Channel") as pbar:
                for host_a in range(host_num - 1):
                    for host_b in range(host_a + 1, host_num):
                        for priority in priority_list:
                            for src_port, metric, dst_port in self.route_dict4tp[(host_a, host_b)]:
                                row = (host_a, src_port, tpn_counters[host_a],
                                       host_b, dst_port, tpn_counters[host_b],
                                       priority, metric)
                                f.write(",".join(map(str, row)) + "\n")
                                tpn_counters[host_a] += 1
                                tpn_counters[host_b] += 1
                            pbar.update(1)
        print(f"TP Channel: {os.path.abspath(filepath)}")

    def _write_node_csv(self, filepath):
        """生成节点配置文件，优化连续范围合并"""
        map_dict = {0: "DEVICE", 1: "SWITCH"}
        
        with open(filepath, "w", newline="", encoding="utf-8") as f:
            f.write("nodeId,nodeType,portNum,allocationDelay,forwardDelay,systemId\n")

            node_rows = []
            for node_id, (node_type, port_num) in enumerate(self.port_list):
                data = self.nodes[node_id]
                node_rows.append((
                    node_type,
                    port_num,
                    data["allocation_delay"],
                    data["forward_delay"],
                    data.get("system_id", 0),
                ))

            # 仅合并节点类型、端口数和时延属性完全相同的连续节点。
            for key, group_iter in groupby(enumerate(node_rows), key=lambda x: x[1]):
                node_indices = [idx for idx, _ in group_iter]
                node_type, port_num, allocation_delay, forward_delay, system_id = key
                
                # 将连续的节点ID合并为范围
                ranges = self._get_consecutive_ranges(node_indices)
                
                for range_start, range_end in ranges:
                    if range_start == range_end:
                        node_id_str = str(range_start)
                    else:
                        node_id_str = f"{range_start}..{range_end}"

                    f.write(
                        f"{node_id_str},{map_dict[node_type]},{port_num},"
                        f"{allocation_delay},{forward_delay},{system_id}\n"
                    )
        print(f"节点配置: {os.path.abspath(filepath)}")
        
    def _get_consecutive_ranges(self, indices):
        """将连续的索引列表转换为范围元组列表"""
        if not indices:
            return []
        
        ranges = []
        start = indices[0]
        prev = indices[0]
        
        for curr in indices[1:] + [None]:  # 添加None作为结束标志
            if curr is None or curr != prev + 1:
                ranges.append((start, prev))
                if curr is not None:
                    start = curr
            if curr is not None:
                prev = curr
        
        return ranges

    def _write_topology_csv(self, filepath):
        """生成拓扑连接配置文件"""
        with open(filepath, "w", newline="", encoding="utf-8") as f:
            f.write("nodeId1,portId1,nodeId2,portId2,bandwidth,delay\n")
            for u, u_port, v, v_port, bandwidth, delay in tqdm(self.link_infos, desc="写入拓扑连接"):
                f.write(f"{u},{u_port},{v},{v_port},{bandwidth},{delay}\n")
                
        print(f"拓扑配置: {os.path.abspath(filepath)}")

def check_route_table_connectivity(route_table_csv_path):
    """
    检查路由表文件中每个节点是否都包含到所有host节点的路由
    
    此函数读取路由表CSV文件，验证每个节点（包括host和switch）是否都有
    到达所有host节点的路由表项。确保网络连通性。
    
    Parameters
    ----------
    route_table_csv_path : str
        路由表CSV文件路径
        文件格式应为: nodeId,dstNodeId,dstPortId,outPorts,metrics
        
    Returns
    -------
    bool
        如果所有节点都有完整的host路由则返回True，否则返回False
        
    Raises
    ------
    FileNotFoundError
        当指定的路由表文件不存在时
    ValueError
        当CSV文件格式不正确时
        
    Notes
    -----
    - 函数会打印详细的检查结果和统计信息
    - 对于缺失路由的情况，会输出具体的缺失信息
    - host节点不需要到自己的路由（自环路由）
    
    Examples
    --------
    >>> check_all_hosts_routes("/path/to/route_table.csv")
    开始检查路由表完整性...
    路由表检查完成: ✓ 所有节点都有完整的host路由
    True
    """
    from collections import defaultdict
    
    if not os.path.exists(route_table_csv_path):
        raise FileNotFoundError(f"路由表文件不存在: {route_table_csv_path}")
    
    print(f"开始检查路由表完整性...")
    print(f"路由表文件: {route_table_csv_path}")
    
    validate_route_table_no_conflicts(route_table_csv_path)

    # 读取路由表并统计。按源节点保存 dstNodeId 区间，避免把压缩路由完全展开。
    node_route_intervals = defaultdict(list)  # node_id -> [(dst_start, dst_end), ...]
    all_nodes = set()
    host_intervals = []
    
    try:
        for row in _read_route_table_rows(route_table_csv_path):
            host_intervals.append((row["dst_start"], row["dst_end"]))
            for node_id in range(row["node_start"], row["node_end"] + 1):
                all_nodes.add(node_id)
                node_route_intervals[node_id].append((row["dst_start"], row["dst_end"]))
    except Exception as e:
        raise ValueError(f"读取CSV文件时发生错误: {e}")

    host_intervals = _merge_intervals(host_intervals)
    
    # 统计信息
    total_nodes = len(all_nodes)
    total_hosts = sum(end - start + 1 for start, end in host_intervals)
    print(f"发现 {total_nodes} 个节点, {total_hosts} 个host节点")
    
    # 检查每个节点的路由完整性
    missing_routes = []
    complete_nodes = 0
    
    for node_id in sorted(all_nodes):
        # host节点不需要到自己的路由
        expected_intervals = (
            _remove_single_value_from_intervals(host_intervals, node_id)
            if any(start <= node_id <= end for start, end in host_intervals)
            else host_intervals
        )
        missing_hosts = _missing_intervals(expected_intervals, node_route_intervals[node_id])
        
        if not missing_hosts:
            complete_nodes += 1
        else:
            missing_routes.append((node_id, missing_hosts))
    
    # 输出检查结果
    print(f"\n路由完整性检查结果:")
    print(f"- 路由完整的节点: {complete_nodes}/{total_nodes}")
    print(f"- 路由不完整的节点: {len(missing_routes)}")
    
    if missing_routes:
        print(f"\n缺失路由详情:")
        for node_id, missing_hosts in missing_routes[:10]:  # 最多显示10个
            missing_list = [_format_range_value(start, end) for start, end in missing_hosts]
            missing_count = sum(end - start + 1 for start, end in missing_hosts)
            if len(missing_list) > 5:
                missing_str = f"{missing_list[:5]}... (共{missing_count}个)"
            else:
                missing_str = str(missing_list)
            print(f"  节点 {node_id}: 缺失到host {missing_str} 的路由")
        
        if len(missing_routes) > 10:
            print(f"  ... 还有 {len(missing_routes) - 10} 个节点存在路由缺失")
        
        print(f"\n路由表检查失败: {len(missing_routes)} 个节点缺失路由")
        return False
    else:
        print(f"\n路由表检查完成: 所有节点都有完整的host路由")
        return True
