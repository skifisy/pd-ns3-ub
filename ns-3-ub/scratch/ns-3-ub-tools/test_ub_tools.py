import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from traffic_maker.algorithms.all_reduce.nhr import build_nhr_schedule
from traffic_maker.algorithms.all_reduce.common import build_allreduce_contract
from traffic_maker.algorithms.all_reduce.recursive_hd import build_ar_rhd_schedule
from traffic_maker.algorithms.all_reduce.ring import build_ar_ring_schedule
from traffic_maker.algorithms.all_gather import (
    build_ag_nhr_schedule,
    build_ag_recursive_doubling_schedule,
    build_ag_ring_schedule,
)
from traffic_maker.algorithms.all_to_all.pairwise import (
    build_a2a_pairwise_contract,
    build_a2a_pairwise_schedule,
)
from traffic_maker.algorithms.all_to_all.pairwisev import (
    build_a2a_pairwisev_contract,
    build_a2a_pairwisev_schedule,
    build_a2a_pairwisev_from_byte_matrix,
    build_random_byte_matrix,
    build_uniform_byte_matrix,
)
from traffic_maker.algorithms.collective_ir import (
    TransferOperation,
    build_allgather_contract,
)
from traffic_maker.algorithms.reduce_scatter import (
    build_reduce_scatter_contract,
    build_rs_nhr_schedule,
    build_rs_recursive_halving_schedule,
    build_rs_ring_schedule,
)
from traffic_maker.algorithms.registry import (
    ALGORITHM_REGISTRY,
    get_algorithm,
    get_legacy_algorithm,
)
from traffic_maker.algorithms.semantic_validator import (
    SemanticValidationError,
    validate_semantic_schedule,
)
from traffic_maker.algorithms.writer import TRAFFIC_HEADER, write_schedule_csv
from traffic_maker.v2 import TrafficRequest, write_traffic
from net_sim_builder import NetworkSimulationGraph
from topology_source import build_plot_graph, load_topology_source, topology_summary


REPO_ROOT = Path(__file__).resolve().parent
CLI = REPO_ROOT / "ub_tools.py"
LEGACY_CLI = REPO_ROOT / "traffic_maker" / "build_traffic.py"

LEGACY_TRAFFIC_FIXTURES = {
    "a2a_pairwise": (
        b"taskId,sourceNodeId,destNodeId,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\r\n"
        b"0,0,1,2,URMA_WRITE,7,0ns,0,\r\n"
        b"1,1,2,2,URMA_WRITE,7,0ns,0,\r\n"
        b"2,2,0,3,URMA_WRITE,7,0ns,0,\r\n"
        b"3,0,2,2,URMA_WRITE,7,0ns,1,0\r\n"
        b"4,1,0,3,URMA_WRITE,7,0ns,1,0\r\n"
        b"5,2,1,2,URMA_WRITE,7,0ns,1,0\r\n"
    ),
    "ar_rhd": (
        b"taskId,sourceNodeId,destNodeId,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\r\n"
        b"0,0,1,7,URMA_WRITE,7,0ns,0,\r\n"
        b"1,1,2,2,URMA_WRITE,7,0ns,1,0\r\n"
        b"2,2,1,5,URMA_WRITE,7,0ns,1,0\r\n"
        b"3,1,2,5,URMA_WRITE,7,0ns,2,1\r\n"
        b"4,2,1,2,URMA_WRITE,7,0ns,2,1\r\n"
        b"5,1,0,7,URMA_WRITE,7,0ns,3,2\r\n"
    ),
}


class UbToolsCliTest(unittest.TestCase):
    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CLI), *args],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    @staticmethod
    def write_shared_case_files(case: Path) -> None:
        (case / "network_attribute.txt").write_text("", encoding="utf-8")
        (case / "traffic.csv").write_text(
            "taskId,sourceNodeId,destNodeId,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,8,URMA_WRITE,7,0ns,0,\n",
            encoding="utf-8",
        )

    @staticmethod
    def write_topology_sources(root: Path) -> dict[str, str]:
        builder = root / "builder.py"
        builder.write_text(
            "from net_sim_builder import NetworkSimulationGraph\n"
            "def build_topology():\n"
            "    graph = NetworkSimulationGraph()\n"
            "    graph.add_netisim_host(0, allocation_delay='1ns', forward_delay='0ns')\n"
            "    graph.add_netisim_host(1, allocation_delay='1ns', forward_delay='0ns')\n"
            "    graph.add_netisim_edge(0, 1, bandwidth='400Gbps', delay='20ns')\n"
            "    return graph\n",
            encoding="utf-8",
        )
        structured = {
            "version": 1,
            "defaults": {
                "node": {"allocationDelay": "1ns", "forwardDelay": "0ns"},
                "link": {"bandwidth": "400Gbps", "delay": "20ns"},
            },
            "nodeGroups": [{"name": "device", "type": "device", "count": 2}],
            "groupConnections": [{"pattern": "fullMesh", "endpoints": [{"group": "device"}]}],
        }
        json_path = root / "topology.json"
        json_path.write_text(json.dumps(structured), encoding="utf-8")
        yaml_path = root / "topology.yaml"
        import yaml
        yaml_path.write_text(yaml.safe_dump(structured), encoding="utf-8")
        csv_case = root / "csv-case"
        csv_case.mkdir()
        (csv_case / "node.csv").write_text(
            "nodeId,nodeType,portNum,allocationDelay,forwardDelay,systemId\n"
            "0,DEVICE,1,1ns,0ns,0\n1,DEVICE,1,1ns,0ns,0\n",
            encoding="utf-8",
        )
        (csv_case / "topology.csv").write_text(
            "nodeId1,portId1,nodeId2,portId2,bandwidth,delay\n"
            "0,0,1,0,400Gbps,20ns\n",
            encoding="utf-8",
        )
        return {
            "python": f"{builder}:build_topology",
            "json": str(json_path),
            "yaml": str(yaml_path),
            "csv": str(csv_case),
        }

    def test_v1_cli_preserves_legacy_csv_bytes_for_non_divisible_three_rank_cases(self):
        for algorithm, expected in LEGACY_TRAFFIC_FIXTURES.items():
            with self.subTest(algorithm=algorithm), tempfile.TemporaryDirectory() as directory:
                arguments = [
                    sys.executable,
                    str(LEGACY_CLI),
                    "-n",
                    "3",
                    "-c",
                    "3",
                    "-b",
                    "7B",
                    "-a",
                    algorithm,
                    "-o",
                    directory,
                ]
                result = subprocess.run(
                    arguments,
                    cwd=LEGACY_CLI.parent,
                    capture_output=True,
                    text=True,
                    check=False,
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                traffic_files = list(Path(directory).glob("*/traffic.csv"))
                self.assertEqual(len(traffic_files), 1)
                self.assertEqual(traffic_files[0].read_bytes(), expected)

    def test_topology_source_loader_normalizes_python_json_yaml_and_csv(self):
        with tempfile.TemporaryDirectory() as directory:
            sources = self.write_topology_sources(Path(directory))
            summaries = {
                name: topology_summary(load_topology_source(source))
                for name, source in sources.items()
            }

        self.assertEqual(
            set(tuple(sorted(summary.items())) for summary in summaries.values()),
            {(('devices', 2), ('links', 1), ('nodes', 2), ('switches', 0))},
        )

    def test_topology_build_check_and_plot_share_the_source_loader(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            case_path = root / "prepared"
            check = self.run_cli("topology", "check", sources["json"])
            build = self.run_cli(
                "topology", "build", sources["python"], "--output-dir", str(case_path),
                "--no-cache",
            )
            plot_path = root / "topology.png"
            plot = self.run_cli(
                "topology", "plot", sources["csv"], "--output", str(plot_path)
            )

            self.assertEqual(check.returncode, 0, check.stderr)
            self.assertEqual(json.loads(check.stdout)["nodes"], 2)
            self.assertEqual(build.returncode, 0, build.stderr)
            self.assertTrue((case_path / "structured_topology.txt").is_file())
            self.assertTrue((case_path / "compressed_routing.bin").is_file())
            self.assertTrue((case_path / "structured_summary.json").is_file())
            traffic = self.run_cli(
                "traffic", "allreduce", "--host-count", "2", "--data-size", "8B",
                "--algorithm", "ring", "--output-dir", str(case_path),
            )
            self.assertEqual(traffic.returncode, 0, traffic.stderr)
            (case_path / "network_attribute.txt").write_text("", encoding="utf-8")
            prepared_check = self.run_cli("case", "check", str(case_path))
            self.assertEqual(prepared_check.returncode, 0, prepared_check.stderr)
            self.assertEqual(plot.returncode, 0, plot.stderr)
            self.assertGreater(plot_path.stat().st_size, 0)

    def test_topology_build_accepts_default_python_builder_and_csv_format(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            builder_path = sources["python"].split(":", 1)[0]
            output_dir = root / "csv-output"

            result = self.run_cli(
                "topology", "build", builder_path, "--output-dir", str(output_dir),
                "--case-format", "csv", "--no-cache",
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("route-generation=full", result.stdout)
            self.assertTrue((output_dir / "node.csv").is_file())
            self.assertTrue((output_dir / "topology.csv").is_file())
            self.assertTrue((output_dir / "routing_table.csv").is_file())
            self.assertFalse((output_dir / "structured_topology.txt").exists())
            self.write_shared_case_files(output_dir)
            checked = self.run_cli("case", "check", str(output_dir), "--json")
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.assertEqual(json.loads(checked.stdout)["format"], "csv")

    def test_csv_format_rejects_structured_and_mixed_route_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = self.write_topology_sources(root)["json"]

            for route_generation in ("structured", "mixed"):
                with self.subTest(route_generation=route_generation):
                    result = self.run_cli(
                        "topology", "build", source, "--output-dir",
                        str(root / route_generation), "--case-format", "csv",
                        "--route-generation", route_generation, "--no-cache",
                    )
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("supports only --route-generation full", result.stderr)
                    self.assertIn("--case-format structured", result.stderr)
                    self.assertIn("--export-csv", result.stderr)

    def test_topology_build_and_plot_refuse_to_overwrite_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            output_dir = root / "prepared"
            output_dir.mkdir()
            topology_path = output_dir / "structured_topology.txt"
            topology_path.write_text("owned by user\n", encoding="utf-8")

            build = self.run_cli(
                "topology", "build", sources["json"], "--output-dir", str(output_dir),
                "--no-cache",
            )
            self.assertNotEqual(build.returncode, 0)
            self.assertIn("output already exists", build.stderr)
            self.assertEqual(topology_path.read_text(encoding="utf-8"), "owned by user\n")

            plot_path = root / "topology.png"
            plot_path.write_text("owned by user\n", encoding="utf-8")
            plot = self.run_cli(
                "topology", "plot", sources["json"], "--output", str(plot_path)
            )
            self.assertNotEqual(plot.returncode, 0)
            self.assertIn("plot output already exists", plot.stderr)
            self.assertEqual(plot_path.read_text(encoding="utf-8"), "owned by user\n")

    def test_topology_build_rejects_export_csv_into_the_structured_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            output_dir = root / "prepared"
            result = self.run_cli(
                "topology", "build", sources["json"], "--output-dir", str(output_dir),
                "--export-csv", str(output_dir), "--no-cache",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must differ from --output-dir", result.stderr)
            self.assertFalse((output_dir / "structured_topology.txt").exists())

    def test_topology_source_errors_are_actionable_and_check_writes_no_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            builder = root / "bad.py"
            builder.write_text("def build_topology():\n    return object()\n", encoding="utf-8")
            bad_type = self.run_cli(
                "topology", "check", f"{builder}:build_topology"
            )
            missing_function = self.run_cli(
                "topology", "check", f"{builder}:missing"
            )

            self.assertNotEqual(bad_type.returncode, 0)
            self.assertIn("must produce NetworkSimulationGraph", bad_type.stderr)
            self.assertNotEqual(missing_function.returncode, 0)
            self.assertIn("function is not callable", missing_function.stderr)
            self.assertEqual(list(root.glob("structured_*")), [])

            runtime_builder = root / "runtime.py"
            runtime_builder.write_text(
                "def build_topology():\n    raise RuntimeError('broken builder')\n",
                encoding="utf-8",
            )
            runtime_error = self.run_cli(
                "topology", "check", f"{runtime_builder}:build_topology"
            )
            self.assertNotEqual(runtime_error.returncode, 0)
            self.assertIn("topology builder failed", runtime_error.stderr)
            self.assertNotIn("Traceback", runtime_error.stderr)

    def test_plot_graph_preserves_parallel_physical_links(self):
        graph = NetworkSimulationGraph()
        graph.add_netisim_host(0, allocation_delay="1ns", forward_delay="0ns")
        graph.add_netisim_host(1, allocation_delay="1ns", forward_delay="0ns")
        graph.add_netisim_edge(
            0, 1, bandwidth="400Gbps", delay="20ns", edge_count=2
        )
        graph.build_graph_config()

        plotted = build_plot_graph(graph)

        self.assertEqual(plotted.number_of_edges(0, 1), 2)

    def test_case_show_and_check_accept_csv_and_structured_cases_without_mutating(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            csv_case = Path(sources["csv"])
            self.write_shared_case_files(csv_case)
            (csv_case / "routing_table.csv").write_text(
                "nodeId,dstNodeId,dstPortId,outPorts,metrics\n"
                "0,1,0,0,1\n1,0,0,0,1\n",
                encoding="utf-8",
            )
            csv_case_hash_before = {
                path.name: path.read_bytes() for path in sorted(csv_case.iterdir())
            }
            csv_show = self.run_cli("case", "show", str(csv_case), "--json")
            csv_check = self.run_cli("case", "check", str(csv_case))
            self.assertEqual(csv_show.returncode, 0, csv_show.stderr)
            self.assertEqual(json.loads(csv_show.stdout)["format"], "csv")
            self.assertEqual(csv_check.returncode, 0, csv_check.stderr)
            self.assertEqual(
                csv_case_hash_before,
                {path.name: path.read_bytes() for path in sorted(csv_case.iterdir())},
            )

            structured_case = root / "structured-case"
            build = self.run_cli(
                "topology", "build", sources["json"], "--output-dir",
                str(structured_case), "--no-cache",
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            self.write_shared_case_files(structured_case)
            structured_hash_before = {
                path.name: path.read_bytes() for path in sorted(structured_case.iterdir())
            }
            structured_show = self.run_cli("case", "show", str(structured_case), "--json")
            structured_check = self.run_cli("case", "check", str(structured_case))
            self.assertEqual(structured_show.returncode, 0, structured_show.stderr)
            self.assertEqual(json.loads(structured_show.stdout)["format"], "structured")
            self.assertEqual(structured_check.returncode, 0, structured_check.stderr)
            self.assertEqual(
                structured_hash_before,
                {path.name: path.read_bytes() for path in sorted(structured_case.iterdir())},
            )

    def test_case_check_reports_missing_and_bad_structured_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "missing"
            missing.mkdir()
            (missing / "structured_topology.txt").write_text(
                "NS3_UB_STRUCTURED 4\nNODES 0\nLINKS 0\n", encoding="utf-8"
            )
            missing_result = self.run_cli("case", "check", str(missing))
            self.assertNotEqual(missing_result.returncode, 0)
            self.assertIn("missing required files", missing_result.stderr)

            sources = self.write_topology_sources(root)
            case_path = root / "structured"
            build = self.run_cli(
                "topology", "build", sources["json"], "--output-dir", str(case_path),
                "--no-cache",
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            self.write_shared_case_files(case_path)
            summary = json.loads((case_path / "structured_summary.json").read_text())
            summary["protocolVersion"] = -1
            (case_path / "structured_summary.json").write_text(
                json.dumps(summary), encoding="utf-8"
            )
            bad_version = self.run_cli("case", "check", str(case_path))
            self.assertNotEqual(bad_version.returncode, 0)
            self.assertIn("protocolVersion", bad_version.stderr)

    def test_case_check_rejects_topology_rules_in_a_csv_runtime_case(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            csv_case = Path(self.write_topology_sources(root)["csv"])
            self.write_shared_case_files(csv_case)
            (csv_case / "routing_table.csv").write_text(
                "nodeId,dstNodeId,dstPortId,outPorts,metrics\n",
                encoding="utf-8",
            )
            (csv_case / "routing_topology_rules.csv").write_text(
                "ruleId,ruleType,shape,connections,nodeId,nodeType,coordinates\n"
                "0,dimension,2,fullMesh,0,host,0\n"
                "0,dimension,2,fullMesh,1,host,1\n",
                encoding="utf-8",
            )

            shown = self.run_cli("case", "show", str(csv_case), "--json")
            checked = self.run_cli("case", "check", str(csv_case))

            self.assertEqual(shown.returncode, 0, shown.stderr)
            self.assertEqual(json.loads(shown.stdout)["routing"]["topologyRuleRows"], 2)
            self.assertEqual(checked.returncode, 2)
            self.assertIn("CSV runtime does not load routing_topology_rules.csv", checked.stderr)
            self.assertIn("regenerate the CSV case with full routing", checked.stderr)

    def test_case_show_reports_routes_and_check_rejects_bad_node_references(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            case = Path(sources["csv"])
            (case / "routing_table.csv").write_text(
                "nodeId,dstNodeId,dstPortId,outPorts,metrics\n"
                "0,1,0,0,1\n",
                encoding="utf-8",
            )
            show = self.run_cli("case", "show", str(case), "--json")
            self.assertEqual(show.returncode, 0, show.stderr)
            self.assertEqual(json.loads(show.stdout)["routing"]["routingTableRows"], 1)
            human_show = self.run_cli("case", "show", str(case))
            self.assertEqual(human_show.returncode, 0, human_show.stderr)
            self.assertIn("routing table rows=1", human_show.stdout)

            (case / "network_attribute.txt").write_text("", encoding="utf-8")

            (case / "traffic.csv").write_text(
                "taskId,sourceNodeId,destNodeId,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
                "0,9,1,8,URMA_WRITE,7,0ns,0,\n",
                encoding="utf-8",
            )
            bad_traffic = self.run_cli("case", "check", str(case))
            self.assertNotEqual(bad_traffic.returncode, 0)
            self.assertIn("traffic.csv:2", bad_traffic.stderr)
            self.assertIn("unknown sourceNodeId 9", bad_traffic.stderr)

            self.write_shared_case_files(case)
            (case / "routing_table.csv").write_text(
                "nodeId,dstNodeId,dstPortId,outPorts,metrics\n"
                "0,1,0,9,1\n",
                encoding="utf-8",
            )
            bad_route = self.run_cli("case", "check", str(case))
            self.assertNotEqual(bad_route.returncode, 0)
            self.assertIn("routing_table.csv:2", bad_route.stderr)
            self.assertIn("out port 9", bad_route.stderr)

    def test_case_check_accepts_legacy_structured_protocol_without_v4_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory) / "legacy"
            case.mkdir()
            (case / "structured_topology.txt").write_text(
                "NS3_UB_STRUCTURED 3\nNODES 2\n"
                "0 DEVICE 1 1ns 0ns 0\n1 DEVICE 1 1ns 0ns 0\n"
                "LINKS 1\n0 0 1 0 400Gbps 10ns\nEND\n",
                encoding="utf-8",
            )
            self.write_shared_case_files(case)
            result = self.run_cli("case", "check", str(case), "--json")

            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(result.stdout)
            self.assertEqual(summary["structuredVersion"], 3)

    def test_v2_trace_parse_matches_legacy_parser_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / "case"
            runlog = case / "runlog"
            runlog.mkdir(parents=True)
            (case / "traffic.csv").write_text(
                "taskId,sourceNodeId,destNodeId,dataSize(Byte)\n"
                "0,0,1,1000\n",
                encoding="utf-8",
            )
            (runlog / "TaskTrace_node_0.tr").write_text(
                "[1.000000us] WQE Starts, taskId: 0\n"
                "[3.000000us] WQE Completes, taskId: 0\n",
                encoding="utf-8",
            )
            (runlog / "PacketTrace_node_0.tr").write_text(
                "[1.500000us] First Packet Sends, taskId: 0\n"
                "[2.500000us] Last Packet ACKs, taskId: 0\n",
                encoding="utf-8",
            )
            (runlog / "PortTrace_node_0_port_0.tr").write_text(
                "[1.000000us] Rx PacketSize: 100\n"
                "[2.000000us] Rx PacketSize: 100\n"
                "[1.000000us] Tx PacketSize: 100\n"
                "[2.000000us] Tx PacketSize: 100\n",
                encoding="utf-8",
            )
            legacy = subprocess.run(
                [sys.executable, str(REPO_ROOT / "trace_analysis" / "parse_trace.py"),
                 str(case), "false"],
                cwd=REPO_ROOT, capture_output=True, text=True, check=False,
            )
            self.assertEqual(legacy.returncode, 0, legacy.stderr)
            legacy_task = (case / "output" / "task_statistics.csv").read_bytes()
            legacy_throughput = (case / "output" / "throughput.csv").read_bytes()

            v2_output = root / "analysis"
            v2 = self.run_cli(
                "trace", "parse", str(case), "--output-dir", str(v2_output)
            )
            self.assertEqual(v2.returncode, 0, v2.stderr)
            self.assertEqual(legacy_task, (v2_output / "task_statistics.csv").read_bytes())
            self.assertEqual(
                legacy_throughput, (v2_output / "throughput.csv").read_bytes()
            )

    def test_v2_trace_parse_rejects_missing_input_and_output_conflicts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = self.run_cli(
                "trace", "parse", str(root / "missing"), "--output-dir", str(root / "out")
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("trace case directory does not exist", missing.stderr)

            case = root / "case"
            (case / "runlog").mkdir(parents=True)
            (case / "traffic.csv").write_text("taskId,dataSize(Byte)\n", encoding="utf-8")
            output = root / "out"
            output.mkdir()
            (output / "task_statistics.csv").write_text("existing\n", encoding="utf-8")
            conflict = self.run_cli(
                "trace", "parse", str(case), "--output-dir", str(output)
            )
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn("analysis output already exists", conflict.stderr)

    def test_allreduce_ring_writes_expected_two_rank_schedule(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic",
                "allreduce",
                "--host-count",
                "2",
                "--data-size",
                "4B",
                "--algorithm",
                "ring",
                "--output-dir",
                str(output_dir),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as traffic_file:
                rows = list(csv.reader(traffic_file))

            self.assertEqual(
                rows,
                [
                    [
                        "taskId",
                        "sourceNodeId",
                        "destNodeId",
                        "dataSize(Byte)",
                        "opType",
                        "priority",
                        "delay",
                        "phaseId",
                        "dependOnPhases",
                    ],
                    ["0", "0", "1", "2", "URMA_WRITE", "7", "0ns", "0", ""],
                    ["1", "1", "0", "2", "URMA_WRITE", "7", "0ns", "0", ""],
                    ["2", "0", "1", "2", "URMA_WRITE", "7", "0ns", "1", "0"],
                    ["3", "1", "0", "2", "URMA_WRITE", "7", "0ns", "1", "0"],
                ],
            )

    def test_allreduce_rejects_an_incomplete_contiguous_group_with_repair_advice(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic",
                "allreduce",
                "--host-count",
                "5",
                "--group-size",
                "2",
                "--data-size",
                "1KB",
                "--algorithm",
                "ring",
                "--output-dir",
                str(output_dir),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("host-count must be divisible by group-size", result.stderr)
            self.assertIn("choose a group-size that divides 5", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse((output_dir / "traffic.csv").exists())

    def test_direct_request_rejects_non_positive_counts_and_data_size_before_writing(self):
        for field in ("host_count", "group_size", "data_size"):
            invalid_values = (-1,) if field == "data_size" else (0, -1)
            for invalid_value in invalid_values:
                with self.subTest(
                    field=field, value=invalid_value
                ), tempfile.TemporaryDirectory() as directory:
                    output_dir = Path(directory) / "case"
                    request_values = {
                        "collective": "allreduce",
                        "host_count": 2,
                        "group_size": 2,
                        "data_size": 1024,
                        "algorithm": "ring",
                        "output_dir": output_dir,
                    }
                    request_values[field] = invalid_value

                    message = f"{field.replace('_', '-')} must be positive"
                    with self.assertRaisesRegex(ValueError, message):
                        write_traffic(TrafficRequest(**request_values))

                    self.assertFalse((output_dir / "traffic.csv").exists())

    def test_cli_rejects_non_positive_counts_and_data_size_without_a_traceback_or_csv(self):
        option_names = {
            "host-count": "--host-count",
            "group-size": "--group-size",
            "data-size": "--data-size",
        }
        for field, option in option_names.items():
            invalid_values = ("-1",) if field == "data-size" else ("0", "-1")
            for invalid_value in invalid_values:
                with self.subTest(
                    field=field, value=invalid_value
                ), tempfile.TemporaryDirectory() as directory:
                    output_dir = Path(directory) / "case"
                    arguments = [
                        "traffic",
                        "allreduce",
                        "--host-count",
                        "2",
                        "--group-size",
                        "2",
                        "--data-size",
                        "1KB",
                        "--algorithm",
                        "ring",
                        "--output-dir",
                        str(output_dir),
                    ]
                    option_index = arguments.index(option)
                    arguments[option_index + 1] = invalid_value

                    result = self.run_cli(*arguments)

                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(field, result.stderr)
                    self.assertIn("positive", result.stderr)
                    self.assertNotIn("Traceback", result.stderr)
                    self.assertFalse((output_dir / "traffic.csv").exists())

    def test_direct_request_rejects_an_unknown_group_layout_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            request = TrafficRequest(
                collective="allreduce",
                host_count=4,
                group_size=2,
                data_size=1024,
                algorithm="ring",
                group_layout="random",
                output_dir=output_dir,
            )

            with self.assertRaisesRegex(ValueError, "group-layout must be contiguous or interleaved"):
                write_traffic(request)

            self.assertFalse((output_dir / "traffic.csv").exists())

    def test_allreduce_rejects_an_unknown_data_size_unit_with_repair_advice(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic",
                "allreduce",
                "--host-count",
                "2",
                "--data-size",
                "1KiB",
                "--algorithm",
                "ring",
                "--output-dir",
                str(output_dir),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("data-size must be a positive integer", result.stderr)
            self.assertIn("for example 1024B, 1KB, 1MB, or 1GB", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse((output_dir / "traffic.csv").exists())

    def test_allreduce_rejects_a_phase_delay_without_a_time_unit(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic",
                "allreduce",
                "--host-count",
                "2",
                "--data-size",
                "1KB",
                "--algorithm",
                "ring",
                "--phase-delay",
                "10",
                "--output-dir",
                str(output_dir),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("phase-delay must include a time unit", result.stderr)
            self.assertIn("for example 0ns, 5us, or 1ms", result.stderr)
            self.assertFalse((output_dir / "traffic.csv").exists())

    def test_allreduce_maps_contiguous_groups_to_independent_phase_chains(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic",
                "allreduce",
                "--host-count",
                "4",
                "--group-size",
                "2",
                "--data-size",
                "1KB",
                "--algorithm",
                "ring",
                "--group-layout",
                "contiguous",
                "--phase-delay",
                "5us",
                "--output-dir",
                str(output_dir),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as traffic_file:
                rows = list(csv.DictReader(traffic_file))

            self.assertEqual(
                [
                    (
                        row["sourceNodeId"],
                        row["destNodeId"],
                        row["dataSize(Byte)"],
                        row["delay"],
                        row["phaseId"],
                        row["dependOnPhases"],
                    )
                    for row in rows
                ],
                [
                    ("0", "1", "512", "0ns", "0", ""),
                    ("1", "0", "512", "0ns", "0", ""),
                    ("0", "1", "512", "5us", "1", "0"),
                    ("1", "0", "512", "5us", "1", "0"),
                    ("2", "3", "512", "0ns", "2", ""),
                    ("3", "2", "512", "0ns", "2", ""),
                    ("2", "3", "512", "5us", "3", "2"),
                    ("3", "2", "512", "5us", "3", "2"),
                ],
            )

    def test_existing_collective_builders_are_registered_under_v2_names(self):
        self.assertEqual(
            get_algorithm("allreduce", "nhr").build(4, 513),
            build_nhr_schedule(build_allreduce_contract(4, 513)),
        )
        self.assertEqual(
            get_algorithm("allreduce", "recursive-halving-doubling").build(3, 513),
            build_ar_rhd_schedule(build_allreduce_contract(3, 513)),
        )
        self.assertEqual(
            get_algorithm("alltoall", "pairwise").builder(
                build_a2a_pairwise_contract(4, 513)
            ),
            build_a2a_pairwise_schedule(build_a2a_pairwise_contract(4, 513)),
        )

    def test_canonical_registry_maps_v2_and_legacy_names_to_the_same_builder(self):
        expected = {
            ("allreduce", "ring", "ar_ring"),
            ("allreduce", "nhr", "ar_nhr"),
            ("allreduce", "recursive-halving-doubling", "ar_rhd"),
            ("reduce-scatter", "ring", None),
            ("reduce-scatter", "nhr", None),
            ("reduce-scatter", "recursive-halving", None),
            ("allgather", "ring", None),
            ("allgather", "nhr", None),
            ("allgather", "recursive-doubling", None),
            ("alltoall", "pairwise", "a2a_pairwise"),
            ("alltoallv", "pairwise", None),
        }
        self.assertEqual(
            {(entry.collective, entry.name, entry.legacy_name) for entry in ALGORITHM_REGISTRY},
            expected,
        )
        for entry in ALGORITHM_REGISTRY:
            self.assertIs(get_algorithm(entry.collective, entry.name), entry)
            if entry.legacy_name is not None:
                self.assertIs(get_legacy_algorithm(entry.legacy_name), entry)

    def test_new_collectives_are_not_exposed_by_the_v1_cli(self):
        result = subprocess.run(
            [sys.executable, str(LEGACY_CLI), "-n", "2", "-c", "2", "-b", "4B",
             "-a", "rs_ring"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice: 'rs_ring'", result.stderr)

    def test_alltoall_pairwise_preserves_non_divisible_input_bytes(self):
        schedule = build_a2a_pairwise_schedule(build_a2a_pairwise_contract(4, 7))
        single_rank = build_a2a_pairwise_schedule(build_a2a_pairwise_contract(1, 7))

        destination_sizes = {
            destination: next(
                chunk.byte_length
                for chunk in schedule.chunks
                if chunk.chunk_id == f"src-0-dst-{destination}"
            )
            for destination in range(4)
        }
        self.assertEqual(destination_sizes, {0: 2, 1: 2, 2: 2, 3: 1})
        self.assertEqual(single_rank.chunks[0].byte_length, 7)
        self.assertTrue(validate_semantic_schedule(schedule)["ok"])

    def test_alltoall_peers_per_phase_only_regroups_pairwise_transfers(self):
        contract = build_a2a_pairwise_contract(4, 7)
        pairwise = get_algorithm("alltoall", "pairwise").builder(contract)
        grouped = get_algorithm("alltoall", "pairwise").builder(
            contract, peers_per_phase=2
        )

        pairwise_transfers = [
            (transfer.sender, transfer.receiver, transfer.byte_length)
            for phase in pairwise.phases
            for transfer in phase.transfers
        ]
        grouped_transfers = [
            (transfer.sender, transfer.receiver, transfer.byte_length)
            for phase in grouped.phases
            for transfer in phase.transfers
        ]
        self.assertEqual(grouped_transfers, pairwise_transfers)
        self.assertEqual(len(grouped.phases), 2)

    def test_alltoallv_uniform_and_random_distributions_preserve_each_row(self):
        uniform = build_uniform_byte_matrix(4, 7)
        random_a = build_random_byte_matrix(4, 7, seed=19)
        random_b = build_random_byte_matrix(4, 7, seed=19)

        self.assertEqual(uniform[0], [2, 2, 2, 1])
        self.assertTrue(all(sum(row) == 7 for row in uniform))
        self.assertEqual(build_uniform_byte_matrix(4, 7, include_local=True), uniform)
        self.assertEqual(random_a, random_b)
        self.assertTrue(all(sum(row) == 7 for row in random_a))

    def test_alltoallv_byte_matrix_sets_chunk_and_transfer_bytes(self):
        matrix = [
            [0, 2, 5],
            [7, 0, 11],
            [13, 17, 0],
        ]
        schedule = build_a2a_pairwisev_from_byte_matrix(matrix)
        transfers = {
            (transfer.sender, transfer.receiver): transfer
            for phase in schedule.phases
            for transfer in phase.transfers
        }

        transfer = transfers[(0, 2)]
        self.assertEqual(transfer.byte_length, 5)
        self.assertEqual(transfer.chunk_ids, ("src-0-dst-2",))
        self.assertTrue(validate_semantic_schedule(schedule)["ok"])

    def test_alltoallv_local_copy_is_emitted_as_a_self_task(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic", "alltoallv", "--host-count", "2", "--data-size", "4B",
                "--algorithm", "pairwise", "--distribution", "uniform",
                "--include-local", "--output-dir", str(output_dir),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as file:
                rows = list(csv.DictReader(file))
            local_rows = [row for row in rows if row["sourceNodeId"] == row["destNodeId"]]
            self.assertEqual(len(local_rows), 2)
            self.assertEqual({row["phaseId"] for row in local_rows}, {"0"})

    def test_alltoallv_single_rank_local_copy_is_emitted_as_a_self_task(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic", "alltoallv", "--host-count", "1", "--data-size", "4B",
                "--algorithm", "pairwise", "--distribution", "uniform",
                "--include-local", "--output-dir", str(output_dir),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["sourceNodeId"], "0")
            self.assertEqual(rows[0]["destNodeId"], "0")
            self.assertEqual(rows[0]["dataSize(Byte)"], "4")

    def test_alltoallv_single_rank_without_local_lowering_writes_no_task(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic", "alltoallv", "--host-count", "1", "--data-size", "4B",
                "--algorithm", "pairwise", "--distribution", "uniform",
                "--output-dir", str(output_dir),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as file:
                self.assertEqual(list(csv.reader(file)), [TRAFFIC_HEADER])

    def test_case_check_requires_runner_inputs_and_rule_file_pairs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = self.write_topology_sources(root)
            case = Path(sources["csv"])
            missing = self.run_cli("case", "check", str(case))
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("network_attribute.txt", missing.stderr)
            self.assertIn("traffic.csv", missing.stderr)
            self.assertIn("routing_table.csv", missing.stderr)

            self.write_shared_case_files(case)
            (case / "routing_table.csv").write_text(
                "nodeId,dstNodeId,dstPortId,outPorts,metrics\n"
                "0,1,0,0,1\n1,0,0,0,1\n",
                encoding="utf-8",
            )
            (case / "routing_port_groups.csv").write_text("nodeId,groupId,ports\n", encoding="utf-8")
            unmatched = self.run_cli("case", "check", str(case))
            self.assertNotEqual(unmatched.returncode, 0)
            self.assertIn("routing_port_groups.csv and routing_rules.csv", unmatched.stderr)

    def test_alltoallv_contract_records_each_rank_input_bytes(self):
        contract = build_a2a_pairwisev_contract([[12, 8], [6, 10]])
        self.assertEqual(contract.input_bytes_by_rank, (20, 16))

    def test_alltoallv_verbose_reports_offsets_without_changing_schedule(self):
        matrix = [[3, 2], [4, 5]]
        default = build_a2a_pairwisev_from_byte_matrix(matrix)
        include_local = build_a2a_pairwisev_from_byte_matrix(
            matrix, include_local=True
        )

        self.assertEqual(default, include_local)
        chunks = {
            chunk["id"]: chunk
            for chunk in validate_semantic_schedule(default, verbose=True)["chunks"]
        }
        self.assertEqual(chunks["src-0-dst-1"]["send_offset"], 3)
        self.assertEqual(chunks["src-1-dst-0"]["receive_offset"], 3)

    def test_alltoallv_rejects_contract_matrix_mismatch(self):
        contract = build_a2a_pairwisev_contract([[0, 2], [3, 0]])
        with self.assertRaisesRegex(ValueError, "does not match traffic_matrix"):
            build_a2a_pairwisev_schedule(
                contract,
                [[0, 3], [2, 0]],
            )

    def test_alltoallv_cli_supports_matrix_uniform_and_random(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            matrix = root / "matrix.csv"
            matrix.write_text("0,2,5\n7,0,11\n13,17,0\n", encoding="utf-8")
            commands = [
                ("matrix", ["--traffic-matrix", str(matrix)]),
                ("uniform", ["--data-size", "7B"]),
                ("random", ["--data-size", "7B", "--seed", "19"]),
            ]
            for distribution, extra in commands:
                with self.subTest(distribution=distribution):
                    output_dir = root / distribution
                    result = self.run_cli(
                        "traffic", "alltoallv", "--host-count", "3", "--algorithm",
                        "pairwise", "--distribution", distribution, "--output-dir",
                        str(output_dir), *extra,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertTrue((output_dir / "traffic.csv").exists())

    def test_alltoallv_cli_rejects_invalid_distribution_inputs(self):
        invalid_cases = [
            ("matrix without file", ["--distribution", "matrix"], "traffic-matrix is required"),
            (
                "uniform without size",
                ["--distribution", "uniform"],
                "data-size is required",
            ),
            (
                "random without seed",
                ["--distribution", "random", "--data-size", "7B"],
                "seed is required",
            ),
        ]
        for name, extra, message in invalid_cases:
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                result = self.run_cli(
                    "traffic", "alltoallv", "--host-count", "3", "--algorithm",
                    "pairwise", "--output-dir", str(Path(directory) / "case"), *extra,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_v2_simai_conversion_matches_v1_converter_and_rank_mapping(self):
        flows = [
            {
                "schema": "simai-flow-v1", "collective_id": 0, "channel_id": 0,
                "flow_id": 0, "src": 0, "dst": 1, "flow_size": 8,
                "parent_flow_id": [],
            },
            {
                "schema": "simai-flow-v1", "collective_id": 0, "channel_id": 0,
                "flow_id": 1, "src": 1, "dst": 0, "flow_size": 4,
                "parent_flow_id": [0],
            },
            {
                "schema": "simai-flow-v1", "collective_id": 1, "channel_id": 0,
                "flow_id": 0, "src": 0, "dst": 1, "flow_size": 2,
                "parent_flow_id": [],
            },
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flow_file = root / "flows.jsonl"
            flow_file.write_text(
                "\n".join(json.dumps(flow) for flow in flows) + "\n", encoding="utf-8"
            )
            rank_map = root / "rank_map.csv"
            rank_map.write_text("simai_rank,ub_node\n0,2\n1,3\n", encoding="utf-8")
            v1_output = root / "v1.csv"
            v2_output = root / "v2.csv"
            v1 = subprocess.run(
                [sys.executable, str(REPO_ROOT / "traffic_maker" / "simai_flow_to_traffic.py"),
                 str(flow_file), "--output", str(v1_output), "--rank-map", str(rank_map),
                 "--host-count", "4"],
                cwd=REPO_ROOT, capture_output=True, text=True, check=False,
            )
            v2 = self.run_cli(
                "traffic", "convert-simai", str(flow_file), "--output", str(v2_output),
                "--rank-map", str(rank_map), "--host-count", "4",
            )
            self.assertEqual(v1.returncode, 0, v1.stderr)
            self.assertEqual(v2.returncode, 0, v2.stderr)
            self.assertEqual(v1_output.read_bytes(), v2_output.read_bytes())

    def test_v2_simai_conversion_preserves_overlap_option_and_rejects_bad_endpoint(self):
        flows = [
            {
                "schema": "simai-flow-v1", "collective_id": collective,
                "channel_id": 0, "flow_id": 0, "src": 0, "dst": 1,
                "flow_size": 8, "parent_flow_id": [],
            }
            for collective in (0, 1)
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flow_file = root / "flows.jsonl"
            flow_file.write_text(
                "\n".join(json.dumps(flow) for flow in flows) + "\n", encoding="utf-8"
            )
            overlap_output = root / "overlap.csv"
            overlap = self.run_cli(
                "traffic", "convert-simai", str(flow_file), "--output", str(overlap_output),
                "--host-count", "2", "--allow-overlap-collectives",
            )
            self.assertEqual(overlap.returncode, 0, overlap.stderr)
            with overlap_output.open(newline="", encoding="utf-8") as output_file:
                rows = list(csv.DictReader(output_file))
            self.assertEqual([row["dependOnPhases"] for row in rows], ["", ""])

            bad_map = root / "rank_map.csv"
            bad_map.write_text("simai_rank,ub_node\n0,9\n1,1\n", encoding="utf-8")
            result = self.run_cli(
                "traffic", "convert-simai", str(flow_file), "--output", str(root / "bad.csv"),
                "--rank-map", str(bad_map), "--host-count", "2",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("outside host id range", result.stderr)

        invalid_matrices = {
            "wrong shape": ("0,1\n1,0\n", "must be 3 x 3"),
            "negative": ("0,1,2\n3,-1,4\n5,6,0\n", "negative byte count"),
            "not integer": ("0,1,2\n3,x,4\n5,6,0\n", "integer byte counts"),
        }
        for name, (contents, message) in invalid_matrices.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                matrix = root / "matrix.csv"
                matrix.write_text(contents, encoding="utf-8")
                result = self.run_cli(
                    "traffic", "alltoallv", "--host-count", "3", "--algorithm",
                    "pairwise", "--distribution", "matrix", "--traffic-matrix",
                    str(matrix), "--output-dir", str(root / "case"),
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_v2_simai_conversion_refuses_to_overwrite_without_explicit_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flow_file = root / "flows.jsonl"
            flow_file.write_text(
                json.dumps({
                    "schema": "simai-flow-v1", "collective_id": 0, "channel_id": 0,
                    "flow_id": 0, "src": 0, "dst": 1, "flow_size": 8,
                    "parent_flow_id": [],
                }) + "\n",
                encoding="utf-8",
            )
            output = root / "traffic.csv"
            output.write_text("keep\n", encoding="utf-8")
            refused = self.run_cli(
                "traffic", "convert-simai", str(flow_file), "--output", str(output)
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("output already exists", refused.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), "keep\n")

    def test_standalone_collectives_satisfy_block_level_postconditions(self):
        portable_cases = [
            (build_rs_ring_schedule, build_reduce_scatter_contract),
            (build_rs_nhr_schedule, build_reduce_scatter_contract),
            (build_ag_nhr_schedule, build_allgather_contract),
        ]
        for builder, contract_builder in portable_cases:
            for rank_size in (1, 2, 4, 5, 8):
                with self.subTest(builder=builder.__name__, rank_size=rank_size):
                    contract = contract_builder(rank_size, rank_size * 128)
                    result = validate_semantic_schedule(builder(contract))
                    self.assertTrue(result["ok"])

        for rank_size in (1, 2, 4, 5, 8):
            with self.subTest(builder=build_ag_ring_schedule.__name__, rank_size=rank_size):
                contract = build_allgather_contract(rank_size, rank_size * 128)
                result = validate_semantic_schedule(build_ag_ring_schedule(contract))
                self.assertTrue(result["ok"])

        for rank_size in (1, 2, 3, 4, 5, 8):
            with self.subTest(
                builder=build_rs_recursive_halving_schedule.__name__,
                rank_size=rank_size,
            ):
                contract = build_reduce_scatter_contract(rank_size, rank_size * 128)
                result = validate_semantic_schedule(
                    build_rs_recursive_halving_schedule(contract)
                )
                self.assertTrue(result["ok"])

        for rank_size in (1, 2, 3, 4, 5, 8):
            with self.subTest(
                builder=build_ag_recursive_doubling_schedule.__name__,
                rank_size=rank_size,
            ):
                contract = build_allgather_contract(rank_size, rank_size * 128)
                result = validate_semantic_schedule(
                    build_ag_recursive_doubling_schedule(contract)
                )
                self.assertTrue(result["ok"])

    def test_allreduce_satisfies_end_to_end_collective_postconditions(self):
        for builder in (
            build_ar_ring_schedule,
            build_nhr_schedule,
            build_ar_rhd_schedule,
        ):
            for rank_size in (1, 2, 3, 4, 5, 8):
                with self.subTest(builder=builder.__name__, rank_size=rank_size):
                    contract = build_allreduce_contract(rank_size, 1024)
                    result = validate_semantic_schedule(builder(contract), verbose=True)
                    self.assertTrue(result["ok"])

    def test_recursive_doubling_allgather_supports_non_power_of_two(self):
        for rank_size in (3, 5):
            with self.subTest(rank_size=rank_size):
                contract = build_allgather_contract(rank_size, rank_size * 128)
                schedule = build_ag_recursive_doubling_schedule(contract)
                self.assertTrue(validate_semantic_schedule(schedule)["ok"])
                self.assertEqual(schedule.phases[0].label, "FoldIn")
                self.assertEqual(schedule.phases[-1].label, "FoldOut")

    def test_semantic_validator_rejects_ring_allgather_mutations(self):
        schedule = build_ag_ring_schedule(build_allgather_contract(4, 1024))
        phase = schedule.phases[1]
        transfers = list(phase.transfers)
        transfers[0] = replace(transfers[0], chunk_ids=("chunk-1",))
        unowned = replace(
            schedule,
            phases=(
                *schedule.phases[:1],
                replace(phase, transfers=tuple(transfers)),
                *schedule.phases[2:],
            ),
        )

        with self.assertRaisesRegex(
            SemanticValidationError, "sends unowned chunks"
        ):
            validate_semantic_schedule(unowned)

        bad_bytes = replace(
            schedule,
            phases=(
                replace(
                    schedule.phases[0],
                    transfers=(
                        replace(schedule.phases[0].transfers[0], byte_length=1023),
                        *schedule.phases[0].transfers[1:],
                    ),
                ),
                *schedule.phases[1:],
            ),
        )
        with self.assertRaisesRegex(SemanticValidationError, "does not match chunk bytes"):
            validate_semantic_schedule(bad_bytes)

        tiny_chunks = tuple(replace(chunk, byte_length=1) for chunk in schedule.chunks)
        tiny_phases = tuple(
            replace(
                phase,
                transfers=tuple(
                    replace(transfer, byte_length=len(transfer.chunk_ids))
                    for transfer in phase.transfers
                ),
            )
            for phase in schedule.phases
        )
        with self.assertRaisesRegex(
            SemanticValidationError, "chunk bytes do not match"
        ):
            validate_semantic_schedule(
                replace(schedule, chunks=tiny_chunks, phases=tiny_phases)
            )

        missing_chunk = replace(schedule, phases=schedule.phases[:-1])
        with self.assertRaisesRegex(SemanticValidationError, "missing chunks"):
            validate_semantic_schedule(missing_chunk)

        wrong_final_owner = replace(
            schedule.contract,
            final_holdings=(frozenset(), *schedule.contract.final_holdings[1:]),
        )
        with self.assertRaisesRegex(
            SemanticValidationError, "do not match allgather semantics"
        ):
            validate_semantic_schedule(replace(schedule, contract=wrong_final_owner))

    def test_semantic_validator_rejects_duplicate_reduction_contributors(self):
        schedule = build_rs_ring_schedule(build_reduce_scatter_contract(4, 1024))
        first = schedule.phases[0].transfers[0]
        duplicate = replace(first, source_retains=True)
        invalid_phase = replace(
            schedule.phases[0],
            transfers=(duplicate, duplicate, *schedule.phases[0].transfers[1:]),
        )
        with self.assertRaisesRegex(
            SemanticValidationError, "receives chunk .* more than once"
        ):
            validate_semantic_schedule(
                replace(schedule, phases=(invalid_phase, *schedule.phases[1:]))
            )

    def test_semantic_validator_rejects_collective_relabel_and_mixed_receives(self):
        allgather = build_ag_ring_schedule(build_allgather_contract(4, 128))
        for collective in ("reduce-scatter", "alltoall", "unsupported"):
            with self.subTest(collective=collective), self.assertRaises(
                SemanticValidationError
            ):
                validate_semantic_schedule(
                    replace(
                        allgather,
                        contract=replace(allgather.contract, collective=collective),
                    )
                )

        reduce_scatter = build_rs_ring_schedule(build_reduce_scatter_contract(4, 128))
        transfer = reduce_scatter.phases[0].transfers[0]
        copied = replace(transfer, operation=TransferOperation.COPY)
        for transfers in ((copied, transfer), (transfer, copied)):
            with self.subTest(order=transfers[0].operation), self.assertRaises(
                SemanticValidationError
            ):
                invalid_phase = replace(
                    reduce_scatter.phases[0],
                    transfers=(*transfers, *reduce_scatter.phases[0].transfers[1:]),
                )
                validate_semantic_schedule(
                    replace(
                        reduce_scatter,
                        phases=(invalid_phase, *reduce_scatter.phases[1:]),
                    )
                )

        allreduce = build_ar_ring_schedule(build_allreduce_contract(4, 128))
        with self.assertRaisesRegex(
            SemanticValidationError, "do not match allreduce semantics"
        ):
            validate_semantic_schedule(
                replace(
                    allreduce,
                    contract=replace(
                        allreduce.contract, required_contributors=()
                    ),
                    phases=(),
                )
            )

    def test_alltoallv_zero_and_matrix_registry_contracts(self):
        with self.assertRaisesRegex(ValueError, "matrix-defined input"):
            get_algorithm("alltoallv", "pairwise").build(2, 8)

        for distribution, extra in (("uniform", ()), ("random", ("--seed", "7"))):
            with self.subTest(distribution=distribution), tempfile.TemporaryDirectory() as directory:
                output_dir = Path(directory) / "case"
                result = self.run_cli(
                    "traffic", "alltoallv", "--host-count", "3",
                    "--data-size", "0B", "--algorithm", "pairwise",
                    "--distribution", distribution, *extra,
                    "--output-dir", str(output_dir),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                with (output_dir / "traffic.csv").open(
                    newline="", encoding="utf-8"
                ) as traffic_file:
                    self.assertEqual(list(csv.reader(traffic_file)), [TRAFFIC_HEADER])

        schedule = build_a2a_pairwisev_from_byte_matrix([[3, 2], [4, 5]])
        self.assertIn("src-0-dst-0", schedule.contract.initial_holdings[0])
        self.assertTrue(validate_semantic_schedule(schedule)["ok"])

    def test_ring_allgather_writer_validates_before_touching_output(self):
        schedule = build_ag_ring_schedule(build_allgather_contract(4, 128))
        invalid = replace(schedule, phases=schedule.phases[:-1])
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "new" / "traffic.csv"
            with self.assertRaisesRegex(SemanticValidationError, "missing chunks"):
                write_schedule_csv(missing, [list(range(4))], invalid)
            self.assertFalse(missing.exists())
            self.assertFalse(missing.parent.exists())

            existing = Path(directory) / "traffic.csv"
            existing.write_text("keep\n", encoding="utf-8")
            with self.assertRaisesRegex(SemanticValidationError, "missing chunks"):
                write_schedule_csv(existing, [list(range(4))], invalid)
            self.assertEqual(existing.read_text(encoding="utf-8"), "keep\n")

    def test_ring_allgather_accepts_zero_bytes_and_writes_header_only(self):
        schedule = build_ag_ring_schedule(build_allgather_contract(4, 0))
        result = validate_semantic_schedule(schedule)
        self.assertTrue(result["ok"])
        self.assertEqual(result["transfer_count"], 0)

        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "ag"
            generated = self.run_cli(
                "traffic", "allgather", "--host-count", "4", "--data-size", "0B",
                "--algorithm", "ring", "--output-dir", str(output_dir),
            )
            self.assertEqual(generated.returncode, 0, generated.stderr)
            with (output_dir / "traffic.csv").open(
                newline="", encoding="utf-8"
            ) as traffic_file:
                self.assertEqual(list(csv.reader(traffic_file)), [TRAFFIC_HEADER])

    def test_ring_collectives_have_n_minus_one_steps_and_expected_wire_bytes(self):
        rank_size = 4
        total_bytes = 1024

        schedule = build_rs_ring_schedule(
            build_reduce_scatter_contract(rank_size, total_bytes)
        )
        self.assertEqual(len(schedule.phases), rank_size - 1)

        allgather = build_ag_ring_schedule(build_allgather_contract(rank_size, total_bytes))
        self.assertEqual(len(allgather.phases), rank_size - 1)
        self.assertTrue(all(len(phase.transfers) == rank_size for phase in allgather.phases))
        for rank in range(rank_size):
            wire_bytes = sum(
                transfer.byte_length
                for phase in allgather.phases
                for transfer in phase.transfers
                if transfer.sender == rank
            )
            self.assertEqual(wire_bytes, total_bytes * (rank_size - 1))

    def test_recursive_collectives_use_logarithmic_steps_for_power_of_two(self):
        rank_size = 8
        total_bytes = 4096

        reduce_scatter = build_rs_recursive_halving_schedule(
            build_reduce_scatter_contract(rank_size, total_bytes)
        )
        allgather = build_ag_recursive_doubling_schedule(
            build_allgather_contract(rank_size, total_bytes)
        )

        self.assertEqual(len(reduce_scatter.phases), 3)
        self.assertEqual(len(allgather.phases), 3)
        self.assertTrue(validate_semantic_schedule(reduce_scatter)["ok"])
        self.assertTrue(validate_semantic_schedule(allgather)["ok"])

    def test_recursive_halving_uses_mpich_fold_for_non_power_of_two(self):
        schedule = build_rs_recursive_halving_schedule(
            build_reduce_scatter_contract(5, 640)
        )
        self.assertEqual(
            [phase.label for phase in schedule.phases],
            ["FoldIn", "ReduceScatter", "ReduceScatter", "FoldOut"],
        )
        fold_in = schedule.phases[0].transfers
        self.assertEqual(
            (fold_in[0].sender, fold_in[0].receiver, fold_in[0].chunk_ids),
            (0, 1, ("chunk-0", "chunk-1", "chunk-2", "chunk-3", "chunk-4")),
        )
        fold_out = schedule.phases[-1].transfers
        self.assertEqual(
            (fold_out[0].sender, fold_out[0].receiver, fold_out[0].chunk_ids),
            (1, 0, ("chunk-0",)),
        )

    def test_nhr_collectives_match_hccl_non_power_of_two_step_formulas(self):
        reduce_scatter = build_rs_nhr_schedule(build_reduce_scatter_contract(5, 640))
        allgather = build_ag_nhr_schedule(build_allgather_contract(5, 640))

        rs_rank_zero = [
            next(transfer for transfer in phase.transfers if transfer.sender == 0)
            for phase in reduce_scatter.phases
        ]
        self.assertEqual(
            [
                (transfer.receiver, transfer.chunk_ids)
                for transfer in rs_rank_zero
            ],
            [
                (4, ("chunk-4", "chunk-2")),
                (3, ("chunk-3",)),
                (1, ("chunk-1",)),
            ],
        )

        ag_rank_zero = [
            next(transfer for transfer in phase.transfers if transfer.sender == 0)
            for phase in allgather.phases
        ]
        self.assertEqual(
            [
                (transfer.receiver, transfer.chunk_ids)
                for transfer in ag_rank_zero
            ],
            [
                (4, ("chunk-0",)),
                (2, ("chunk-0",)),
                (1, ("chunk-0", "chunk-3")),
            ],
        )

    def test_reduce_scatter_and_allgather_are_available_from_v2_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reduce_scatter = self.run_cli(
                "traffic", "reduce-scatter", "--host-count", "3", "--data-size", "513B",
                "--algorithm", "recursive-halving", "--output-dir", str(root / "rs"),
            )
            allgather = self.run_cli(
                "traffic", "allgather", "--host-count", "4", "--data-size", "513B",
                "--algorithm", "recursive-doubling", "--output-dir", str(root / "ag"),
            )

            self.assertEqual(reduce_scatter.returncode, 0, reduce_scatter.stderr)
            self.assertEqual(allgather.returncode, 0, allgather.stderr)
            self.assertTrue((root / "rs" / "traffic.csv").exists())
            self.assertTrue((root / "ag" / "traffic.csv").exists())

    def test_v2_cli_supports_recursive_doubling_for_non_power_of_two_group(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_cli(
                "traffic",
                "allgather",
                "--host-count",
                "3",
                "--data-size",
                "513B",
                "--algorithm",
                "recursive-doubling",
                "--output-dir",
                str(Path(directory) / "ag"),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((Path(directory) / "ag" / "traffic.csv").exists())

    def test_v2_cli_validates_allgather_to_json_stdout(self):
        result = self.run_cli(
            "traffic",
            "validate",
            "allgather",
            "--rank-size",
            "4",
            "--data-size",
            "1KB",
            "--algorithm",
            "ring",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        trace = json.loads(result.stdout)
        self.assertTrue(trace["ok"])
        self.assertEqual(trace["collective"], "allgather")
        self.assertEqual(trace["rank_size"], 4)
        self.assertNotIn("final_holdings", trace)
        self.assertEqual(trace["transfer_count"], 12)

        verbose = self.run_cli(
            "traffic", "validate", "allgather", "--rank-size", "4",
            "--data-size", "1KB", "--algorithm", "ring", "--verbose",
        )
        self.assertEqual(verbose.returncode, 0, verbose.stderr)
        detail = json.loads(verbose.stdout)
        self.assertEqual(detail["final_holdings"]["0"], [
            "chunk-0", "chunk-1", "chunk-2", "chunk-3"
        ])

    def test_v2_cli_validates_allreduce_alltoall_and_alltoallv(self):
        scalar_cases = (
            ("allreduce", "recursive-halving-doubling", "5", "513B"),
            ("alltoall", "pairwise", "4", "7B"),
        )
        for collective, algorithm, rank_size, data_size in scalar_cases:
            with self.subTest(collective=collective):
                result = self.run_cli(
                    "traffic", "validate", collective,
                    "--rank-size", rank_size,
                    "--data-size", data_size,
                    "--algorithm", algorithm,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(json.loads(result.stdout)["ok"])

        with tempfile.TemporaryDirectory() as directory:
            matrix = Path(directory) / "matrix.csv"
            matrix.write_text("0,2,5\n7,0,11\n13,17,0\n", encoding="utf-8")
            result = self.run_cli(
                "traffic", "validate", "alltoallv",
                "--rank-size", "3",
                "--algorithm", "pairwise",
                "--distribution", "matrix",
                "--traffic-matrix", str(matrix),
                "--verbose",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            detail = json.loads(result.stdout)
            self.assertTrue(detail["ok"])
            self.assertEqual(detail["contract"]["input_bytes_by_rank"], [7, 18, 30])

    def test_v2_cli_writes_reduce_scatter_validation_without_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "traces" / "reduce-scatter.json"
            arguments = (
                "traffic",
                "validate",
                "reduce-scatter",
                "--rank-size",
                "5",
                "--data-size",
                "640B",
                "--algorithm",
                "recursive-halving",
                "--output",
                str(output),
                "--verbose",
            )
            created = self.run_cli(*arguments)
            refused = self.run_cli(*arguments)
            overwritten = self.run_cli(*arguments, "--overwrite")

            self.assertEqual(created.returncode, 0, created.stderr)
            self.assertTrue(output.exists())
            trace = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(trace["ok"])
            self.assertEqual(
                trace["final_holdings"]["0"]["chunk-0"], [0, 1, 2, 3, 4]
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("output already exists", refused.stderr)
            self.assertEqual(overwritten.returncode, 0, overwritten.stderr)

    def test_v2_cli_rejects_validation_overwrite_without_output(self):
        result = self.run_cli(
            "traffic",
            "validate",
            "allgather",
            "--rank-size",
            "4",
            "--data-size",
            "1KB",
            "--algorithm",
            "ring",
            "--overwrite",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("--overwrite requires --output", result.stderr)

    def test_v2_writer_preserves_each_existing_schedule_phase_and_transfer(self):
        cases = [
            ("allreduce", "nhr", None),
            ("allreduce", "recursive-halving-doubling", None),
            ("alltoall", "pairwise", None),
        ]
        for collective, algorithm, peers_per_phase in cases:
            with self.subTest(collective=collective, algorithm=algorithm), tempfile.TemporaryDirectory() as directory:
                output_dir = Path(directory) / "case"
                arguments = [
                    "traffic", collective, "--host-count", "4", "--data-size", "513B",
                    "--algorithm", algorithm, "--output-dir", str(output_dir),
                ]
                if peers_per_phase is not None:
                    arguments.extend(["--peers-per-phase", str(peers_per_phase)])
                result = self.run_cli(*arguments)
                self.assertEqual(result.returncode, 0, result.stderr)

                algorithm_spec = get_algorithm(collective, algorithm)
                if peers_per_phase is None:
                    schedule = algorithm_spec.build(4, 513)
                else:
                    schedule = algorithm_spec.build(
                        4, 513, peers_per_phase=peers_per_phase
                    )
                expected = [
                    (
                        str(phase.phase),
                        str(transfer.sender),
                        str(transfer.receiver),
                        str(transfer.byte_length),
                    )
                    for phase in schedule.phases
                    for transfer in phase.transfers
                ]
                with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as traffic_file:
                    rows = list(csv.DictReader(traffic_file))
                actual = [
                    (row["phaseId"], row["sourceNodeId"], row["destNodeId"], row["dataSize(Byte)"])
                    for row in rows
                ]
                self.assertEqual(actual, expected)

    def test_interleaved_layout_maps_rank_groups_without_using_domain_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            result = self.run_cli(
                "traffic", "allreduce", "--host-count", "8", "--group-size", "4",
                "--data-size", "8B", "--algorithm", "ring", "--group-layout",
                "interleaved", "--output-dir", str(output_dir),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as traffic_file:
                rows = list(csv.DictReader(traffic_file))
            first_group_nodes = {
                int(row["sourceNodeId"]) for row in rows if int(row["phaseId"]) < 6
            }
            second_group_nodes = {
                int(row["sourceNodeId"]) for row in rows if int(row["phaseId"]) >= 6
            }
            self.assertEqual(first_group_nodes, {0, 2, 4, 6})
            self.assertEqual(second_group_nodes, {1, 3, 5, 7})

    def test_placement_file_maps_group_rank_to_explicit_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            placement = root / "placement.csv"
            placement.write_text(
                "groupId,rankId,nodeId\n0,0,17\n0,1,9\n1,0,4\n1,1,12\n",
                encoding="utf-8",
            )
            output_dir = root / "case"
            result = self.run_cli(
                "traffic", "allreduce", "--data-size", "4B", "--algorithm", "ring",
                "--placement-file", str(placement), "--output-dir", str(output_dir),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as traffic_file:
                rows = list(csv.DictReader(traffic_file))
            self.assertEqual(
                [(row["sourceNodeId"], row["destNodeId"]) for row in rows],
                [("17", "9"), ("9", "17"), ("17", "9"), ("9", "17"),
                 ("4", "12"), ("12", "4"), ("4", "12"), ("12", "4")],
            )

    def test_placement_file_allows_node_reuse_across_groups_and_counts_unique_hosts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            placement = root / "placement.csv"
            placement.write_text(
                "groupId,rankId,nodeId\n0,0,7\n0,1,8\n1,0,7\n1,1,9\n",
                encoding="utf-8",
            )
            output_dir = root / "case"
            result = self.run_cli(
                "traffic", "allreduce", "--host-count", "3", "--group-size", "2",
                "--data-size", "4B", "--algorithm", "ring", "--placement-file",
                str(placement), "--output-dir", str(output_dir),
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            with (output_dir / "traffic.csv").open(newline="", encoding="utf-8") as traffic_file:
                rows = list(csv.DictReader(traffic_file))
            self.assertEqual(
                {int(row["sourceNodeId"]) for row in rows},
                {7, 8, 9},
            )

    def test_placement_file_rejects_invalid_or_conflicting_mappings(self):
        invalid_files = {
            "missing rank": "groupId,rankId,nodeId\n0,0,7\n0,2,8\n",
            "duplicate node": "groupId,rankId,nodeId\n0,0,7\n0,1,7\n",
            "unequal groups": "groupId,rankId,nodeId\n0,0,7\n0,1,8\n1,0,9\n",
        }
        for expected, contents in invalid_files.items():
            with self.subTest(expected=expected), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                placement = root / "placement.csv"
                placement.write_text(contents, encoding="utf-8")
                result = self.run_cli(
                    "traffic", "allreduce", "--data-size", "4B", "--algorithm", "ring",
                    "--placement-file", str(placement), "--output-dir", str(root / "case"),
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse((root / "case" / "traffic.csv").exists())

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            placement = root / "placement.csv"
            placement.write_text("groupId,rankId,nodeId\n0,0,7\n0,1,8\n", encoding="utf-8")
            result = self.run_cli(
                "traffic", "allreduce", "--host-count", "3", "--group-size", "2",
                "--data-size", "4B", "--algorithm", "ring", "--placement-file",
                str(placement), "--output-dir", str(root / "case"),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("host-count 3 does not match placement-file host count 2", result.stderr)

    def test_group_layout_and_placement_file_are_mutually_exclusive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            placement = root / "placement.csv"
            placement.write_text("groupId,rankId,nodeId\n0,0,7\n0,1,8\n", encoding="utf-8")
            result = self.run_cli(
                "traffic", "allreduce", "--data-size", "4B", "--algorithm", "ring",
                "--group-layout", "contiguous", "--placement-file", str(placement),
                "--output-dir", str(root / "case"),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not allowed with argument --group-layout", result.stderr)

    def test_peers_per_phase_is_pairwise_policy_and_range_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            accepted = self.run_cli(
                "traffic", "alltoall", "--host-count", "4", "--data-size", "7B",
                "--algorithm", "pairwise", "--peers-per-phase", "2",
                "--output-dir", str(Path(directory) / "case"),
            )
            self.assertEqual(accepted.returncode, 0, accepted.stderr)

        for algorithm, peers, message in (
            ("pairwise", "0", "must satisfy 1 <= K < group-size"),
            ("pairwise", "4", "must satisfy 1 <= K < group-size"),
        ):
            with self.subTest(algorithm=algorithm, peers=peers), tempfile.TemporaryDirectory() as directory:
                result = self.run_cli(
                    "traffic", "alltoall", "--host-count", "4", "--data-size", "7B",
                    "--algorithm", algorithm, "--peers-per-phase", peers, "--output-dir",
                    str(Path(directory) / "case"),
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_v2_rejects_legacy_options_and_algorithm_names(self):
        legacy_option = self.run_cli("traffic", "allreduce", "-n", "4")
        with tempfile.TemporaryDirectory() as directory:
            legacy_algorithm = self.run_cli(
                "traffic", "allreduce", "--host-count", "4", "--data-size", "4B",
                "--algorithm", "ar_nhr", "--output-dir", str(Path(directory) / "case"),
            )
        self.assertNotEqual(legacy_option.returncode, 0)
        self.assertNotEqual(legacy_algorithm.returncode, 0)
        self.assertIn("invalid choice: 'ar_nhr'", legacy_algorithm.stderr)

    def test_legacy_cli_keeps_timestamp_directory_and_representative_csv(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, "build_traffic.py", "-n", "2", "-c", "2", "-b", "4B",
                 "-a", "ar_ring", "-o", directory],
                cwd=REPO_ROOT / "traffic_maker", capture_output=True, text=True, check=False,
                env={**os.environ, "PYTHONPATH": str(REPO_ROOT / "traffic_maker")},
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = list(Path(directory).glob("*_2_2_4_ar_ring_d0_linear/traffic.csv"))
            self.assertEqual(len(generated), 1)
            with generated[0].open(newline="", encoding="utf-8") as traffic_file:
                rows = list(csv.reader(traffic_file))
            self.assertEqual(rows[1:], [
                ["0", "0", "1", "2", "URMA_WRITE", "7", "0ns", "0", ""],
                ["1", "1", "0", "2", "URMA_WRITE", "7", "0ns", "0", ""],
                ["2", "0", "1", "2", "URMA_WRITE", "7", "0ns", "1", "0"],
                ["3", "1", "0", "2", "URMA_WRITE", "7", "0ns", "1", "0"],
            ])

    def test_version_and_each_available_help_level_are_discoverable(self):
        version = self.run_cli("--version")
        top_help = self.run_cli("--help")
        traffic_help = self.run_cli("traffic", "--help")
        traffic_validate_help = self.run_cli("traffic", "validate", "--help")
        allgather_validate_help = self.run_cli(
            "traffic", "validate", "allgather", "--help"
        )
        allreduce_help = self.run_cli("traffic", "allreduce", "--help")
        simai_help = self.run_cli("traffic", "convert-simai", "--help")
        topology_help = self.run_cli("topology", "--help")
        topology_build_help = self.run_cli("topology", "build", "--help")
        topology_check_help = self.run_cli("topology", "check", "--help")
        topology_plot_help = self.run_cli("topology", "plot", "--help")
        case_help = self.run_cli("case", "--help")
        case_show_help = self.run_cli("case", "show", "--help")
        case_check_help = self.run_cli("case", "check", "--help")
        trace_help = self.run_cli("trace", "--help")
        trace_parse_help = self.run_cli("trace", "parse", "--help")

        self.assertEqual(version.returncode, 0, version.stderr)
        self.assertIn("interface v2", version.stdout)
        self.assertIn("tools 2.1.0", version.stdout)
        for result in (
            top_help, traffic_help, traffic_validate_help, allgather_validate_help,
            allreduce_help, simai_help, topology_help,
            topology_build_help, topology_check_help, topology_plot_help,
            case_help, case_show_help, case_check_help, trace_help, trace_parse_help,
        ):
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Example:", result.stdout)
        self.assertIn("traffic", top_help.stdout)
        self.assertNotIn("convert-simai", top_help.stdout)
        self.assertIn("allreduce", traffic_help.stdout)
        self.assertIn("convert-simai", traffic_help.stdout)
        self.assertIn("validate", traffic_help.stdout)
        self.assertIn("reduce-scatter", traffic_validate_help.stdout)
        self.assertIn("recursive-doubling", allgather_validate_help.stdout)
        self.assertIn("--group-size", allreduce_help.stdout)
        self.assertIn("default: host-count", allreduce_help.stdout)
        self.assertIn("ring", allreduce_help.stdout)

    def test_missing_commands_show_complete_help_for_the_current_level(self):
        missing_top_command = self.run_cli()
        missing_traffic_command = self.run_cli("traffic")

        self.assertNotEqual(missing_top_command.returncode, 0)
        self.assertIn("Prepare ns-3-ub input artifacts", missing_top_command.stderr)
        self.assertIn("traffic", missing_top_command.stderr)
        self.assertIn("options:", missing_top_command.stderr)
        self.assertIn("Example:", missing_top_command.stderr)

        self.assertNotEqual(missing_traffic_command.returncode, 0)
        self.assertIn("Generate traffic.csv or validate", missing_traffic_command.stderr)
        self.assertIn("allreduce", missing_traffic_command.stderr)
        self.assertIn("options:", missing_traffic_command.stderr)
        self.assertIn("Example:", missing_traffic_command.stderr)

    def test_allreduce_does_not_overwrite_an_existing_traffic_file(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory) / "case"
            output_dir.mkdir()
            traffic_path = output_dir / "traffic.csv"
            traffic_path.write_text("owned by another tool\n", encoding="utf-8")

            result = self.run_cli(
                "traffic",
                "allreduce",
                "--host-count",
                "2",
                "--data-size",
                "1KB",
                "--algorithm",
                "ring",
                "--output-dir",
                str(output_dir),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("traffic.csv already exists", result.stderr)
            self.assertIn("choose an empty output-dir", result.stderr)
            self.assertEqual(traffic_path.read_text(encoding="utf-8"), "owned by another tool\n")

    def test_traffic_write_failure_is_reported_without_traceback(self):
        result = self.run_cli(
            "traffic", "allreduce", "--host-count", "2", "--data-size", "1KB",
            "--algorithm", "ring", "--output-dir", "/dev/null/ub-tools-output",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Traceback", result.stderr)
        self.assertIn("Not a directory", result.stderr)


if __name__ == "__main__":
    unittest.main()
