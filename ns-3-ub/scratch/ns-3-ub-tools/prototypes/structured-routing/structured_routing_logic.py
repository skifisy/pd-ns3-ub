"""PROTOTYPE ONLY: pure structure discovery and verification logic."""

from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field
from typing import Hashable

import networkx as nx


Node = Hashable
Edge = frozenset[Node]
RouteItem = tuple[Node, Node, int]


@dataclass(frozen=True)
class PhysicalLink:
    left: Node
    left_port: int
    right: Node
    right_port: int

    def edge(self) -> Edge:
        return frozenset((self.left, self.right))

    def peer(self, node: Node) -> tuple[Node, int, int]:
        if node == self.left:
            return self.right, self.left_port, self.right_port
        if node == self.right:
            return self.left, self.right_port, self.left_port
        raise ValueError(f"node {node!r} is not on link")


@dataclass
class StructureView:
    kind: str
    name: str
    nodes: set[Node]
    edges: set[Edge]
    compressible: bool
    parameters: dict[str, object] = field(default_factory=dict)
    coordinates: dict[Node, tuple[int, ...]] = field(default_factory=dict)


@dataclass
class Analysis:
    case_name: str
    views: list[StructureView]
    graph_verified: bool
    uncovered_nodes: set[Node]
    uncovered_edges: set[Edge]
    duplicate_edges: set[Edge]
    baseline_items: int
    equivalent_items: int
    remaining_table_items: int
    extra_rule_items: int
    candidate_differences: list[tuple[RouteItem, tuple[int, ...], tuple[int, ...]]]
    port_groups: int

    @property
    def route_coverage(self) -> float:
        if self.baseline_items == 0:
            return 1.0
        return self.equivalent_items / self.baseline_items


def edge_key(left: Node, right: Node) -> Edge:
    return frozenset((left, right))


def _is_clique(graph: nx.Graph) -> bool:
    node_count = graph.number_of_nodes()
    return graph.number_of_edges() == node_count * (node_count - 1) // 2


def _edge_attrs(graph: nx.Graph, left: Node, right: Node) -> tuple[object, ...]:
    data = graph.edges[left, right]
    return (data.get("bandwidth"), data.get("delay"), int(data.get("edge_count", 1)))


def expand_physical_links(graph: nx.Graph) -> list[PhysicalLink]:
    """Expand edge_count and assign ports exactly once from the input edge order."""
    next_port: dict[Node, int] = defaultdict(int)
    links: list[PhysicalLink] = []
    for left, right, data in graph.edges(data=True):
        for _ in range(int(data.get("edge_count", 1))):
            links.append(PhysicalLink(left, next_port[left], right, next_port[right]))
            next_port[left] += 1
            next_port[right] += 1
    return links


def _recognize_dimension_fullmesh(component: nx.Graph, index: int) -> StructureView | None:
    if component.number_of_nodes() < 4 or not nx.is_connected(component):
        return None

    maximal_cliques = [set(clique) for clique in nx.find_cliques(component) if len(clique) >= 2]
    if not maximal_cliques:
        return None

    edge_owners: dict[Edge, int] = defaultdict(int)
    node_cliques: dict[Node, list[int]] = defaultdict(list)
    for clique_index, clique in enumerate(maximal_cliques):
        for node in clique:
            node_cliques[node].append(clique_index)
        ordered = list(clique)
        for left_index, left in enumerate(ordered):
            for right in ordered[left_index + 1:]:
                edge_owners[edge_key(left, right)] += 1

    if any(edge_owners[edge_key(left, right)] != 1 for left, right in component.edges):
        return None
    if any(len(node_cliques[node]) != 2 for node in component.nodes):
        return None

    clique_intersections = nx.Graph()
    clique_intersections.add_nodes_from(range(len(maximal_cliques)))
    for left in range(len(maximal_cliques)):
        for right in range(left + 1, len(maximal_cliques)):
            overlap = maximal_cliques[left] & maximal_cliques[right]
            if len(overlap) > 1:
                return None
            if overlap:
                clique_intersections.add_edge(left, right)

    disjoint_graph = nx.complement(clique_intersections)
    dimensions = [set(group) for group in nx.connected_components(disjoint_graph)]
    if len(dimensions) != 2:
        return None
    if any(not _is_clique(disjoint_graph.subgraph(group)) for group in dimensions):
        return None
    if any(maximal_cliques[left] & maximal_cliques[right]
           for group in dimensions
           for left in group
           for right in group
           if left < right):
        return None
    if any(len(maximal_cliques[left] & maximal_cliques[right]) != 1
           for left in dimensions[0]
           for right in dimensions[1]):
        return None

    first = sorted(dimensions[0])
    second = sorted(dimensions[1])
    first_pos = {clique: position for position, clique in enumerate(first)}
    second_pos = {clique: position for position, clique in enumerate(second)}
    coordinates = {}
    for node, memberships in node_cliques.items():
        first_membership = next((value for value in memberships if value in first_pos), None)
        second_membership = next((value for value in memberships if value in second_pos), None)
        if first_membership is None or second_membership is None:
            return None
        coordinates[node] = (first_pos[first_membership], second_pos[second_membership])

    expected = {
        edge_key(left, right)
        for left in component.nodes
        for right in component.nodes
        if left != right
        and sum(a != b for a, b in zip(coordinates[left], coordinates[right])) == 1
        and (coordinates[left][0] == coordinates[right][0]
             or coordinates[left][1] == coordinates[right][1])
    }
    actual = {edge_key(left, right) for left, right in component.edges}
    if expected != actual:
        return None

    return StructureView(
        kind="dimension-fullmesh-2d",
        name=f"dimension-fullmesh-2d:{index}",
        nodes=set(component.nodes),
        edges=actual,
        compressible=True,
        parameters={"shape": (len(first), len(second))},
        coordinates=coordinates,
    )


def recognize_dimension_fullmesh_groups(graph: nx.Graph) -> list[StructureView]:
    host_nodes = [node for node, data in graph.nodes(data=True) if data.get("type") == "host"]
    host_graph = graph.subgraph(host_nodes)
    views = []
    for index, nodes in enumerate(nx.connected_components(host_graph)):
        view = _recognize_dimension_fullmesh(host_graph.subgraph(nodes).copy(), index)
        if view is not None:
            views.append(view)
    return views


def recognize_two_layer_clos(graph: nx.Graph) -> list[StructureView]:
    switches = {node for node, data in graph.nodes(data=True) if data.get("type") == "node"}
    hosts = set(graph.nodes) - switches
    leaves = {switch for switch in switches if any(peer in hosts for peer in graph.neighbors(switch))}

    leaves_by_spines: dict[frozenset[Node], set[Node]] = defaultdict(set)
    for leaf in leaves:
        spine_neighbors = frozenset(peer for peer in graph.neighbors(leaf) if peer in switches - leaves)
        if spine_neighbors:
            leaves_by_spines[spine_neighbors].add(leaf)

    views = []
    for index, (spines_key, block_leaves) in enumerate(leaves_by_spines.items()):
        spines = set(spines_key)
        if len(block_leaves) < 2 or not spines:
            continue
        if any(not graph.has_edge(leaf, spine) for leaf in block_leaves for spine in spines):
            continue
        if any(graph.has_edge(left, right)
               for left in block_leaves
               for right in block_leaves
               if left != right):
            continue
        if any(graph.has_edge(left, right)
               for left in spines
               for right in spines
               if left != right):
            continue

        attached_hosts = {leaf: {peer for peer in graph.neighbors(leaf) if peer in hosts}
                          for leaf in block_leaves}
        if any(not group for group in attached_hosts.values()):
            continue
        host_counts = {len(group) for group in attached_hosts.values()}
        if len(host_counts) != 1:
            continue

        owned_edges = {edge_key(leaf, spine) for leaf in block_leaves for spine in spines}
        for leaf, leaf_hosts in attached_hosts.items():
            owned_edges.update(edge_key(leaf, host) for host in leaf_hosts)
        nodes = set().union(block_leaves, spines, *attached_hosts.values())
        boundaries = {
            peer
            for spine in spines
            for peer in graph.neighbors(spine)
            if peer not in block_leaves
        }
        views.append(StructureView(
            kind="two-layer-clos",
            name=f"two-layer-clos:{index}",
            nodes=nodes,
            edges=owned_edges,
            compressible=True,
            parameters={
                "leaf_count": len(block_leaves),
                "spine_count": len(spines),
                "hosts_per_leaf": next(iter(host_counts)),
                "boundary_switches": tuple(sorted(boundaries, key=repr)),
            },
        ))
    return views


def recognize_port_extensions(graph: nx.Graph, clos_views: list[StructureView]) -> list[StructureView]:
    clos_edges = set().union(*(view.edges for view in clos_views)) if clos_views else set()
    clos_nodes = set().union(*(view.nodes for view in clos_views)) if clos_views else set()
    switches = {node for node, data in graph.nodes(data=True) if data.get("type") == "node"}
    extension_edges = {
        edge_key(left, right)
        for left, right in graph.edges
        if edge_key(left, right) not in clos_edges
        and left in switches
        and right in switches
        and (left in clos_nodes or right in clos_nodes)
    }
    if not extension_edges:
        return []
    nodes = set().union(*extension_edges)
    return [StructureView(
        kind="port-extension",
        name="port-extension:0",
        nodes=nodes,
        edges=extension_edges,
        compressible=False,
        parameters={"link_count": len(extension_edges)},
    )]


def discover_structure(graph: nx.Graph) -> list[StructureView]:
    dimension_views = recognize_dimension_fullmesh_groups(graph)
    clos_views = recognize_two_layer_clos(graph)
    extension_views = recognize_port_extensions(graph, clos_views)
    return dimension_views + clos_views + extension_views


def verify_graph_coverage(graph: nx.Graph, views: list[StructureView]) -> tuple[bool, set[Node], set[Edge], set[Edge]]:
    edge_owners: dict[Edge, int] = defaultdict(int)
    covered_nodes = set()
    for view in views:
        covered_nodes.update(view.nodes)
        for edge in view.edges:
            edge_owners[edge] += 1

    actual_edges = {edge_key(left, right) for left, right in graph.edges}
    uncovered_nodes = set(graph.nodes) - covered_nodes
    uncovered_edges = actual_edges - set(edge_owners)
    duplicate_edges = {edge for edge, owners in edge_owners.items() if owners > 1}
    unknown_edges = set(edge_owners) - actual_edges

    # Attribute and parallel-link preservation is checked on every claimed physical edge.
    claimed_attrs = [_edge_attrs(graph, *tuple(edge)) for edge in edge_owners if edge in actual_edges]
    attrs_valid = all(bandwidth is not None and delay is not None and edge_count > 0
                      for bandwidth, delay, edge_count in claimed_attrs)
    verified = not uncovered_nodes and not uncovered_edges and not duplicate_edges and not unknown_edges and attrs_valid
    return verified, uncovered_nodes, uncovered_edges, duplicate_edges


def _links_by_pair(links: list[PhysicalLink]) -> dict[Edge, list[PhysicalLink]]:
    result: dict[Edge, list[PhysicalLink]] = defaultdict(list)
    for link in links:
        result[link.edge()].append(link)
    return result


def build_route_items(graph: nx.Graph, links: list[PhysicalLink]) -> dict[RouteItem, tuple[int, ...]]:
    """Build exact shortest-path output-port sets, including destination-port identity."""
    routes: dict[RouteItem, tuple[int, ...]] = {}
    links_by_pair = _links_by_pair(links)
    hosts = [node for node, data in graph.nodes(data=True) if data.get("type") == "host"]

    for destination in hosts:
        distances_to_destination = nx.single_source_shortest_path_length(graph, destination)
        ingress_links = [link for link in links if destination in (link.left, link.right)]
        for ingress_link in ingress_links:
            ingress, destination_port, _ = ingress_link.peer(destination)
            distances_to_ingress = nx.single_source_shortest_path_length(graph, ingress)
            for current in graph.nodes:
                if current == destination or current not in distances_to_destination:
                    continue
                if current not in distances_to_ingress:
                    continue
                if distances_to_ingress[current] + 1 != distances_to_destination[current]:
                    continue

                if current == ingress:
                    _, current_port, _ = ingress_link.peer(current)
                    candidates = {current_port}
                else:
                    next_distance = distances_to_ingress[current] - 1
                    next_hops = {
                        peer
                        for peer in graph.neighbors(current)
                        if distances_to_ingress.get(peer) == next_distance
                    }
                    candidates = set()
                    for peer in next_hops:
                        for link in links_by_pair[edge_key(current, peer)]:
                            _, current_port, _ = link.peer(current)
                            candidates.add(current_port)
                if candidates:
                    routes[(current, destination, destination_port)] = tuple(sorted(candidates))
    return routes


def analyze(case_name: str, graph: nx.Graph) -> Analysis:
    views = discover_structure(graph)
    graph_verified, uncovered_nodes, uncovered_edges, duplicate_edges = verify_graph_coverage(graph, views)
    physical_links = expand_physical_links(graph)
    baseline = build_route_items(graph, physical_links)

    structured_edges = set().union(
        *(view.edges for view in views if view.compressible)
    ) if views else set()
    rule_graph = nx.Graph()
    rule_graph.add_nodes_from(graph.nodes(data=True))
    for left, right, data in graph.edges(data=True):
        if edge_key(left, right) in structured_edges:
            rule_graph.add_edge(left, right, **data)
    rule_links = [link for link in physical_links if link.edge() in structured_edges]
    rules = build_route_items(rule_graph, rule_links)

    equivalent_keys = {key for key, value in baseline.items() if rules.get(key) == value}
    extra_rule_items = len(set(rules) - set(baseline))
    differences = [
        (key, baseline[key], rules.get(key, ()))
        for key in baseline
        if key not in equivalent_keys
    ][:8]
    port_groups = len({baseline[key] for key in equivalent_keys})
    return Analysis(
        case_name=case_name,
        views=views,
        graph_verified=graph_verified,
        uncovered_nodes=uncovered_nodes,
        uncovered_edges=uncovered_edges,
        duplicate_edges=duplicate_edges,
        baseline_items=len(baseline),
        equivalent_items=len(equivalent_keys),
        remaining_table_items=len(baseline) - len(equivalent_keys),
        extra_rule_items=extra_rule_items,
        candidate_differences=differences,
        port_groups=port_groups,
    )
