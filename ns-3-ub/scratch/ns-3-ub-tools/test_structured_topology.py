import json
import tempfile
import unittest
from pathlib import Path

from structured_topology import build_structured_topology, load_structured_topology


def topology_defaults():
    return {
        "node": {"allocationDelay": "1ns", "forwardDelay": "0ns"},
        "link": {"bandwidth": "400Gbps", "delay": "20ns"},
    }


class StructuredTopologyTest(unittest.TestCase):
    def test_combines_fullmesh_port_extension_and_explicit_ports(self):
        data = {
            "version": 1,
            "defaults": topology_defaults(),
            "nodeGroups": [
                {"name": "device", "type": "device", "count": 4, "portCount": 8},
                {"name": "switch", "type": "switch", "count": 2, "portCount": 8},
            ],
            "explicitConnections": [{
                "endpoints": [
                    {"group": "device", "index": 0, "port": 7},
                    {"group": "switch", "index": 0, "port": 7},
                ],
            }],
            "groupConnections": [
                {
                    "pattern": "fullMesh",
                    "endpoints": [{"group": "device", "ports": [0, 2]}],
                },
                {
                    "pattern": "allToAll",
                    "endpoints": [
                        {"group": "device", "ports": [3, 6]},
                        {"group": "switch", "ports": [0, 6]},
                    ],
                },
            ],
        }

        graph = build_structured_topology(data)
        topology = graph.get_physical_topology()

        self.assertEqual(len(topology.nodes), 6)
        self.assertEqual(len(topology.links), 15)
        self.assertEqual(graph.port_list[0][1], 8)
        self.assertIn((0, 7), {
            (link.left_node, link.left_port)
            for link in topology.links
        })
        self.assertEqual(graph.graph["node_mapping"][4], {
            "group": "switch",
            "index": 0,
            "nodeId": 4,
        })

    def test_layered_connections_partition_separate_planes(self):
        data = {
            "version": 1,
            "defaults": topology_defaults(),
            "nodeGroups": [
                {"name": "device", "type": "device", "count": 4},
                {"name": "edge", "type": "switch", "count": 4},
                {"name": "top", "type": "switch", "count": 2},
            ],
            "groupConnections": [{
                "pattern": "oneToOne",
                "endpoints": [{"group": "device"}, {"group": "edge"}],
            }],
            "layeredConnections": [{
                "planeCount": 2,
                "planeMode": "separate",
                "layers": [{"group": "edge"}, {"group": "top"}],
                "connections": [{
                    "betweenLayers": [0, 1],
                    "pattern": "allToAll",
                }],
            }],
        }

        graph = build_structured_topology(data)

        self.assertEqual(len(graph.get_physical_topology().links), 8)
        self.assertEqual(set(graph.neighbors(8)), {4, 5})
        self.assertEqual(set(graph.neighbors(9)), {6, 7})

    def test_infers_unspecified_port_counts(self):
        data = {
            "version": 1,
            "defaults": topology_defaults(),
            "nodeGroups": [
                {"name": "left", "type": "device", "count": 2},
                {"name": "right", "type": "switch", "count": 1, "portCount": 4},
            ],
            "groupConnections": [{
                "pattern": "allToAll",
                "endpoints": [{"group": "left"}, {"group": "right"}],
            }],
        }

        graph = build_structured_topology(data)

        self.assertEqual([entry[1] for entry in graph.port_list], [1, 1, 4])

    def test_rejects_duplicate_explicit_port_use(self):
        data = {
            "version": 1,
            "defaults": topology_defaults(),
            "nodeGroups": [
                {"name": "device", "type": "device", "count": 3},
            ],
            "explicitConnections": [
                {
                    "endpoints": [
                        {"group": "device", "index": 0, "port": 0},
                        {"group": "device", "index": 1, "port": 0},
                    ],
                },
                {
                    "endpoints": [
                        {"group": "device", "index": 0, "port": 0},
                        {"group": "device", "index": 2, "port": 0},
                    ],
                },
            ],
        }

        with self.assertRaisesRegex(ValueError, "reuses node 0 port 0"):
            build_structured_topology(data)

    def test_loads_json_and_yaml_into_the_same_graph(self):
        data = {
            "version": 1,
            "defaults": topology_defaults(),
            "nodeGroups": [{"name": "device", "type": "device", "count": 2}],
            "groupConnections": [{
                "pattern": "oneToOne",
                "endpoints": [
                    {"group": "device"},
                    {"group": "device-copy"},
                ],
            }],
        }
        data["nodeGroups"].append({
            "name": "device-copy",
            "type": "device",
            "count": 2,
        })

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            json_path = root / "topology.json"
            yaml_path = root / "topology.yaml"
            json_path.write_text(json.dumps(data), encoding="utf-8")
            import yaml

            yaml_path.write_text(yaml.safe_dump(data), encoding="utf-8")

            json_graph = load_structured_topology(json_path)
            yaml_graph = load_structured_topology(yaml_path)

        self.assertEqual(json_graph.link_infos, yaml_graph.link_infos)
        self.assertEqual(json_graph.port_list, yaml_graph.port_list)


if __name__ == "__main__":
    unittest.main()
