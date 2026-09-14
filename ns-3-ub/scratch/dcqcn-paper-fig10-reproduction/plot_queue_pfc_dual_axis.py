#!/usr/bin/env python3
from __future__ import annotations

import csv
import math
import re
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


CASE_DIR = Path(__file__).resolve().parent
RUNLOG_DIR = CASE_DIR / "runlog"
ANALYSIS_DIR = CASE_DIR / "analysis"

BIN_US = 1000.0
STOP_MS = 200.0
THROUGHPUT_YMAX_GBPS = 42.0
QUEUE_YMAX_KB = 220.0

PORT_PATTERN = re.compile(r"\[(\d+(?:\.\d+)?)us\]\s+Port\s+(Tx|Rx), port ID: (\d+) PacketSize: (\d+)")
QUEUE_PATTERN = re.compile(
    r"\[(\d+(?:\.\d+)?)us\]\s+Queue Update, source: (\S+)\s+voqBytes: (\d+)\s+egressBytes: (\d+)\s+totalBytes: (\d+)"
)
PFC_PATTERN = re.compile(
    r"\[(\d+(?:\.\d+)?)us\]\s+PFC\s+(PAUSE|RESUME), priority: (\d+)\s+ingressBytes: (\d+)"
)
PFC_FILE_PATTERN = re.compile(r"PfcTrace_node_(\d+)_port_(\d+)\.tr")


def load_traffic() -> pd.DataFrame:
    return pd.read_csv(CASE_DIR / "traffic.csv")


def identify_receiver_node() -> int:
    traffic_df = load_traffic()
    receivers = sorted(int(node_id) for node_id in traffic_df["destNode"].unique().tolist())
    if len(receivers) != 1:
        raise RuntimeError(f"expected one receiver node, got {receivers}")
    return receivers[0]


def identify_sender_nodes() -> list[int]:
    traffic_df = load_traffic()
    return sorted(int(node_id) for node_id in traffic_df["sourceNode"].unique().tolist())


def identify_bottleneck_port() -> tuple[int, int]:
    receiver_node = identify_receiver_node()
    with (CASE_DIR / "topology.csv").open(newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            lhs = int(row["nodeId1"])
            rhs = int(row["nodeId2"])
            if lhs == receiver_node:
                return rhs, int(row["portId2"])
            if rhs == receiver_node:
                return lhs, int(row["portId1"])
    raise RuntimeError("unable to identify bottleneck egress port")


def load_port_events(trace_file: Path) -> pd.DataFrame:
    records = []
    for line in trace_file.read_text().splitlines():
        match = PORT_PATTERN.search(line)
        if not match:
            continue
        ts_us, direction, port_id, size = match.groups()
        records.append(
            {
                "time_us": float(ts_us),
                "direction": direction,
                "port_id": int(port_id),
                "size_B": int(size),
            }
        )
    if not records:
        raise RuntimeError(f"no port events found in {trace_file}")
    return pd.DataFrame.from_records(records).sort_values("time_us")


def compute_realtime_throughput(port_df: pd.DataFrame, bin_us: float) -> pd.DataFrame:
    tx_df = port_df[port_df["direction"] == "Tx"].copy()
    if tx_df.empty:
        return pd.DataFrame(columns=["bin_start_us", "time_ms", "throughput_Gbps"])

    bin_starts = [float(i) * bin_us for i in range(int(math.ceil(STOP_MS * 1000.0 / bin_us)))]
    bins_df = pd.DataFrame({"bin_start_us": bin_starts})

    tx_df["bin_start_us"] = (tx_df["time_us"] // bin_us) * bin_us
    series = (
        tx_df.groupby("bin_start_us", as_index=False)["size_B"]
        .sum()
        .rename(columns={"size_B": "bytes_in_bin"})
        .sort_values("bin_start_us")
    )
    merged = bins_df.merge(series, on="bin_start_us", how="left").fillna({"bytes_in_bin": 0})
    merged["bytes_in_bin"] = merged["bytes_in_bin"].astype(int)
    merged["throughput_Gbps"] = merged["bytes_in_bin"] * 8.0 / (bin_us * 1000.0)
    merged["time_ms"] = merged["bin_start_us"] / 1000.0
    return merged


def load_queue_events(trace_file: Path) -> pd.DataFrame:
    records = []
    for line in trace_file.read_text().splitlines():
        match = QUEUE_PATTERN.search(line)
        if not match:
            continue
        ts_us, source, voq_bytes, egress_bytes, total_bytes = match.groups()
        records.append(
            {
                "time_us": float(ts_us),
                "source": source,
                "voq_B": int(voq_bytes),
                "egress_B": int(egress_bytes),
                "total_B": int(total_bytes),
            }
        )
    if not records:
        raise RuntimeError(f"no queue events found in {trace_file}")
    return pd.DataFrame.from_records(records).sort_values("time_us")


def build_queue_step_series(queue_df: pd.DataFrame, bin_us: float) -> pd.DataFrame:
    bin_starts = [float(i) * bin_us for i in range(int(math.ceil(STOP_MS * 1000.0 / bin_us)))]
    bins_df = pd.DataFrame({"bin_start_us": bin_starts})
    queue_state = queue_df[["time_us", "voq_B", "egress_B", "total_B"]].copy()
    merged = pd.merge_asof(
        bins_df,
        queue_state,
        left_on="bin_start_us",
        right_on="time_us",
        direction="backward",
    )
    merged[["voq_B", "egress_B", "total_B"]] = merged[["voq_B", "egress_B", "total_B"]].fillna(0).astype(int)
    merged["voq_KB"] = merged["voq_B"] / 1024.0
    merged["egress_KB"] = merged["egress_B"] / 1024.0
    merged["total_KB"] = merged["total_B"] / 1024.0
    merged["time_ms"] = merged["bin_start_us"] / 1000.0
    return merged


def load_pfc_events() -> pd.DataFrame:
    records = []
    for trace_file in sorted(RUNLOG_DIR.glob("PfcTrace_node_*_port_*.tr")):
        match_file = PFC_FILE_PATTERN.fullmatch(trace_file.name)
        if match_file is None:
            continue
        node_id = int(match_file.group(1))
        port_id = int(match_file.group(2))
        for line in trace_file.read_text().splitlines():
            match = PFC_PATTERN.search(line)
            if not match:
                continue
            ts_us, action, priority, ingress_bytes = match.groups()
            records.append(
                {
                    "time_us": float(ts_us),
                    "time_ms": float(ts_us) / 1000.0,
                    "node_id": node_id,
                    "port_id": port_id,
                    "action": action,
                    "priority": int(priority),
                    "ingress_B": int(ingress_bytes),
                }
            )
    if not records:
        return pd.DataFrame(columns=["time_us", "time_ms", "node_id", "port_id", "action", "priority", "ingress_B"])
    return pd.DataFrame.from_records(records).sort_values(["time_us", "node_id", "port_id"])


def main() -> None:
    ANALYSIS_DIR.mkdir(exist_ok=True)

    sender_nodes = identify_sender_nodes()
    bottleneck_node, bottleneck_port = identify_bottleneck_port()

    flow_series = []
    for sender_node in sender_nodes:
        trace_file = RUNLOG_DIR / f"PortTrace_node_{sender_node}_port_0.tr"
        throughput_df = compute_realtime_throughput(load_port_events(trace_file), BIN_US)
        throughput_df["sender_node"] = sender_node
        flow_series.append(throughput_df)
    throughput_all = pd.concat(flow_series, ignore_index=True)

    queue_trace_file = RUNLOG_DIR / f"QueueTrace_node_{bottleneck_node}_port_{bottleneck_port}.tr"
    queue_df = build_queue_step_series(load_queue_events(queue_trace_file), BIN_US)
    pfc_df = load_pfc_events()

    merged = queue_df[["bin_start_us", "time_ms", "voq_KB", "egress_KB", "total_KB"]].copy()
    for sender_node in sender_nodes:
        sender_df = throughput_all[throughput_all["sender_node"] == sender_node][
            ["bin_start_us", "throughput_Gbps"]
        ].rename(columns={"throughput_Gbps": f"flow_{sender_node}_Gbps"})
        merged = merged.merge(sender_df, on="bin_start_us", how="left")
    merged = merged.fillna(0)
    merged["sum_Gbps"] = sum(merged[f"flow_{sender_node}_Gbps"] for sender_node in sender_nodes)

    csv_path = ANALYSIS_DIR / "queue_pfc_dual_axis_1ms.csv"
    merged.to_csv(csv_path, index=False)

    pfc_csv_path = ANALYSIS_DIR / "pfc_events.csv"
    pfc_df.to_csv(pfc_csv_path, index=False)

    plt.style.use("seaborn-v0_8-whitegrid")
    fig, ax_left = plt.subplots(figsize=(13.5, 6.8), constrained_layout=True)
    ax_right = ax_left.twinx()

    colors = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd"]
    for idx, sender_node in enumerate(sender_nodes):
        sender_df = throughput_all[throughput_all["sender_node"] == sender_node]
        ax_left.plot(
            sender_df["time_ms"],
            sender_df["throughput_Gbps"],
            linewidth=1.7,
            color=colors[idx % len(colors)],
            label=f"Flow from node {sender_node}",
        )

    ax_right.step(
        queue_df["time_ms"],
        queue_df["total_KB"],
        where="post",
        color="#111111",
        linewidth=1.6,
        label="Switch queue total",
    )

    pause_label_drawn = False
    resume_label_drawn = False
    for _, event in pfc_df.iterrows():
        color = "#ff7f0e" if event["action"] == "PAUSE" else "#7f7f7f"
        linestyle = "--" if event["action"] == "PAUSE" else ":"
        alpha = 0.55 if event["action"] == "PAUSE" else 0.30
        label = None
        if event["action"] == "PAUSE" and not pause_label_drawn:
            label = "PFC pause"
            pause_label_drawn = True
        elif event["action"] == "RESUME" and not resume_label_drawn:
            label = "PFC resume"
            resume_label_drawn = True
        ax_left.axvline(event["time_ms"], color=color, linestyle=linestyle, linewidth=0.9, alpha=alpha, label=label)

    ax_left.set_xlim(0.0, STOP_MS)
    ax_left.set_ylim(0.0, THROUGHPUT_YMAX_GBPS)
    ax_right.set_ylim(0.0, QUEUE_YMAX_KB)
    ax_left.set_xlabel("Time (ms)")
    ax_left.set_ylabel("Per-flow throughput (Gbps)")
    ax_right.set_ylabel("Bottleneck switch queue (KB)")
    ax_left.set_title(
        f"DCQCN paper fig10 reproduction, 1 ms bins, bottleneck queue node {bottleneck_node} port {bottleneck_port}"
    )

    left_handles, left_labels = ax_left.get_legend_handles_labels()
    right_handles, right_labels = ax_right.get_legend_handles_labels()
    ax_left.legend(left_handles + right_handles, left_labels + right_labels, loc="upper right", ncol=2, frameon=True)

    png_path = ANALYSIS_DIR / "queue_pfc_dual_axis_1ms.png"
    fig.savefig(png_path, dpi=220)
    plt.close(fig)

    print(f"Wrote {png_path}")
    print(f"Wrote {csv_path}")
    print(f"Wrote {pfc_csv_path}")


if __name__ == "__main__":
    main()
