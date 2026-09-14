#!/usr/bin/env python3
"""Small end-to-end checks on the generated OCS topology and the TE LP."""

import csv
import sys
import tempfile
import unittest
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_ocs_topology import (generate_compute_links, generate_ids,
                                   generate_ocs_links, generate_storage_links,
                                   generate_topology_csv)
from generate_ocs_routing_table import generate_routing_table
from jupiter_te_solver import candidates, read_topology, solve


class JupiterTeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.topology = Path(cls.tmp.name) / "topology.csv"
        cls.routing = Path(cls.tmp.name) / "routing_table.csv"
        compute, storage, compute_leaves, storage_leaves = generate_ids()
        links = []
        link_id = generate_compute_links(links, 0, compute, compute_leaves)
        link_id = generate_storage_links(links, link_id, storage, storage_leaves)
        generate_ocs_links(links, link_id, compute_leaves + storage_leaves)
        generate_topology_csv(links, cls.topology)
        generate_routing_table(cls.topology, cls.routing)
        cls.ports, cls.capacity, cls.host_leaf = read_topology(cls.topology)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_parallel_links_are_one_logical_edge_and_paths_have_at_most_two_hops(self):
        self.assertEqual(len(self.ports[37, 53]), 4)
        self.assertEqual(self.capacity[37, 53], 1.6e12)
        self.assertEqual(len(candidates(37, 53, self.capacity)), 13)
        self.assertEqual(len(candidates(37, 38, self.capacity)), 16)
        for src, dst in ((37, 53), (37, 38)):
            for _, _, transit, edges in candidates(src, dst, self.capacity):
                self.assertEqual(edges[0][0], src)
                self.assertEqual(edges[-1][1], dst)
                self.assertLessEqual(len(edges), 2)
                self.assertEqual(transit == -1, len(edges) == 1)

    def test_exact_host_port_at_final_leaf_and_four_ports_for_direct_hop(self):
        seen = {}
        with open(self.routing, newline="") as stream:
            for row in csv.DictReader(stream):
                node = int(row["nodeId"])
                if node in (37, 53) and int(row["dstNodeId"]) == 16 and int(row["dstPortId"]) == 0:
                    seen[node] = list(map(int, row["outPorts"].split()))
        self.assertEqual(self.host_leaf[16, 0], 53)
        self.assertEqual(len(seen[37]), 4)
        self.assertEqual(len(seen[53]), 1)

    def test_lp_limits_utilization_and_s_one_enforces_equal_capacity_shares(self):
        demand = {(37, 53): 0.8e12, (38, 53): 0.8e12}
        for s in (0, 0.5, 1):
            weights, util = solve(self.capacity, demand, s)
            loads = defaultdict(float)
            for (src, dst, transit), weight in weights.items():
                path = next(p for p in candidates(src, dst, self.capacity) if p[2] == transit)
                for edge in path[3]:
                    loads[edge] += demand[src, dst] * weight
            for od in demand:
                self.assertAlmostEqual(sum(w for k, w in weights.items() if k[:2] == od), 1, places=7)
            self.assertLessEqual(max(loads[e] / self.capacity[e] for e in loads), util + 1e-6)
            if s == 1:
                self.assertAlmostEqual(weights[37, 53, -1], 1 / 13, places=6)

    def test_no_direct_with_zero_history_has_sixteen_backup_paths(self):
        paths = candidates(37, 38, self.capacity)
        self.assertTrue(all(transit != -1 for _, _, transit, _ in paths))
        self.assertEqual(len(paths), 16)


if __name__ == "__main__":
    unittest.main()
