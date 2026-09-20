#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Compare task-level flow completion time for direct ECMP and MLU-WCMP.

The script intentionally consumes only artifacts that already exist after a
normal ns-3 trace parse.  It does not read WCMP decision traces and does not
require another simulation run.

Inputs discovered below each case directory:
  traffic.csv
  output/task_statistics.csv

Default output:
  WCMP_CASE/output/fct_comparison_vs_ecmp/
"""

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


TRAFFIC_REL = Path("traffic.csv")
TASK_STATISTICS_REL = Path("output/task_statistics.csv")
DEFAULT_OUTPUT_REL = Path("output/fct_comparison_vs_ecmp")

TRAFFIC_REQUIRED = ["taskId", "dataSize(Byte)", "opType"]
STATISTICS_REQUIRED = [
    "taskId",
    "taskStartTime(us)",
    "taskCompletesTime(us)",
]
OPTIONAL_TRAFFIC_COLUMNS = [
    "sourceNode",
    "sourceNodeId",
    "destNode",
    "destNodeId",
    "priority",
    "delay",
    "phaseId",
]


def read_required_csv(path, required):
    if not path.is_file():
        raise SystemExit(f"Missing input CSV:\n{path}")
    frame = pd.read_csv(path, low_memory=False)
    missing = [column for column in required if column not in frame.columns]
    if missing:
        raise RuntimeError(
            f"Missing required columns in {path}\n"
            f"Missing: {missing}\nExisting: {list(frame.columns)}"
        )
    return frame


def normalize_task_ids(frame, path):
    values = pd.to_numeric(frame["taskId"], errors="coerce")
    invalid = values.isna()
    if invalid.any():
        raise RuntimeError(f"{int(invalid.sum())} rows have invalid taskId in {path}")
    frame = frame.copy()
    frame["taskId"] = values.astype(np.int64)
    duplicates = frame["taskId"].duplicated(keep=False)
    if duplicates.any():
        examples = frame.loc[duplicates, "taskId"].head(10).tolist()
        raise RuntimeError(f"Duplicate taskId values in {path}: {examples}")
    return frame


def load_traffic(path):
    traffic = normalize_task_ids(read_required_csv(path, TRAFFIC_REQUIRED), path)
    traffic["dataSize(Byte)"] = pd.to_numeric(
        traffic["dataSize(Byte)"], errors="coerce"
    )
    invalid_size = traffic["dataSize(Byte)"].isna() | traffic["dataSize(Byte)"].lt(0)
    if invalid_size.any():
        raise RuntimeError(
            f"{int(invalid_size.sum())} rows have invalid dataSize(Byte) in {path}"
        )
    traffic["opType"] = traffic["opType"].astype(str).str.strip().str.upper()
    keep = TRAFFIC_REQUIRED + [
        column for column in OPTIONAL_TRAFFIC_COLUMNS if column in traffic.columns
    ]
    return traffic[keep].sort_values("taskId").reset_index(drop=True)


def compare_traffic(ecmp, wcmp):
    ecmp_ids = ecmp["taskId"].to_numpy(dtype=np.int64)
    wcmp_ids = wcmp["taskId"].to_numpy(dtype=np.int64)
    same_task_ids = len(ecmp_ids) == len(wcmp_ids) and np.array_equal(
        ecmp_ids, wcmp_ids
    )
    common = np.intersect1d(ecmp_ids, wcmp_ids, assume_unique=True)
    report = {
        "ecmp_rows": int(len(ecmp)),
        "wcmp_rows": int(len(wcmp)),
        "same_task_id_set": bool(same_task_ids),
        "ecmp_only_task_ids": int(len(ecmp_ids) - len(common)),
        "wcmp_only_task_ids": int(len(wcmp_ids) - len(common)),
        "common_task_ids": int(len(common)),
        "core_metadata_mismatches": 0,
    }
    if len(common):
        columns = ["taskId", "dataSize(Byte)", "opType"]
        left = ecmp[ecmp["taskId"].isin(common)][columns]
        right = wcmp[wcmp["taskId"].isin(common)][columns]
        merged = left.merge(
            right,
            on="taskId",
            how="inner",
            suffixes=("_ecmp", "_wcmp"),
            validate="one_to_one",
        )
        mismatch = (
            merged["dataSize(Byte)_ecmp"].ne(merged["dataSize(Byte)_wcmp"])
            | merged["opType_ecmp"].ne(merged["opType_wcmp"])
        )
        report["core_metadata_mismatches"] = int(mismatch.sum())
    report["lightweight_match"] = bool(
        report["same_task_id_set"] and report["core_metadata_mismatches"] == 0
    )
    return report


def load_task_statistics(path, scenario):
    statistics = normalize_task_ids(
        read_required_csv(path, STATISTICS_REQUIRED), path
    )
    start = pd.to_numeric(statistics["taskStartTime(us)"], errors="coerce")
    complete = pd.to_numeric(statistics["taskCompletesTime(us)"], errors="coerce")
    output = pd.DataFrame(
        {
            "taskId": statistics["taskId"],
            f"{scenario}_statistics_present": True,
            f"{scenario}_task_start_us": start,
            f"{scenario}_task_complete_us": complete,
        }
    )
    valid = (
        start.notna()
        & complete.notna()
        & np.isfinite(start)
        & np.isfinite(complete)
        & start.ge(0)
        & complete.ge(start)
    )
    output[f"{scenario}_completed"] = valid
    output[f"{scenario}_fct_us"] = (complete - start).where(valid)

    packet_columns = ["firstPacketSends(us)", "lastPacketACKs(us)"]
    if all(column in statistics.columns for column in packet_columns):
        first = pd.to_numeric(statistics[packet_columns[0]], errors="coerce")
        last = pd.to_numeric(statistics[packet_columns[1]], errors="coerce")
        wire_valid = (
            first.notna()
            & last.notna()
            & np.isfinite(first)
            & np.isfinite(last)
            & first.ge(0)
            & last.ge(first)
        )
        output[f"{scenario}_first_packet_us"] = first
        output[f"{scenario}_last_packet_ack_us"] = last
        output[f"{scenario}_wire_fct_us"] = (last - first).where(wire_valid)
    if "taskThroughput(Gbps)" in statistics.columns:
        output[f"{scenario}_reported_throughput_gbps"] = pd.to_numeric(
            statistics["taskThroughput(Gbps)"], errors="coerce"
        )
    return output


def assign_size_classes(frame, reference_sizes):
    valid = pd.to_numeric(reference_sizes, errors="coerce").dropna()
    unique = np.sort(valid.unique())
    metadata = {
        "method": "traffic_quantiles",
        "reference_tasks": int(len(valid)),
        "unique_sizes": int(len(unique)),
        "p50_bytes": None,
        "p90_bytes": None,
    }
    result = pd.Series("unknown", index=frame.index, dtype="object")
    size = pd.to_numeric(frame["dataSize(Byte)"], errors="coerce")
    if len(unique) == 0:
        return result, metadata
    if len(unique) == 1:
        result.loc[size.notna()] = "single_size"
        metadata["method"] = "single_size"
        metadata["p50_bytes"] = float(unique[0])
        metadata["p90_bytes"] = float(unique[0])
        return result, metadata

    p50 = float(valid.quantile(0.50))
    p90 = float(valid.quantile(0.90))
    metadata["p50_bytes"] = p50
    metadata["p90_bytes"] = p90
    result.loc[size.le(p50)] = "small_le_p50"
    if p90 > p50:
        result.loc[size.gt(p50) & size.lt(p90)] = "medium_p50_to_p90"
        result.loc[size.ge(p90)] = "large_ge_p90"
    else:
        result.loc[size.gt(p50)] = "large_gt_p50"
        metadata["method"] = "collapsed_quantiles"
    return result, metadata


def build_task_table(ecmp_traffic, wcmp_traffic, ecmp_stats, wcmp_stats):
    task_ids = np.unique(
        np.concatenate(
            (
                ecmp_traffic["taskId"].to_numpy(dtype=np.int64),
                wcmp_traffic["taskId"].to_numpy(dtype=np.int64),
                ecmp_stats["taskId"].to_numpy(dtype=np.int64),
                wcmp_stats["taskId"].to_numpy(dtype=np.int64),
            )
        )
    )
    tasks = pd.DataFrame({"taskId": task_ids})
    tasks = tasks.merge(ecmp_traffic, on="taskId", how="left", validate="one_to_one")

    tasks["in_ecmp_traffic"] = tasks["taskId"].isin(ecmp_traffic["taskId"])
    tasks["in_wcmp_traffic"] = tasks["taskId"].isin(wcmp_traffic["taskId"])
    tasks = tasks.merge(ecmp_stats, on="taskId", how="left", validate="one_to_one")
    tasks = tasks.merge(wcmp_stats, on="taskId", how="left", validate="one_to_one")

    for scenario in ("ecmp", "wcmp"):
        tasks[f"{scenario}_statistics_present"] = tasks[
            f"{scenario}_statistics_present"
        ].fillna(False).astype(bool)
        tasks[f"{scenario}_completed"] = tasks[f"{scenario}_completed"].fillna(
            False
        ).astype(bool)

    both = tasks["ecmp_completed"] & tasks["wcmp_completed"]
    ecmp_only = tasks["ecmp_completed"] & ~tasks["wcmp_completed"]
    wcmp_only = ~tasks["ecmp_completed"] & tasks["wcmp_completed"]
    tasks["completion_status"] = np.select(
        [both, ecmp_only, wcmp_only],
        ["both_completed", "ecmp_only_completed", "wcmp_only_completed"],
        default="neither_completed",
    )
    tasks["fct_delta_wcmp_minus_ecmp_us"] = (
        tasks["wcmp_fct_us"] - tasks["ecmp_fct_us"]
    ).where(both)
    tasks["fct_saved_by_wcmp_us"] = (
        tasks["ecmp_fct_us"] - tasks["wcmp_fct_us"]
    ).where(both)
    valid_denominator = both & tasks["ecmp_fct_us"].gt(0)
    tasks["wcmp_improvement_pct"] = np.where(
        valid_denominator,
        tasks["fct_saved_by_wcmp_us"] / tasks["ecmp_fct_us"] * 100.0,
        np.nan,
    )
    valid_ratio = both & tasks["wcmp_fct_us"].gt(0)
    tasks["ecmp_over_wcmp_speedup"] = np.where(
        valid_ratio,
        tasks["ecmp_fct_us"] / tasks["wcmp_fct_us"],
        np.nan,
    )
    tasks["size_class"], size_metadata = assign_size_classes(
        tasks, ecmp_traffic["dataSize(Byte)"]
    )
    tasks["opType"] = tasks["opType"].fillna("UNKNOWN").astype(str)
    return tasks.sort_values("taskId").reset_index(drop=True), size_metadata


def distribution_metrics(values):
    values = pd.to_numeric(values, errors="coerce").dropna().to_numpy(dtype=float)
    if len(values) == 0:
        return {
            "mean_fct_us": np.nan,
            "std_fct_us": np.nan,
            "min_fct_us": np.nan,
            "p50_fct_us": np.nan,
            "p90_fct_us": np.nan,
            "p95_fct_us": np.nan,
            "p99_fct_us": np.nan,
            "max_fct_us": np.nan,
            "p99_over_p50": np.nan,
        }
    quantiles = np.quantile(values, [0.50, 0.90, 0.95, 0.99])
    p50 = float(quantiles[0])
    p99 = float(quantiles[3])
    return {
        "mean_fct_us": float(np.mean(values)),
        "std_fct_us": float(np.std(values, ddof=1)) if len(values) > 1 else 0.0,
        "min_fct_us": float(np.min(values)),
        "p50_fct_us": p50,
        "p90_fct_us": float(quantiles[1]),
        "p95_fct_us": float(quantiles[2]),
        "p99_fct_us": p99,
        "max_fct_us": float(np.max(values)),
        "p99_over_p50": p99 / p50 if p50 > 0 else np.nan,
    }


def iter_groups(frame):
    yield "overall", "all", frame
    for value, group in frame.groupby("opType", dropna=False, sort=True):
        yield "op_type", str(value), group
    for value, group in frame.groupby("size_class", dropna=False, sort=True):
        yield "size_class", str(value), group
    for keys, group in frame.groupby(
        ["opType", "size_class"], dropna=False, sort=True
    ):
        yield "op_type_x_size_class", f"{keys[0]}|{keys[1]}", group


def build_fct_summary(tasks):
    rows = []
    paired = tasks[tasks["completion_status"].eq("both_completed")]
    for scenario in ("ecmp", "wcmp"):
        completed_column = f"{scenario}_completed"
        fct_column = f"{scenario}_fct_us"
        for scope, source in (("all_expected", tasks), ("paired_common", paired)):
            for group_type, group_value, group in iter_groups(source):
                completed = group[group[completed_column]]
                row = {
                    "scope": scope,
                    "scenario": scenario,
                    "group_type": group_type,
                    "group_value": group_value,
                    "expected_tasks": int(len(group)),
                    "completed_tasks": int(len(completed)),
                    "completion_rate_pct": (
                        100.0 * len(completed) / len(group) if len(group) else np.nan
                    ),
                }
                row.update(distribution_metrics(completed[fct_column]))
                rows.append(row)
    return pd.DataFrame(rows)


def percent_change(new, old):
    return (new - old) / old * 100.0 if pd.notna(old) and old != 0 else np.nan


def build_paired_summary(tasks):
    paired = tasks[tasks["completion_status"].eq("both_completed")].copy()
    rows = []
    for group_type, group_value, group in iter_groups(paired):
        ecmp = distribution_metrics(group["ecmp_fct_us"])
        wcmp = distribution_metrics(group["wcmp_fct_us"])
        delta = group["fct_delta_wcmp_minus_ecmp_us"]
        improvement = group["wcmp_improvement_pct"].dropna()
        speedup = group["ecmp_over_wcmp_speedup"].dropna()
        improved = delta.lt(0)
        worsened = delta.gt(0)
        unchanged = delta.eq(0)
        row = {
            "group_type": group_type,
            "group_value": group_value,
            "paired_tasks": int(len(group)),
            "ecmp_mean_fct_us": ecmp["mean_fct_us"],
            "wcmp_mean_fct_us": wcmp["mean_fct_us"],
            "mean_fct_change_pct": percent_change(
                wcmp["mean_fct_us"], ecmp["mean_fct_us"]
            ),
            "ecmp_p50_fct_us": ecmp["p50_fct_us"],
            "wcmp_p50_fct_us": wcmp["p50_fct_us"],
            "p50_fct_change_pct": percent_change(
                wcmp["p50_fct_us"], ecmp["p50_fct_us"]
            ),
            "ecmp_p90_fct_us": ecmp["p90_fct_us"],
            "wcmp_p90_fct_us": wcmp["p90_fct_us"],
            "p90_fct_change_pct": percent_change(
                wcmp["p90_fct_us"], ecmp["p90_fct_us"]
            ),
            "ecmp_p95_fct_us": ecmp["p95_fct_us"],
            "wcmp_p95_fct_us": wcmp["p95_fct_us"],
            "p95_fct_change_pct": percent_change(
                wcmp["p95_fct_us"], ecmp["p95_fct_us"]
            ),
            "ecmp_p99_fct_us": ecmp["p99_fct_us"],
            "wcmp_p99_fct_us": wcmp["p99_fct_us"],
            "p99_fct_change_pct": percent_change(
                wcmp["p99_fct_us"], ecmp["p99_fct_us"]
            ),
            "mean_paired_delta_us": float(delta.mean()) if len(group) else np.nan,
            "median_paired_delta_us": float(delta.median()) if len(group) else np.nan,
            "mean_task_improvement_pct": (
                float(improvement.mean()) if len(improvement) else np.nan
            ),
            "median_task_improvement_pct": (
                float(improvement.median()) if len(improvement) else np.nan
            ),
            "geomean_ecmp_over_wcmp_speedup": (
                float(np.exp(np.log(speedup).mean()))
                if len(speedup) and speedup.gt(0).all()
                else np.nan
            ),
            "improved_tasks": int(improved.sum()),
            "worsened_tasks": int(worsened.sum()),
            "unchanged_tasks": int(unchanged.sum()),
            "improved_tasks_pct": (
                100.0 * improved.mean() if len(group) else np.nan
            ),
            "worsened_tasks_pct": (
                100.0 * worsened.mean() if len(group) else np.nan
            ),
        }
        rows.append(row)
    return pd.DataFrame(rows)


def build_window_summary(tasks, bin_s):
    if bin_s <= 0:
        raise ValueError("--bin-s must be greater than zero")
    bin_us = float(bin_s) * 1e6
    rows = []
    for scenario in ("ecmp", "wcmp"):
        start_column = f"{scenario}_task_start_us"
        fct_column = f"{scenario}_fct_us"
        subset = tasks[tasks[f"{scenario}_completed"]].copy()
        subset = subset[subset[start_column].notna()]
        if subset.empty:
            continue
        subset["window_id"] = np.floor(subset[start_column] / bin_us).astype(np.int64)
        for window_id, group in subset.groupby("window_id", sort=True):
            row = {
                "scenario": scenario,
                "window_id": int(window_id),
                "window_start_s": float(window_id * bin_s),
                "window_end_s": float((window_id + 1) * bin_s),
                "completed_tasks": int(len(group)),
            }
            row.update(distribution_metrics(group[fct_column]))
            rows.append(row)
    return pd.DataFrame(rows)


def save_empty_plot(path, title, message, dpi):
    fig, axis = plt.subplots(figsize=(8, 5))
    axis.axis("off")
    axis.set_title(title)
    axis.text(0.5, 0.5, message, ha="center", va="center")
    fig.tight_layout()
    fig.savefig(path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def plot_cdf_and_ccdf(tasks, output_dir, dpi):
    paired = tasks[tasks["completion_status"].eq("both_completed")]
    if paired.empty:
        for name, title in (("fct_cdf.png", "FCT CDF"), ("fct_ccdf.png", "FCT CCDF")):
            save_empty_plot(output_dir / name, title, "No paired completed tasks", dpi)
        return

    for ccdf, filename in ((False, "fct_cdf.png"), (True, "fct_ccdf.png")):
        fig, axis = plt.subplots(figsize=(8.5, 5.5))
        for scenario, label in (("ecmp", "Direct ECMP"), ("wcmp", "MLU-WCMP")):
            values = np.sort(paired[f"{scenario}_fct_us"].to_numpy(dtype=float) / 1e3)
            if ccdf:
                probability = (len(values) - np.arange(len(values))) / len(values)
            else:
                probability = np.arange(1, len(values) + 1) / len(values)
            axis.plot(values, probability, label=label, linewidth=1.8)
        axis.set_xlabel("Task FCT (ms)")
        axis.set_ylabel("CCDF: P(FCT >= x)" if ccdf else "CDF")
        axis.set_title("Paired task-level FCT " + ("CCDF" if ccdf else "CDF"))
        if ccdf:
            axis.set_yscale("log")
        axis.grid(True, alpha=0.3)
        axis.legend()
        fig.tight_layout()
        fig.savefig(output_dir / filename, dpi=dpi, bbox_inches="tight")
        plt.close(fig)


def plot_percentiles(paired_summary, output_dir, dpi):
    overall = paired_summary[
        paired_summary["group_type"].eq("overall")
    ]
    path = output_dir / "fct_p50_p99_comparison.png"
    if overall.empty or int(overall.iloc[0]["paired_tasks"]) == 0:
        save_empty_plot(path, "FCT summary", "No paired completed tasks", dpi)
        return
    row = overall.iloc[0]
    names = ["Mean", "P50", "P95", "P99"]
    ecmp = np.asarray(
        [
            row["ecmp_mean_fct_us"],
            row["ecmp_p50_fct_us"],
            row["ecmp_p95_fct_us"],
            row["ecmp_p99_fct_us"],
        ]
    ) / 1e3
    wcmp = np.asarray(
        [
            row["wcmp_mean_fct_us"],
            row["wcmp_p50_fct_us"],
            row["wcmp_p95_fct_us"],
            row["wcmp_p99_fct_us"],
        ]
    ) / 1e3
    x = np.arange(len(names))
    width = 0.36
    fig, axis = plt.subplots(figsize=(8.5, 5.5))
    axis.bar(x - width / 2, ecmp, width, label="Direct ECMP")
    axis.bar(x + width / 2, wcmp, width, label="MLU-WCMP")
    axis.set_xticks(x, names)
    axis.set_ylabel("Task FCT (ms)")
    axis.set_title("Paired task-level FCT summary")
    axis.grid(axis="y", alpha=0.3)
    axis.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def plot_by_size(paired_summary, output_dir, dpi):
    grouped = paired_summary[
        paired_summary["group_type"].eq("size_class")
    ].copy()
    path = output_dir / "fct_by_size_comparison.png"
    if grouped.empty:
        save_empty_plot(path, "FCT by size class", "No paired completed tasks", dpi)
        return
    labels = grouped["group_value"].tolist()
    x = np.arange(len(labels))
    width = 0.36
    fig, axes = plt.subplots(1, 2, figsize=(12, 5.2), sharex=True)
    for axis, metric, title in (
        (axes[0], "p50", "P50"),
        (axes[1], "p99", "P99"),
    ):
        ecmp = grouped[f"ecmp_{metric}_fct_us"].to_numpy(dtype=float) / 1e3
        wcmp = grouped[f"wcmp_{metric}_fct_us"].to_numpy(dtype=float) / 1e3
        axis.bar(x - width / 2, ecmp, width, label="Direct ECMP")
        axis.bar(x + width / 2, wcmp, width, label="MLU-WCMP")
        axis.set_title(title)
        axis.set_ylabel("Task FCT (ms)")
        axis.set_xticks(x, labels, rotation=25, ha="right")
        axis.grid(axis="y", alpha=0.3)
    axes[0].legend()
    fig.suptitle("Paired task-level FCT by workload size class")
    fig.tight_layout()
    fig.savefig(path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def plot_over_time(window_summary, output_dir, dpi):
    path = output_dir / "fct_over_time.png"
    if window_summary.empty:
        save_empty_plot(path, "FCT over time", "No completed tasks", dpi)
        return
    fig, axes = plt.subplots(2, 1, figsize=(10.5, 7.5), sharex=True)
    for scenario, label in (("ecmp", "Direct ECMP"), ("wcmp", "MLU-WCMP")):
        group = window_summary[window_summary["scenario"].eq(scenario)]
        x = group["window_start_s"]
        axes[0].plot(x, group["p50_fct_us"] / 1e3, marker="o", label=label)
        axes[1].plot(x, group["p99_fct_us"] / 1e3, marker="o", label=label)
    axes[0].set_ylabel("P50 FCT (ms)")
    axes[1].set_ylabel("P99 FCT (ms)")
    axes[1].set_xlabel("Task-start window (s)")
    axes[0].set_title("Task-level FCT over simulation time")
    for axis in axes:
        axis.grid(True, alpha=0.3)
        axis.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def plot_paired_scatter(tasks, output_dir, dpi, max_points):
    paired = tasks[
        tasks["completion_status"].eq("both_completed")
        & tasks["ecmp_fct_us"].gt(0)
        & tasks["wcmp_fct_us"].gt(0)
    ]
    path = output_dir / "paired_fct_scatter.png"
    if paired.empty:
        save_empty_plot(path, "Paired FCT", "No positive paired FCT values", dpi)
        return
    if len(paired) > max_points:
        paired = paired.sample(max_points, random_state=0)
    x = paired["ecmp_fct_us"].to_numpy(dtype=float) / 1e3
    y = paired["wcmp_fct_us"].to_numpy(dtype=float) / 1e3
    lower = min(float(np.min(x)), float(np.min(y)))
    upper = max(float(np.max(x)), float(np.max(y)))
    fig, axis = plt.subplots(figsize=(7, 7))
    axis.scatter(x, y, s=6, alpha=0.25, edgecolors="none")
    axis.plot([lower, upper], [lower, upper], linestyle="--", color="black", linewidth=1)
    axis.set_xscale("log")
    axis.set_yscale("log")
    axis.set_xlabel("Direct ECMP task FCT (ms)")
    axis.set_ylabel("MLU-WCMP task FCT (ms)")
    axis.set_title("Per-task paired FCT")
    axis.grid(True, which="both", alpha=0.25)
    fig.tight_layout()
    fig.savefig(path, dpi=dpi, bbox_inches="tight")
    plt.close(fig)


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Compare task-level FCT between an existing direct-ECMP case and "
            "an existing MLU-WCMP case without reading WCMP traces."
        )
    )
    parser.add_argument("--ecmp-case", required=True, help="Direct ECMP case directory")
    parser.add_argument("--wcmp-case", required=True, help="MLU-WCMP case directory")
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Default: WCMP_CASE/output/fct_comparison_vs_ecmp",
    )
    parser.add_argument(
        "--bin-s",
        type=float,
        default=30.0,
        help="Task-start window width in seconds. Default: 30",
    )
    parser.add_argument("--dpi", type=int, default=180)
    parser.add_argument("--max-scatter-points", type=int, default=100_000)
    return parser.parse_args()


def main():
    args = parse_args()
    ecmp_case = Path(args.ecmp_case).resolve()
    wcmp_case = Path(args.wcmp_case).resolve()
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir
        else wcmp_case / DEFAULT_OUTPUT_REL
    )
    if args.bin_s <= 0:
        raise SystemExit("--bin-s must be greater than zero")
    if args.dpi <= 0:
        raise SystemExit("--dpi must be greater than zero")
    if args.max_scatter_points <= 0:
        raise SystemExit("--max-scatter-points must be greater than zero")
    output_dir.mkdir(parents=True, exist_ok=True)

    ecmp_traffic_path = ecmp_case / TRAFFIC_REL
    wcmp_traffic_path = wcmp_case / TRAFFIC_REL
    ecmp_statistics_path = ecmp_case / TASK_STATISTICS_REL
    wcmp_statistics_path = wcmp_case / TASK_STATISTICS_REL

    ecmp_traffic = load_traffic(ecmp_traffic_path)
    wcmp_traffic = load_traffic(wcmp_traffic_path)
    traffic_check = compare_traffic(ecmp_traffic, wcmp_traffic)
    ecmp_stats = load_task_statistics(ecmp_statistics_path, "ecmp")
    wcmp_stats = load_task_statistics(wcmp_statistics_path, "wcmp")
    tasks, size_metadata = build_task_table(
        ecmp_traffic, wcmp_traffic, ecmp_stats, wcmp_stats
    )

    summary = build_fct_summary(tasks)
    paired_summary = build_paired_summary(tasks)
    window_summary = build_window_summary(tasks, args.bin_s)
    incomplete = tasks[~tasks["completion_status"].eq("both_completed")].copy()

    tasks.to_csv(output_dir / "fct_per_task.csv", index=False)
    summary.to_csv(output_dir / "fct_summary.csv", index=False)
    paired_summary.to_csv(
        output_dir / "fct_paired_comparison_summary.csv", index=False
    )
    paired_summary[paired_summary["group_type"].eq("size_class")].to_csv(
        output_dir / "fct_by_size.csv", index=False
    )
    paired_summary[paired_summary["group_type"].eq("op_type")].to_csv(
        output_dir / "fct_by_op_type.csv", index=False
    )
    window_summary.to_csv(output_dir / "fct_window_summary.csv", index=False)
    incomplete.to_csv(output_dir / "incomplete_and_unmatched_tasks.csv", index=False)

    metadata = {
        "metric_definition": {
            "task_fct_us": "taskCompletesTime(us) - taskStartTime(us)",
            "paired_delta_us": "WCMP FCT - ECMP FCT; negative means WCMP is faster",
            "improvement_pct": "(ECMP FCT - WCMP FCT) / ECMP FCT * 100",
        },
        "inputs": {
            "ecmp_case": str(ecmp_case),
            "wcmp_case": str(wcmp_case),
            "ecmp_traffic": str(ecmp_traffic_path),
            "wcmp_traffic": str(wcmp_traffic_path),
            "ecmp_task_statistics": str(ecmp_statistics_path),
            "wcmp_task_statistics": str(wcmp_statistics_path),
        },
        "traffic_check": traffic_check,
        "size_classes": size_metadata,
        "window_seconds": float(args.bin_s),
        "counts": {
            "canonical_tasks": int(len(tasks)),
            "ecmp_completed": int(tasks["ecmp_completed"].sum()),
            "wcmp_completed": int(tasks["wcmp_completed"].sum()),
            "both_completed": int(
                tasks["completion_status"].eq("both_completed").sum()
            ),
            "ecmp_only_completed": int(
                tasks["completion_status"].eq("ecmp_only_completed").sum()
            ),
            "wcmp_only_completed": int(
                tasks["completion_status"].eq("wcmp_only_completed").sum()
            ),
            "neither_completed": int(
                tasks["completion_status"].eq("neither_completed").sum()
            ),
        },
        "limitations": [
            "This analysis is task-level, not an individual transport-flow lifecycle trace.",
            "No WCMP trace is read, so direct and transit WCMP tasks are not separated.",
            "A single run pair supports this simulation comparison but not cross-seed inference.",
        ],
    }
    with (output_dir / "analysis_metadata.json").open("w", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2, ensure_ascii=False)
        stream.write("\n")

    plot_cdf_and_ccdf(tasks, output_dir, args.dpi)
    plot_percentiles(paired_summary, output_dir, args.dpi)
    plot_by_size(paired_summary, output_dir, args.dpi)
    plot_over_time(window_summary, output_dir, args.dpi)
    plot_paired_scatter(
        tasks, output_dir, args.dpi, max_points=args.max_scatter_points
    )

    overall = paired_summary[paired_summary["group_type"].eq("overall")]
    print("=" * 88)
    print("Direct ECMP vs. MLU-WCMP task-level FCT comparison")
    print("=" * 88)
    print(f"ECMP case                : {ecmp_case}")
    print(f"WCMP case                : {wcmp_case}")
    print(f"Output                   : {output_dir}")
    print(f"Traffic lightweight match: {traffic_check['lightweight_match']}")
    if not traffic_check["lightweight_match"]:
        print("WARNING: traffic.csv core metadata differs; ECMP traffic is canonical.")
    for status, count in tasks["completion_status"].value_counts().items():
        print(f"{status:25s}: {count:,}")
    if not overall.empty and int(overall.iloc[0]["paired_tasks"]) > 0:
        row = overall.iloc[0]
        print("-" * 88)
        print(
            f"Mean FCT change (WCMP vs ECMP): {row['mean_fct_change_pct']:+.3f}%"
        )
        print(
            f"P50  FCT change (WCMP vs ECMP): {row['p50_fct_change_pct']:+.3f}%"
        )
        print(
            f"P99  FCT change (WCMP vs ECMP): {row['p99_fct_change_pct']:+.3f}%"
        )
        print(f"Tasks faster under WCMP         : {row['improved_tasks_pct']:.3f}%")
    else:
        print("No tasks completed in both cases; paired FCT metrics are unavailable.")
    print("=" * 88)


if __name__ == "__main__":
    main()
