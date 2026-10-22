import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_sim_builder as netsim


class PhysicalTopologyTest(unittest.TestCase):
    def test_preserves_parallel_links_ports_and_attributes(self):
        graph = netsim.NetworkSimulationGraph()
        graph.add_netisim_host(0, forward_delay="2ns", rack="rack-a")
        graph.add_netisim_node(1, forward_delay="3ns", layer="leaf")
        graph.add_netisim_edge(
            0,
            1,
            bandwidth="400Gbps",
            delay="10ns",
            edge_count=2,
            failure_domain="cable-pair-a",
        )

        graph.build_graph_config()
        topology = graph.get_physical_topology()

        self.assertEqual([node.node_id for node in topology.nodes], [0, 1])
        self.assertEqual(topology.nodes[0].node_type, "host")
        self.assertEqual(topology.nodes[0].attributes["rack"], "rack-a")
        self.assertEqual(topology.nodes[1].node_type, "node")
        self.assertEqual(topology.nodes[1].attributes["layer"], "leaf")

        self.assertEqual(len(topology.links), 2)
        self.assertEqual(
            [
                (link.left_node, link.left_port, link.right_node, link.right_port)
                for link in topology.links
            ],
            [(0, 0, 1, 0), (0, 1, 1, 1)],
        )
        self.assertEqual([link.link_id for link in topology.links], [0, 1])
        self.assertTrue(all(link.bandwidth == "400Gbps" for link in topology.links))
        self.assertTrue(all(link.delay == "10ns" for link in topology.links))
        self.assertTrue(
            all(link.attributes["failure_domain"] == "cable-pair-a" for link in topology.links)
        )

    def test_returns_the_same_normalized_object_until_the_graph_is_rebuilt(self):
        graph = netsim.NetworkSimulationGraph()
        graph.add_netisim_host(0)
        graph.add_netisim_node(1)
        graph.add_netisim_edge(0, 1)

        graph.build_graph_config()

        self.assertIs(graph.get_physical_topology(), graph.get_physical_topology())


if __name__ == "__main__":
    unittest.main()
