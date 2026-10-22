import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from route_split import (
    RouteCandidates,
    RouteSplit,
    build_direct_shortest_route_config,
    build_route_lookups,
    build_rule_route_lookups,
    build_shortest_route_lookups_for_destination,
    compare_topology_rules_to_brute_force,
    compile_route_config,
    group_route_candidates,
    split_route_lookups,
)
from test_topology_recognition import (
    build_balanced_sparse_tree,
    build_clos_with_port_extension,
    build_dimension_network,
    build_dimension_fullmesh,
    build_dragonfly,
    build_grouped_fullmesh_with_direct_switches,
    build_grouped_fullmesh_with_multi_plane_switches,
    build_grouped_fullmesh_with_shared_upper_planes,
    build_ring_inside_dragonfly,
)
from topology_recognition import discover_topology_structures
from topology_routing import build_topology_rule_lookups, compile_topology_rules
from topology_structure import verify_topology_structures


class RouteSplitTest(unittest.TestCase):
    def compile_candidates(self, candidates_by_destination, table_lookups=None):
        rule_lookups = {
            (5, destination, 2): candidates
            for destination, candidates in candidates_by_destination.items()
        }
        table_lookups = {} if table_lookups is None else table_lookups
        return compile_route_config(RouteSplit(
            rule_lookups=rule_lookups,
            table_lookups=table_lookups,
            extra_rule_lookups=(),
            baseline_lookup_count=len(rule_lookups) + len(table_lookups),
        ))

    def test_groups_shortest_and_merged_non_shortest_ports(self):
        candidates = group_route_candidates({4: 1, 2: 1, 8: 3, 6: 2})

        self.assertEqual(candidates, RouteCandidates((2, 4), (6, 8)))

    def test_keeps_an_entire_lookup_in_the_table_when_either_group_differs(self):
        lookup = (5, 9, 2)
        baseline = {lookup: {1: 1, 2: 1, 3: 2}}
        missing_non_shortest = {lookup: {1: 1, 2: 1}}

        split = split_route_lookups(baseline, missing_non_shortest)

        self.assertEqual(split.mode, "table")
        self.assertEqual(split.rule_lookups, {})
        self.assertEqual(split.table_lookups, baseline)

    def test_keeps_all_destination_ports_in_one_representation(self):
        first_port = (5, 9, 2)
        second_port = (5, 9, 3)
        baseline = {
            first_port: {1: 1, 2: 1},
            second_port: {1: 1, 2: 1},
        }
        rule_candidates = {
            first_port: {1: 4, 2: 4},
            second_port: {1: 4},
        }

        split = split_route_lookups(baseline, rule_candidates)

        self.assertEqual(split.mode, "table")
        self.assertEqual(split.rule_lookups, {})
        self.assertEqual(set(split.table_lookups), {first_port, second_port})

        compiled = compile_route_config(split)
        self.assertEqual(len(compiled.routing_rules), 0)
        self.assertEqual(len(compiled.port_groups), 0)

    def test_exact_rule_overrides_a_default_range(self):
        default = RouteCandidates((1, 2), (3,))
        exception = RouteCandidates((4,), (5, 6))

        compiled = self.compile_candidates({100: default, 101: exception, 102: default})

        self.assertEqual(len(compiled.routing_range_rules), 1)
        self.assertEqual(len(compiled.routing_rules), 1)
        range_rule = compiled.routing_range_rules[0]
        exact_rule = compiled.routing_rules[0]
        self.assertEqual(
            (range_rule.destination_start_node_id, range_rule.destination_end_node_id),
            (100, 102),
        )
        self.assertEqual(exact_rule.destination_node_id, 101)

        groups = {
            (group.node_id, group.group_id): group.ports
            for group in compiled.port_groups
        }
        self.assertEqual(
            tuple(groups[(5, group)] for group in range_rule.shortest_port_groups),
            (default.shortest_ports,),
        )
        self.assertEqual(
            tuple(groups[(5, group)] for group in range_rule.non_shortest_port_groups),
            (default.non_shortest_ports,),
        )
        self.assertEqual(
            tuple(groups[(5, group)] for group in exact_rule.shortest_port_groups),
            (exception.shortest_ports,),
        )
        self.assertEqual(
            tuple(groups[(5, group)] for group in exact_rule.non_shortest_port_groups),
            (exception.non_shortest_ports,),
        )

    def test_prefers_exact_rules_when_a_range_does_not_reduce_record_count(self):
        first = RouteCandidates((1,), ())
        second = RouteCandidates((2,), ())

        compiled = self.compile_candidates({100: first, 101: second})

        self.assertEqual(len(compiled.routing_rules), 2)
        self.assertFalse(compiled.routing_range_rules)

    def test_keeps_a_constant_run_as_one_range(self):
        candidates = RouteCandidates((1,), (2,))

        compiled = self.compile_candidates({100: candidates, 101: candidates})

        self.assertFalse(compiled.routing_rules)
        self.assertEqual(len(compiled.routing_range_rules), 1)

    def test_does_not_extend_a_range_across_an_unowned_destination(self):
        candidates = RouteCandidates((1,), ())
        table_lookup = (5, 101, 2)

        for table_lookups in ({}, {table_lookup: {9: 1}}):
            with self.subTest(table_owned=bool(table_lookups)):
                compiled = self.compile_candidates(
                    {100: candidates, 102: candidates},
                    table_lookups,
                )

                self.assertEqual(len(compiled.routing_rules), 2)
                self.assertFalse(compiled.routing_range_rules)
                self.assertEqual(compiled.table_lookups, table_lookups)

    def test_one_default_range_can_have_multiple_exact_exceptions(self):
        default = RouteCandidates((1,), (2,))
        first_exception = RouteCandidates((3,), ())
        second_exception = RouteCandidates((4,), (5,))

        compiled = self.compile_candidates({
            100: default,
            101: first_exception,
            102: default,
            103: second_exception,
            104: default,
        })

        self.assertEqual(len(compiled.routing_range_rules), 1)
        self.assertEqual(len(compiled.routing_rules), 2)
        self.assertEqual(
            [rule.destination_node_id for rule in compiled.routing_rules],
            [101, 103],
        )

    def test_verified_fullmesh_rules_match_every_baseline_lookup(self):
        graph = build_dimension_fullmesh(rows=3, columns=3)
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        baseline = build_route_lookups(graph)
        rule_candidates = build_rule_route_lookups(graph, verification)
        split = split_route_lookups(baseline, rule_candidates)

        self.assertEqual(split.mode, "rule")
        self.assertEqual(split.rule_coverage, 1.0)
        self.assertFalse(split.extra_rule_lookups)

    def test_shortest_path_dag_matches_enumerated_paths(self):
        for graph in (
            build_dimension_fullmesh(rows=3, columns=3),
            build_clos_with_port_extension(),
            build_balanced_sparse_tree(),
        ):
            enumerated = build_route_lookups(graph)
            direct = {}
            for destination in graph.host_ids:
                direct.update(
                    build_shortest_route_lookups_for_destination(graph, destination)
                )

            self.assertEqual(direct, enumerated)

    def test_direct_compiler_fully_covers_balanced_sparse_tree(self):
        graph = build_balanced_sparse_tree()
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        compiled = build_direct_shortest_route_config(graph, verification, "rule")

        self.assertEqual(compiled.mode, "rule")
        self.assertEqual(compiled.rule_coverage, 1.0)
        self.assertFalse(compiled.table_lookups)
        self.assertGreater(compiled.rule_lookup_count, 0)

    def test_direct_compiler_fully_covers_torus_and_dragonfly(self):
        for graph in (
            build_dimension_network((4, 5), ("ring", "ring")),
            build_dragonfly(routers_per_group=3, global_links_per_router=1),
            build_ring_inside_dragonfly(),
        ):
            topology = graph.get_physical_topology()
            verification = verify_topology_structures(
                topology,
                discover_topology_structures(topology),
            )

            compiled = build_direct_shortest_route_config(graph, verification, "rule")

            self.assertTrue(verification.is_fully_covered)
            self.assertEqual(compiled.mode, "rule")
            self.assertEqual(compiled.rule_coverage, 1.0)
            self.assertFalse(compiled.table_lookups)

    def test_topology_rules_match_brute_force_for_required_families(self):
        graphs = (
            build_dimension_network(
                (3, 3, 2),
                ("fullMesh", "fullMesh", "fullMesh"),
            ),
            build_grouped_fullmesh_with_direct_switches(),
            build_grouped_fullmesh_with_multi_plane_switches(),
            build_grouped_fullmesh_with_shared_upper_planes(),
            build_dragonfly(routers_per_group=3, global_links_per_router=1),
        )
        for graph in graphs:
            with self.subTest(node_count=len(graph)):
                topology = graph.get_physical_topology()
                verification = verify_topology_structures(
                    topology,
                    discover_topology_structures(topology),
                )
                topology_rules = compile_topology_rules(verification)

                self.assertIsNotNone(topology_rules)
                brute_force = build_route_lookups(graph)
                direct_rules = build_topology_rule_lookups(graph, topology_rules)
                self.assertEqual(direct_rules, brute_force)

                comparison = compare_topology_rules_to_brute_force(
                    graph,
                    topology_rules,
                )
                self.assertTrue(comparison.is_equivalent)
                self.assertEqual(
                    comparison.brute_force_lookups,
                    comparison.rule_lookups,
                )

    def test_topology_rules_reject_an_overlap_that_can_create_hybrid_paths(self):
        graph = build_ring_inside_dragonfly()
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        self.assertIsNone(compile_topology_rules(verification))

    def test_default_compiler_emits_topology_rules_without_expanding_lookups(self):
        graph = build_grouped_fullmesh_with_multi_plane_switches()

        compiled = graph.gen_structured_route_config(
            routing_mode="mixed",
            write_file=False,
        )

        self.assertEqual(compiled.mode, "rule")
        self.assertTrue(compiled.topology_rules)
        self.assertFalse(compiled.port_groups)
        self.assertFalse(compiled.routing_rules)
        self.assertFalse(compiled.routing_range_rules)
        self.assertFalse(compiled.table_lookups)
        self.assertEqual(compiled.rule_coverage, 1.0)
        self.assertEqual(compiled.rule_lookup_count, 0)

    def test_direct_compiler_compresses_lookups_into_range_rules(self):
        graph = build_dimension_fullmesh(rows=4, columns=4)
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        compiled = build_direct_shortest_route_config(graph, verification, "mixed")

        self.assertEqual(compiled.mode, "rule")
        self.assertEqual(compiled.rule_lookup_count, compiled.baseline_lookup_count)
        self.assertTrue(compiled.routing_range_rules)
        self.assertLess(
            len(compiled.routing_rules) + len(compiled.routing_range_rules),
            compiled.rule_lookup_count,
        )

    def test_direct_compiler_keeps_destination_ports_in_one_representation(self):
        graph = build_clos_with_port_extension()
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        compiled = build_direct_shortest_route_config(graph, verification, "mixed")
        rule_pairs = {
            (rule.current_node_id, rule.destination_node_id)
            for rule in compiled.routing_rules
        }
        rule_pairs.update(
            (rule.current_node_id, destination)
            for rule in compiled.routing_range_rules
            for destination in range(
                rule.destination_start_node_id,
                rule.destination_end_node_id + 1,
            )
        )
        table_pairs = {
            (current_node, destination_node)
            for current_node, destination_node, _ in compiled.table_lookups
        }

        self.assertFalse(rule_pairs & table_pairs)

    def test_port_extension_leaves_a_mixed_result(self):
        graph = build_clos_with_port_extension()
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        baseline = build_route_lookups(graph)
        rule_candidates = build_rule_route_lookups(graph, verification)
        split = split_route_lookups(baseline, rule_candidates)

        self.assertEqual(split.mode, "mixed")
        self.assertGreater(len(split.rule_lookups), 0)
        self.assertGreater(len(split.table_lookups), 0)

    def test_can_validate_a_bounded_set_of_host_pairs(self):
        graph = build_dimension_fullmesh(rows=3, columns=3)
        topology = graph.get_physical_topology()
        verification = verify_topology_structures(
            topology,
            discover_topology_structures(topology),
        )

        pairs = ((0, 1), (0, 8))
        baseline = build_route_lookups(graph, host_pairs=pairs)
        rule_candidates = build_rule_route_lookups(
            graph,
            verification,
            host_pairs=pairs,
        )

        self.assertEqual(baseline, rule_candidates)
        self.assertTrue(baseline)


if __name__ == "__main__":
    unittest.main()
