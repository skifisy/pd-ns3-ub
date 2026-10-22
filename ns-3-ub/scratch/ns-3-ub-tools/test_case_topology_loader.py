import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from case_topology_loader import load_case_route_lookups, load_case_topology


class CaseTopologyLoaderTest(unittest.TestCase):
    def test_preserves_reserved_ports_and_parallel_links(self):
        with tempfile.TemporaryDirectory() as directory:
            case_dir = Path(directory)
            with (case_dir / "node.csv").open("w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(["nodeId", "nodeType", "portNum", "forwardDelay"])
                writer.writerow(["0..1", "DEVICE", 8, "1ns"])
                writer.writerow([2, "SWITCH", 8, "2ns"])
            with (case_dir / "topology.csv").open("w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay"
                ])
                writer.writerow([0, 4, 2, 0, "400Gbps", "1ns"])
                writer.writerow([0, 7, 2, 3, "400Gbps", "1ns"])
                writer.writerow([1, 5, 2, 6, "400Gbps", "1ns"])

            graph = load_case_topology(case_dir)
            topology = graph.get_physical_topology()

            self.assertEqual(graph.get_link_ports(0, 2), [4, 7])
            self.assertEqual(graph.port_list, [(0, 8), (0, 8), (1, 8)])
            self.assertEqual(len(topology.links), 3)
            self.assertEqual(graph.edges[0, 2]["edge_count"], 2)

    def test_reads_compressed_route_ranges(self):
        with tempfile.TemporaryDirectory() as directory:
            case_dir = Path(directory)
            with (case_dir / "routing_table.csv").open("w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow(["nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"])
                writer.writerow(["0..1", 3, "4..5", "6 7", "1 2"])

            lookups = load_case_route_lookups(case_dir)

            self.assertEqual(len(lookups), 4)
            self.assertEqual(lookups[(1, 3, 5)], {6: 1, 7: 2})
            filtered = load_case_route_lookups(case_dir, {(1, 3, 5)})
            self.assertEqual(filtered, {(1, 3, 5): {6: 1, 7: 2}})

    def test_accepts_allocation_delay_when_forward_delay_is_empty(self):
        with tempfile.TemporaryDirectory() as directory:
            case_dir = Path(directory)
            with (case_dir / "node.csv").open("w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "nodeId", "nodeType", "portNum", "forwardDelay", "allocationDelay"
                ])
                writer.writerow([0, "DEVICE", 1, "", "3ns"])
                writer.writerow([1, "SWITCH", 1, "", "4ns"])
            with (case_dir / "topology.csv").open("w", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([
                    "nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay"
                ])
                writer.writerow([0, 0, 1, 0, "400Gbps", "1ns"])

            graph = load_case_topology(case_dir)

            self.assertEqual(graph.nodes[0]["allocation_delay"], "3ns")
            self.assertEqual(graph.nodes[1]["allocation_delay"], "4ns")
            self.assertEqual(graph.nodes[0]["forward_delay"], "0ns")
            self.assertEqual(graph.nodes[1]["forward_delay"], "0ns")


if __name__ == "__main__":
    unittest.main()
