#!/usr/bin/env python3
"""Make a short ToE smoke trace from actual host-to-leaf attachments."""
import argparse
import csv
from pathlib import Path


def make_trace(case_path, output):
    hosts = {}
    with (case_path / "topology.csv").open(newline="") as file:
        rows = csv.reader(file)
        next(rows)
        for row in rows:
            if not row or row[0].startswith("#"):
                continue
            a, ap, b, bp = map(int, row[:4])
            if a < 37 and 37 <= b <= 56:
                hosts.setdefault(a, set()).add((b - 37) // 4)
            elif b < 37 and 37 <= a <= 56:
                hosts.setdefault(b, set()).add((a - 37) // 4)
    representatives = {}
    for host, groups in sorted(hosts.items()):
        if len(groups) == 1:
            representatives.setdefault(next(iter(groups)), host)
    if set(representatives) != set(range(5)):
        raise ValueError("need a host attached within each of the five leaf groups")
    if output.resolve() == (case_path / "traffic.csv").resolve():
        raise ValueError("output must be a new file, not the original traffic.csv")
    pairs = [(representatives[a], representatives[b])
             for a in range(5) for b in range(5) if a != b]
    hot_src, hot_dst = representatives[0], representatives[4]
    records = []

    def add(src, dst, size, start_ns, phase=0):
        records.append([len(records), src, dst, size, "URMA_WRITE", 7,
                        f"{start_ns}ns", phase, ""])

    for index in range(32):
        add(hot_src, hot_dst, 4 * 1024 * 1024, 1_000_000_000 + index * 100_000)
    for index in range(8):
        add(hot_src, hot_dst, 4 * 1024 * 1024, 29_999_999_000)
    # Cover many cross-group pairs while the 2ms switch guard is active.
    for batch in range(50):
        for src, dst in pairs:
            add(src, dst, 8192, 30_000_010_000 + batch * 100_000)
    add(hot_src, hot_dst, 1024 * 1024, 31_000_000_000)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["taskId", "sourceNode", "destNode", "dataSize(Byte)", "opType",
                         "priority", "delay", "phaseId", "dependOnPhases"])
        writer.writerows(records)
    print(f"Generated {len(records)} tasks, last start=31s: {output}")
    print(f"Group representatives={representatives}; TP ports are selected by the simulator")
    return len(records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case-path", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        make_trace(args.case_path, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"ERROR: {error}\n")


if __name__ == "__main__":
    main()
