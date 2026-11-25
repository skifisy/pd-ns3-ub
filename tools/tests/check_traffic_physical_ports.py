"""Run small RTP endpoint/READ-response regressions against a built simulator.

Usage: python3 tools/tests/check_traffic_physical_ports.py --binary <ub-quick-example>
"""

import argparse
import csv
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
NS3 = ROOT / "ns-3-ub"
BASE_HEADER = ["taskId", "sourceNode", "destNode", "dataSize(Byte)", "opType",
               "priority", "delay", "phaseId", "dependOnPhases"]


def rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def check(binary):
    with tempfile.TemporaryDirectory(prefix="ub-physical-ports-") as temporary:
        root = Path(temporary)

        def run(name, header, traffic, expected_error=None, attributes=""):
            case = root / name
            case.mkdir()
            template = NS3 / "scratch/2nodes_multiple-tp"
            for filename in ["node.csv", "topology.csv", "routing_table.csv"]:
                shutil.copyfile(template / filename, case / filename)
            settings = (template / "network_attribute.txt").read_text()
            for key in ["UB_TRACE_ENABLE", "UB_TASK_TRACE_ENABLE", "UB_PACKET_TRACE_ENABLE",
                        "UB_PORT_TRACE_ENABLE", "UB_RECORD_PKT_TRACE", "UB_PARSE_TRACE_ENABLE"]:
                settings = settings.replace(f'global {key} "true"', f'global {key} "false"')
            # Replay aggregates parallel circuits by node pair. Native port
            # counters provide the physical-port evidence for this small test.
            for key in ["UB_TRACE_ENABLE", "UB_PORT_TRACE_ENABLE"]:
                settings = settings.replace(f'global {key} "false"', f'global {key} "true"')
            (case / "network_attribute.txt").write_text(settings + "\n" + attributes)
            with (case / "traffic.csv").open("w", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerow(header)
                writer.writerows(traffic)
            result = subprocess.run([str(binary), "--case-path=" + str(case), "--replay",
                                     "--dependency-visibility-delay=1ns"],
                                    cwd=NS3, capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            if expected_error:
                assert result.returncode != 0 and expected_error in output, (name, output)
            else:
                assert result.returncode == 0, (name, output)
                metadata = {row["key"]: row["value"] for row in rows(case / "replay/replay_run.csv")}
                assert int(metadata["completed_tasks"]) == len(traffic), (name, metadata, output)
            print("PASS", name)
            return case

        header = BASE_HEADER + ["sourcePort", "destPort"]
        traffic = [[0, 0, 1, 8192, "URMA_WRITE", 7, "0ns", 0, "", 1, 1],
                   [1, 0, 1, 8192, "URMA_READ", 7, "0ns", 1, "0", 2, 2],
                   [2, 0, 0, 0, "TIMER", 7, "1ns", 2, "1", "", ""]]
        for spray in [False, True]:
            case = run("read-write-spray-" + str(spray), header, traffic,
                       attributes=f'default ns3::UbTransportChannel::UsePacketSpray "{str(spray).lower()}"\n')
            def transmitted(node, port):
                path = case / "runlog" / f"PortTrace_node_{node}_port_{port}.tr"
                text = path.read_text() if path.exists() else ""
                return sum(map(int, re.findall(r"Port Tx, port ID: \d+ PacketSize: (\d+)", text)))

            assert transmitted(0, 0) == transmitted(1, 0) == 0
            assert transmitted(0, 1) > 8192  # WRITE payload
            assert transmitted(1, 2) > 8192  # READ_RESPONSE payload
            assert transmitted(1, 1) < 8192  # WRITE acknowledgments only

        run("port-zero", header, [[0, 0, 1, 4096, "URMA_WRITE", 7, "0ns", 0, "", 0, 0]])
        run("legacy-auto", BASE_HEADER, [[0, 0, 1, 4096, "URMA_WRITE", 7, "0ns", 0, ""]])
        # Column order is also used by the phase-indexing pass.
        order = [10, 7, 9, 0, 1, 2, 3, 4, 5, 6, 8]
        run("reordered", [header[i] for i in order], [[row[i] for i in order] for row in traffic])
        for name, source_port, dest_port, error in [
                ("negative", -1, 1, "signed values are not supported"),
                ("out-of-range", 3, 1, "port outside node device range"),
                ("unencodable", 255, 1, "physical ports must be 0..254"),
                ("missing-port", "", 1, "requires both sourcePort and destPort values"),
                ("unreachable", 1, 2, "No route for physical endpoints")]:
            run(name, header, [[0, 0, 1, 4096, "URMA_WRITE", 7, "0ns", 0, "", source_port, dest_port]], error)
        run("partial-header", BASE_HEADER + ["sourcePort"],
            [[0, 0, 1, 4096, "URMA_WRITE", 7, "0ns", 0, "", 1]],
            "requires both sourcePort and destPort columns")
        run("ctp-explicit", header, [traffic[0]], "require RTP URMA_READ/URMA_WRITE",
            attributes='default ns3::UbApp::TransportMode "CTP"\n')


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except ImportError:
        pass
    check(arguments.binary.resolve())
