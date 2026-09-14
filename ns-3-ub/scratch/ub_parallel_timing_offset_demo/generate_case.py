#!/usr/bin/env python3
"""Generate the public parallel timing-offset demo case with ns-3-ub-tools."""

import argparse
import csv
from pathlib import Path
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "scratch" / "ns-3-ub-tools"
CASE_DIR = Path(__file__).resolve().parent
RUN_EXPERIMENT_SCRIPTS = (
    REPO_ROOT / ".codex" / "skills" / "openusim-run-experiment" / "scripts"
)

sys.path.insert(0, str(TOOLS_DIR))
import net_sim_builder as netsim  # noqa: E402
sys.path.insert(0, str(RUN_EXPERIMENT_SCRIPTS))
from openusim_run_experiment import network_attribute_writer  # noqa: E402


EXPLICIT_NETWORK_OVERRIDES = {
    "ns3::UbJetty::UbJettyInflightMax": "10000",
    "ns3::UbSwitchAllocator::AllocationTime": "1ns",
}

OBSERVABILITY_OVERRIDES = {
    "UB_TRACE_ENABLE": "true",
    "UB_TASK_TRACE_ENABLE": "true",
    "UB_PACKET_TRACE_ENABLE": "false",
    "UB_PORT_TRACE_ENABLE": "false",
    "UB_QUEUE_TRACE_ENABLE": "false",
    "UB_FLOW_CONTROL_TRACE_ENABLE": "false",
    "UB_CONGESTION_CONTROL_TRACE_ENABLE": "false",
    "UB_RECORD_PKT_TRACE": "false",
    "UB_PARSE_TRACE_ENABLE": "false",
}


def build_topology(case_dir: Path, host_count: int, leaf_count: int) -> None:
    if host_count % leaf_count != 0:
        raise ValueError("host-count must be divisible by leaf-count")

    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = str(case_dir)

    spine_count = host_count // leaf_count
    leaf_base = host_count
    spine_base = host_count + leaf_count

    leaf_ids = []
    spine_ids = []
    for host_id in range(host_count):
        graph.add_netisim_host(host_id, forward_delay="1ns")

    for leaf_index in range(leaf_count):
        leaf_id = leaf_base + leaf_index
        graph.add_netisim_node(leaf_id, forward_delay="1ns")
        leaf_ids.append(leaf_id)

    for spine_index in range(spine_count):
        spine_id = spine_base + spine_index
        graph.add_netisim_node(spine_id, forward_delay="1ns")
        spine_ids.append(spine_id)

    hosts_per_leaf = host_count // leaf_count
    for host_id in range(host_count):
        leaf_id = leaf_ids[host_id // hosts_per_leaf]
        graph.add_netisim_edge(host_id, leaf_id, bandwidth="400Gbps", delay="20ns", edge_count=1)

    for leaf_id in leaf_ids:
        for spine_id in spine_ids:
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth="400Gbps", delay="20ns", edge_count=1)

    graph.build_graph_config()
    graph.gen_2layer_clos_compressed_route_table(host_count, leaf_count)
    graph.write_config(include_transport=False)


def build_traffic(args: argparse.Namespace, case_dir: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="ub-parallel-timing-offset-") as tmp:
        cmd = [
            sys.executable,
            str(TOOLS_DIR / "traffic_maker" / "build_traffic.py"),
            "--host-count",
            str(args.host_count),
            "--comm-domain-size",
            str(args.comm_domain_size),
            "--data-size",
            args.data_size,
            "--algo",
            args.algo,
            "--phase-delay",
            str(args.phase_delay_ns),
            "--rank-mapping",
            args.rank_mapping,
            "--output-dir",
            tmp,
        ]
        if args.algo == "a2a_scatter":
            cmd.extend(["--scatter-k", str(args.scatter_k)])
        subprocess.run(cmd, check=True)
        generated = sorted(Path(tmp).glob("*/traffic.csv"))
        if len(generated) != 1:
            raise RuntimeError(f"expected one generated traffic.csv, found {len(generated)}")
        with generated[0].open(newline="", encoding="utf-8") as source:
            rows = list(csv.reader(source))
        with (case_dir / "traffic.csv").open("w", newline="", encoding="utf-8") as destination:
            csv.writer(destination, lineterminator="\n").writerows(rows)


def write_network_attributes(case_dir: Path) -> None:
    network_attribute_writer.write_network_attributes(
        case_dir,
        explicit_overrides=EXPLICIT_NETWORK_OVERRIDES,
        observability_overrides=OBSERVABILITY_OVERRIDES,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the public parallel timing-offset demo case."
    )
    parser.add_argument("--host-count", type=int, default=8)
    parser.add_argument("--leaf-count", type=int, default=4)
    parser.add_argument("--comm-domain-size", type=int, default=4)
    parser.add_argument("--data-size", default="1MB")
    parser.add_argument(
        "--algo",
        choices=["ar_ring", "ar_nhr", "ar_rhd", "a2a_pairwise", "a2a_scatter"],
        default="a2a_scatter",
    )
    parser.add_argument("--scatter-k", type=int, default=2)
    parser.add_argument("--phase-delay-ns", type=int, default=10)
    parser.add_argument("--rank-mapping", choices=["linear", "round-robin"], default="linear")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.host_count % args.comm_domain_size != 0:
        raise ValueError("host-count must be divisible by comm-domain-size")

    for filename in [
        "node.csv",
        "topology.csv",
        "routing_table.csv",
        "transport_channel.csv",
        "traffic.csv",
        "network_attribute.txt",
    ]:
        path = CASE_DIR / filename
        if path.exists():
            path.unlink()

    build_topology(CASE_DIR, args.host_count, args.leaf_count)
    build_traffic(args, CASE_DIR)
    write_network_attributes(CASE_DIR)
    print(f"case written to {CASE_DIR}")


if __name__ == "__main__":
    main()
