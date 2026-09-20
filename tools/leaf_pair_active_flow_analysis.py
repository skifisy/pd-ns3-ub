#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Build exact directed Leaf-pair task-concurrency intervals from Jupiter WCMP traces.

Inputs:
  CASE_DIR/output/task_statistics.csv
  CASE_DIR/traffic.csv
  CASE_DIR/jupiter_te/WcmpSelectionTrace.csv

The WCMP trace records actual payload direction.  Therefore:
  URMA_WRITE: task source Leaf      -> task destination Leaf
  URMA_READ : task destination Leaf -> task source Leaf

Outputs (default CASE_DIR/output/leaf_pair_active_flow_analysis/):
  task_leaf_mapping.csv
  unresolved_tasks.csv
  same_leaf_tasks.csv
  leaf_pair_active_flow_events.csv
  leaf_pair_active_flow_intervals.csv
  leaf_pair_summary.csv
"""

import argparse
import ipaddress
from pathlib import Path

import numpy as np
import pandas as pd


DEFAULT_TRACE_REL = Path("jupiter_te/WcmpSelectionTrace.csv")
DEFAULT_OUTPUT_REL = Path("output/leaf_pair_active_flow_analysis")


def parse_node_set(spec):
    out = set()
    for token in str(spec).split(","):
        token = token.strip()
        if not token:
            continue
        if ".." in token:
            a, b = token.split("..", 1)
            a, b = int(a), int(b)
            out.update(range(min(a, b), max(a, b) + 1))
        elif "-" in token:
            a, b = token.split("-", 1)
            a, b = int(a), int(b)
            out.update(range(min(a, b), max(a, b) + 1))
        else:
            out.add(int(token))
    return out


def read_required_csv(path, required):
    if not path.exists():
        raise SystemExit(f"Missing input CSV:\n{path}")
    df = pd.read_csv(path)
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise RuntimeError(
            f"Missing required columns in {path}\n"
            f"Missing: {missing}\nExisting: {list(df.columns)}"
        )
    return df


def ip_to_int(value):
    if pd.isna(value):
        return np.nan
    text = str(value).strip()
    try:
        if "." in text:
            return int(ipaddress.IPv4Address(text))
        return int(float(text))
    except (ValueError, ipaddress.AddressValueError):
        return np.nan


def ip_to_node(ip_value):
    value = int(ip_value)
    base = int(ipaddress.IPv4Address("10.0.0.0"))
    if value < base:
        return np.nan
    return (value - base) >> 8


def load_tasks(task_file, traffic_file):
    required = [
        "taskId",
        "sourceNode",
        "destNode",
        "taskStartTime(us)",
        "taskCompletesTime(us)",
    ]
    tasks = read_required_csv(task_file, required)
    for column in required:
        tasks[column] = pd.to_numeric(tasks[column], errors="coerce")
    tasks = tasks.dropna(subset=required).copy()
    for column in ["taskId", "sourceNode", "destNode"]:
        tasks[column] = tasks[column].astype(np.int64)

    traffic = read_required_csv(traffic_file, ["taskId", "opType"])[
        ["taskId", "opType"]
    ].copy()
    traffic["taskId"] = pd.to_numeric(traffic["taskId"], errors="coerce")
    traffic = traffic.dropna(subset=["taskId"]).copy()
    traffic["taskId"] = traffic["taskId"].astype(np.int64)
    traffic = traffic.drop_duplicates("taskId", keep="first")
    if "opType" in tasks.columns:
        tasks = tasks.drop(columns="opType")
    tasks = tasks.merge(traffic, on="taskId", how="left", validate="one_to_one")

    if "opType" not in tasks.columns or tasks["opType"].isna().any():
        missing = int(tasks["opType"].isna().sum()) if "opType" in tasks.columns else len(tasks)
        raise RuntimeError(f"{missing} completed tasks have no opType")
    tasks["opType"] = tasks["opType"].astype(str).str.strip().str.upper()

    duplicate_ids = tasks["taskId"].duplicated(keep=False)
    if duplicate_ids.any():
        examples = tasks.loc[duplicate_ids, "taskId"].head(10).tolist()
        raise RuntimeError(f"task_statistics.csv contains duplicate taskId values: {examples}")
    return tasks


def load_wcmp_trace(path):
    required = [
        "sim_time_seconds",
        "task_id",
        "epoch",
        "flow_hash",
        "sip",
        "dip",
        "src_leaf",
        "dst_leaf",
        "transit_leaf",
        "next_leaf",
        "out_port",
        "decision_source",
    ]
    trace = read_required_csv(path, required)
    numeric = [
        "sim_time_seconds",
        "task_id",
        "epoch",
        "src_leaf",
        "dst_leaf",
        "transit_leaf",
        "next_leaf",
        "out_port",
    ]
    for column in numeric:
        trace[column] = pd.to_numeric(trace[column], errors="coerce")
    trace["sip_int"] = trace["sip"].map(ip_to_int)
    trace["dip_int"] = trace["dip"].map(ip_to_int)
    trace = trace.dropna(
        subset=numeric + ["sip_int", "dip_int", "decision_source"]
    ).copy()
    for column in [
        "task_id",
        "epoch",
        "src_leaf",
        "dst_leaf",
        "transit_leaf",
        "next_leaf",
        "out_port",
        "sip_int",
        "dip_int",
    ]:
        trace[column] = trace[column].astype(np.int64)
    trace["trace_source_node"] = trace["sip_int"].map(ip_to_node)
    trace["trace_destination_node"] = trace["dip_int"].map(ip_to_node)
    invalid_nodes = trace[
        trace[["trace_source_node", "trace_destination_node"]].isna().any(axis=1)
    ]
    if not invalid_nodes.empty:
        raise RuntimeError(
            f"{len(invalid_nodes)} WCMP trace rows contain IPs outside 10.0.0.0/8"
        )
    trace["trace_source_node"] = trace["trace_source_node"].astype(np.int64)
    trace["trace_destination_node"] = trace["trace_destination_node"].astype(np.int64)
    return trace.sort_values(["task_id", "sim_time_seconds", "flow_hash"])


def resolve_task_mapping(tasks, trace, leaf_nodes):
    leaf_nodes = set(leaf_nodes)
    pairs = trace[
        [
            "task_id",
            "src_leaf",
            "dst_leaf",
            "trace_source_node",
            "trace_destination_node",
        ]
    ].drop_duplicates()

    pair_counts = pairs.groupby("task_id", sort=False).size()
    ambiguous_ids = set(pair_counts[pair_counts > 1].index.astype(int))
    first_trace = trace.drop_duplicates("task_id", keep="first")
    mapping = tasks.merge(
        first_trace,
        left_on="taskId",
        right_on="task_id",
        how="left",
        validate="one_to_one",
        indicator=True,
    )

    reasons = pd.Series("", index=mapping.index, dtype="object")
    reasons.loc[mapping["_merge"].eq("left_only")] = "no_wcmp_selection_trace"
    reasons.loc[mapping["taskId"].isin(ambiguous_ids)] = "multiple_leaf_pairs_for_task"

    traced = mapping["_merge"].eq("both")
    expected_source = np.where(
        mapping["opType"].eq("URMA_READ"),
        mapping["destNode"],
        mapping["sourceNode"],
    )
    expected_destination = np.where(
        mapping["opType"].eq("URMA_READ"),
        mapping["sourceNode"],
        mapping["destNode"],
    )
    direction_mismatch = traced & (
        mapping["trace_source_node"].ne(expected_source)
        | mapping["trace_destination_node"].ne(expected_destination)
    )
    reasons.loc[direction_mismatch & reasons.eq("")] = "payload_direction_mismatch"

    invalid_leaf = traced & (
        ~mapping["src_leaf"].isin(leaf_nodes)
        | ~mapping["dst_leaf"].isin(leaf_nodes)
    )
    reasons.loc[invalid_leaf & reasons.eq("")] = "leaf_outside_configured_set"

    mapping["status"] = np.where(reasons.eq(""), "resolved", "unresolved")
    mapping["reason"] = reasons
    mapping["trace_rows_for_task"] = mapping["taskId"].map(
        trace.groupby("task_id").size()
    ).fillna(0).astype(np.int64)

    unresolved = mapping[mapping["status"].eq("unresolved")].copy()
    resolved = mapping[mapping["status"].eq("resolved")].copy()
    resolved["leaf_a"] = resolved["src_leaf"].astype(np.int32)
    resolved["leaf_b"] = resolved["dst_leaf"].astype(np.int32)
    resolved["same_leaf"] = resolved["leaf_a"].eq(resolved["leaf_b"])

    return resolved, unresolved


def build_events(mapping):
    columns = ["time_us", "leaf_a", "leaf_b", "delta", "starts", "ends", "active_flows"]
    if mapping.empty:
        return pd.DataFrame(columns=columns)

    base = mapping[
        ["taskStartTime(us)", "taskCompletesTime(us)", "leaf_a", "leaf_b"]
    ].copy()
    base = base[base["taskCompletesTime(us)"] > base["taskStartTime(us)"]]

    starts = base[["taskStartTime(us)", "leaf_a", "leaf_b"]].rename(
        columns={"taskStartTime(us)": "time_us"}
    )
    starts["delta"] = 1
    starts["starts"] = 1
    starts["ends"] = 0

    ends = base[["taskCompletesTime(us)", "leaf_a", "leaf_b"]].rename(
        columns={"taskCompletesTime(us)": "time_us"}
    )
    ends["delta"] = -1
    ends["starts"] = 0
    ends["ends"] = 1

    events = pd.concat([starts, ends], ignore_index=True)
    events = (
        events.groupby(["leaf_a", "leaf_b", "time_us"], as_index=False)
        .agg(delta=("delta", "sum"), starts=("starts", "sum"), ends=("ends", "sum"))
        .sort_values(["leaf_a", "leaf_b", "time_us"])
        .reset_index(drop=True)
    )
    events["active_flows"] = events.groupby(
        ["leaf_a", "leaf_b"], sort=False
    )["delta"].cumsum()
    return events[columns]


def build_intervals(events):
    columns = [
        "leaf_a",
        "leaf_b",
        "interval_start_us",
        "interval_end_us",
        "active_flows",
        "duration_us",
    ]
    if events.empty:
        return pd.DataFrame(columns=columns)

    intervals = events[["leaf_a", "leaf_b", "time_us", "active_flows"]].copy()
    intervals["interval_end_us"] = intervals.groupby(
        ["leaf_a", "leaf_b"], sort=False
    )["time_us"].shift(-1)
    intervals = intervals.rename(columns={"time_us": "interval_start_us"})
    intervals = intervals.dropna(subset=["interval_end_us"])
    intervals = intervals[
        (intervals["active_flows"] > 0)
        & (intervals["interval_end_us"] > intervals["interval_start_us"])
    ].copy()
    intervals["duration_us"] = (
        intervals["interval_end_us"] - intervals["interval_start_us"]
    )
    return intervals[columns].reset_index(drop=True)


def build_summary(mapping, intervals):
    base = (
        mapping.groupby(["leaf_a", "leaf_b"], as_index=False)
        .agg(total_tasks=("taskId", "nunique"))
    )
    if intervals.empty:
        base["max_active_flows"] = 0
        base["time_weighted_avg_active_flows"] = 0.0
        return base

    aggregate = (
        intervals.assign(
            weighted=lambda frame: frame["active_flows"] * frame["duration_us"]
        )
        .groupby(["leaf_a", "leaf_b"], as_index=False)
        .agg(
            max_active_flows=("active_flows", "max"),
            weighted_active_flow_us=("weighted", "sum"),
            active_span_start_us=("interval_start_us", "min"),
            active_span_end_us=("interval_end_us", "max"),
        )
    )
    span = aggregate["active_span_end_us"] - aggregate["active_span_start_us"]
    aggregate["time_weighted_avg_active_flows"] = np.where(
        span > 0,
        aggregate["weighted_active_flow_us"] / span,
        0.0,
    )
    summary = base.merge(
        aggregate[
            [
                "leaf_a",
                "leaf_b",
                "max_active_flows",
                "time_weighted_avg_active_flows",
            ]
        ],
        on=["leaf_a", "leaf_b"],
        how="left",
    )
    return summary.sort_values(
        ["max_active_flows", "total_tasks"], ascending=[False, False]
    ).reset_index(drop=True)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build exact directed Leaf-pair active-task intervals from WCMP trace."
    )
    parser.add_argument("case_dir", help="ns-3 case directory")
    parser.add_argument("--wcmp-trace", default=None)
    parser.add_argument("--leaf-nodes", default="37-56")
    parser.add_argument("--include-same-leaf", action="store_true")
    parser.add_argument("--output-dir", default=None)
    return parser.parse_args()


def main():
    args = parse_args()
    case = Path(args.case_dir).resolve()
    trace_file = (
        Path(args.wcmp_trace).resolve()
        if args.wcmp_trace
        else case / DEFAULT_TRACE_REL
    )
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir
        else case / DEFAULT_OUTPUT_REL
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    leaf_nodes = parse_node_set(args.leaf_nodes)
    if not leaf_nodes:
        raise RuntimeError("No Leaf nodes specified")

    tasks = load_tasks(case / "output/task_statistics.csv", case / "traffic.csv")
    trace = load_wcmp_trace(trace_file)
    mapping, unresolved = resolve_task_mapping(tasks, trace, leaf_nodes)

    same_leaf = mapping[mapping["same_leaf"]].copy()
    used = mapping[
        (~mapping["same_leaf"]) | bool(args.include_same_leaf)
    ].copy()

    mapping.to_csv(output_dir / "task_leaf_mapping.csv", index=False)
    unresolved.to_csv(output_dir / "unresolved_tasks.csv", index=False)
    same_leaf.to_csv(output_dir / "same_leaf_tasks.csv", index=False)

    events = build_events(used)
    events.to_csv(output_dir / "leaf_pair_active_flow_events.csv", index=False)
    intervals = build_intervals(events)
    intervals.to_csv(output_dir / "leaf_pair_active_flow_intervals.csv", index=False)
    summary = build_summary(used, intervals)
    summary.to_csv(output_dir / "leaf_pair_summary.csv", index=False)

    total = len(tasks)
    print("=" * 88)
    print("Leaf-pair active-flow analysis (Jupiter WCMP)")
    print("=" * 88)
    print(f"Completed task rows              : {total:,}")
    print(f"Resolved task-to-Leaf mappings   : {len(mapping):,} "
          f"({100.0 * len(mapping) / max(total, 1):.2f}%)")
    print(f"Unresolved tasks                 : {len(unresolved):,}")
    print(f"Resolved same-Leaf tasks         : {len(same_leaf):,}")
    print(f"Tasks used in pair timeline      : {len(used):,}")
    print(f"WCMP trace rows                  : {len(trace):,}")
    print(f"Output                           : {output_dir}")
    if not unresolved.empty:
        print("Unresolved reasons:")
        print(unresolved["reason"].value_counts().to_string())
    print("Top Leaf-pair concurrency:")
    print(summary.head(20).to_string(index=False))
    print("=" * 88)


if __name__ == "__main__":
    main()
