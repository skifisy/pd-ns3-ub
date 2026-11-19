"""Static Mooncake topology regressions; no ns-3 build or HiGHS required."""

from collections import Counter
import csv
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


STATIC = Path(__file__).resolve().parents[1]
TOOLS = STATIC.parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(STATIC))
import build_static_ds_topology as static
import generate_ocs_topology as original


def read_rows(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


class StaticDsTopologyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="static-ds-test-")
        cls.root = Path(cls.temp.name)
        cls.base = cls.root / "base"
        cls.base.mkdir()
        ids = original.generate_ids()
        links = []
        next_id = original.generate_compute_links(links, 0, ids[0], ids[2])
        next_id = original.generate_storage_links(links, next_id, ids[1], ids[3])
        original.generate_ocs_links(links, next_id, ids[2] + ids[3])
        original.generate_node_csv(cls.base / "node.csv", *ids)
        original.generate_topology_csv(links, cls.base / "topology.csv")
        (cls.base / "traffic.csv").write_bytes(b"taskId,sourceNode\r\n0,0\r\n")
        (cls.base / "network_attribute.txt").write_bytes(b"# retained config\n")
        (cls.base / "transport_channel.csv").write_bytes(b"# retained TP input\n")
        cls.before_hashes = {p.name: static.sha256(p) for p in cls.base.iterdir()}
        cls.out = cls.root / "expanded"
        cls.summary = static.build_case(cls.base, cls.out)
        cls.before = read_rows(cls.base / "topology.csv")
        cls.after = read_rows(cls.out / "topology.csv")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_exact_capacity_and_port_conservation(self):
        self.assertEqual(self.summary["after"]["circuits_by_role"],
                         {"PP": 240, "PD": 144, "PS": 144, "DS": 112})
        self.assertEqual(self.summary["after"]["ds_capacity_gbps_per_direction"], 44800)
        matrix = self.summary["after"]["ds_matrix"]
        self.assertEqual(matrix, [[7] * 4 for _ in range(4)])
        self.assertEqual([sum(row) for row in matrix], [28] * 4)
        self.assertEqual([sum(row[i] for row in matrix) for i in range(4)], [28] * 4)
        old = {static.Link.from_row(row) for row in self.before}
        new = {static.Link.from_row(row) for row in self.after}
        self.assertEqual(len(old - new), 96)
        self.assertEqual(len(new - old), 96)
        self.assertEqual({ep for link in old for ep in link.endpoints()},
                         {ep for link in new for ep in link.endpoints()})
        self.assertEqual([row for row in self.before if int(row["nodeId1"]) < 37],
                         [row for row in self.after if int(row["nodeId1"]) < 37])
        removed = Counter()
        for link in old - new:
            removed[link.a, static.role(link.b)] += 1
        for leaf in range(37, 49):
            self.assertEqual(removed[leaf, "D"], 4)
            self.assertEqual(removed[leaf, "S"], 4)
        degrees = Counter()
        for link in new:
            if link.a >= 37:
                self.assertNotEqual(static.group(link.a), static.group(link.b))
                degrees[link.a] += 1
                degrees[link.b] += 1
        self.assertEqual(dict(degrees), {leaf: 64 for leaf in range(37, 57)})

    def test_each_leaf_pair_has_the_requested_parallel_circuits(self):
        counts = Counter((link.a, link.b) for row in self.after
                         if (link := static.Link.from_row(row)).a >= 37)
        self.assertEqual(len(counts), 160)
        for (a, b), count in counts.items():
            with self.subTest(pair=(a, b)):
                self.assertEqual(count, {"PP": 5, "PD": 3, "PS": 3, "DS": 7}[
                    static.role(a) + static.role(b)])
        removed_pairs = Counter((link.a, link.b) for row in self.before
                                if (link := static.Link.from_row(row)).a >= 37)
        removed_pairs.subtract(counts)
        self.assertEqual(sum(removed_pairs[p, s] for p in range(37, 49)
                             for s in range(53, 57)), 48)
        self.assertEqual(sum(removed_pairs[p, d] for p in range(37, 49)
                             for d in range(49, 53)), 48)

    def test_new_routing_ports_match_the_physical_peer(self):
        links = [static.Link.from_row(row) for row in self.after]
        destination = next(link for link in links if link.a == 16 and link.ap == 3)
        self.assertEqual(destination.b, 54)
        with (self.out / "routing_table.csv").open(newline="") as stream:
            route = next(row for row in csv.DictReader(stream)
                         if (row["nodeId"], row["dstNodeId"], row["dstPortId"])
                         == ("49", "16", "3"))
        ports = {int(p) for p in route["outPorts"].split()}
        expected = {link.ap for link in links if (link.a, link.b) == (49, 54)}
        self.assertEqual(ports, expected)
        self.assertEqual(len(ports), 7)
        self.assertEqual(len(route["metrics"].split()), 7)
        self.assertEqual(self.summary["route_rows_checked"], 1276 * 56)

    def test_copies_inputs_and_does_not_touch_base(self):
        for name, digest in self.before_hashes.items():
            self.assertEqual(static.sha256(self.base / name), digest)
            if name != "topology.csv":
                self.assertEqual(static.sha256(self.out / name), digest)
        report = json.loads((self.out / "static_ds_summary.json").read_text())
        self.assertEqual(report["missing_run_inputs"], [])
        self.assertFalse(report["simulation_started"])
        changes = read_rows(self.out / "static_ds_changes.csv")
        self.assertEqual(Counter(row["status"] for row in changes), {"remove": 96, "add": 96})
        self.assertEqual(Counter(row["swap_id"] for row in changes),
                         {str(swap): 4 for swap in range(1, 49)})
        self.assertEqual(report["pd_removed_per_pair"], 1)
        self.assertEqual(report["ps_removed_per_pair"], 1)
        self.assertEqual(report["ds_added_per_pair"], 3)

    def test_repeated_generation_is_deterministic(self):
        other = self.root / "expanded-again"
        static.build_case(self.base, other)
        for name in ("node.csv", "topology.csv", "routing_table.csv", "static_ds_changes.csv"):
            self.assertEqual(static.sha256(self.out / name), static.sha256(other / name))

    def test_refuses_existing_and_overlapping_directories(self):
        for out in (self.out, self.base, self.base / "nested", self.root):
            with self.subTest(out=out), self.assertRaises(ValueError):
                static.build_case(self.base, out)
        self.assertEqual(static.sha256(self.out / "topology.csv"),
                         self.summary["sha256"]["topology.csv"])

    def test_refuses_duplicate_physical_ports_and_wrong_nodes(self):
        rows = [dict(row) for row in self.before]
        rows.append(dict(rows[-1]))
        nodes = static.read_nodes(self.base / "node.csv")
        with self.assertRaisesRegex(ValueError, "duplicate physical endpoint"):
            static.transform(rows, nodes)
        bad_nodes = self.root / "bad-node.csv"
        bad_nodes.write_text("nodeId,nodeType,portNum\n0..56,DEVICE,64\n")
        with self.assertRaises(ValueError):
            static.read_nodes(bad_nodes)

    def test_refuses_reapplying_to_an_expanded_topology(self):
        output = self.root / "cannot-apply-twice"
        with self.assertRaisesRegex(ValueError, "original topology"):
            static.build_case(self.out, output)
        self.assertFalse(output.exists())

    def test_corrupt_route_is_rejected(self):
        bad = self.root / "bad-routing.csv"
        with (self.out / "routing_table.csv").open(newline="") as src, bad.open("w", newline="") as dest:
            reader = csv.DictReader(src)
            writer = csv.DictWriter(dest, reader.fieldnames)
            writer.writeheader()
            row = next(reader)
            row["outPorts"] = "127"
            writer.writerow(row)
        with self.assertRaisesRegex(ValueError, "incorrect output ports"):
            static.validate_routes(bad, self.after)

    def test_failure_does_not_publish_a_partial_case(self):
        output = self.root / "route-failure"
        with patch.object(static, "generate_routing_table", side_effect=OSError("route generation failed")):
            with self.assertRaisesRegex(OSError, "route generation failed"):
                static.build_case(self.base, output)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob(".route-failure-*")), [])

    def test_cli_generates_three_files_without_optional_run_inputs(self):
        bare = self.root / "bare"
        bare.mkdir()
        for name in ("node.csv", "topology.csv"):
            shutil.copy2(self.base / name, bare / name)
        output = self.root / "topology-only"
        output.mkdir()
        run = subprocess.run([sys.executable, str(STATIC / "build_static_ds_topology.py"),
                              "--base-case", str(bare), "--out-dir", str(output)],
                             capture_output=True, text=True, timeout=30, cwd=self.root)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("D-S circuits: 64 -> 112", run.stdout)
        self.assertIn("Topology-only output", run.stdout)
        for name in ("node.csv", "topology.csv", "routing_table.csv"):
            self.assertTrue((output / name).is_file())
        report = json.loads((output / "static_ds_summary.json").read_text())
        self.assertEqual(report["missing_run_inputs"], ["traffic.csv", "network_attribute.txt"])


if __name__ == "__main__":
    unittest.main()
