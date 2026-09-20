#!/usr/bin/env python3
"""Small end-to-end checks on the generated OCS topology and the TE LP."""

import csv
import subprocess
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
from analyze_jupiter_te import read_topology, summarize


def candidates(src, dst, capacities):
    paths = []
    if (src, dst) in capacities:
        paths.append((src, dst, -1, ((src, dst),)))
    leaves = sorted({node for edge in capacities for node in edge})
    for middle in leaves:
        if (middle != src and middle != dst and
                (src, middle) in capacities and (middle, dst) in capacities):
            paths.append((src, dst, middle, ((src, middle), (middle, dst))))
    return paths


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
        cls.capacity = read_topology(cls.topology)
        cls.ports = defaultdict(list)
        cls.host_leaf = {}
        with open(cls.topology, newline="") as stream:
            for row in csv.DictReader(stream):
                a, ap = int(row["nodeId1"]), int(row["portId1"])
                b, bp = int(row["nodeId2"]), int(row["portId2"])
                if a >= 37 and b >= 37:
                    cls.ports[a, b].append(ap)
                    cls.ports[b, a].append(bp)
                else:
                    host, hp, leaf = (a, ap, b) if a < 37 else (b, bp, a)
                    cls.host_leaf[host, hp] = leaf

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

    def test_standalone_routing_generator_cli_matches_library_output(self):
        output = Path(self.tmp.name) / "routing_table_cli.csv"
        script = Path(__file__).resolve().parent / "generate_routing_table.py"
        subprocess.run(
            [
                sys.executable,
                str(script),
                "--topology",
                str(self.topology),
                "--output",
                str(output),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(output.read_text(), self.routing.read_text())

    def test_no_direct_with_zero_history_has_sixteen_backup_paths(self):
        paths = candidates(37, 38, self.capacity)
        self.assertTrue(all(transit != -1 for _, _, transit, _ in paths))
        self.assertEqual(len(paths), 16)

    def test_utilization_excludes_warmup_and_partial_window(self):
        link_bytes = Path(self.tmp.name) / "link_bytes.csv"
        output = Path(self.tmp.name) / "utilization.csv"
        with open(link_bytes, "w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("window", "src_leaf", "dst_leaf", "wire_bytes", "complete"))
            writer.writerow((19, 37, 53, 10**12, 1))
            writer.writerow((20, 37, 53, int(0.25 * 30 * self.capacity[37, 53] / 8), 1))
            writer.writerow((21, 37, 53, 10**12, 0))
        summarize(self.topology, link_bytes, output)
        with open(output, newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["window"], "20")
        self.assertAlmostEqual(float(rows[0]["max_utilization"]), 0.25)

    def test_smoke_traffic_crosses_two_te_boundaries(self):
        traffic = Path(__file__).resolve().parent / "jupiter_te_smoke_traffic.csv"
        with open(traffic, newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 24)
        self.assertEqual([int(row["taskId"]) for row in rows], list(range(24)))
        self.assertEqual({row["opType"] for row in rows}, {"URMA_WRITE"})
        self.assertEqual({int(row["dataSize(Byte)"]) for row in rows}, {1048576})
        self.assertEqual({int(row["phaseId"]) for row in rows[:8]}, {0})
        self.assertEqual({row["dependOnPhases"] for row in rows[:8]}, {""})
        self.assertEqual({int(row["phaseId"]) for row in rows[8:16]}, {1})
        self.assertEqual({int(row["phaseId"]) for row in rows[16:]}, {2})
        self.assertEqual({row["dependOnPhases"] for row in rows}, {""})
        self.assertEqual(
            [row["delay"] for row in rows[8:16]],
            ["31s", "31100ms", "31200ms", "31300ms", "31400ms", "31500ms",
             "31600ms", "31700ms"],
        )
        self.assertEqual(
            [row["delay"] for row in rows[16:]],
            ["62s", "62100ms", "62200ms", "62300ms", "62400ms", "62500ms",
             "62600ms", "62700ms"],
        )

    def test_wcmp_trace_drives_directed_task_intervals_and_hotspot_matrices(self):
        case = Path(self.tmp.name) / "wcmp-analysis-case"
        output = case / "output"
        te_output = case / "jupiter_te"
        output.mkdir(parents=True)
        te_output.mkdir(parents=True)

        with (output / "task_statistics.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                (
                    "taskId",
                    "sourceNode",
                    "destNode",
                    "taskStartTime(us)",
                    "taskCompletesTime(us)",
                    "opType",
                )
            )
            writer.writerow((1, 1, 2, 0, 20_000_000, "URMA_WRITE"))
            writer.writerow((2, 1, 2, 10_000_000, 30_000_000, "URMA_READ"))

        with (case / "traffic.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("taskId", "opType"))
            writer.writerow((1, "URMA_WRITE"))
            writer.writerow((2, "URMA_READ"))

        with (te_output / "WcmpSelectionTrace.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(
                (
                    "sim_time_seconds",
                    "task_id",
                    "epoch",
                    "flow_hash",
                    "sip",
                    "dip",
                    "sport",
                    "dport",
                    "priority",
                    "src_leaf",
                    "dst_leaf",
                    "transit_leaf",
                    "next_leaf",
                    "out_port",
                    "decision_source",
                )
            )
            writer.writerow(
                (0.0, 1, 0, 101, "10.0.1.1", "10.0.2.1", 1, 2, 7,
                 37, 53, -1, 53, 1, "fallback")
            )
            # A second transport flow for one task must not double-count its lifetime.
            writer.writerow(
                (0.1, 1, 0, 102, "10.0.1.1", "10.0.2.1", 3, 4, 7,
                 37, 53, -1, 53, 2, "pinned")
            )
            # READ payload travels from the task destination back to its source.
            writer.writerow(
                (10.0, 2, 0, 201, "10.0.2.1", "10.0.1.1", 5, 6, 7,
                 53, 37, -1, 37, 3, "fallback")
            )

        analysis_script = Path(__file__).resolve().parent / "leaf_pair_active_flow_analysis.py"
        subprocess.run(
            [sys.executable, str(analysis_script), str(case), "--leaf-nodes=37,53"],
            check=True,
            capture_output=True,
            text=True,
        )
        analysis_output = output / "leaf_pair_active_flow_analysis"
        with (analysis_output / "task_leaf_mapping.csv").open(newline="") as stream:
            mappings = list(csv.DictReader(stream))
        self.assertEqual(
            [(row["taskId"], row["leaf_a"], row["leaf_b"]) for row in mappings],
            [("1", "37", "53"), ("2", "53", "37")],
        )
        with (analysis_output / "leaf_pair_active_flow_intervals.csv").open(
            newline=""
        ) as stream:
            intervals = list(csv.DictReader(stream))
        self.assertEqual(len(intervals), 2)
        self.assertEqual({float(row["duration_us"]) for row in intervals}, {20_000_000.0})

        plot_script = (
            Path(__file__).resolve().parent
            / "analyze_leaf_pair_hotspot_duration_matrix_60s.py"
        )
        subprocess.run(
            [
                sys.executable,
                str(plot_script),
                str(case),
                "--leaf-nodes=37,53",
                "--threshold=1",
                "--bin-s=20",
                "--origin=zero",
                "--no-annotate",
                "--dpi=40",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        hotspot_output = output / "leaf_pair_hotspot_duration_60s"
        with (hotspot_output / "leaf_pair_hotspot_duration_long.csv").open(
            newline=""
        ) as stream:
            hotspot_rows = list(csv.DictReader(stream))
        self.assertEqual(len(hotspot_rows), 3)
        self.assertEqual(
            sum(float(row["hotspot_duration_s"]) for row in hotspot_rows),
            40.0,
        )
        self.assertTrue(
            (hotspot_output / "00_all_leaf_pair_hotspot_duration_20s.png").exists()
        )


if __name__ == "__main__":
    unittest.main()
