import csv
import sys
import tempfile
import unittest
from pathlib import Path

import networkx as nx

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_sim_builder as netsim


def all_shortest_paths(graph, source, target):
    try:
        return nx.all_shortest_paths(graph, source, target)
    except nx.NetworkXNoPath:
        return []


def parse_range(field):
    if ".." not in field:
        value = int(field)
        return value, value
    start, end = field.split("..", 1)
    return int(start), int(end)


def expanded_route_map(path):
    routes = {}
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            node_start, node_end = parse_range(row["nodeId"])
            dst_start, dst_end = parse_range(row["dstNodeId"])
            dst_port = int(row["dstPortId"])
            outports = tuple(int(value) for value in row["outPorts"].split())
            metrics = tuple(int(value) for value in row["metrics"].split())
            for node_id in range(node_start, node_end + 1):
                for dst_id in range(dst_start, dst_end + 1):
                    key = (node_id, dst_id, dst_port)
                    value = (outports, metrics)
                    old_value = routes.get(key)
                    if old_value is not None and old_value != value:
                        raise AssertionError(f"conflicting route for {key}: {old_value} != {value}")
                    routes[key] = value
    return routes


def build_two_switch_graph():
    graph = netsim.NetworkSimulationGraph()
    for host_id in range(4):
        graph.add_netisim_host(host_id, forward_delay="1ns")
    graph.add_netisim_node(4, forward_delay="1ns")
    graph.add_netisim_node(5, forward_delay="1ns")
    graph.add_netisim_edge(0, 4, bandwidth="400Gbps", delay="20ns")
    graph.add_netisim_edge(1, 4, bandwidth="400Gbps", delay="20ns")
    graph.add_netisim_edge(2, 5, bandwidth="400Gbps", delay="20ns")
    graph.add_netisim_edge(3, 5, bandwidth="400Gbps", delay="20ns")
    graph.add_netisim_edge(4, 5, bandwidth="400Gbps", delay="20ns")
    return graph


def build_two_layer_clos_graph(host_num=8, leaf_sw_num=2):
    graph = netsim.NetworkSimulationGraph()
    spine_sw_num = host_num // leaf_sw_num
    leaf_base = host_num
    spine_base = host_num + leaf_sw_num

    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay="1ns")
    for leaf_id in range(leaf_base, leaf_base + leaf_sw_num):
        graph.add_netisim_node(leaf_id, forward_delay="1ns")
    for spine_id in range(spine_base, spine_base + spine_sw_num):
        graph.add_netisim_node(spine_id, forward_delay="1ns")

    host_per_leaf = host_num // leaf_sw_num
    for host_id in range(host_num):
        leaf_id = leaf_base + (host_id // host_per_leaf)
        graph.add_netisim_edge(host_id, leaf_id, bandwidth="400Gbps", delay="20ns")
    for leaf_id in range(leaf_base, leaf_base + leaf_sw_num):
        for spine_id in range(spine_base, spine_base + spine_sw_num):
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth="400Gbps", delay="20ns")
    return graph


class CompressedRouteTableTest(unittest.TestCase):
    def test_generic_compressed_routes_match_exact_routes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)

            exact = build_two_switch_graph()
            exact.output_dir = str(tmpdir / "exact")
            exact.build_graph_config()
            exact.gen_route_table(path_finding_algo=all_shortest_paths, multiple_workers=1)

            compressed = build_two_switch_graph()
            compressed.output_dir = str(tmpdir / "compressed")
            compressed.build_graph_config()
            compressed.gen_compressed_route_table(path_finding_algo=all_shortest_paths)

            exact_csv = tmpdir / "exact" / "routing_table.csv"
            compressed_csv = tmpdir / "compressed" / "routing_table.csv"
            self.assertEqual(expanded_route_map(compressed_csv), expanded_route_map(exact_csv))

            compressed_text = compressed_csv.read_text(encoding="utf-8")
            self.assertIn("..", compressed_text)

    def test_parallel_compressed_routes_match_serial_compressed_routes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)

            serial = build_two_switch_graph()
            serial.output_dir = str(tmpdir / "serial")
            serial.build_graph_config()
            serial.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=1)

            parallel = build_two_switch_graph()
            parallel.output_dir = str(tmpdir / "parallel")
            parallel.build_graph_config()
            parallel.gen_compressed_route_table(
                path_finding_algo=all_shortest_paths,
                multiple_workers=2,
                chunk_size=2,
            )

            serial_csv = tmpdir / "serial" / "routing_table.csv"
            parallel_csv = tmpdir / "parallel" / "routing_table.csv"
            self.assertEqual(expanded_route_map(parallel_csv), expanded_route_map(serial_csv))

    def test_compressed_routes_require_on_demand_transport(self):
        with tempfile.TemporaryDirectory() as tmp:
            graph = build_two_switch_graph()
            graph.output_dir = str(Path(tmp) / "compressed")
            graph.build_graph_config()
            graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths)

            with self.assertRaisesRegex(ValueError, "include_transport=False"):
                graph.write_config()

    def test_compressed_routes_merge_each_destination_port_independently(self):
        graph = build_two_switch_graph()
        graph.build_graph_config()
        route_table = graph.init_sparse_routing_table()
        route_table[(4, 2, 1)] = {0: 3}
        route_table[(4, 3, 1)] = {0: 3}
        route_table[(4, 2, 2)] = {1: 3}
        route_table[(4, 3, 2)] = {1: 3}

        rows = graph.compressed_route_rows(route_table)

        self.assertIn(("4", "2..3", 1, "0", "3"), rows)
        self.assertIn(("4", "2..3", 2, "1", "3"), rows)

    def test_route_table_validator_rejects_conflicting_range_overlap(self):
        with tempfile.TemporaryDirectory() as tmp:
            route_csv = Path(tmp) / "routing_table.csv"
            route_csv.write_text(
                "\n".join(
                    [
                        "nodeId,dstNodeId,dstPortId,outPorts,metrics",
                        "10..12,1..3,2,7,2",
                        "11..13,2..4,2,8,2",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "冲突覆盖"):
                netsim.validate_route_table_no_conflicts(route_csv)

    def test_route_table_validator_allows_redundant_identical_overlap(self):
        with tempfile.TemporaryDirectory() as tmp:
            route_csv = Path(tmp) / "routing_table.csv"
            route_csv.write_text(
                "\n".join(
                    [
                        "nodeId,dstNodeId,dstPortId,outPorts,metrics",
                        "10..12,1..3,2,7,2",
                        "11,2,2,7,2",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            self.assertTrue(netsim.validate_route_table_no_conflicts(route_csv))

    def test_specialized_clos_compressed_routes_match_exact_routes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            host_num = 8
            leaf_sw_num = 2

            exact = build_two_layer_clos_graph(host_num, leaf_sw_num)
            exact.output_dir = str(tmpdir / "exact")
            exact.build_graph_config()
            exact.gen_route_table(path_finding_algo=all_shortest_paths, multiple_workers=1)

            specialized = build_two_layer_clos_graph(host_num, leaf_sw_num)
            specialized.output_dir = str(tmpdir / "specialized")
            specialized.build_graph_config()
            specialized.gen_2layer_clos_compressed_route_table(host_num, leaf_sw_num)

            exact_csv = tmpdir / "exact" / "routing_table.csv"
            specialized_csv = tmpdir / "specialized" / "routing_table.csv"
            self.assertEqual(expanded_route_map(specialized_csv), expanded_route_map(exact_csv))

    def test_generic_compressed_clos_routes_match_exact_routes(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            host_num = 8
            leaf_sw_num = 2

            exact = build_two_layer_clos_graph(host_num, leaf_sw_num)
            exact.output_dir = str(tmpdir / "exact")
            exact.build_graph_config()
            exact.gen_route_table(path_finding_algo=all_shortest_paths, multiple_workers=1)

            generic = build_two_layer_clos_graph(host_num, leaf_sw_num)
            generic.output_dir = str(tmpdir / "generic")
            generic.build_graph_config()
            generic.gen_compressed_route_table(
                path_finding_algo=all_shortest_paths,
                multiple_workers=2,
                chunk_size=4,
            )
            generic.write_config(include_transport=False)

            exact_csv = tmpdir / "exact" / "routing_table.csv"
            generic_csv = tmpdir / "generic" / "routing_table.csv"
            self.assertEqual(expanded_route_map(generic_csv), expanded_route_map(exact_csv))
            self.assertFalse((tmpdir / "generic" / "transport_channel.csv").exists())


if __name__ == "__main__":
    unittest.main()
