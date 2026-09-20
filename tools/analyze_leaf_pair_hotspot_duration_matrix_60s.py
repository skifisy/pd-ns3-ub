#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Plot directed Leaf-pair hotspot duration matrices from WCMP task intervals.

For each fixed time window (60 seconds by default), cell ``(i, j)`` is the
total duration for which ``Leaf i -> Leaf j`` had at least ``threshold``
concurrent active tasks.

Default input:
  CASE_DIR/output/leaf_pair_active_flow_analysis/
    leaf_pair_active_flow_intervals.csv

Default output:
  CASE_DIR/output/leaf_pair_hotspot_duration_60s/
    leaf_pair_hotspot_duration_long.csv
    leaf_pair_hotspot_duration_summary.csv
    leaf_pair_hotspot_duration_<start>_<end>s.csv
    leaf_pair_hotspot_duration_<start>_<end>s.png
    00_all_leaf_pair_hotspot_duration_60s.png
"""

import argparse
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


DEFAULT_INTERVALS_REL = Path(
    "output/leaf_pair_active_flow_analysis/leaf_pair_active_flow_intervals.csv"
)
DEFAULT_OUTPUT_REL = Path("output/leaf_pair_hotspot_duration_60s")


def parse_node_set(spec):
    nodes = set()
    for token in str(spec).split(","):
        token = token.strip()
        if not token:
            continue
        if ".." in token:
            first, last = token.split("..", 1)
            first, last = int(first), int(last)
            nodes.update(range(min(first, last), max(first, last) + 1))
        elif "-" in token:
            first, last = token.split("-", 1)
            first, last = int(first), int(last)
            nodes.update(range(min(first, last), max(first, last) + 1))
        else:
            nodes.add(int(token))
    return nodes


def safe_read_csv(path, usecols):
    try:
        return pd.read_csv(path, usecols=usecols)
    except ValueError as error:
        existing = list(pd.read_csv(path, nrows=2).columns)
        raise RuntimeError(
            f"Missing required columns in {path}\n"
            f"Required: {usecols}\nExisting: {existing}"
        ) from error


def sec_label(value):
    if float(value).is_integer():
        return f"{int(value):06d}"
    return f"{value:010.3f}".replace(".", "p")


def load_intervals(path, leaf_nodes):
    required = [
        "leaf_a",
        "leaf_b",
        "interval_start_us",
        "interval_end_us",
        "active_flows",
    ]
    intervals = safe_read_csv(path, required)
    for column in required:
        intervals[column] = pd.to_numeric(intervals[column], errors="coerce")
    intervals = intervals.dropna(subset=required).copy()
    intervals = intervals[
        (intervals["interval_end_us"] > intervals["interval_start_us"])
        & (intervals["active_flows"] > 0)
    ].copy()
    intervals["leaf_a"] = intervals["leaf_a"].astype(np.int32)
    intervals["leaf_b"] = intervals["leaf_b"].astype(np.int32)

    leaf_nodes = set(leaf_nodes)
    intervals = intervals[
        intervals["leaf_a"].isin(leaf_nodes)
        & intervals["leaf_b"].isin(leaf_nodes)
        & intervals["leaf_a"].ne(intervals["leaf_b"])
    ].copy()
    if intervals.empty:
        raise RuntimeError("No inter-Leaf active-flow intervals remain after filtering")

    intervals = intervals.sort_values(
        ["leaf_a", "leaf_b", "interval_start_us", "interval_end_us"]
    ).reset_index(drop=True)
    previous_end = intervals.groupby(["leaf_a", "leaf_b"], sort=False)[
        "interval_end_us"
    ].shift(1)
    overlap = previous_end.notna() & intervals["interval_start_us"].lt(previous_end)
    if overlap.any():
        bad = intervals.loc[
            overlap,
            ["leaf_a", "leaf_b", "interval_start_us", "interval_end_us"],
        ].head(10)
        raise RuntimeError(
            "Overlapping intervals found for the same directed Leaf pair:\n"
            + bad.to_string(index=False)
        )
    return intervals


def build_hotspot_duration_table(intervals, threshold, bin_s, origin_mode):
    minimum_start_us = float(intervals["interval_start_us"].min())
    maximum_end_us = float(intervals["interval_end_us"].max())
    origin_us = 0.0 if origin_mode == "zero" else minimum_start_us
    bin_us = float(bin_s) * 1e6
    if bin_us <= 0:
        raise ValueError("--bin-s must be greater than zero")
    if float(threshold) <= 0:
        raise ValueError("--threshold must be greater than zero")

    number_of_bins = max(
        1, int(math.ceil((maximum_end_us - origin_us) / bin_us))
    )
    hot = intervals[intervals["active_flows"] >= float(threshold)]
    accumulated = defaultdict(float)

    for row in hot.itertuples(index=False):
        source = int(row.leaf_a)
        destination = int(row.leaf_b)
        start = max(float(row.interval_start_us), origin_us)
        end = min(float(row.interval_end_us), maximum_end_us)
        if end <= start:
            continue

        first_bin = int(math.floor((start - origin_us) / bin_us))
        last_bin = int(
            math.floor((np.nextafter(end, -np.inf) - origin_us) / bin_us)
        )
        first_bin = max(0, min(first_bin, number_of_bins - 1))
        last_bin = max(0, min(last_bin, number_of_bins - 1))
        for bin_id in range(first_bin, last_bin + 1):
            bin_start = origin_us + bin_id * bin_us
            bin_end = min(bin_start + bin_us, maximum_end_us)
            overlap = min(end, bin_end) - max(start, bin_start)
            if overlap > 0:
                accumulated[(bin_id, source, destination)] += overlap

    columns = [
        "bin_id",
        "bin_start_us",
        "bin_end_us",
        "elapsed_start_s",
        "elapsed_end_s",
        "src_leaf",
        "dst_leaf",
        "hotspot_threshold_active_flows",
        "hotspot_duration_us",
        "hotspot_duration_s",
        "hotspot_fraction_pct",
    ]
    rows = []
    for (bin_id, source, destination), duration_us in accumulated.items():
        bin_start = origin_us + bin_id * bin_us
        bin_end = min(bin_start + bin_us, maximum_end_us)
        actual_bin_us = max(bin_end - bin_start, 0.0)
        if duration_us > actual_bin_us + 1e-6:
            raise RuntimeError(
                f"Hotspot duration exceeds bin width for {source}->{destination}, "
                f"bin {bin_id}: {duration_us} > {actual_bin_us} us"
            )
        rows.append(
            {
                "bin_id": bin_id,
                "bin_start_us": bin_start,
                "bin_end_us": bin_end,
                "elapsed_start_s": bin_id * float(bin_s),
                "elapsed_end_s": min(
                    (bin_id + 1) * float(bin_s),
                    (maximum_end_us - origin_us) / 1e6,
                ),
                "src_leaf": source,
                "dst_leaf": destination,
                "hotspot_threshold_active_flows": float(threshold),
                "hotspot_duration_us": duration_us,
                "hotspot_duration_s": duration_us / 1e6,
                "hotspot_fraction_pct": (
                    duration_us / actual_bin_us * 100.0
                    if actual_bin_us > 0
                    else 0.0
                ),
            }
        )

    if not rows:
        return pd.DataFrame(columns=columns), origin_us, maximum_end_us, number_of_bins
    table = pd.DataFrame(rows)[columns]
    table = table.sort_values(["bin_id", "src_leaf", "dst_leaf"])
    return table.reset_index(drop=True), origin_us, maximum_end_us, number_of_bins


def make_matrix(binned, bin_id, leaf_ids):
    matrix = pd.DataFrame(0.0, index=leaf_ids, columns=leaf_ids)
    for row in binned[binned["bin_id"].eq(bin_id)].itertuples(index=False):
        matrix.loc[int(row.src_leaf), int(row.dst_leaf)] += float(
            row.hotspot_duration_s
        )
    return matrix


def image_values(matrix):
    values = matrix.to_numpy(dtype=float)
    return np.ma.masked_less_equal(values, 0.0)


def hotspot_colormap():
    color_map = plt.get_cmap("YlOrRd").copy()
    color_map.set_bad("white")
    return color_map


def plot_one_matrix(
    matrix,
    bin_id,
    bin_s,
    observation_end_s,
    threshold,
    maximum_s,
    output_file,
    dpi,
    annotate,
):
    values = matrix.to_numpy(dtype=float)
    leaf_ids = list(matrix.index)
    count = len(leaf_ids)
    side = max(10.5, 0.52 * count)
    figure, axis = plt.subplots(figsize=(side + 2.0, side))
    image = axis.imshow(
        image_values(matrix),
        cmap=hotspot_colormap(),
        vmin=0.0,
        vmax=maximum_s,
        aspect="equal",
        interpolation="nearest",
    )

    positions = np.arange(count)
    axis.set_xticks(positions)
    axis.set_yticks(positions)
    axis.set_xticklabels(leaf_ids, rotation=45, ha="right")
    axis.set_yticklabels(leaf_ids)
    axis.set_xlabel("Destination Leaf Switch")
    axis.set_ylabel("Source Leaf Switch")
    start_s = bin_id * bin_s
    end_s = min((bin_id + 1) * bin_s, observation_end_s)
    axis.set_title(
        "Directed Leaf-to-Leaf Hotspot Duration\n"
        f"{start_s:.0f}-{end_s:.0f} s | active tasks >= {threshold:g}",
        pad=14,
        fontweight="bold",
    )
    axis.set_xticks(np.arange(-0.5, count, 1), minor=True)
    axis.set_yticks(np.arange(-0.5, count, 1), minor=True)
    axis.grid(which="minor", linewidth=0.45, alpha=0.35)
    axis.tick_params(which="minor", bottom=False, left=False)

    if annotate:
        for row in range(count):
            for column in range(count):
                value = float(values[row, column])
                if value <= 0:
                    continue
                if value >= 10:
                    label = f"{value:.1f}"
                elif value >= 1:
                    label = f"{value:.2f}"
                else:
                    label = f"{value:.3f}"
                color = "white" if value / maximum_s > 0.55 else "black"
                axis.text(
                    column,
                    row,
                    label,
                    ha="center",
                    va="center",
                    fontsize=6.3,
                    color=color,
                )

    colorbar = figure.colorbar(image, ax=axis, fraction=0.046, pad=0.04)
    colorbar.set_label("Hotspot duration within this window (s)")
    figure.tight_layout()
    figure.savefig(output_file, dpi=dpi, bbox_inches="tight")
    plt.close(figure)


def plot_all_bins_grid(
    binned,
    leaf_ids,
    number_of_bins,
    bin_s,
    observation_end_s,
    threshold,
    maximum_s,
    output_file,
    dpi,
    columns,
):
    columns = max(1, int(columns))
    rows = int(math.ceil(number_of_bins / columns))
    figure, axes = plt.subplots(
        rows,
        columns,
        figsize=(columns * 5.2, rows * 4.8),
        squeeze=False,
    )
    positions = np.arange(len(leaf_ids))
    last_image = None
    for bin_id in range(number_of_bins):
        row, column = divmod(bin_id, columns)
        axis = axes[row][column]
        matrix = make_matrix(binned, bin_id, leaf_ids)
        last_image = axis.imshow(
            image_values(matrix),
            cmap=hotspot_colormap(),
            vmin=0.0,
            vmax=maximum_s,
            aspect="equal",
            interpolation="nearest",
        )
        start_s = bin_id * bin_s
        end_s = min((bin_id + 1) * bin_s, observation_end_s)
        axis.set_title(f"{start_s:.0f}-{end_s:.0f} s", fontsize=10, fontweight="bold")
        axis.set_xticks(positions)
        axis.set_yticks(positions)
        axis.set_xticklabels(leaf_ids, rotation=90, fontsize=5.5)
        if column == 0:
            axis.set_yticklabels(leaf_ids, fontsize=5.5)
            axis.set_ylabel("Source Leaf", fontsize=8)
        else:
            axis.set_yticklabels([])
        if row == rows - 1:
            axis.set_xlabel("Destination Leaf", fontsize=8)

    for bin_id in range(number_of_bins, rows * columns):
        row, column = divmod(bin_id, columns)
        axes[row][column].axis("off")

    figure.suptitle(
        "Directed Leaf-to-Leaf Hotspot Duration Over Time\n"
        f"Window = {bin_s:g} s; hotspot = active tasks >= {threshold:g}",
        fontsize=16,
        fontweight="bold",
        y=0.995,
    )
    if last_image is not None:
        figure.subplots_adjust(right=0.91, top=0.94, hspace=0.28, wspace=0.16)
        color_axis = figure.add_axes([0.925, 0.14, 0.014, 0.72])
        colorbar = figure.colorbar(last_image, cax=color_axis)
        colorbar.set_label("Hotspot duration within window (s)")
    figure.savefig(output_file, dpi=dpi, bbox_inches="tight")
    plt.close(figure)


def build_summary(binned):
    columns = [
        "src_leaf",
        "dst_leaf",
        "total_hotspot_duration_s",
        "max_window_hotspot_duration_s",
        "hotspot_windows",
    ]
    if binned.empty:
        return pd.DataFrame(columns=columns)
    summary = (
        binned.groupby(["src_leaf", "dst_leaf"], as_index=False)
        .agg(
            total_hotspot_duration_s=("hotspot_duration_s", "sum"),
            max_window_hotspot_duration_s=("hotspot_duration_s", "max"),
            hotspot_windows=("bin_id", "nunique"),
        )
        .sort_values(
            ["total_hotspot_duration_s", "max_window_hotspot_duration_s"],
            ascending=[False, False],
        )
    )
    return summary.reset_index(drop=True)[columns]


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Generate directed Leaf-pair matrices whose cells contain the "
            "duration above a concurrent-task threshold"
        )
    )
    parser.add_argument("case_dir", help="ns-3 case directory")
    parser.add_argument("--intervals-csv", default=None)
    parser.add_argument("--bin-s", type=float, default=60.0)
    parser.add_argument("--threshold", type=float, default=16.0)
    parser.add_argument("--leaf-nodes", default="37-56")
    parser.add_argument("--origin", choices=["first", "zero"], default="first")
    parser.add_argument("--output-dir", default=None)
    parser.add_argument("--vmax-s", type=float, default=None)
    parser.add_argument("--combined-cols", type=int, default=3)
    parser.add_argument("--no-annotate", action="store_true")
    parser.add_argument("--dpi", type=int, default=220)
    return parser.parse_args()


def main():
    args = parse_args()
    case = Path(args.case_dir).resolve()
    intervals_file = (
        Path(args.intervals_csv).resolve()
        if args.intervals_csv
        else case / DEFAULT_INTERVALS_REL
    )
    output_dir = (
        Path(args.output_dir).resolve()
        if args.output_dir
        else case / DEFAULT_OUTPUT_REL
    )
    if not intervals_file.exists():
        raise SystemExit(f"Missing intervals CSV:\n{intervals_file}")
    output_dir.mkdir(parents=True, exist_ok=True)

    leaf_ids = sorted(parse_node_set(args.leaf_nodes))
    if not leaf_ids:
        raise RuntimeError("No Leaf nodes specified")
    intervals = load_intervals(intervals_file, leaf_ids)
    binned, origin_us, maximum_end_us, number_of_bins = build_hotspot_duration_table(
        intervals,
        threshold=args.threshold,
        bin_s=args.bin_s,
        origin_mode=args.origin,
    )

    long_file = output_dir / "leaf_pair_hotspot_duration_long.csv"
    binned.to_csv(long_file, index=False)
    summary = build_summary(binned)
    summary_file = output_dir / "leaf_pair_hotspot_duration_summary.csv"
    summary.to_csv(summary_file, index=False)

    observation_end_s = (maximum_end_us - origin_us) / 1e6
    if args.vmax_s is not None:
        maximum_s = float(args.vmax_s)
    elif not binned.empty:
        maximum_s = float(binned["hotspot_duration_s"].max())
    else:
        maximum_s = float(args.bin_s)
    if maximum_s <= 0:
        raise ValueError("--vmax-s must be greater than zero")

    print("=" * 82)
    print("Directed Leaf-to-Leaf Hotspot Duration Matrix (WCMP)")
    print("=" * 82)
    print("CASE      :", case)
    print("INPUT     :", intervals_file)
    print("OUTPUT    :", output_dir)
    print("LEAVES    :", leaf_ids)
    print("WINDOW    :", f"{args.bin_s:g} s")
    print("THRESHOLD :", f"active tasks >= {args.threshold:g}")
    print("INTERVALS :", f"{len(intervals):,}")
    print("HOT ROWS  :", f"{len(binned):,}")

    for bin_id in range(number_of_bins):
        matrix = make_matrix(binned, bin_id, leaf_ids)
        start_s = bin_id * args.bin_s
        end_s = min((bin_id + 1) * args.bin_s, observation_end_s)
        start_label = sec_label(start_s)
        end_label = sec_label(end_s)
        csv_file = output_dir / (
            f"leaf_pair_hotspot_duration_{start_label}_{end_label}s.csv"
        )
        png_file = output_dir / (
            f"leaf_pair_hotspot_duration_{start_label}_{end_label}s.png"
        )
        matrix.to_csv(csv_file, index_label="SourceLeaf")
        plot_one_matrix(
            matrix,
            bin_id,
            args.bin_s,
            observation_end_s,
            args.threshold,
            maximum_s,
            png_file,
            args.dpi,
            not args.no_annotate,
        )
        print(f"[{bin_id + 1:>3}/{number_of_bins}] {png_file.name}")

    bin_label = f"{args.bin_s:g}".replace(".", "p")
    combined_file = output_dir / (
        f"00_all_leaf_pair_hotspot_duration_{bin_label}s.png"
    )
    plot_all_bins_grid(
        binned,
        leaf_ids,
        number_of_bins,
        args.bin_s,
        observation_end_s,
        args.threshold,
        maximum_s,
        combined_file,
        args.dpi,
        args.combined_cols,
    )

    print("Top directed Leaf pairs by total hotspot duration:")
    if summary.empty:
        print("No hotspot interval reached the threshold")
    else:
        print(summary.head(20).to_string(index=False))
    print("Generated:")
    print(" ", long_file)
    print(" ", summary_file)
    print(" ", combined_file)
    print("=" * 82)


if __name__ == "__main__":
    main()
