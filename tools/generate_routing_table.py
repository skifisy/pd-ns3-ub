#!/usr/bin/env python3
"""Generate ns-3-UB routing_table.csv from node.csv and topology.csv.

The generator keeps every equal-cost shortest-path candidate, preserves parallel
physical links as distinct outPorts, and keeps destination-port-aware routing
keys. It only depends on the target case's node.csv and topology.csv.
"""

import argparse
import csv
import sys
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CASE_DIR = (
    PROJECT_ROOT / "ns-3-ub" / "scratch" / "mooncake_pd_storage_ocs_topology"
)


@dataclass(frozen=True)
class NodeSpec:
    node_type: str
    port_num: int


@dataclass(frozen=True)
class Link:
    peer: int
    local_port: int
    peer_port: int


def expand_node_ids(value: str):
    """Expand either an integer node ID or an inclusive ``a..b`` range."""
    text = value.strip()
    if not text:
        raise ValueError("empty nodeId")

    if ".." not in text:
        return (int(text),)

    parts = text.split("..")
    if len(parts) != 2:
        raise ValueError(f"invalid nodeId range: {value!r}")

    start, end = (int(part.strip()) for part in parts)
    if start > end:
        raise ValueError(f"invalid descending nodeId range: {value!r}")
    return range(start, end + 1)


def read_nodes(path: Path):
    required = {"nodeId", "nodeType", "portNum"}
    nodes = {}

    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise ValueError(f"{path}: empty CSV or missing header")

        missing = required - set(reader.fieldnames)
        if missing:
            raise ValueError(f"{path}: missing columns: {sorted(missing)}")

        for row_number, row in enumerate(reader, start=2):
            try:
                node_type = row["nodeType"].strip().upper()
                if node_type not in {"DEVICE", "SWITCH"}:
                    raise ValueError(
                        f"nodeType must be DEVICE or SWITCH, got {row['nodeType']!r}"
                    )

                port_num = int(row["portNum"])
                if port_num <= 0:
                    raise ValueError(f"portNum must be positive, got {port_num}")

                for node_id in expand_node_ids(row["nodeId"]):
                    if node_id in nodes:
                        raise ValueError(f"duplicate nodeId {node_id}")
                    nodes[node_id] = NodeSpec(node_type=node_type, port_num=port_num)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{path}:{row_number}: {exc}") from exc

    if not nodes:
        raise ValueError(f"{path}: no nodes found")

    return nodes


def read_topology(path: Path, nodes):
    required = {"nodeId1", "portId1", "nodeId2", "portId2"}
    adjacency = {node_id: [] for node_id in nodes}
    used_ports = set()

    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise ValueError(f"{path}: empty CSV or missing header")

        missing = required - set(reader.fieldnames)
        if missing:
            raise ValueError(f"{path}: missing columns: {sorted(missing)}")

        for row_number, row in enumerate(reader, start=2):
            try:
                node_a = int(row["nodeId1"])
                port_a = int(row["portId1"])
                node_b = int(row["nodeId2"])
                port_b = int(row["portId2"])

                if node_a == node_b:
                    raise ValueError(f"self-link on node {node_a}")
                if node_a not in nodes or node_b not in nodes:
                    raise ValueError(
                        f"link references unknown node(s): {node_a}, {node_b}"
                    )

                for node_id, port_id in ((node_a, port_a), (node_b, port_b)):
                    if not 0 <= port_id < nodes[node_id].port_num:
                        raise ValueError(
                            f"port {port_id} out of range for node {node_id} "
                            f"(portNum={nodes[node_id].port_num})"
                        )
                    endpoint = (node_id, port_id)
                    if endpoint in used_ports:
                        raise ValueError(
                            f"physical port reused: node {node_id}, port {port_id}"
                        )
                    used_ports.add(endpoint)

                adjacency[node_a].append(
                    Link(peer=node_b, local_port=port_a, peer_port=port_b)
                )
                adjacency[node_b].append(
                    Link(peer=node_a, local_port=port_b, peer_port=port_a)
                )
            except (TypeError, ValueError) as exc:
                raise ValueError(f"{path}:{row_number}: {exc}") from exc

    for links in adjacency.values():
        links.sort(key=lambda link: (link.peer, link.local_port, link.peer_port))

    return adjacency


def shortest_distances(destination: int, adjacency):
    distances = {destination: 0}
    queue = deque([destination])

    while queue:
        current = queue.popleft()
        next_distance = distances[current] + 1

        for link in adjacency[current]:
            if link.peer in distances:
                continue
            distances[link.peer] = next_distance
            queue.append(link.peer)

    return distances


def routes_to_destination(destination: int, adjacency):
    """Return shortest-hop distance and destination-port-aware route candidates."""
    distances = shortest_distances(destination, adjacency)
    reachable = defaultdict(lambda: defaultdict(set))

    # Seed the reverse shortest-path DAG from every physical port on the
    # destination device. This preserves multi-port endpoint semantics.
    for current, distance in distances.items():
        if distance != 1:
            continue
        for link in adjacency[current]:
            if link.peer == destination:
                reachable[current][link.peer_port].add(link.local_port)

    nodes_by_distance = defaultdict(list)
    for node_id, distance in distances.items():
        nodes_by_distance[distance].append(node_id)

    max_distance = max(distances.values(), default=0)
    for distance in range(2, max_distance + 1):
        for current in sorted(nodes_by_distance[distance]):
            for link in adjacency[current]:
                if distances.get(link.peer) != distance - 1:
                    continue

                for destination_port in sorted(reachable.get(link.peer, {})):
                    reachable[current][destination_port].add(link.local_port)

    return distances, reachable


def resolve_case_path(case_dir: Path, override, default_name: str) -> Path:
    if override is None:
        return case_dir / default_name
    expanded = override.expanduser()
    return expanded if expanded.is_absolute() else case_dir / expanded


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Generate ns-3-UB routing_table.csv from a case's node.csv and "
            "topology.csv. All equal-cost shortest-hop outports are retained."
        )
    )
    parser.add_argument(
        "case_dir",
        nargs="?",
        type=Path,
        default=DEFAULT_CASE_DIR,
        help=(
            "case directory containing node.csv and topology.csv "
            f"(default: {DEFAULT_CASE_DIR})"
        ),
    )
    parser.add_argument(
        "--node-file",
        type=Path,
        default=None,
        help="node CSV path, absolute or relative to case_dir (default: node.csv)",
    )
    parser.add_argument(
        "--topology-file",
        type=Path,
        default=None,
        help=(
            "topology CSV path, absolute or relative to case_dir "
            "(default: topology.csv)"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help=(
            "output path, absolute or relative to case_dir "
            "(default: routing_table.csv)"
        ),
    )
    return parser.parse_args()


def main():
    args = parse_args()
    case_dir = args.case_dir.expanduser().resolve()

    node_path = resolve_case_path(case_dir, args.node_file, "node.csv")
    topology_path = resolve_case_path(case_dir, args.topology_file, "topology.csv")
    output_path = resolve_case_path(case_dir, args.output, "routing_table.csv")

    missing = [path for path in (node_path, topology_path) if not path.is_file()]
    if missing:
        missing_text = ", ".join(str(path) for path in missing)
        raise SystemExit(
            "Missing input file(s): "
            f"{missing_text}\n"
            "Generate/copy node.csv and topology.csv first. For the default "
            "Mooncake OCS case, run: python3 tools/generate_ocs_topology.py"
        )

    try:
        nodes = read_nodes(node_path)
        adjacency = read_topology(topology_path, nodes)
    except ValueError as exc:
        raise SystemExit(f"Routing-table generation failed: {exc}") from exc

    destinations = sorted(
        node_id for node_id, spec in nodes.items() if spec.node_type == "DEVICE"
    )
    if not destinations:
        raise SystemExit(f"{node_path}: no DEVICE nodes found")

    output_path.parent.mkdir(parents=True, exist_ok=True)

    rows = []
    unreachable_pairs = 0

    for destination in destinations:
        distances, reachable = routes_to_destination(destination, adjacency)
        unreachable_pairs += sum(
            1
            for node_id in nodes
            if node_id != destination and node_id not in distances
        )

        for current in sorted(reachable):
            if current == destination:
                continue

            metric = distances[current]
            for destination_port in sorted(reachable[current]):
                out_ports = sorted(reachable[current][destination_port])
                if not out_ports:
                    continue

                rows.append(
                    [
                        current,
                        destination,
                        destination_port,
                        " ".join(str(port) for port in out_ports),
                        " ".join(str(metric) for _ in out_ports),
                    ]
                )

    rows.sort(key=lambda row: (row[0], row[1], row[2]))

    with output_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(
            ["nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"]
        )
        writer.writerows(rows)

    print(f"Generated: {output_path}")
    print(f"  DEVICE destinations: {len(destinations)}")
    print(f"  Route rows: {len(rows)}")

    if unreachable_pairs:
        print(
            "WARNING: "
            f"{unreachable_pairs} source/destination node pairs are disconnected "
            "and were omitted.",
            file=sys.stderr,
        )


if __name__ == "__main__":
    main()
