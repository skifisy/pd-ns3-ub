from collections import Counter, defaultdict
from itertools import combinations, product
from math import prod

import networkx as nx

from physical_topology import PhysicalLink, PhysicalTopology
from topology_structure import (
    DestinationCoverage,
    LocalPortGroup,
    StructureLink,
    StructureNode,
    TopologyStructure,
)


def _edge_key(left, right):
    return frozenset((left, right))


def _build_graph(topology: PhysicalTopology):
    graph = nx.Graph()
    graph.add_nodes_from((node.node_id, {"node_type": node.node_type}) for node in topology.nodes)
    graph.add_edges_from((link.left_node, link.right_node) for link in topology.links)
    return graph


def _infer_dimension_fullmesh_shape(component: nx.Graph) -> tuple[int, ...] | None:
    if not component or not nx.is_connected(component):
        return None
    degrees = {degree for _, degree in component.degree()}
    if len(degrees) != 1:
        return None

    anchor = min(component.nodes)
    neighbor_graph = component.subgraph(component.neighbors(anchor))
    dimension_sizes = []
    for dimension_nodes in nx.connected_components(neighbor_graph):
        dimension = neighbor_graph.subgraph(dimension_nodes)
        dimension_node_count = dimension.number_of_nodes()
        if dimension.number_of_edges() != dimension_node_count * (dimension_node_count - 1) // 2:
            return None
        dimension_sizes.append(dimension_node_count + 1)

    shape = tuple(sorted(dimension_sizes))
    if not shape or prod(shape) != component.number_of_nodes():
        return None
    if next(iter(degrees)) != sum(size - 1 for size in shape):
        return None
    return shape


def _build_dimension_template(shape: tuple[int, ...],
                              connections: tuple[str, ...],
                              node_type: str) -> nx.Graph:
    if len(shape) != len(connections):
        raise ValueError("dimension shape and connection counts must match")

    template = nx.Graph()
    coordinates = tuple(product(*(range(size) for size in shape)))
    template.add_nodes_from(coordinates)
    for coordinate in coordinates:
        for dimension, (size, connection) in enumerate(zip(shape, connections)):
            if connection == "line":
                peers = (coordinate[dimension] + 1,)
            elif connection == "ring":
                peers = ((coordinate[dimension] + 1) % size,)
            elif connection == "fullMesh":
                peers = range(coordinate[dimension] + 1, size)
            else:
                raise ValueError(f"unsupported dimension connection {connection}")
            for peer in peers:
                if peer >= size:
                    continue
                neighbor = list(coordinate)
                neighbor[dimension] = peer
                template.add_edge(coordinate, tuple(neighbor))
    nx.set_node_attributes(template, node_type, "node_type")
    return template


def _factor_shapes(node_count: int,
                   dimensions: int,
                   minimum_size: int) -> tuple[tuple[int, ...], ...]:
    shapes = []

    def visit(remaining, remaining_dimensions, minimum, prefix):
        if remaining_dimensions == 1:
            if remaining >= minimum:
                shapes.append((*prefix, remaining))
            return
        factor = minimum
        while factor ** remaining_dimensions <= remaining:
            if remaining % factor == 0:
                visit(
                    remaining // factor,
                    remaining_dimensions - 1,
                    factor,
                    (*prefix, factor),
                )
            factor += 1

    if dimensions > 0:
        visit(node_count, dimensions, minimum_size, ())
    return tuple(shapes)


def _dimension_degree_histogram(shape: tuple[int, ...],
                                connections: tuple[str, ...]) -> Counter:
    degree_counts = Counter({0: 1})
    for size, connection in zip(shape, connections):
        if connection == "line":
            factor_counts = Counter({1: 2})
            if size > 2:
                factor_counts[2] = size - 2
        elif connection == "ring":
            factor_counts = Counter({2: size})
        elif connection == "fullMesh":
            factor_counts = Counter({size - 1: size})
        else:
            raise ValueError(f"unsupported dimension connection {connection}")
        combined_counts = Counter()
        for left_degree, left_count in degree_counts.items():
            for right_degree, right_count in factor_counts.items():
                combined_counts[left_degree + right_degree] += left_count * right_count
        degree_counts = combined_counts
    return degree_counts


def _dimension_corner_eccentricity(shape: tuple[int, ...],
                                   connections: tuple[str, ...]) -> int:
    eccentricity = 0
    for size, connection in zip(shape, connections):
        if connection == "line":
            eccentricity += size - 1
        elif connection == "ring":
            eccentricity += size // 2
        elif connection == "fullMesh":
            eccentricity += 1
        else:
            raise ValueError(f"unsupported dimension connection {connection}")
    return eccentricity


def _links_by_edge(topology: PhysicalTopology):
    links = defaultdict(list)
    for link in topology.links:
        links[_edge_key(link.left_node, link.right_node)].append(link)
    return links


def _recognize_fullmesh(component: nx.Graph,
                        topology: PhysicalTopology,
                        structure_index: int) -> TopologyStructure | None:
    node_count = component.number_of_nodes()
    if node_count < 3 or component.number_of_edges() != node_count * (node_count - 1) // 2:
        return None
    node_types = {data["node_type"] for _, data in component.nodes(data=True)}
    if len(node_types) != 1:
        return None

    ordered_nodes = sorted(component.nodes)
    coordinates = {node_id: (index,) for index, node_id in enumerate(ordered_nodes)}
    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    structure_links = []
    ports_by_node = defaultdict(list)
    for left, right in component.edges:
        physical_links = links_by_edge[_edge_key(left, right)]
        structure_links.append(StructureLink(
            left=coordinates[left],
            right=coordinates[right],
            physical_link_ids=tuple(link.link_id for link in physical_links),
        ))
        for link in physical_links:
            ports_by_node[left].append(_local_port(link, left))
            ports_by_node[right].append(_local_port(link, right))

    return TopologyStructure(
        name=f"fullmesh:{structure_index}",
        structure_type="fullmesh",
        nodes=tuple(
            StructureNode(
                physical_node_id=node_id,
                coordinates=coordinates[node_id],
                node_type=physical_nodes[node_id].node_type,
            )
            for node_id in ordered_nodes
        ),
        links=tuple(structure_links),
        port_groups=tuple(
            LocalPortGroup(
                name=f"node:{node_id}:peers",
                physical_node_id=node_id,
                port_ids=tuple(sorted(ports)),
            )
            for node_id, ports in sorted(ports_by_node.items())
        ),
        destination_coverage=(DestinationCoverage(
            name="all-nodes",
            node_ids=tuple(ordered_nodes),
        ),),
        parameters={"node_count": node_count},
    )


def _local_port(link: PhysicalLink, node_id: int) -> int:
    if link.left_node == node_id:
        return link.left_port
    if link.right_node == node_id:
        return link.right_port
    raise ValueError(f"node {node_id} is not an endpoint of physical link {link.link_id}")


def _match_dimension_template(component: nx.Graph,
                              shape: tuple[int, ...],
                              connections: tuple[str, ...]) -> dict[int, tuple[int, ...]] | None:
    node_types = {data["node_type"] for _, data in component.nodes(data=True)}
    if len(node_types) != 1:
        return None
    template = _build_dimension_template(shape, connections, next(iter(node_types)))
    if not nx.could_be_isomorphic(component, template, properties="dt"):
        return None
    return nx.vf2pp_isomorphism(component, template, node_label="node_type")


def _dimension_candidates(component: nx.Graph):
    seen = set()
    shape = _infer_dimension_fullmesh_shape(component)
    if shape is not None and len(shape) >= 2:
        candidate = (shape, ("fullMesh",) * len(shape))
        seen.add(candidate)
        yield candidate

    degrees = [degree for _, degree in component.degree()]
    if not degrees:
        return
    node_count = component.number_of_nodes()
    corner = min(component.degree(), key=lambda item: (item[1], item[0]))[0]
    corner_eccentricity = nx.eccentricity(component, corner)

    if len(set(degrees)) == 1 and degrees[0] % 2 == 0:
        dimensions = degrees[0] // 2
        if dimensions > 0 and node_count >= 3 ** dimensions:
            for candidate_shape in _factor_shapes(node_count, dimensions, 3):
                candidate = (candidate_shape, ("ring",) * dimensions)
                if _dimension_corner_eccentricity(*candidate) != corner_eccentricity:
                    continue
                if candidate not in seen:
                    seen.add(candidate)
                    yield candidate

    dimensions = min(degrees)
    if dimensions > 0 and max(degrees) <= 2 * dimensions and node_count >= 2 ** dimensions:
        for candidate_shape in _factor_shapes(node_count, dimensions, 2):
            candidate = (candidate_shape, ("line",) * dimensions)
            if _dimension_corner_eccentricity(*candidate) != corner_eccentricity:
                continue
            if candidate not in seen:
                seen.add(candidate)
                yield candidate

    actual_degree_histogram = Counter(degrees)
    dimensions = 1
    while 2 ** dimensions <= node_count:
        for candidate_shape in _factor_shapes(node_count, dimensions, 2):
            connection_options = []
            for size in candidate_shape:
                if size == 2:
                    connection_options.append(("fullMesh",))
                elif size == 3:
                    connection_options.append(("line", "fullMesh"))
                else:
                    connection_options.append(("line", "ring", "fullMesh"))
            for connections in product(*connection_options):
                if len(set(connections)) == 1:
                    continue
                candidate = (candidate_shape, connections)
                if candidate in seen:
                    continue
                if _dimension_degree_histogram(*candidate) != actual_degree_histogram:
                    continue
                if _dimension_corner_eccentricity(*candidate) != corner_eccentricity:
                    continue
                seen.add(candidate)
                yield candidate
        dimensions += 1


def _build_dimension_structure(component: nx.Graph,
                               topology: PhysicalTopology,
                               structure_index: int,
                               shape: tuple[int, ...],
                               connections: tuple[str, ...],
                               coordinates: dict[int, tuple[int, ...]]) -> TopologyStructure:
    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    structure_links = []
    port_groups = defaultdict(list)
    for left, right in sorted(component.edges):
        left_coordinates = coordinates[left]
        right_coordinates = coordinates[right]
        changing_dimensions = [
            index
            for index, values in enumerate(zip(left_coordinates, right_coordinates))
            if values[0] != values[1]
        ]
        if len(changing_dimensions) != 1:
            raise ValueError("a dimension-network edge must change exactly one coordinate")
        changing_dimension = changing_dimensions[0]
        physical_links = links_by_edge[_edge_key(left, right)]
        structure_links.append(StructureLink(
            left=left_coordinates,
            right=right_coordinates,
            physical_link_ids=tuple(link.link_id for link in physical_links),
        ))
        for link in physical_links:
            port_groups[(left, changing_dimension)].append(_local_port(link, left))
            port_groups[(right, changing_dimension)].append(_local_port(link, right))

    return TopologyStructure(
        name=f"dimension-network:{structure_index}",
        structure_type="dimension-network",
        nodes=tuple(
            StructureNode(
                physical_node_id=node_id,
                coordinates=node_coordinates,
                node_type=physical_nodes[node_id].node_type,
            )
            for node_id, node_coordinates in sorted(coordinates.items(), key=lambda item: item[1])
        ),
        links=tuple(structure_links),
        port_groups=tuple(
            LocalPortGroup(
                name=f"node:{node_id}:dimension:{dimension}",
                physical_node_id=node_id,
                port_ids=tuple(sorted(ports)),
            )
            for (node_id, dimension), ports in sorted(port_groups.items())
        ),
        destination_coverage=(DestinationCoverage(
            name="all-nodes",
            node_ids=tuple(sorted(component.nodes)),
        ),),
        parameters={"shape": shape, "connections": connections},
    )


def _recognize_dimension_network(component: nx.Graph,
                                 topology: PhysicalTopology,
                                 structure_index: int) -> TopologyStructure | None:
    if component.number_of_nodes() < 4 or not nx.is_connected(component):
        return None

    for shape, connections in _dimension_candidates(component):
        coordinates = _match_dimension_template(component, shape, connections)
        if coordinates is not None:
            return _build_dimension_structure(
                component,
                topology,
                structure_index,
                shape,
                connections,
                coordinates,
            )
    return None


def _verify_two_layer_clos_template(graph: nx.Graph,
                                    hosts_by_leaf: dict[int, list[int]],
                                    spines: set[int]) -> bool:
    candidate = nx.Graph()
    ordered_leaves = sorted(hosts_by_leaf)
    ordered_spines = sorted(spines)
    for leaf in ordered_leaves:
        candidate.add_node(leaf, role="leaf")
        for host in hosts_by_leaf[leaf]:
            candidate.add_node(host, role="host")
            candidate.add_edge(host, leaf)
    for spine in ordered_spines:
        candidate.add_node(spine, role="spine")
        for leaf in ordered_leaves:
            if graph.has_edge(leaf, spine):
                candidate.add_edge(leaf, spine)

    template = nx.Graph()
    hosts_per_leaf = len(hosts_by_leaf[ordered_leaves[0]])
    for leaf_index in range(len(ordered_leaves)):
        leaf = (1, leaf_index, 0)
        template.add_node(leaf, role="leaf")
        for host_index in range(hosts_per_leaf):
            host = (0, leaf_index, host_index)
            template.add_node(host, role="host")
            template.add_edge(host, leaf)
    for spine_index in range(len(ordered_spines)):
        spine = (2, spine_index, 0)
        template.add_node(spine, role="spine")
        for leaf_index in range(len(ordered_leaves)):
            template.add_edge((1, leaf_index, 0), spine)

    if not nx.could_be_isomorphic(candidate, template, properties="dt"):
        return False
    return nx.vf2pp_is_isomorphic(candidate, template, node_label="role")


def _recognize_two_layer_clos(topology: PhysicalTopology,
                              graph: nx.Graph) -> list[TopologyStructure]:
    switches = {node for node, data in graph.nodes(data=True) if data["node_type"] == "node"}
    hosts = set(graph.nodes) - switches
    leaves = {
        switch
        for switch in switches
        if any(peer in hosts for peer in graph.neighbors(switch))
    }
    leaves_by_spines = defaultdict(set)
    for leaf in leaves:
        spines = frozenset(peer for peer in graph.neighbors(leaf) if peer in switches - leaves)
        if spines:
            leaves_by_spines[spines].add(leaf)

    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    structures = []
    for spines_key, leaves_group in leaves_by_spines.items():
        spines = set(spines_key)
        if len(leaves_group) < 2 or not spines:
            continue
        if any(not graph.has_edge(leaf, spine) for leaf in leaves_group for spine in spines):
            continue
        if any(graph.has_edge(left, right)
               for left in leaves_group
               for right in leaves_group
               if left != right):
            continue
        if any(graph.has_edge(left, right)
               for left in spines
               for right in spines
               if left != right):
            continue
        if any(any(peer in hosts for peer in graph.neighbors(spine)) for spine in spines):
            continue

        ordered_leaves = sorted(leaves_group)
        ordered_spines = sorted(spines)
        hosts_by_leaf = {
            leaf: sorted(peer for peer in graph.neighbors(leaf) if peer in hosts)
            for leaf in ordered_leaves
        }
        host_counts = {len(leaf_hosts) for leaf_hosts in hosts_by_leaf.values()}
        if len(host_counts) != 1 or not next(iter(host_counts)):
            continue
        attached_hosts = [host for leaf_hosts in hosts_by_leaf.values() for host in leaf_hosts]
        if len(set(attached_hosts)) != len(attached_hosts):
            continue
        if not _verify_two_layer_clos_template(graph, hosts_by_leaf, spines):
            continue

        coordinates = {}
        for leaf_index, leaf in enumerate(ordered_leaves):
            coordinates[leaf] = (1, leaf_index, 0)
            for host_index, host in enumerate(hosts_by_leaf[leaf]):
                coordinates[host] = (0, leaf_index, host_index)
        for spine_index, spine in enumerate(ordered_spines):
            coordinates[spine] = (2, spine_index, 0)

        structure_links = []
        port_groups = []
        for leaf_index, leaf in enumerate(ordered_leaves):
            uplink_ports = []
            for host in hosts_by_leaf[leaf]:
                physical_links = links_by_edge[_edge_key(host, leaf)]
                structure_links.append(StructureLink(
                    left=coordinates[host],
                    right=coordinates[leaf],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{host}:uplinks",
                    physical_node_id=host,
                    port_ids=tuple(sorted(_local_port(link, host) for link in physical_links)),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{leaf}:host:{host}",
                    physical_node_id=leaf,
                    port_ids=tuple(sorted(_local_port(link, leaf) for link in physical_links)),
                ))
            for spine in ordered_spines:
                physical_links = links_by_edge[_edge_key(leaf, spine)]
                structure_links.append(StructureLink(
                    left=coordinates[leaf],
                    right=coordinates[spine],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                uplink_ports.extend(_local_port(link, leaf) for link in physical_links)
            port_groups.append(LocalPortGroup(
                name=f"node:{leaf}:spines",
                physical_node_id=leaf,
                port_ids=tuple(sorted(uplink_ports)),
            ))

        for spine in ordered_spines:
            for leaf in ordered_leaves:
                physical_links = links_by_edge[_edge_key(spine, leaf)]
                port_groups.append(LocalPortGroup(
                    name=f"node:{spine}:leaf:{leaf}",
                    physical_node_id=spine,
                    port_ids=tuple(sorted(_local_port(link, spine) for link in physical_links)),
                ))

        all_hosts = sorted(set().union(*(set(values) for values in hosts_by_leaf.values())))
        destination_coverage = [
            DestinationCoverage(name=f"leaf:{leaf_index}", node_ids=tuple(hosts_by_leaf[leaf]))
            for leaf_index, leaf in enumerate(ordered_leaves)
        ]
        destination_coverage.append(
            DestinationCoverage(name="all-hosts", node_ids=tuple(all_hosts))
        )
        structure_index = len(structures)
        structures.append(TopologyStructure(
            name=f"two-layer-clos:{structure_index}",
            structure_type="two-layer-clos",
            nodes=tuple(
                StructureNode(
                    physical_node_id=node_id,
                    coordinates=node_coordinates,
                    node_type=physical_nodes[node_id].node_type,
                )
                for node_id, node_coordinates in sorted(
                    coordinates.items(), key=lambda item: item[1]
                )
            ),
            links=tuple(structure_links),
            port_groups=tuple(port_groups),
            destination_coverage=tuple(destination_coverage),
            parameters={
                "host_count": len(all_hosts),
                "leaf_count": len(ordered_leaves),
                "spine_count": len(ordered_spines),
                "hosts_per_leaf": next(iter(host_counts)),
            },
        ))
    return structures


def _recognize_layered_switch_networks(topology: PhysicalTopology,
                                       graph: nx.Graph) -> list[TopologyStructure]:
    """Recognize host-leaf-upper fabrics with local and shared upper switches."""
    switches = {node for node, data in graph.nodes(data=True) if data["node_type"] == "node"}
    hosts = set(graph.nodes) - switches
    leaves = {
        switch
        for switch in switches
        if any(peer in hosts for peer in graph.neighbors(switch))
    }
    upper_switches = switches - leaves
    switch_graph = graph.subgraph(switches)
    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    structures = []

    for component_nodes in nx.connected_components(switch_graph):
        component_leaves = sorted(set(component_nodes) & leaves)
        component_uppers = sorted(set(component_nodes) & upper_switches)
        if len(component_leaves) < 2 or not component_uppers:
            continue
        leaf_set = set(component_leaves)
        upper_set = set(component_uppers)
        if any(
            set(graph.neighbors(leaf)) - hosts - upper_set
            for leaf in component_leaves
        ):
            continue
        if any(set(graph.neighbors(upper)) - leaf_set for upper in component_uppers):
            continue

        hosts_by_leaf = {
            leaf: sorted(peer for peer in graph.neighbors(leaf) if peer in hosts)
            for leaf in component_leaves
        }
        host_counts = {len(leaf_hosts) for leaf_hosts in hosts_by_leaf.values()}
        if len(host_counts) != 1 or not next(iter(host_counts), 0):
            continue
        attached_hosts = [host for values in hosts_by_leaf.values() for host in values]
        if len(set(attached_hosts)) != len(attached_hosts):
            continue
        if any(
            (set(graph.neighbors(host)) & leaf_set) != {leaf}
            for leaf, leaf_hosts in hosts_by_leaf.items()
            for host in leaf_hosts
        ):
            continue

        blocks = {
            upper: frozenset(set(graph.neighbors(upper)) & leaf_set)
            for upper in component_uppers
        }
        pair_counts = Counter(
            tuple(sorted(pair))
            for block in blocks.values()
            for pair in combinations(block, 2)
        )
        expected_pair_count = len(component_leaves) * (len(component_leaves) - 1) // 2
        if len(pair_counts) != expected_pair_count:
            continue
        # Equal upper sets are already represented by the smaller two-layer rule. A design
        # where every pair meets exactly once remains the separate pair-design structure.
        upper_sets = {
            frozenset(set(graph.neighbors(leaf)) & upper_set)
            for leaf in component_leaves
        }
        if len(upper_sets) == 1 or all(count == 1 for count in pair_counts.values()):
            continue

        coordinates = {}
        for leaf_index, leaf in enumerate(component_leaves):
            coordinates[leaf] = (1, leaf_index, 0)
            for host_index, host in enumerate(hosts_by_leaf[leaf]):
                coordinates[host] = (0, leaf_index, host_index)
        for upper_index, upper in enumerate(component_uppers):
            coordinates[upper] = (2, upper_index, 0)

        structure_links = []
        port_groups = []
        for leaf in component_leaves:
            upper_ports = []
            for host in hosts_by_leaf[leaf]:
                physical_links = links_by_edge[_edge_key(host, leaf)]
                structure_links.append(StructureLink(
                    left=coordinates[host],
                    right=coordinates[leaf],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{host}:leaf",
                    physical_node_id=host,
                    port_ids=tuple(sorted(_local_port(link, host) for link in physical_links)),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{leaf}:host:{host}",
                    physical_node_id=leaf,
                    port_ids=tuple(sorted(_local_port(link, leaf) for link in physical_links)),
                ))
            for upper in sorted(set(graph.neighbors(leaf)) & upper_set):
                physical_links = links_by_edge[_edge_key(leaf, upper)]
                structure_links.append(StructureLink(
                    left=coordinates[leaf],
                    right=coordinates[upper],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                upper_ports.extend(_local_port(link, leaf) for link in physical_links)
            port_groups.append(LocalPortGroup(
                name=f"node:{leaf}:uppers",
                physical_node_id=leaf,
                port_ids=tuple(sorted(upper_ports)),
            ))

        for upper in component_uppers:
            for leaf in sorted(blocks[upper]):
                physical_links = links_by_edge[_edge_key(leaf, upper)]
                port_groups.append(LocalPortGroup(
                    name=f"node:{upper}:leaf:{leaf}",
                    physical_node_id=upper,
                    port_ids=tuple(sorted(_local_port(link, upper) for link in physical_links)),
                ))

        all_hosts = tuple(sorted(attached_hosts))
        structures.append(TopologyStructure(
            name=f"layered-switch-network:{len(structures)}",
            structure_type="layered-switch-network",
            nodes=tuple(
                StructureNode(
                    physical_node_id=node_id,
                    coordinates=node_coordinates,
                    node_type=physical_nodes[node_id].node_type,
                )
                for node_id, node_coordinates in sorted(
                    coordinates.items(), key=lambda item: item[1]
                )
            ),
            links=tuple(structure_links),
            port_groups=tuple(port_groups),
            destination_coverage=tuple([
                DestinationCoverage(
                    name=f"leaf:{leaf_index}",
                    node_ids=tuple(hosts_by_leaf[leaf]),
                )
                for leaf_index, leaf in enumerate(component_leaves)
            ] + [DestinationCoverage(name="all-hosts", node_ids=all_hosts)]),
            parameters={
                "host_count": len(all_hosts),
                "leaf_count": len(component_leaves),
                "upper_count": len(component_uppers),
                "hosts_per_leaf": next(iter(host_counts)),
                "minimum_shared_uppers": min(pair_counts.values()),
                "maximum_shared_uppers": max(pair_counts.values()),
            },
        ))
    return structures


def _recognize_direct_switch_networks(topology: PhysicalTopology,
                                      graph: nx.Graph) -> list[TopologyStructure]:
    hosts = {node for node, data in graph.nodes(data=True) if data["node_type"] == "host"}
    terminal_switches = {
        node
        for node, data in graph.nodes(data=True)
        if data["node_type"] == "node"
        and graph.degree(node) > 0
        and set(graph.neighbors(node)) <= hosts
    }
    switches_by_hosts = defaultdict(list)
    for switch in terminal_switches:
        switches_by_hosts[frozenset(graph.neighbors(switch))].append(switch)

    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    structures = []
    for host_set, switches in sorted(
        switches_by_hosts.items(), key=lambda item: (min(item[0]), min(item[1]))
    ):
        if len(host_set) < 2:
            continue
        ordered_hosts = sorted(host_set)
        ordered_switches = sorted(switches)

        candidate = nx.Graph()
        candidate.add_nodes_from(
            (node_id, graph.nodes[node_id]) for node_id in ordered_hosts + ordered_switches
        )
        candidate.add_edges_from(
            (host, switch) for host in ordered_hosts for switch in ordered_switches
        )
        template = nx.complete_bipartite_graph(len(ordered_hosts), len(ordered_switches))
        for node in range(len(ordered_hosts)):
            template.nodes[node]["node_type"] = "host"
        for node in range(len(ordered_hosts), template.number_of_nodes()):
            template.nodes[node]["node_type"] = "node"
        if not nx.could_be_isomorphic(candidate, template, properties="dt"):
            continue
        if not nx.vf2pp_is_isomorphic(candidate, template, node_label="node_type"):
            continue

        coordinates = {
            **{node_id: (0, index) for index, node_id in enumerate(ordered_hosts)},
            **{node_id: (1, index) for index, node_id in enumerate(ordered_switches)},
        }
        structure_links = []
        host_ports = defaultdict(list)
        switch_ports = defaultdict(list)
        for host in ordered_hosts:
            for switch in ordered_switches:
                physical_links = links_by_edge[_edge_key(host, switch)]
                structure_links.append(StructureLink(
                    left=coordinates[host],
                    right=coordinates[switch],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                for link in physical_links:
                    host_ports[host].append(_local_port(link, host))
                    switch_ports[(switch, host)].append(_local_port(link, switch))

        structure_index = len(structures)
        structures.append(TopologyStructure(
            name=f"direct-switch-network:{structure_index}",
            structure_type="direct-switch-network",
            nodes=tuple(
                StructureNode(
                    physical_node_id=node_id,
                    coordinates=coordinates[node_id],
                    node_type=physical_nodes[node_id].node_type,
                )
                for node_id in ordered_hosts + ordered_switches
            ),
            links=tuple(structure_links),
            port_groups=tuple(
                [
                    LocalPortGroup(
                        name=f"node:{host}:switches",
                        physical_node_id=host,
                        port_ids=tuple(sorted(host_ports[host])),
                    )
                    for host in ordered_hosts
                ]
                + [
                    LocalPortGroup(
                        name=f"node:{switch}:host:{host}",
                        physical_node_id=switch,
                        port_ids=tuple(sorted(switch_ports[(switch, host)])),
                    )
                    for switch in ordered_switches
                    for host in ordered_hosts
                ]
            ),
            destination_coverage=(DestinationCoverage(
                name="all-hosts",
                node_ids=tuple(ordered_hosts),
            ),),
            parameters={
                "host_count": len(ordered_hosts),
                "switch_count": len(ordered_switches),
            },
        ))
    return structures


def _recognize_pair_design_networks(topology: PhysicalTopology,
                                    graph: nx.Graph) -> list[TopologyStructure]:
    """Recognize sparse switch incidence graphs where every lower pair meets once."""
    hosts = {node for node, data in graph.nodes(data=True) if data["node_type"] == "host"}
    switches = set(graph.nodes) - hosts
    lower_candidates = {
        switch
        for switch in switches
        if any(peer in hosts for peer in graph.neighbors(switch))
    }

    incidence_graph = nx.Graph()
    for lower in lower_candidates:
        for peer in graph.neighbors(lower):
            if peer in switches - lower_candidates:
                incidence_graph.add_edge(lower, peer)

    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    structures = []
    for component_nodes in nx.connected_components(incidence_graph):
        lowers = sorted(component_nodes & lower_candidates)
        uppers = sorted(component_nodes - lower_candidates)
        if len(lowers) < 3 or not uppers:
            continue

        lower_set = set(lowers)
        upper_set = set(uppers)
        if any(set(graph.neighbors(lower)) - hosts - upper_set for lower in lowers):
            continue
        if any(set(graph.neighbors(upper)) - lower_set for upper in uppers):
            continue

        hosts_by_lower = {
            lower: sorted(peer for peer in graph.neighbors(lower) if peer in hosts)
            for lower in lowers
        }
        host_counts = {len(lower_hosts) for lower_hosts in hosts_by_lower.values()}
        if len(host_counts) != 1 or not next(iter(host_counts)):
            continue
        attached_hosts = [host for lower_hosts in hosts_by_lower.values() for host in lower_hosts]
        if len(set(attached_hosts)) != len(attached_hosts):
            continue
        if any(
            set(graph.neighbors(host)) != {lower}
            for lower, lower_hosts in hosts_by_lower.items()
            for host in lower_hosts
        ):
            continue

        blocks = {
            upper: frozenset(peer for peer in graph.neighbors(upper) if peer in lower_set)
            for upper in uppers
        }
        block_sizes = {len(block) for block in blocks.values()}
        lower_degrees = {
            len(set(graph.neighbors(lower)) & upper_set) for lower in lowers
        }
        if len(block_sizes) != 1 or len(lower_degrees) != 1:
            continue
        block_size = next(iter(block_sizes))
        lower_degree = next(iter(lower_degrees))
        if block_size < 2 or block_size >= len(lowers) or lower_degree < 2:
            continue
        pair_counts = Counter(
            tuple(sorted(pair))
            for block in blocks.values()
            for pair in combinations(block, 2)
        )
        if (
            len(pair_counts) != len(lowers) * (len(lowers) - 1) // 2
            or any(count != 1 for count in pair_counts.values())
        ):
            continue

        coordinates = {}
        for lower_index, lower in enumerate(lowers):
            coordinates[lower] = (1, lower_index, 0)
            for host_index, host in enumerate(hosts_by_lower[lower]):
                coordinates[host] = (0, lower_index, host_index)
        for upper_index, upper in enumerate(uppers):
            coordinates[upper] = (2, upper_index, 0)

        structure_links = []
        port_groups = []
        for lower in lowers:
            upper_ports = []
            for host in hosts_by_lower[lower]:
                physical_links = links_by_edge[_edge_key(host, lower)]
                structure_links.append(StructureLink(
                    left=coordinates[host],
                    right=coordinates[lower],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{host}:uplinks",
                    physical_node_id=host,
                    port_ids=tuple(sorted(_local_port(link, host) for link in physical_links)),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{lower}:host:{host}",
                    physical_node_id=lower,
                    port_ids=tuple(sorted(_local_port(link, lower) for link in physical_links)),
                ))
            for upper in sorted(set(graph.neighbors(lower)) & upper_set):
                upper_ports.extend(
                    _local_port(link, lower)
                    for link in links_by_edge[_edge_key(lower, upper)]
                )
            port_groups.append(LocalPortGroup(
                name=f"node:{lower}:uppers",
                physical_node_id=lower,
                port_ids=tuple(sorted(upper_ports)),
            ))

        for upper in uppers:
            for lower in sorted(blocks[upper]):
                physical_links = links_by_edge[_edge_key(lower, upper)]
                structure_links.append(StructureLink(
                    left=coordinates[lower],
                    right=coordinates[upper],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{upper}:lower:{lower}",
                    physical_node_id=upper,
                    port_ids=tuple(sorted(_local_port(link, upper) for link in physical_links)),
                ))

        all_hosts = tuple(sorted(attached_hosts))
        destination_coverage = [
            DestinationCoverage(
                name=f"lower:{lower_index}",
                node_ids=tuple(hosts_by_lower[lower]),
            )
            for lower_index, lower in enumerate(lowers)
        ]
        destination_coverage.append(
            DestinationCoverage(name="all-hosts", node_ids=all_hosts)
        )
        structures.append(TopologyStructure(
            name=f"pair-design-network:{len(structures)}",
            structure_type="pair-design-network",
            nodes=tuple(
                StructureNode(
                    physical_node_id=node_id,
                    coordinates=node_coordinates,
                    node_type=physical_nodes[node_id].node_type,
                )
                for node_id, node_coordinates in sorted(
                    coordinates.items(), key=lambda item: item[1]
                )
            ),
            links=tuple(structure_links),
            port_groups=tuple(port_groups),
            destination_coverage=tuple(destination_coverage),
            parameters={
                "lower_count": len(lowers),
                "upper_count": len(uppers),
                "upper_degree": block_size,
                "lower_uplink_degree": lower_degree,
                "hosts_per_lower": next(iter(host_counts)),
                "pair_multiplicity": 1,
            },
        ))
    return structures


def _valid_dragonfly_partition(router_graph: nx.Graph,
                               groups: tuple[tuple[int, ...], ...],
                               global_links_per_router: int) -> bool:
    group_by_router = {
        router: group_index
        for group_index, group in enumerate(groups)
        for router in group
    }
    if len(group_by_router) != router_graph.number_of_nodes():
        return False

    links_by_group_pair = Counter()
    global_degrees = Counter()
    for left, right in router_graph.edges:
        left_group = group_by_router[left]
        right_group = group_by_router[right]
        if left_group == right_group:
            continue
        links_by_group_pair[tuple(sorted((left_group, right_group)))] += 1
        global_degrees[left] += 1
        global_degrees[right] += 1

    expected_pairs = {
        pair for pair in combinations(range(len(groups)), 2)
    }
    return (
        set(links_by_group_pair) == expected_pairs
        and all(count == 1 for count in links_by_group_pair.values())
        and all(
            global_degrees[router] == global_links_per_router
            for router in router_graph.nodes
        )
    )


def _find_dragonfly_groups(router_graph: nx.Graph,
                           routers_per_group: int,
                           group_count: int,
                           global_links_per_router: int) -> tuple[tuple[int, ...], ...] | None:
    group_candidates = []
    for clique in nx.find_cliques(router_graph):
        if len(clique) == routers_per_group:
            group_candidates.append(tuple(sorted(clique)))
            if len(group_candidates) > 4096:
                return None
    group_candidates = sorted(set(group_candidates))
    candidates_by_router = defaultdict(list)
    for candidate in group_candidates:
        for router in candidate:
            candidates_by_router[router].append(candidate)
    if any(not candidates_by_router[router] for router in router_graph.nodes):
        return None

    all_routers = frozenset(router_graph.nodes)

    def search(uncovered, selected):
        if not uncovered:
            groups = tuple(sorted(selected))
            if len(groups) == group_count and _valid_dragonfly_partition(
                router_graph,
                groups,
                global_links_per_router,
            ):
                return groups
            return None
        if len(selected) >= group_count:
            return None

        router = min(
            uncovered,
            key=lambda node: sum(
                set(candidate) <= uncovered for candidate in candidates_by_router[node]
            ),
        )
        for candidate in candidates_by_router[router]:
            candidate_nodes = frozenset(candidate)
            if candidate_nodes <= uncovered:
                result = search(uncovered - candidate_nodes, (*selected, candidate))
                if result is not None:
                    return result
        return None

    return search(all_routers, ())


def _dragonfly_parameter_candidates(router_graph: nx.Graph):
    router_count = router_graph.number_of_nodes()
    degrees = {degree for _, degree in router_graph.degree()}
    if len(degrees) != 1:
        return
    router_degree = next(iter(degrees))
    for routers_per_group in range(2, router_count):
        if router_count % routers_per_group != 0:
            continue
        group_count = router_count // routers_per_group
        if group_count < 3 or (group_count - 1) % routers_per_group != 0:
            continue
        global_links_per_router = (group_count - 1) // routers_per_group
        if router_degree != routers_per_group - 1 + global_links_per_router:
            continue
        yield routers_per_group, group_count, global_links_per_router


def _build_dragonfly_structure(topology: PhysicalTopology,
                               graph: nx.Graph,
                               groups: tuple[tuple[int, ...], ...],
                               hosts_by_router: dict[int, list[int]],
                               global_links_per_router: int,
                               structure_index: int) -> TopologyStructure:
    physical_nodes = {node.node_id: node for node in topology.nodes}
    links_by_edge = _links_by_edge(topology)
    coordinates = {}
    group_by_router = {}
    for group_index, group in enumerate(groups):
        for local_index, router in enumerate(group):
            group_by_router[router] = group_index
            coordinates[router] = (1, group_index, local_index, 0)
            for host_index, host in enumerate(hosts_by_router[router]):
                coordinates[host] = (0, group_index, local_index, host_index)

    structure_links = []
    port_groups = []
    for group in groups:
        for router in group:
            for host in hosts_by_router[router]:
                physical_links = links_by_edge[_edge_key(host, router)]
                structure_links.append(StructureLink(
                    left=coordinates[host],
                    right=coordinates[router],
                    physical_link_ids=tuple(link.link_id for link in physical_links),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{host}:router",
                    physical_node_id=host,
                    port_ids=tuple(sorted(_local_port(link, host) for link in physical_links)),
                ))
                port_groups.append(LocalPortGroup(
                    name=f"node:{router}:host:{host}",
                    physical_node_id=router,
                    port_ids=tuple(sorted(_local_port(link, router) for link in physical_links)),
                ))

    routers = set(group_by_router)
    for left, right in sorted(graph.subgraph(routers).edges):
        physical_links = links_by_edge[_edge_key(left, right)]
        structure_links.append(StructureLink(
            left=coordinates[left],
            right=coordinates[right],
            physical_link_ids=tuple(link.link_id for link in physical_links),
        ))
        if group_by_router[left] == group_by_router[right]:
            relation = f"router:{right}"
            reverse_relation = f"router:{left}"
        else:
            relation = f"group:{group_by_router[right]}"
            reverse_relation = f"group:{group_by_router[left]}"
        port_groups.append(LocalPortGroup(
            name=f"node:{left}:{relation}",
            physical_node_id=left,
            port_ids=tuple(sorted(_local_port(link, left) for link in physical_links)),
        ))
        port_groups.append(LocalPortGroup(
            name=f"node:{right}:{reverse_relation}",
            physical_node_id=right,
            port_ids=tuple(sorted(_local_port(link, right) for link in physical_links)),
        ))

    all_hosts = tuple(sorted(host for hosts in hosts_by_router.values() for host in hosts))
    coverage = []
    for group_index, group in enumerate(groups):
        group_hosts = tuple(sorted(host for router in group for host in hosts_by_router[router]))
        coverage.append(DestinationCoverage(name=f"group:{group_index}", node_ids=group_hosts))
    coverage.append(DestinationCoverage(name="all-hosts", node_ids=all_hosts))

    return TopologyStructure(
        name=f"dragonfly:{structure_index}",
        structure_type="dragonfly",
        nodes=tuple(
            StructureNode(
                physical_node_id=node_id,
                coordinates=node_coordinates,
                node_type=physical_nodes[node_id].node_type,
            )
            for node_id, node_coordinates in sorted(coordinates.items(), key=lambda item: item[1])
        ),
        links=tuple(structure_links),
        port_groups=tuple(port_groups),
        destination_coverage=tuple(coverage),
        parameters={
            "group_count": len(groups),
            "routers_per_group": len(groups[0]),
            "global_links_per_router": global_links_per_router,
            "hosts_per_router": len(hosts_by_router[groups[0][0]]),
        },
    )


def _recognize_dragonflies(topology: PhysicalTopology,
                           graph: nx.Graph) -> list[TopologyStructure]:
    switches = {node for node, data in graph.nodes(data=True) if data["node_type"] == "node"}
    hosts = set(graph.nodes) - switches
    routers = {
        switch
        for switch in switches
        if any(peer in hosts for peer in graph.neighbors(switch))
    }
    router_graph = graph.subgraph(routers)
    structures = []
    for component_nodes in nx.connected_components(router_graph):
        component = router_graph.subgraph(component_nodes).copy()
        hosts_by_router = {
            router: sorted(peer for peer in graph.neighbors(router) if peer in hosts)
            for router in component_nodes
        }
        host_counts = {len(router_hosts) for router_hosts in hosts_by_router.values()}
        attached_hosts = [
            host for router_hosts in hosts_by_router.values() for host in router_hosts
        ]
        if len(host_counts) != 1 or not next(iter(host_counts), 0):
            continue
        if len(set(attached_hosts)) != len(attached_hosts):
            continue
        if any((set(graph.neighbors(host)) & set(component_nodes)) != {router}
               for router, router_hosts in hosts_by_router.items()
               for host in router_hosts):
            continue

        for routers_per_group, group_count, global_links_per_router in (
            _dragonfly_parameter_candidates(component)
        ):
            groups = _find_dragonfly_groups(
                component,
                routers_per_group,
                group_count,
                global_links_per_router,
            )
            if groups is None:
                continue
            structures.append(_build_dragonfly_structure(
                topology,
                graph,
                groups,
                hosts_by_router,
                global_links_per_router,
                len(structures),
            ))
            break
    return structures


def _recognize_port_extensions(topology: PhysicalTopology,
                               structures: list[TopologyStructure]) -> list[TopologyStructure]:
    clos_structures = [
        structure for structure in structures if structure.structure_type == "two-layer-clos"
    ]
    if not clos_structures:
        return []
    claimed_link_ids = {
        link_id
        for structure in clos_structures
        for link in structure.links
        for link_id in link.physical_link_ids
    }
    clos_node_ids = {
        node.physical_node_id for structure in clos_structures for node in structure.nodes
    }
    physical_nodes = {node.node_id: node for node in topology.nodes}
    extension_links = [
        link
        for link in topology.links
        if link.link_id not in claimed_link_ids
        and physical_nodes[link.left_node].node_type == "node"
        and physical_nodes[link.right_node].node_type == "node"
        and (link.left_node in clos_node_ids or link.right_node in clos_node_ids)
    ]
    if not extension_links:
        return []

    extension_graph = nx.Graph()
    extension_graph.add_edges_from((link.left_node, link.right_node) for link in extension_links)
    structures_by_component = []
    for component_index, component_nodes in enumerate(nx.connected_components(extension_graph)):
        ordered_nodes = sorted(component_nodes)
        coordinates = {node_id: (index,) for index, node_id in enumerate(ordered_nodes)}
        component_links = [
            link
            for link in extension_links
            if link.left_node in component_nodes and link.right_node in component_nodes
        ]
        links_by_edge = defaultdict(list)
        for link in component_links:
            links_by_edge[_edge_key(link.left_node, link.right_node)].append(link)
        structure_links = []
        ports_by_node = defaultdict(list)
        for edge, physical_links in links_by_edge.items():
            left, right = sorted(edge)
            structure_links.append(StructureLink(
                left=coordinates[left],
                right=coordinates[right],
                physical_link_ids=tuple(link.link_id for link in physical_links),
            ))
            for link in physical_links:
                ports_by_node[left].append(_local_port(link, left))
                ports_by_node[right].append(_local_port(link, right))
        structures_by_component.append(TopologyStructure(
            name=f"port-extension:{component_index}",
            structure_type="port-extension",
            nodes=tuple(
                StructureNode(
                    physical_node_id=node_id,
                    coordinates=coordinates[node_id],
                    node_type=physical_nodes[node_id].node_type,
                )
                for node_id in ordered_nodes
            ),
            links=tuple(structure_links),
            port_groups=tuple(
                LocalPortGroup(
                    name=f"node:{node_id}:extension",
                    physical_node_id=node_id,
                    port_ids=tuple(sorted(ports)),
                )
                for node_id, ports in sorted(ports_by_node.items())
            ),
            parameters={"link_count": len(component_links)},
            rule_capable=False,
        ))
    return structures_by_component


def discover_topology_structures(topology: PhysicalTopology) -> tuple[TopologyStructure, ...]:
    graph = _build_graph(topology)
    host_nodes = [node for node, data in graph.nodes(data=True) if data["node_type"] == "host"]
    host_graph = graph.subgraph(host_nodes)
    structures = []
    for component_index, component_nodes in enumerate(nx.connected_components(host_graph)):
        component = host_graph.subgraph(component_nodes).copy()
        structure = _recognize_fullmesh(component, topology, component_index)
        if structure is None:
            structure = _recognize_dimension_network(component, topology, component_index)
        if structure is not None:
            structures.append(structure)
    structures.extend(_recognize_direct_switch_networks(topology, graph))
    layered_structures = _recognize_layered_switch_networks(topology, graph)
    layered_link_ids = {
        link_id
        for structure in layered_structures
        for link in structure.links
        for link_id in link.physical_link_ids
    }
    structures.extend(layered_structures)
    structures.extend(
        structure
        for structure in _recognize_two_layer_clos(topology, graph)
        if not any(
            link_id in layered_link_ids
            for link in structure.links
            for link_id in link.physical_link_ids
        )
    )
    structures.extend(_recognize_dragonflies(topology, graph))
    structures.extend(
        structure
        for structure in _recognize_pair_design_networks(topology, graph)
        if not any(
            link_id in layered_link_ids
            for link in structure.links
            for link_id in link.physical_link_ids
        )
    )
    structures.extend(_recognize_port_extensions(topology, structures))
    return tuple(structures)
