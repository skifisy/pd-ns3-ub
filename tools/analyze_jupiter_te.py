#!/usr/bin/env python3
"""Report post-warmup maximum directed OCS-edge utilization per 30s window."""

import argparse
import csv
import statistics
from collections import defaultdict
from pathlib import Path

from jupiter_te_solver import read_topology


def summarize(topology, observed_link_bytes, output, warmup_seconds=600):
    _, capacities, _ = read_topology(topology)
    windows = defaultdict(dict)
    with open(observed_link_bytes, newline="") as stream:
        for row in csv.DictReader(stream):
            if row["complete"] != "1":
                continue
            w = int(row["window"])
            if w * 30 < warmup_seconds:
                continue
            edge = int(row["src_leaf"]), int(row["dst_leaf"])
            if edge not in capacities:
                raise ValueError(f"unknown logical OCS edge {edge}")
            windows[w][edge] = int(row["wire_bytes"])
    peaks = []
    with open(output, "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("window", "start_seconds", "max_utilization", "busiest_src_leaf", "busiest_dst_leaf"))
        for window, edges in sorted(windows.items()):
            edge, utilization = max(((edge, 8 * bytes / (30 * capacities[edge]))
                                     for edge, bytes in edges.items()), key=lambda item: item[1])
            peaks.append(utilization)
            writer.writerow((window, window * 30, f"{utilization:.9g}", edge[0], edge[1]))
    if peaks:
        print(f"complete post-warmup windows: {len(peaks)}; mean peak U={statistics.mean(peaks):.6g}; "
              f"worst U={max(peaks):.6g}")
    else:
        print("no completed OCS-traffic window after warmup; no utilization conclusion")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology", type=Path, required=True)
    parser.add_argument("--link-bytes", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--warmup-seconds", type=float, default=600)
    args = parser.parse_args()
    summarize(args.topology, args.link_bytes, args.output, args.warmup_seconds)


if __name__ == "__main__":
    main()
