import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

import networkx as nx

sys.path.insert(0, str(Path(__file__).resolve().parent))

from net_sim_builder import NetworkSimulationGraph
from compressed_routing import (
    HEADER_FORMAT,
    HEADER_SIZE,
    MAGIC,
    build_compressed_routing_artifact,
    read_compressed_routing_artifact,
)
from route_split import CompiledRouteConfig
from prepare_structured_case import main as prepare_main
from structured_case import (
    compile_structured_routes,
    export_structured_config,
    prepare_structured_case,
    serialize_structured_config,
    write_structured_config,
)
from test_topology_recognition import (
    build_clos_with_port_extension,
    build_dimension_fullmesh,
    build_grouped_fullmesh_with_direct_switches,
    build_grouped_fullmesh_with_multi_plane_switches,
)


class StructuredCaseTest(unittest.TestCase):
    @staticmethod
    def routing_lookup_signature(graph, artifact_data):
        index = read_compressed_routing_artifact(artifact_data)
        destination_ports = {node_id: set() for node_id in graph.host_ids}
        for link in graph.get_physical_topology().links:
            if link.left_node in destination_ports:
                destination_ports[link.left_node].add(link.left_port)
            if link.right_node in destination_ports:
                destination_ports[link.right_node].add(link.right_port)
        return tuple(
            (
                current,
                destination,
                destination_port,
                index.lookup(current, destination, destination_port),
            )
            for current in sorted(graph.nodes)
            for destination, ports in sorted(destination_ports.items())
            for destination_port in sorted(ports)
        )

    def test_compiles_without_writing_intermediate_files(self):
        with tempfile.TemporaryDirectory() as directory:
            graph = build_dimension_fullmesh(rows=2, columns=2)
            graph.output_dir = str(Path(directory) / "unused-output")

            result = compile_structured_routes(
                graph,
                use_cache=False,
            )

            self.assertEqual(result.config.mode, "rule")
            self.assertFalse(Path(graph.output_dir).exists())

    def test_reuses_content_addressed_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            graph = build_dimension_fullmesh(rows=2, columns=2)

            first = compile_structured_routes(graph, cache_dir=directory)
            second = compile_structured_routes(graph, cache_dir=directory)

            self.assertFalse(first.cache_hit)
            self.assertTrue(second.cache_hit)
            self.assertEqual(first.cache_key, second.cache_key)
            self.assertEqual(first.config, second.config)
            self.assertEqual(len(list(Path(directory).glob("*.json"))), 1)

    def test_cache_key_is_stable_after_networkx_dispatch_compiles(self):
        with tempfile.TemporaryDirectory() as directory:
            first = compile_structured_routes(
                build_dimension_fullmesh(rows=2, columns=2),
                cache_dir=directory,
            )
            second = compile_structured_routes(
                build_dimension_fullmesh(rows=2, columns=2),
                cache_dir=directory,
            )

            self.assertFalse(first.cache_hit)
            self.assertTrue(second.cache_hit)
            self.assertEqual(first.cache_key, second.cache_key)

    def test_serializes_counted_internal_protocol(self):
        graph = build_dimension_fullmesh(rows=2, columns=2)
        result = compile_structured_routes(graph, use_cache=False)

        payload = serialize_structured_config(
            graph,
            result.config,
            routing_fingerprint=result.cache_key,
        )

        self.assertTrue(payload.startswith("NS3_UB_STRUCTURED 4\nNODES 4\n"))
        self.assertIn(f"\nROUTING_FINGERPRINT {result.cache_key}\n", payload)
        self.assertIn("\nLINKS 4\n", payload)
        self.assertIn("\nTABLE 0\n", payload)
        self.assertIn("\nPORT_GROUPS ", payload)
        self.assertIn("\nRULE_RANGES ", payload)
        self.assertIn("\nTOPOLOGY_RULES 1\n", payload)
        self.assertIn("dimension 2 2 2 2 fullMesh fullMesh 4\n", payload)
        self.assertTrue(payload.endswith("END\n"))

    def test_stream_writer_matches_string_serializer(self):
        graph = build_dimension_fullmesh(rows=2, columns=2)
        result = compile_structured_routes(graph, use_cache=False)
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as output:
            write_structured_config(
                graph,
                result.config,
                output,
                routing_fingerprint=result.cache_key,
            )
            output.seek(0)
            self.assertEqual(
                output.read(),
                serialize_structured_config(
                    graph,
                    result.config,
                    routing_fingerprint=result.cache_key,
                ),
            )

    def test_version_four_serializer_requires_routing_fingerprint(self):
        graph = build_dimension_fullmesh(rows=2, columns=2)
        result = compile_structured_routes(graph, use_cache=False)

        with self.assertRaisesRegex(ValueError, "64-character hexadecimal"):
            serialize_structured_config(graph, result.config)

        with self.assertRaisesRegex(ValueError, "64-character hexadecimal"):
            serialize_structured_config(
                graph,
                result.config,
                routing_fingerprint="not-a-sha256",
            )

    def test_prepares_reusable_case_without_running_simulator(self):
        with tempfile.TemporaryDirectory() as directory:
            case_path = Path(directory) / "case"
            case_path.mkdir()
            sentinel = case_path / "traffic.csv"
            sentinel.write_text("existing workload\n", encoding="utf-8")

            prepared = prepare_structured_case(
                build_dimension_fullmesh(rows=2, columns=2),
                case_path,
                use_cache=False,
            )

            self.assertEqual(prepared.case_path, case_path.resolve())
            self.assertTrue(prepared.topology_path.is_file())
            self.assertTrue(prepared.compressed_routing_path.is_file())
            self.assertTrue(prepared.summary_path.is_file())
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "existing workload\n")
            self.assertTrue(
                prepared.topology_path.read_text(encoding="utf-8").endswith("END\n")
            )
            summary = json.loads(
                prepared.summary_path.read_text(encoding="utf-8")
            )
            self.assertEqual(summary["protocolVersion"], 4)
            self.assertEqual(summary["compilerVersion"], 8)
            self.assertEqual(summary["routeGeneration"], "mixed")
            self.assertEqual(
                summary["compressedRouting"]["bytes"],
                prepared.compressed_routing_path.stat().st_size,
            )

    def test_compressed_routing_artifact_has_portable_header_and_checksum(self):
        with tempfile.TemporaryDirectory() as directory:
            prepared = prepare_structured_case(
                build_dimension_fullmesh(rows=2, columns=2),
                Path(directory) / "case",
                use_cache=False,
            )

            data = prepared.compressed_routing_path.read_bytes()
            header = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
            self.assertEqual(header[0], MAGIC)
            self.assertEqual(header[1:3], (1, 0))
            self.assertEqual(header[4], HEADER_SIZE)
            self.assertEqual(header[5:10], (4, 4, 4, 8, 4))
            self.assertEqual(header[10:13], (1, 1, 0))
            self.assertEqual(header[13].hex(), prepared.compile_result.cache_key)
            payload = data[HEADER_SIZE:]
            self.assertEqual(header[14], len(payload))
            self.assertEqual(header[15], zlib.crc32(payload) & 0xFFFFFFFF)
            index = read_compressed_routing_artifact(
                data,
                prepared.compile_result.cache_key,
            )
            self.assertEqual(index.lookup(0, 1, 0), ((0,), ()))
            self.assertEqual(index.lookup(0, 1), ((0,), ()))

    def test_shortest_dag_fast_path_matches_topology_rule_interpretation(self):
        graph = build_grouped_fullmesh_with_multi_plane_switches()
        result = compile_structured_routes(graph, use_cache=False)

        fast = build_compressed_routing_artifact(
            graph,
            result.config,
            result.cache_key,
            use_shortest_dag_fast_path=True,
        )
        interpreted = build_compressed_routing_artifact(
            graph,
            result.config,
            result.cache_key,
            use_shortest_dag_fast_path=False,
        )

        self.assertEqual(
            self.routing_lookup_signature(graph, fast.data),
            self.routing_lookup_signature(graph, interpreted.data),
        )

    def test_rejects_ambiguous_node_primary_candidate_classification(self):
        graph = NetworkSimulationGraph()
        for node_id in range(4):
            graph.add_netisim_host(node_id)
        graph.add_netisim_edge(0, 1)
        graph.add_netisim_edge(0, 2)
        graph.add_netisim_edge(1, 3)
        graph.add_netisim_edge(2, 3)
        graph.build_graph_config()

        config = CompiledRouteConfig(
            port_groups=(),
            routing_rules=(),
            routing_range_rules=(),
            topology_rules=(),
            table_lookups={
                (0, 3, 0): {0: 1, 1: 2},
                (0, 3, 1): {0: 2, 1: 1},
            },
            mode="table",
            rule_coverage=0.0,
            rule_lookup_count=0,
            baseline_lookup_count=2,
        )

        with self.assertRaisesRegex(
            ValueError,
            r"both shortest and non-shortest candidates for node-primary lookup \(0, 3\)",
        ):
            build_compressed_routing_artifact(graph, config, "0" * 64)

    def test_route_verification_is_explicit_and_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            prepared = prepare_structured_case(
                build_dimension_fullmesh(rows=2, columns=2),
                Path(directory) / "case",
                use_cache=False,
                verify_routes=True,
            )

            summary = json.loads(prepared.summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["verification"]["mismatchCount"], 0)
            self.assertEqual(
                summary["verification"]["routeLookupCount"],
                summary["compressedRouting"]["routeLookupCount"],
            )

    def test_route_generation_strategies_match_for_supported_compositions(self):
        factories = {
            "ubx-shaped": build_grouped_fullmesh_with_direct_switches,
            "1dpod-shaped": build_grouped_fullmesh_with_multi_plane_switches,
        }
        for name, factory in factories.items():
            with self.subTest(topology=name), tempfile.TemporaryDirectory() as directory:
                lookup_signatures = set()
                lookup_counts = set()
                for generation in ("full", "structured", "mixed"):
                    prepared = prepare_structured_case(
                        factory(),
                        Path(directory) / generation,
                        route_generation=generation,
                        use_cache=False,
                        verify_routes=True,
                    )
                    summary = json.loads(
                        prepared.summary_path.read_text(encoding="utf-8")
                    )
                    self.assertEqual(summary["verification"]["mismatchCount"], 0)
                    lookup_counts.add(summary["verification"]["routeLookupCount"])
                    lookup_signatures.add(
                        self.routing_lookup_signature(
                            factory(),
                            prepared.compressed_routing_path.read_bytes(),
                        )
                    )
                self.assertEqual(len(lookup_counts), 1)
                self.assertEqual(len(lookup_signatures), 1)

    def test_mixed_generation_covers_unrecognized_port_extension(self):
        with tempfile.TemporaryDirectory() as directory:
            prepared = prepare_structured_case(
                build_clos_with_port_extension(),
                Path(directory) / "mixed",
                route_generation="mixed",
                use_cache=False,
                verify_routes=True,
            )
            summary = json.loads(prepared.summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["verification"]["mismatchCount"], 0)
            self.assertGreater(summary["tableLookupCount"], 0)
            with self.assertRaisesRegex(ValueError, "requires every route lookup"):
                prepare_structured_case(
                    build_clos_with_port_extension(),
                    Path(directory) / "structured",
                    route_generation="structured",
                    use_cache=False,
                )

    def test_full_generation_keeps_complete_routes_out_of_topology_text(self):
        with tempfile.TemporaryDirectory() as directory:
            prepared = prepare_structured_case(
                build_dimension_fullmesh(rows=2, columns=2),
                Path(directory) / "case",
                route_generation="full",
                use_cache=False,
            )

            topology = prepared.topology_path.read_text(encoding="utf-8")
            self.assertIn("\nROUTING_FINGERPRINT ", topology)
            self.assertIn("\nTABLE 0\n", topology)
            self.assertTrue(prepared.compressed_routing_path.is_file())

    def test_prepare_cli_accepts_python_builder(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            builder = root / "topology_builder.py"
            builder.write_text(
                """\
from net_sim_builder import NetworkSimulationGraph


def build_topology():
    graph = NetworkSimulationGraph()
    graph.add_netisim_host(0, allocation_delay=\"1ns\", forward_delay=\"0ns\")
    graph.add_netisim_host(1, allocation_delay=\"1ns\", forward_delay=\"0ns\")
    graph.add_netisim_edge(0, 1)
    graph.build_graph_config()
    return graph
""",
                encoding="utf-8",
            )
            case_path = root / "python-case"

            with contextlib.redirect_stdout(io.StringIO()):
                status = prepare_main([
                    f"{builder}:build_topology",
                    "--case-path",
                    str(case_path),
                    "--no-cache",
                ])

            self.assertEqual(status, 0)
            self.assertTrue((case_path / "structured_topology.txt").is_file())
            self.assertTrue((case_path / "compressed_routing.bin").is_file())
            self.assertTrue((case_path / "structured_summary.json").is_file())

    def test_prepare_cli_accepts_yaml_topology(self):
        import yaml

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            topology_path = root / "topology.yaml"
            topology_path.write_text(
                yaml.safe_dump({
                    "version": 1,
                    "defaults": {
                        "node": {"allocationDelay": "1ns", "forwardDelay": "0ns"},
                        "link": {"bandwidth": "400Gbps", "delay": "20ns"},
                    },
                    "nodeGroups": [
                        {"name": "device", "type": "device", "count": 2},
                    ],
                    "groupConnections": [{
                        "pattern": "fullMesh",
                        "endpoints": [{"group": "device"}],
                    }],
                }),
                encoding="utf-8",
            )
            case_path = root / "yaml-case"

            with contextlib.redirect_stdout(io.StringIO()):
                status = prepare_main([
                    str(topology_path),
                    "--case-path",
                    str(case_path),
                    "--no-cache",
                ])

            self.assertEqual(status, 0)
            self.assertTrue((case_path / "structured_topology.txt").is_file())
            self.assertTrue((case_path / "compressed_routing.bin").is_file())
            self.assertTrue((case_path / "structured_summary.json").is_file())

    def test_diagnostic_export_is_a_snapshot_of_compiled_config(self):
        with tempfile.TemporaryDirectory() as directory:
            graph = build_dimension_fullmesh(rows=2, columns=2)
            result = compile_structured_routes(graph, use_cache=False)

            export_structured_config(graph, result, directory)

            output = Path(directory)
            self.assertEqual(
                {path.name for path in output.iterdir()},
                {
                    "node.csv",
                    "topology.csv",
                    "routing_table.csv",
                    "routing_port_groups.csv",
                    "routing_rules.csv",
                    "routing_rule_ranges.csv",
                    "routing_topology_rules.csv",
                    "structured_summary.json",
                },
            )
            self.assertEqual(
                len((output / "routing_rules.csv").read_text().splitlines()) - 1,
                len(result.config.routing_rules),
            )
            self.assertEqual(
                len((output / "routing_topology_rules.csv").read_text().splitlines()) - 1,
                sum(len(rule.nodes) for rule in result.config.topology_rules),
            )

    def test_node_delay_fields_are_independent(self):
        graph = NetworkSimulationGraph()
        graph.add_netisim_host(0, allocation_delay="3ns", forward_delay="7ns")
        graph.add_netisim_host(1, allocation_delay="5ns", forward_delay="11ns")
        graph.add_netisim_edge(0, 1)
        graph.build_graph_config()

        topology = graph.get_physical_topology()
        self.assertEqual(topology.nodes[0].allocation_delay, "3ns")
        self.assertEqual(topology.nodes[0].forward_delay, "7ns")

    def test_custom_path_finder_keeps_the_baseline_verification_path(self):
        calls = []

        def custom_paths(graph, source, destination):
            calls.append((source, destination))
            return nx.all_shortest_paths(graph, source, destination)

        result = compile_structured_routes(
            build_dimension_fullmesh(rows=2, columns=2),
            path_finding_algo=custom_paths,
            use_cache=False,
        )

        self.assertTrue(calls)
        self.assertEqual(result.config.mode, "rule")


if __name__ == "__main__":
    unittest.main()
