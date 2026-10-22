#!/usr/bin/env python3
"""Interactive shell for the throwaway structured-routing prototype."""

from __future__ import annotations

import argparse
import random
import sys
import tempfile
import time
from pathlib import Path

import networkx as nx

sys.path.insert(0, str(Path(__file__).resolve().parent))
from structured_routing_logic import Analysis, analyze, build_route_items, expand_physical_links


BOLD = "\x1b[1m"
DIM = "\x1b[2m"
RESET = "\x1b[0m"


def add_host(graph: nx.Graph, node: int) -> None:
    graph.add_node(node, type="host", forward_delay="1ns")


def add_switch(graph: nx.Graph, node: int) -> None:
    graph.add_node(node, type="node", forward_delay="1ns")


def add_link(graph: nx.Graph, left: int, right: int, edge_count: int = 1) -> None:
    graph.add_edge(left, right, bandwidth="400Gbps", delay="20ns", edge_count=edge_count)


def add_dimension_fullmesh(graph: nx.Graph, nodes: list[int], rows: int, columns: int) -> None:
    assert len(nodes) == rows * columns
    for node in nodes:
        add_host(graph, node)
    for left_index, left in enumerate(nodes):
        left_coordinate = divmod(left_index, columns)
        for right_index, right in enumerate(nodes[left_index + 1:], start=left_index + 1):
            right_coordinate = divmod(right_index, columns)
            if left_coordinate[0] == right_coordinate[0] or left_coordinate[1] == right_coordinate[1]:
                add_link(graph, left, right)


def add_clos(graph: nx.Graph,
             hosts: list[int],
             leaves: list[int],
             spines: list[int],
             parallel_uplinks: int = 1) -> None:
    assert len(hosts) % len(leaves) == 0
    for host in hosts:
        if host not in graph:
            add_host(graph, host)
    for switch in leaves + spines:
        add_switch(graph, switch)
    hosts_per_leaf = len(hosts) // len(leaves)
    for index, host in enumerate(hosts):
        add_link(graph, host, leaves[index // hosts_per_leaf])
    for leaf in leaves:
        for spine in spines:
            add_link(graph, leaf, spine, edge_count=parallel_uplinks)


def dimension_fullmesh_4x4() -> nx.Graph:
    graph = nx.Graph()
    add_dimension_fullmesh(graph, list(range(16)), 4, 4)
    return graph


def two_layer_clos(parallel_uplinks: int = 1) -> nx.Graph:
    graph = nx.Graph()
    add_clos(graph, list(range(8)), [8, 9], [10, 11, 12, 13], parallel_uplinks)
    return graph


def clos_with_port_extension() -> nx.Graph:
    graph = nx.Graph()
    add_clos(graph, [0, 1, 2, 3], [4, 5], [6, 7])
    add_clos(graph, [10, 11, 12, 13], [14, 15], [16, 17])
    add_switch(graph, 20)
    for spine in [6, 7, 16, 17]:
        add_link(graph, spine, 20)
    return graph


def fullmesh_with_multi_plane_clos() -> nx.Graph:
    graph = nx.Graph()
    first_group = list(range(9))
    second_group = list(range(9, 18))
    add_dimension_fullmesh(graph, first_group, 3, 3)
    add_dimension_fullmesh(graph, second_group, 3, 3)

    for leaves, spines in [([20, 21], [22, 23]), ([30, 31], [32, 33])]:
        for switch in leaves + spines:
            add_switch(graph, switch)
        for host in first_group:
            add_link(graph, host, leaves[0])
        for host in second_group:
            add_link(graph, host, leaves[1])
        for leaf in leaves:
            for spine in spines:
                add_link(graph, leaf, spine)
    return graph


def shuffled_ids_and_edges() -> nx.Graph:
    source = fullmesh_with_multi_plane_clos()
    randomizer = random.Random(20260717)
    old_nodes = list(source.nodes)
    new_ids = randomizer.sample(range(100, 100 + len(old_nodes) * 10), len(old_nodes))
    mapping = dict(zip(old_nodes, new_ids))
    edges = list(source.edges(data=True))
    randomizer.shuffle(edges)

    graph = nx.Graph()
    shuffled_nodes = list(source.nodes(data=True))
    randomizer.shuffle(shuffled_nodes)
    for node, data in shuffled_nodes:
        graph.add_node(mapping[node], **data)
    for left, right, data in edges:
        graph.add_edge(mapping[left], mapping[right], **data)
    return graph


def missing_link() -> nx.Graph:
    graph = fullmesh_with_multi_plane_clos()
    graph.remove_edge(20, 22)
    return graph


def extra_link() -> nx.Graph:
    graph = fullmesh_with_multi_plane_clos()
    add_link(graph, 22, 23)
    return graph


def dimension_edge_swap() -> nx.Graph:
    graph = dimension_fullmesh_4x4()
    graph.remove_edge(0, 1)
    add_link(graph, 0, 5)
    return graph


CASES = {
    "dimension-fullmesh-4x4": dimension_fullmesh_4x4,
    "two-layer-clos": two_layer_clos,
    "parallel-link-clos": lambda: two_layer_clos(parallel_uplinks=2),
    "clos-with-port-extension": clos_with_port_extension,
    "fullmesh-with-multi-plane-clos": fullmesh_with_multi_plane_clos,
    "shuffled-ids-and-edges": shuffled_ids_and_edges,
    "missing-link": missing_link,
    "extra-link": extra_link,
    "dimension-edge-swap": dimension_edge_swap,
}


def render(analysis: Analysis, elapsed_ms: float, current_builder_match: bool | None) -> str:
    lines = [
        f"{BOLD}{analysis.case_name}{RESET}",
        f"  graph verified:       {analysis.graph_verified}",
        f"  analysis time:        {elapsed_ms:>7.2f} ms",
        f"  baseline route items: {analysis.baseline_items}",
        f"  equivalent rule items:{analysis.equivalent_items:>7}",
        f"  remaining table items:{analysis.remaining_table_items:>7}",
        f"  route coverage:       {analysis.route_coverage:>6.1%}",
        f"  local port groups:    {analysis.port_groups}",
        f"  extra rule items:     {analysis.extra_rule_items}",
        f"  uncovered nodes:      {len(analysis.uncovered_nodes)}",
        f"  uncovered edges:      {len(analysis.uncovered_edges)}",
        f"  duplicate edge owners:{len(analysis.duplicate_edges):>7}",
        f"  {BOLD}views{RESET}",
    ]
    if current_builder_match is not None:
        lines.insert(2, f"  current builder match:{str(current_builder_match):>7}")
    if analysis.views:
        for view in analysis.views:
            lines.append(
                f"    {view.name}: nodes={len(view.nodes)} edges={len(view.edges)} "
                f"compressible={view.compressible} parameters={view.parameters}"
            )
    else:
        lines.append("    none")
    if analysis.candidate_differences:
        lines.append(f"  {BOLD}candidate differences (first {len(analysis.candidate_differences)}){RESET}")
        for key, baseline, rules in analysis.candidate_differences:
            lines.append(f"    {key}: table={baseline} rules={rules}")
    return "\n".join(lines)


def _parse_range(value: object) -> range:
    text = str(value)
    if ".." not in text:
        number = int(text)
        return range(number, number + 1)
    start, end = (int(part) for part in text.split("..", 1))
    return range(start, end + 1)


def compare_current_builder(graph: nx.Graph) -> bool:
    tool_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(tool_root))
    import net_sim_builder as netsim

    current = netsim.NetworkSimulationGraph()
    for node, data in graph.nodes(data=True):
        if data["type"] == "host":
            current.add_netisim_host(node, forward_delay=data["forward_delay"])
        else:
            current.add_netisim_node(node, forward_delay=data["forward_delay"])
    for left, right, data in graph.edges(data=True):
        current.add_netisim_edge(left, right, **data)

    with tempfile.TemporaryDirectory(prefix="structured-routing-prototype-") as output_dir:
        current.output_dir = output_dir
        current.build_graph_config()
        rows = current.gen_compressed_route_table(write_file=False, multiple_workers=1)

    expanded = {}
    for node_range, destination_range, destination_port, outports, _metrics in rows:
        candidates = tuple(sorted(int(port) for port in str(outports).split()))
        for node in _parse_range(node_range):
            for destination in _parse_range(destination_range):
                expanded[(node, destination, int(destination_port))] = candidates
    prototype_baseline = build_route_items(graph, expand_physical_links(graph))
    return expanded == prototype_baseline


def run_case(name: str) -> str:
    graph = CASES[name]()
    started = time.perf_counter()
    analysis = analyze(name, graph)
    elapsed_ms = (time.perf_counter() - started) * 1000
    current_builder_match = None
    if name in {"dimension-fullmesh-4x4", "two-layer-clos", "parallel-link-clos"}:
        current_builder_match = compare_current_builder(graph)
    return render(analysis, elapsed_ms, current_builder_match)


def interactive() -> None:
    names = list(CASES)
    while True:
        print("\x1b[2J\x1b[H", end="")
        print(f"{BOLD}Structured-routing prototype{RESET}\n")
        for index, name in enumerate(names, start=1):
            print(f"  [{index}] {name}")
        print(f"\n  [a] run all  [q] quit\n{DIM}Select a case:{RESET} ", end="", flush=True)
        choice = input().strip().lower()
        if choice == "q":
            return
        selected = names if choice == "a" else []
        if choice.isdigit() and 1 <= int(choice) <= len(names):
            selected = [names[int(choice) - 1]]
        if not selected:
            continue
        print("\x1b[2J\x1b[H", end="")
        print("\n\n".join(run_case(name) for name in selected))
        input(f"\n{DIM}Press Enter to continue...{RESET}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true", help="run all built-in cases without interaction")
    parser.add_argument("--case", choices=CASES, help="run one built-in case")
    args = parser.parse_args()
    if args.all:
        print("\n\n".join(run_case(name) for name in CASES))
    elif args.case:
        print(run_case(args.case))
    else:
        interactive()


if __name__ == "__main__":
    main()
