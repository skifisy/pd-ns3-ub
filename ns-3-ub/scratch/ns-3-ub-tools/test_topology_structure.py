import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_sim_builder as netsim
from topology_structure import (
    DestinationCoverage,
    LocalPortGroup,
    StructureLink,
    StructureNode,
    TopologyStructure,
    verify_topology_structures,
)


class TopologyStructureTest(unittest.TestCase):
    def test_verifies_a_complete_parallel_link_mapping(self):
        graph = netsim.NetworkSimulationGraph()
        graph.add_netisim_host(0)
        graph.add_netisim_node(1)
        graph.add_netisim_edge(0, 1, edge_count=2)
        graph.build_graph_config()

        candidate = TopologyStructure(
            name="host-link:0",
            structure_type="host-link",
            nodes=(
                StructureNode(physical_node_id=0, coordinates=(0,), node_type="host"),
                StructureNode(physical_node_id=1, coordinates=(1,), node_type="node"),
            ),
            links=(
                StructureLink(left=(0,), right=(1,), physical_link_ids=(0, 1)),
            ),
        )

        report = verify_topology_structures(graph.get_physical_topology(), (candidate,))

        self.assertTrue(report.is_fully_covered)
        self.assertEqual(report.verified_structures, (candidate,))
        self.assertEqual(report.rejected_structures, ())
        self.assertEqual(report.uncovered_link_ids, ())
        self.assertEqual(report.duplicate_link_ids, ())

    def test_rejects_every_structure_that_claims_the_same_physical_link(self):
        graph = netsim.NetworkSimulationGraph()
        graph.add_netisim_host(0)
        graph.add_netisim_node(1)
        graph.add_netisim_edge(0, 1)
        graph.build_graph_config()

        def candidate(name):
            return TopologyStructure(
                name=name,
                structure_type="host-link",
                nodes=(
                    StructureNode(physical_node_id=0, coordinates=(0,), node_type="host"),
                    StructureNode(physical_node_id=1, coordinates=(1,), node_type="node"),
                ),
                links=(StructureLink(left=(0,), right=(1,), physical_link_ids=(0,)),),
            )

        report = verify_topology_structures(
            graph.get_physical_topology(),
            (candidate("first"), candidate("second")),
        )

        self.assertFalse(report.is_fully_covered)
        self.assertEqual(report.verified_structures, ())
        self.assertEqual(
            [rejected.structure.name for rejected in report.rejected_structures],
            ["first", "second"],
        )
        self.assertEqual(report.duplicate_link_ids, (0,))

    def test_rejects_a_structure_that_omits_an_internal_physical_link(self):
        graph = netsim.NetworkSimulationGraph()
        graph.add_netisim_host(0)
        graph.add_netisim_host(1)
        graph.add_netisim_host(2)
        graph.add_netisim_edge(0, 1)
        graph.add_netisim_edge(0, 2)
        graph.add_netisim_edge(1, 2)
        graph.build_graph_config()

        candidate = TopologyStructure(
            name="path:0",
            structure_type="path",
            nodes=(
                StructureNode(physical_node_id=0, coordinates=(0,), node_type="host"),
                StructureNode(physical_node_id=1, coordinates=(1,), node_type="host"),
                StructureNode(physical_node_id=2, coordinates=(2,), node_type="host"),
            ),
            links=(
                StructureLink(left=(0,), right=(1,), physical_link_ids=(0,)),
                StructureLink(left=(1,), right=(2,), physical_link_ids=(2,)),
            ),
        )

        report = verify_topology_structures(graph.get_physical_topology(), (candidate,))

        self.assertFalse(report.is_fully_covered)
        self.assertEqual(report.verified_structures, ())
        self.assertEqual(report.rejected_structures[0].structure, candidate)
        self.assertIn("internal", " ".join(report.rejected_structures[0].reasons))

    def test_rejects_candidates_with_the_same_name(self):
        graph = netsim.NetworkSimulationGraph()
        for node in range(4):
            graph.add_netisim_host(node)
        graph.add_netisim_edge(0, 1)
        graph.add_netisim_edge(2, 3)
        graph.build_graph_config()

        candidates = tuple(
            TopologyStructure(
                name="duplicate",
                structure_type="host-link",
                nodes=(
                    StructureNode(physical_node_id=left, coordinates=(0,), node_type="host"),
                    StructureNode(physical_node_id=right, coordinates=(1,), node_type="host"),
                ),
                links=(StructureLink(left=(0,), right=(1,), physical_link_ids=(link_id,)),),
            )
            for left, right, link_id in ((0, 1, 0), (2, 3, 1))
        )

        report = verify_topology_structures(graph.get_physical_topology(), candidates)

        self.assertEqual(report.verified_structures, ())
        self.assertEqual(len(report.rejected_structures), 2)
        self.assertTrue(
            all("unique" in " ".join(rejected.reasons) for rejected in report.rejected_structures)
        )

    def test_rejects_a_local_port_group_with_a_port_from_another_node(self):
        graph = netsim.NetworkSimulationGraph()
        graph.add_netisim_host(0)
        graph.add_netisim_node(1)
        graph.add_netisim_edge(0, 1)
        graph.build_graph_config()

        candidate = TopologyStructure(
            name="host-link:0",
            structure_type="host-link",
            nodes=(
                StructureNode(physical_node_id=0, coordinates=(0,), node_type="host"),
                StructureNode(physical_node_id=1, coordinates=(1,), node_type="node"),
            ),
            links=(StructureLink(left=(0,), right=(1,), physical_link_ids=(0,)),),
            port_groups=(LocalPortGroup(name="uplinks", physical_node_id=0, port_ids=(1,)),),
            destination_coverage=(DestinationCoverage(name="targets", node_ids=(1,)),),
        )

        report = verify_topology_structures(graph.get_physical_topology(), (candidate,))

        self.assertEqual(report.verified_structures, ())
        self.assertIn("port", " ".join(report.rejected_structures[0].reasons))


if __name__ == "__main__":
    unittest.main()
