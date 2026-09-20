#!/usr/bin/env python3
"""Collect a bounded, paste-friendly Jupiter TE diagnostic report."""

import argparse
import csv
import io
import os
import subprocess
import sys
import time
from collections import Counter, deque
from contextlib import redirect_stdout
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TE_DIR = PROJECT_ROOT / "ns-3-ub" / "scratch" / "mooncake_pd_ocs" / "jupiter_te"

FILES = (
    "te_debug.log",
    "live_status.csv",
    "epoch_summary.csv",
    "demand_events.csv",
    "observed-current.csv",
    "observed.csv",
    "matrix-current.csv",
    "prediction.csv",
    "weights.csv",
    "link_bytes.csv",
    "path_decisions.csv",
    "WcmpSelectionTrace.csv",
    "transit_forwards.csv",
    "policy-current.csv",
    "solver-summary-current.csv",
)


def print_command(title: str, command: list[str]) -> None:
    """Run a read-only command and include its output in the report."""
    print(f"\n===== {title} =====")
    try:
        result = subprocess.run(
            command,
            cwd=PROJECT_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as error:
        print(f"COMMAND_ERROR: {error}")
        return
    output = result.stdout.rstrip()
    print(output if output else "<no output>")
    print(f"exit_code={result.returncode}")


def print_file_excerpt(path: Path, head_lines: int, tail_lines: int) -> None:
    """Print file metadata and a bounded head/tail excerpt without loading it all."""
    print(f"\n===== {path.name} =====")
    if not path.exists():
        print("MISSING")
        return

    first = []
    last = deque(maxlen=tail_lines)
    line_count = 0
    try:
        with path.open(encoding="utf-8", errors="replace") as stream:
            for raw_line in stream:
                line = raw_line.rstrip("\n")
                if len(first) < head_lines:
                    first.append(line)
                last.append(line)
                line_count += 1
    except OSError as error:
        print(f"READ_ERROR: {error}")
        return

    stat = path.stat()
    print(f"size_bytes={stat.st_size} lines={line_count} mtime={stat.st_mtime:.6f}")
    if line_count == 0:
        print("EMPTY")
        return

    tail = list(last)
    if line_count <= len(first) + len(tail):
        overlap = len(first) + len(tail) - line_count
        selected = first + tail[overlap:]
    else:
        selected = first + [f"... {line_count - len(first) - len(tail)} lines omitted ..."] + tail
    print("\n".join(selected))


def print_path_decision_summary(path: Path) -> None:
    """Summarize all path decisions without printing every row."""
    print("\n===== PATH_DECISION_SUMMARY =====")
    if not path.exists() or path.stat().st_size == 0:
        print("NO_DATA")
        return

    rows = 0
    minimum_time = None
    maximum_time = None
    epochs = Counter()
    sources = Counter()
    path_types = Counter()
    leaf_ods = set()
    try:
        with path.open(newline="", encoding="utf-8", errors="replace") as stream:
            for row in csv.DictReader(stream):
                rows += 1
                try:
                    simulation_time = float(row["sim_time_seconds"])
                    minimum_time = (
                        simulation_time
                        if minimum_time is None
                        else min(minimum_time, simulation_time)
                    )
                    maximum_time = (
                        simulation_time
                        if maximum_time is None
                        else max(maximum_time, simulation_time)
                    )
                except (KeyError, TypeError, ValueError):
                    pass
                epochs[row.get("epoch", "<missing>")] += 1
                sources[row.get("decision_source", "<missing>")] += 1
                path_types[
                    "direct" if row.get("transit_leaf") == "-1" else "transit"
                ] += 1
                leaf_ods.add((row.get("src_leaf"), row.get("dst_leaf")))
    except (OSError, csv.Error) as error:
        print(f"SUMMARY_ERROR: {error}")
        return

    print(f"rows={rows}")
    print(f"time_range={minimum_time}..{maximum_time}")
    print(f"epochs={dict(epochs)}")
    print(f"decision_sources={dict(sources)}")
    print(f"direct_vs_transit={dict(path_types)}")
    print(f"unique_leaf_ods={len(leaf_ods)}")


def read_last_csv_rows(path: Path, count: int = 2) -> list[dict[str, str]]:
    """Read only the last few parsed rows from a CSV file."""
    rows = deque(maxlen=count)
    if not path.exists() or path.stat().st_size == 0:
        return []
    try:
        with path.open(newline="", encoding="utf-8", errors="replace") as stream:
            for row in csv.DictReader(stream):
                rows.append(row)
    except (OSError, csv.Error):
        return []
    return list(rows)


def print_live_status_summary(path: Path) -> None:
    """Print the newest counters and their delta from the previous snapshot."""
    print("\n===== LIVE_STATUS_SUMMARY =====")
    rows = read_last_csv_rows(path)
    if not rows:
        print("NO_DATA")
        return

    keys = (
        "sim_time_seconds",
        "wall_time_unix_ms",
        "trigger",
        "total_tasks",
        "pending_tasks",
        "ready_tasks",
        "running_tasks",
        "completed_tasks",
        "new_data_callbacks",
        "accepted_data_packets",
        "business_bytes",
        "source_mapping_misses",
        "destination_mapping_misses",
        "same_leaf_packets",
        "ocs_packets",
        "ocs_wire_bytes",
        "flow_decisions",
        "route_controller_calls",
        "current_od_entries",
    )
    latest = rows[-1]
    print("latest=" + " ".join(f"{key}={latest.get(key, '<missing>')}" for key in keys))
    try:
        controller_calls = int(latest["route_controller_calls"])
        flow_decisions = int(latest["flow_decisions"])
        callbacks = int(latest["new_data_callbacks"])
        print(
            "route_cache_check="
            f"controller_calls_per_flow={controller_calls / max(flow_decisions, 1):.3f} "
            f"controller_calls_per_business_packet={controller_calls / max(callbacks, 1):.6f}"
        )
    except (KeyError, TypeError, ValueError):
        print("route_cache_check=<unavailable>")
    if len(rows) < 2:
        return

    previous = rows[-2]
    delta_keys = (
        "sim_time_seconds",
        "wall_time_unix_ms",
        "completed_tasks",
        "new_data_callbacks",
        "accepted_data_packets",
        "business_bytes",
        "ocs_packets",
        "ocs_wire_bytes",
        "flow_decisions",
        "route_controller_calls",
    )
    deltas = []
    for key in delta_keys:
        try:
            delta = float(latest[key]) - float(previous[key])
            deltas.append(f"{key}={delta:g}")
        except (KeyError, TypeError, ValueError):
            deltas.append(f"{key}=<unavailable>")
    print("last_row_delta=" + " ".join(deltas))


def print_category_summary(title: str, path: Path, column: str) -> None:
    """Count values of one diagnostic CSV column."""
    print(f"\n===== {title} =====")
    if not path.exists() or path.stat().st_size == 0:
        print("NO_DATA")
        return
    counts = Counter()
    try:
        with path.open(newline="", encoding="utf-8", errors="replace") as stream:
            for row in csv.DictReader(stream):
                counts[row.get(column, "<missing>")] += 1
    except (OSError, csv.Error) as error:
        print(f"SUMMARY_ERROR: {error}")
        return
    print(dict(counts) if counts else "NO_ROWS")


def print_policy_summary(path: Path) -> None:
    """Check that every emitted OD has finite, normalized path weights."""
    print("\n===== POLICY_WEIGHT_SUMMARY =====")
    if not path.exists() or path.stat().st_size == 0:
        print("NO_DATA")
        return
    totals = Counter()
    invalid_rows = 0
    try:
        with path.open(newline="", encoding="utf-8", errors="replace") as stream:
            for row in csv.DictReader(stream):
                try:
                    weight = float(row["weight"])
                    if weight < 0 or weight != weight or weight in (float("inf"), -float("inf")):
                        invalid_rows += 1
                        continue
                    totals[row["src_leaf"], row["dst_leaf"]] += weight
                except (KeyError, TypeError, ValueError):
                    invalid_rows += 1
    except (OSError, csv.Error) as error:
        print(f"SUMMARY_ERROR: {error}")
        return
    bad_ods = {od: total for od, total in totals.items() if abs(total - 1.0) > 1e-6}
    values = list(totals.values())
    print(f"ods={len(totals)} invalid_rows={invalid_rows} invalid_weight_sums={len(bad_ods)}")
    if values:
        print(f"weight_sum_range={min(values):.17g}..{max(values):.17g}")
    if bad_ods:
        print(f"bad_od_examples={dict(list(bad_ods.items())[:10])}")


def collect_report(te_dir: Path, head_lines: int, tail_lines: int) -> str:
    """Build and return the complete diagnostic report."""
    output = io.StringIO()
    with redirect_stdout(output):
        print(f"DIAGNOSTIC_TIME={time.strftime('%Y-%m-%d %H:%M:%S %z')}")
        print(f"PROJECT_ROOT={PROJECT_ROOT}")
        print(f"TE_DIR={te_dir.resolve()}")
        print(f"TE_DIR_EXISTS={te_dir.exists()}")

        print_command("GIT_HEAD", ["git", "rev-parse", "HEAD"])
        print_command(
            "TRACKED_CHANGES", ["git", "status", "--short", "--untracked-files=no"]
        )
        print_command("RUNNING_SIMULATION", ["pgrep", "-af", "ub-quick-example"])

        for name in FILES:
            path = te_dir / name
            if name == "te_debug.log":
                print_file_excerpt(path, head_lines, max(tail_lines, 100))
            else:
                print_file_excerpt(path, head_lines, tail_lines)

        print_live_status_summary(te_dir / "live_status.csv")
        print_path_decision_summary(te_dir / "path_decisions.csv")
        print_category_summary(
            "WCMP_TASK_SELECTION_SUMMARY",
            te_dir / "WcmpSelectionTrace.csv",
            "decision_source",
        )
        print_policy_summary(te_dir / "policy-current.csv")
        print_category_summary(
            "DEMAND_EVENT_SUMMARY", te_dir / "demand_events.csv", "result"
        )

    return output.getvalue()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--te-dir",
        type=Path,
        default=DEFAULT_TE_DIR,
        help=f"TE debug directory (default: {DEFAULT_TE_DIR})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="also save the same report to this file",
    )
    parser.add_argument("--head-lines", type=int, default=6)
    parser.add_argument("--tail-lines", type=int, default=35)
    args = parser.parse_args()
    if args.head_lines < 0 or args.tail_lines < 1:
        parser.error("--head-lines must be >= 0 and --tail-lines must be >= 1")

    report = collect_report(args.te_dir, args.head_lines, args.tail_lines)
    sys.stdout.write(report)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")
        print(f"\nREPORT_SAVED={args.output.resolve()}")


if __name__ == "__main__":
    main()
