import csv
from collections import defaultdict
from pathlib import Path

from net_sim_builder import NetworkSimulationGraph, parse_id_range


def _read_nodes(node_path: Path):
    nodes = {}
    with node_path.open(newline="", encoding="utf-8") as file:
        reader = csv.DictReader(file)
        legacy_forward_is_allocation = reader.fieldnames == [
            "nodeId", "nodeType", "portNum", "forwardDelay"
        ]
        for row_num, row in enumerate(reader, start=2):
            start, end = parse_id_range(row["nodeId"])
            node_type = row["nodeType"].strip().upper()
            if node_type not in {"DEVICE", "SWITCH"}:
                raise ValueError(f"{node_path}:{row_num}: unsupported nodeType {node_type}")
            if legacy_forward_is_allocation:
                allocation_delay = row.get("forwardDelay", "").strip()
                forward_delay = "0ns"
            else:
                allocation_delay = row.get("allocationDelay", "").strip() or "10ns"
                forward_delay = row.get("forwardDelay", "").strip() or "0ns"
            for node_id in range(start, end + 1):
                if node_id in nodes:
                    raise ValueError(f"{node_path}:{row_num}: duplicate node {node_id}")
                nodes[node_id] = {
                    "node_type": node_type,
                    "port_count": int(row["portNum"]),
                    "allocation_delay": allocation_delay,
                    "forward_delay": forward_delay,
                    "system_id": int(row.get("systemId", "") or 0),
                }
    if sorted(nodes) != list(range(len(nodes))):
        raise ValueError(f"{node_path}: node IDs must form a dense 0..N-1 range")
    return nodes


def _read_links(topology_path: Path):
    links = []
    with topology_path.open(newline="", encoding="utf-8") as file:
        for row_num, row in enumerate(csv.DictReader(file), start=2):
            try:
                links.append((
                    int(row["nodeId1"]),
                    int(row["portId1"]),
                    int(row["nodeId2"]),
                    int(row["portId2"]),
                    row["bandwidth"],
                    row["delay"],
                ))
            except (KeyError, ValueError) as error:
                raise ValueError(f"{topology_path}:{row_num}: invalid physical link") from error
    return links


def load_topology_files(node_path, topology_path) -> NetworkSimulationGraph:
    """Load ns-3 UB node and topology files without changing physical port IDs."""
    nodes = _read_nodes(Path(node_path))
    links = _read_links(Path(topology_path))

    graph = NetworkSimulationGraph()
    for node_id, node in nodes.items():
        add_node = (
            graph.add_netisim_host
            if node["node_type"] == "DEVICE"
            else graph.add_netisim_node
        )
        add_node(
            node_id,
            allocation_delay=node["allocation_delay"],
            forward_delay=node["forward_delay"],
            system_id=node["system_id"],
        )
    graph.build_graph_config_from_links(
        links,
        port_counts={node_id: node["port_count"] for node_id, node in nodes.items()},
    )
    return graph


def load_case_topology(case_dir) -> NetworkSimulationGraph:
    """Load node.csv and topology.csv from an ns-3 UB custom case."""
    case_path = Path(case_dir)
    return load_topology_files(case_path / "node.csv", case_path / "topology.csv")


def load_case_route_lookups(case_dir, lookup_filter=None):
    """Read compressed route rows into the lookup representation used by route splitting."""
    route_path = Path(case_dir) / "routing_table.csv"
    lookups = defaultdict(dict)
    lookup_filter = None if lookup_filter is None else set(lookup_filter)
    with route_path.open(newline="", encoding="utf-8") as file:
        for row_num, row in enumerate(csv.DictReader(file), start=2):
            node_start, node_end = parse_id_range(row["nodeId"])
            dst_start, dst_end = parse_id_range(row["dstNodeId"])
            port_start, port_end = parse_id_range(row["dstPortId"])
            out_ports = tuple(int(value) for value in row["outPorts"].split())
            metrics = tuple(int(value) for value in row["metrics"].split())
            if len(out_ports) != len(metrics):
                raise ValueError(
                    f"{route_path}:{row_num}: outPorts and metrics must have equal lengths"
                )
            if (
                lookup_filter is not None
                and node_start == node_end
                and dst_start == dst_end
                and port_start == port_end
            ):
                row_lookups = ((node_start, dst_start, port_start),)
            else:
                row_lookups = (
                    (node_id, dst_id, dst_port)
                    for node_id in range(node_start, node_end + 1)
                    for dst_id in range(dst_start, dst_end + 1)
                    for dst_port in range(port_start, port_end + 1)
                )
            for lookup in row_lookups:
                if lookup_filter is None or lookup in lookup_filter:
                    for out_port, metric in zip(out_ports, metrics):
                        old_metric = lookups[lookup].get(out_port)
                        if old_metric is None or metric < old_metric:
                            lookups[lookup][out_port] = metric
    return {lookup: dict(port_metrics) for lookup, port_metrics in lookups.items()}
