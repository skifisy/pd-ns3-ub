import sys
import random
import csv
import unittest
import tempfile
from itertools import combinations, product
from pathlib import Path

import networkx as nx

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_sim_builder as netsim
from topology_recognition import discover_topology_structures
from topology_structure import verify_topology_structures


def build_dimension_fullmesh(rows=4, columns=4):
    graph = netsim.NetworkSimulationGraph()
    node_count = rows * columns
    for node in range(node_count):
        graph.add_netisim_host(node)
    for left in range(node_count):
        left_coordinates = divmod(left, columns)
        for right in range(left + 1, node_count):
            right_coordinates = divmod(right, columns)
            if (
                left_coordinates[0] == right_coordinates[0]
                or left_coordinates[1] == right_coordinates[1]
            ):
                graph.add_netisim_edge(left, right)
    graph.build_graph_config()
    return graph


def build_dimension_network(shape, connections):
    graph = netsim.NetworkSimulationGraph()
    coordinates = tuple(product(*(range(size) for size in shape)))
    node_by_coordinates = {
        node_coordinates: node_id
        for node_id, node_coordinates in enumerate(coordinates)
    }
    for node_id in range(len(coordinates)):
        graph.add_netisim_host(node_id)
    for node_coordinates in coordinates:
        left = node_by_coordinates[node_coordinates]
        for dimension, (size, connection) in enumerate(zip(shape, connections)):
            if connection == "line":
                peers = (node_coordinates[dimension] + 1,)
            elif connection == "ring":
                peers = ((node_coordinates[dimension] + 1) % size,)
            elif connection == "fullMesh":
                peers = range(node_coordinates[dimension] + 1, size)
            else:
                raise ValueError(connection)
            for peer in peers:
                if peer >= size:
                    continue
                right_coordinates = list(node_coordinates)
                right_coordinates[dimension] = peer
                right = node_by_coordinates[tuple(right_coordinates)]
                if not graph.has_edge(left, right):
                    graph.add_netisim_edge(left, right)
    graph.build_graph_config()
    return graph


def build_dragonfly(routers_per_group=4, global_links_per_router=1, hosts_per_router=1):
    group_count = routers_per_group * global_links_per_router + 1
    router_count = routers_per_group * group_count
    host_count = router_count * hosts_per_router
    router_base = host_count
    graph = netsim.NetworkSimulationGraph()
    for host in range(host_count):
        graph.add_netisim_host(host)
    for router in range(router_base, router_base + router_count):
        graph.add_netisim_node(router)

    def router(group, local_index):
        return router_base + group * routers_per_group + local_index

    for group in range(group_count):
        group_routers = [router(group, index) for index in range(routers_per_group)]
        for left, right in combinations(group_routers, 2):
            graph.add_netisim_edge(left, right)
        for local_index, group_router in enumerate(group_routers):
            router_offset = group * routers_per_group + local_index
            for host_index in range(hosts_per_router):
                graph.add_netisim_edge(
                    router_offset * hosts_per_router + host_index,
                    group_router,
                )

    for left_group, right_group in combinations(range(group_count), 2):
        left_peer_index = right_group if right_group < left_group else right_group - 1
        right_peer_index = left_group if left_group < right_group else left_group - 1
        graph.add_netisim_edge(
            router(left_group, left_peer_index // global_links_per_router),
            router(right_group, right_peer_index // global_links_per_router),
        )
    graph.build_graph_config()
    return graph


def build_ring_inside_dragonfly():
    graph = build_dragonfly(routers_per_group=3, global_links_per_router=1)
    host_count = 12
    for host in range(host_count):
        graph.add_netisim_edge(host, (host + 1) % host_count)
    graph.build_graph_config()
    return graph


def build_two_layer_clos(host_count=8, leaf_count=2, spine_count=4):
    graph = netsim.NetworkSimulationGraph()
    leaf_base = host_count
    spine_base = leaf_base + leaf_count
    for host in range(host_count):
        graph.add_netisim_host(host)
    for switch in range(leaf_base, spine_base + spine_count):
        graph.add_netisim_node(switch)
    hosts_per_leaf = host_count // leaf_count
    for host in range(host_count):
        graph.add_netisim_edge(host, leaf_base + host // hosts_per_leaf)
    for leaf in range(leaf_base, spine_base):
        for spine in range(spine_base, spine_base + spine_count):
            graph.add_netisim_edge(leaf, spine)
    graph.build_graph_config()
    return graph


def build_fullmesh_with_direct_switches(host_count=4, switch_count=4):
    graph = netsim.NetworkSimulationGraph()
    for host in range(host_count):
        graph.add_netisim_host(host)
    for switch in range(host_count, host_count + switch_count):
        graph.add_netisim_node(switch)
    for left in range(host_count):
        for right in range(left + 1, host_count):
            graph.add_netisim_edge(left, right)
    for host in range(host_count):
        for switch in range(host_count, host_count + switch_count):
            graph.add_netisim_edge(host, switch)
    graph.build_graph_config()
    return graph


def build_grouped_fullmesh_with_direct_switches(group_count=4,
                                                hosts_per_group=4,
                                                switch_count=4):
    graph = netsim.NetworkSimulationGraph()
    host_count = group_count * hosts_per_group
    for host in range(host_count):
        graph.add_netisim_host(host)
    for switch in range(host_count, host_count + switch_count):
        graph.add_netisim_node(switch)
    for group in range(group_count):
        hosts = range(group * hosts_per_group, (group + 1) * hosts_per_group)
        for left, right in combinations(hosts, 2):
            graph.add_netisim_edge(left, right)
    for host in range(host_count):
        for switch in range(host_count, host_count + switch_count):
            graph.add_netisim_edge(host, switch)
    graph.build_graph_config()
    return graph


def build_grouped_fullmesh_with_multi_plane_switches(group_count=4,
                                                      hosts_per_group=4,
                                                      plane_count=3,
                                                      upper_count=2):
    graph = netsim.NetworkSimulationGraph()
    host_count = group_count * hosts_per_group
    leaves_per_plane = group_count
    switches_per_plane = leaves_per_plane + upper_count
    for host in range(host_count):
        graph.add_netisim_host(host)
    for switch in range(
        host_count,
        host_count + plane_count * switches_per_plane,
    ):
        graph.add_netisim_node(switch)

    for group in range(group_count):
        hosts = range(group * hosts_per_group, (group + 1) * hosts_per_group)
        for left, right in combinations(hosts, 2):
            graph.add_netisim_edge(left, right)

    for plane in range(plane_count):
        plane_start = host_count + plane * switches_per_plane
        upper_start = plane_start + leaves_per_plane
        for group in range(group_count):
            leaf = plane_start + group
            for host in range(group * hosts_per_group, (group + 1) * hosts_per_group):
                graph.add_netisim_edge(host, leaf)
            for upper in range(upper_start, upper_start + upper_count):
                graph.add_netisim_edge(leaf, upper)
    graph.build_graph_config()
    return graph


def build_grouped_fullmesh_with_shared_upper_planes(group_count=4,
                                                    hosts_per_group=3,
                                                    plane_count=2):
    graph = netsim.NetworkSimulationGraph()
    host_count = group_count * hosts_per_group
    leaves_per_plane = group_count
    local_uppers_per_plane = 2
    shared_uppers_per_plane = 1
    switches_per_plane = (
        leaves_per_plane + local_uppers_per_plane + shared_uppers_per_plane
    )
    for host in range(host_count):
        graph.add_netisim_host(host)
    for switch in range(host_count, host_count + plane_count * switches_per_plane):
        graph.add_netisim_node(switch)

    for group in range(group_count):
        hosts = range(group * hosts_per_group, (group + 1) * hosts_per_group)
        for left, right in combinations(hosts, 2):
            graph.add_netisim_edge(left, right)

    for plane in range(plane_count):
        plane_start = host_count + plane * switches_per_plane
        local_upper_start = plane_start + leaves_per_plane
        shared_upper = local_upper_start + local_uppers_per_plane
        for group in range(group_count):
            leaf = plane_start + group
            for host in range(group * hosts_per_group, (group + 1) * hosts_per_group):
                graph.add_netisim_edge(host, leaf)
            graph.add_netisim_edge(leaf, local_upper_start + group // 2)
            graph.add_netisim_edge(leaf, shared_upper)
    graph.build_graph_config()
    return graph


def build_clos_with_port_extension():
    graph = netsim.NetworkSimulationGraph()
    for host in range(8):
        graph.add_netisim_host(host)
    for switch in range(8, 17):
        graph.add_netisim_node(switch)

    for host in range(4):
        graph.add_netisim_edge(host, 8 + host // 2)
    for host in range(4, 8):
        graph.add_netisim_edge(host, 10 + (host - 4) // 2)
    for leaf in (8, 9):
        for spine in (12, 13):
            graph.add_netisim_edge(leaf, spine)
    for leaf in (10, 11):
        for spine in (14, 15):
            graph.add_netisim_edge(leaf, spine)
    for spine in (12, 13, 14, 15):
        graph.add_netisim_edge(spine, 16)
    graph.build_graph_config()
    return graph


def build_fullmesh_with_multi_plane_clos():
    graph = netsim.NetworkSimulationGraph()
    group_size = 9
    for host in range(group_size * 2):
        graph.add_netisim_host(host)
    for switch in range(18, 26):
        graph.add_netisim_node(switch)

    for group_start in (0, group_size):
        group = list(range(group_start, group_start + group_size))
        for left_index, left in enumerate(group):
            left_coordinates = divmod(left_index, 3)
            for right_index, right in enumerate(group[left_index + 1:], start=left_index + 1):
                right_coordinates = divmod(right_index, 3)
                if (
                    left_coordinates[0] == right_coordinates[0]
                    or left_coordinates[1] == right_coordinates[1]
                ):
                    graph.add_netisim_edge(left, right)

    for leaves, spines in (((18, 19), (20, 21)), ((22, 23), (24, 25))):
        for host in range(9):
            graph.add_netisim_edge(host, leaves[0])
        for host in range(9, 18):
            graph.add_netisim_edge(host, leaves[1])
        for leaf in leaves:
            for spine in spines:
                graph.add_netisim_edge(leaf, spine)
    graph.build_graph_config()
    return graph


def build_balanced_sparse_tree():
    """Build the paper's minimal nontrivial bst(7, 3, 1) Fano-plane group."""
    graph = netsim.NetworkSimulationGraph()
    lower_base = 7
    upper_base = 14
    for host in range(7):
        graph.add_netisim_host(host)
    for switch in range(lower_base, upper_base + 7):
        graph.add_netisim_node(switch)

    for host in range(7):
        graph.add_netisim_edge(host, lower_base + host)

    # Figure 4: every pair of lower switches occurs in exactly one Fano block.
    blocks = (
        (0, 1, 3),
        (0, 2, 4),
        (0, 5, 6),
        (1, 2, 6),
        (1, 4, 5),
        (2, 3, 5),
        (3, 4, 6),
    )
    for upper_index, block in enumerate(blocks):
        for lower_index in block:
            graph.add_netisim_edge(
                lower_base + lower_index,
                upper_base + upper_index,
            )
    graph.build_graph_config()
    return graph


def shuffle_ids_and_edges(source):
    randomizer = random.Random(20260717)
    host_ids = sorted(node_id for node_id in source.nodes if source.node_is_host(node_id))
    switch_ids = sorted(node_id for node_id in source.nodes if source.node_is_switch(node_id))
    shuffled_hosts = randomizer.sample(host_ids, len(host_ids))
    shuffled_switches = randomizer.sample(switch_ids, len(switch_ids))
    mapping = dict(zip(host_ids + switch_ids, shuffled_hosts + shuffled_switches))

    graph = netsim.NetworkSimulationGraph()
    for node_id in host_ids:
        graph.add_netisim_host(node_id)
    for node_id in switch_ids:
        graph.add_netisim_node(node_id)
    edges = list(source.edges(data=True))
    randomizer.shuffle(edges)
    for left, right, data in edges:
        graph.add_netisim_edge(mapping[left], mapping[right], **data)
    graph.build_graph_config()
    return graph


class TopologyRecognitionTest(unittest.TestCase):
    def test_discovers_paper_balanced_sparse_tree_as_a_pair_design(self):
        original = build_balanced_sparse_tree()
        reordered = shuffle_ids_and_edges(original)

        for graph in (original, reordered):
            structures = discover_topology_structures(graph.get_physical_topology())
            report = verify_topology_structures(graph.get_physical_topology(), structures)

            self.assertTrue(report.is_fully_covered)
            self.assertEqual(len(structures), 1)
            structure = structures[0]
            self.assertEqual(structure.structure_type, "pair-design-network")
            self.assertEqual(
                structure.parameters,
                {
                    "lower_count": 7,
                    "upper_count": 7,
                    "upper_degree": 3,
                    "lower_uplink_degree": 3,
                    "hosts_per_lower": 1,
                    "pair_multiplicity": 1,
                },
            )
            self.assertEqual(len(structure.nodes), 21)
            self.assertEqual(len(structure.links), 28)
            self.assertEqual(len(structure.port_groups), 42)

    def test_rejects_balanced_sparse_tree_with_a_broken_pair_design(self):
        graph = build_balanced_sparse_tree()
        graph.remove_edge(7, 14)
        graph.build_graph_config()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertFalse(report.is_fully_covered)
        self.assertNotIn(
            "pair-design-network",
            [structure.structure_type for structure in structures],
        )

    def test_discovers_fullmesh_with_direct_switch_network(self):
        graph = build_fullmesh_with_direct_switches()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(
            [structure.structure_type for structure in structures],
            ["fullmesh", "direct-switch-network"],
        )
        self.assertEqual(structures[0].parameters, {"node_count": 4})
        self.assertEqual(
            structures[1].parameters,
            {"host_count": 4, "switch_count": 4},
        )

    def test_discovers_and_verifies_a_two_dimensional_fullmesh(self):
        graph = build_dimension_fullmesh()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(len(structures), 1)
        structure = structures[0]
        self.assertEqual(structure.structure_type, "dimension-network")
        self.assertEqual(structure.parameters["shape"], (4, 4))
        self.assertEqual(structure.parameters["connections"], ("fullMesh", "fullMesh"))
        self.assertEqual(len(structure.nodes), 16)
        self.assertEqual(len(structure.links), 48)
        self.assertEqual(len(structure.port_groups), 32)
        self.assertEqual(structure.destination_coverage[0].node_ids, tuple(range(16)))

    def test_infers_a_non_square_dimension_fullmesh_shape(self):
        graph = build_dimension_fullmesh(rows=3, columns=4)

        structures = discover_topology_structures(graph.get_physical_topology())

        self.assertEqual(len(structures), 1)
        self.assertEqual(structures[0].parameters["shape"], (3, 4))

    def test_discovers_ring_mesh_torus_and_nd_fullmesh(self):
        cases = (
            ((6,), ("ring",)),
            ((3, 4), ("line", "line")),
            ((4, 5), ("ring", "ring")),
            ((4, 5, 6), ("ring", "ring", "ring")),
            ((2, 3, 4), ("fullMesh", "fullMesh", "fullMesh")),
            ((3, 5), ("line", "ring")),
        )
        for shape, connections in cases:
            with self.subTest(shape=shape, connections=connections):
                original = build_dimension_network(shape, connections)
                reordered = shuffle_ids_and_edges(original)
                for graph in (original, reordered):
                    structures = discover_topology_structures(graph.get_physical_topology())
                    report = verify_topology_structures(
                        graph.get_physical_topology(),
                        structures,
                    )

                    self.assertTrue(report.is_fully_covered)
                    self.assertEqual(len(structures), 1)
                    self.assertEqual(structures[0].structure_type, "dimension-network")
                    self.assertEqual(structures[0].parameters["shape"], shape)
                    self.assertEqual(structures[0].parameters["connections"], connections)

    def test_discovers_classic_dragonfly_after_reordering(self):
        cases = ((4, 1, 1), (3, 2, 2))
        for routers_per_group, global_links_per_router, hosts_per_router in cases:
            original = build_dragonfly(
                routers_per_group,
                global_links_per_router,
                hosts_per_router,
            )
            reordered = shuffle_ids_and_edges(original)
            for graph in (original, reordered):
                structures = discover_topology_structures(graph.get_physical_topology())
                report = verify_topology_structures(
                    graph.get_physical_topology(),
                    structures,
                )

                self.assertTrue(report.is_fully_covered)
                self.assertEqual(len(structures), 1)
                self.assertEqual(structures[0].structure_type, "dragonfly")
                self.assertEqual(
                    structures[0].parameters,
                    {
                        "group_count": routers_per_group * global_links_per_router + 1,
                        "routers_per_group": routers_per_group,
                        "global_links_per_router": global_links_per_router,
                        "hosts_per_router": hosts_per_router,
                    },
                )

    def test_rejects_broken_dragonfly_invariants(self):
        missing_local = build_dragonfly()
        missing_local.remove_edge(20, 21)
        missing_local.build_graph_config()

        rewired_global = build_dragonfly()
        rewired_global.remove_edge(20, 24)
        rewired_global.remove_edge(30, 34)
        rewired_global.add_netisim_edge(20, 30)
        rewired_global.add_netisim_edge(24, 34)
        rewired_global.build_graph_config()

        for graph in (missing_local, rewired_global):
            structures = discover_topology_structures(graph.get_physical_topology())
            report = verify_topology_structures(graph.get_physical_topology(), structures)

            self.assertFalse(report.is_fully_covered)
            self.assertNotIn(
                "dragonfly",
                [structure.structure_type for structure in structures],
            )

    def test_keeps_parallel_dragonfly_links_in_the_physical_structure(self):
        graph = build_dragonfly()
        graph.edges[0, 20]["edge_count"] = 2
        graph.edges[20, 24]["edge_count"] = 2
        graph.build_graph_config()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(len(structures), 1)
        self.assertEqual(
            sum(len(link.physical_link_ids) for link in structures[0].links),
            len(graph.get_physical_topology().links),
        )

    def test_discovers_ring_and_dragonfly_on_the_same_hosts(self):
        graph = build_ring_inside_dragonfly()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(
            [structure.structure_type for structure in structures],
            ["dimension-network", "dragonfly"],
        )

    def test_keeps_dragonfly_when_an_unrecognized_switch_extension_is_present(self):
        graph = build_dragonfly()
        graph.add_netisim_node(40)
        graph.add_netisim_edge(20, 40)
        graph.build_graph_config()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertIn(
            "dragonfly",
            [structure.structure_type for structure in report.verified_structures],
        )
        self.assertEqual(len(report.uncovered_link_ids), 1)

    def test_rejects_a_regular_graph_with_the_wrong_structure(self):
        graph = netsim.NetworkSimulationGraph()
        wrong_structure = nx.complete_bipartite_graph(4, 4)
        for node in wrong_structure.nodes:
            graph.add_netisim_host(node)
        for left, right in wrong_structure.edges:
            graph.add_netisim_edge(left, right)
        graph.build_graph_config()

        structures = discover_topology_structures(graph.get_physical_topology())

        self.assertEqual(structures, ())

    def test_keeps_parallel_links_in_a_verified_dimension_fullmesh(self):
        graph = build_dimension_fullmesh(rows=2, columns=2)
        graph.edges[0, 1]["edge_count"] = 2
        graph.build_graph_config()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(len(structures[0].links), 4)
        self.assertEqual(
            sum(len(link.physical_link_ids) for link in structures[0].links),
            5,
        )

    def test_discovers_and_verifies_a_two_layer_clos(self):
        graph = build_two_layer_clos()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(len(structures), 1)
        structure = structures[0]
        self.assertEqual(structure.structure_type, "two-layer-clos")
        self.assertEqual(
            structure.parameters,
            {"host_count": 8, "leaf_count": 2, "spine_count": 4, "hosts_per_leaf": 4},
        )
        self.assertEqual(len(structure.nodes), 14)
        self.assertEqual(len(structure.links), 16)
        self.assertEqual(len(structure.port_groups), 26)
        self.assertEqual(
            {coverage.name: coverage.node_ids for coverage in structure.destination_coverage},
            {"leaf:0": (0, 1, 2, 3), "leaf:1": (4, 5, 6, 7), "all-hosts": tuple(range(8))},
        )

    def test_keeps_switch_port_extension_as_an_explicit_non_rule_structure(self):
        graph = build_clos_with_port_extension()

        structures = discover_topology_structures(graph.get_physical_topology())
        report = verify_topology_structures(graph.get_physical_topology(), structures)

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(
            [structure.structure_type for structure in structures],
            ["two-layer-clos", "two-layer-clos", "port-extension"],
        )
        extension = structures[-1]
        self.assertFalse(extension.rule_capable)
        self.assertEqual(extension.parameters, {"link_count": 4})
        self.assertEqual(len(extension.links), 4)

    def test_writes_a_mixed_route_config_for_a_port_extension(self):
        graph = build_clos_with_port_extension()
        with tempfile.TemporaryDirectory() as output_dir:
            graph.output_dir = output_dir

            compiled = graph.gen_structured_route_config(routing_mode="mixed")

            self.assertEqual(compiled.mode, "mixed")
            with open(Path(output_dir) / "routing_rules.csv", newline="") as file:
                rule_rows = list(csv.DictReader(file))
            with open(Path(output_dir) / "routing_table.csv", newline="") as file:
                table_rows = list(csv.DictReader(file))
            self.assertTrue(rule_rows)
            self.assertTrue(table_rows)

    def test_rejects_rule_only_output_when_any_lookup_needs_a_table(self):
        graph = build_clos_with_port_extension()

        with self.assertRaisesRegex(ValueError, "requires every route lookup"):
            graph.gen_structured_route_config(routing_mode="rule", write_file=False)

    def test_discovers_overlapping_internal_and_multi_plane_structures_after_reordering(self):
        original = build_fullmesh_with_multi_plane_clos()
        reordered = shuffle_ids_and_edges(original)

        for graph in (original, reordered):
            structures = discover_topology_structures(graph.get_physical_topology())
            report = verify_topology_structures(graph.get_physical_topology(), structures)

            self.assertTrue(report.is_fully_covered)
            self.assertEqual(
                [structure.structure_type for structure in structures],
                [
                    "dimension-network",
                    "dimension-network",
                    "two-layer-clos",
                    "two-layer-clos",
                ],
            )

    def test_does_not_fully_cover_missing_extra_or_swapped_links(self):
        missing = build_fullmesh_with_multi_plane_clos()
        missing.remove_edge(18, 20)
        missing.build_graph_config()

        extra = build_fullmesh_with_multi_plane_clos()
        extra.add_netisim_edge(20, 21)
        extra.build_graph_config()

        swapped = build_dimension_fullmesh()
        swapped.remove_edge(0, 1)
        swapped.add_netisim_edge(0, 5)
        swapped.build_graph_config()

        for graph in (missing, extra, swapped):
            structures = discover_topology_structures(graph.get_physical_topology())
            report = verify_topology_structures(graph.get_physical_topology(), structures)
            self.assertFalse(report.is_fully_covered)


if __name__ == "__main__":
    unittest.main()
