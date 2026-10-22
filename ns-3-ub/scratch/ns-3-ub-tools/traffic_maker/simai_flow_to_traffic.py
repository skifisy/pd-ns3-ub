#!/usr/bin/env python3
"""Convert SimAI MockNCCL flow exports to ns-3-ub traffic.csv."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict, deque
from pathlib import Path
from typing import Any


TRAFFIC_HEADER = [
    "taskId",
    "sourceNode",
    "destNode",
    "dataSize(Byte)",
    "opType",
    "priority",
    "delay",
    "phaseId",
    "dependOnPhases",
]


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    flows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as file:
        for line_no, line in enumerate(file, start=1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            record = json.loads(line)
            if record.get("schema") != "simai-flow-v1":
                raise ValueError(f"{path}:{line_no}: unsupported schema {record.get('schema')!r}")
            flows.append(record)
    if not flows:
        raise ValueError(f"{path}: no flow records found")
    return flows


def read_rank_map(path: Path | None) -> dict[int, int]:
    if path is None:
        return {}
    mapping: dict[int, int] = {}
    with path.open("r", encoding="utf-8", newline="") as file:
        reader = csv.DictReader(file)
        if reader.fieldnames is None:
            raise ValueError(f"{path}: rank map is empty")
        required = {"simai_rank", "ub_node"}
        if not required.issubset(set(reader.fieldnames)):
            raise ValueError(f"{path}: rank map header must include simai_rank,ub_node")
        for row in reader:
            mapping[int(row["simai_rank"])] = int(row["ub_node"])
    return mapping


def map_endpoint(rank: int, rank_map: dict[int, int]) -> int:
    return rank_map.get(rank, rank)


def validate_endpoint(node: int, host_count: int | None, flow_key: str, field_name: str) -> None:
    if host_count is None:
        return
    if node < 0 or node >= host_count:
        raise ValueError(
            f"{flow_key}: mapped {field_name}={node} is outside host id range 0..{host_count - 1}. "
            "If this is an NVLS/NVSwitch endpoint, it cannot be represented as a ns-3-ub "
            "traffic.csv task without an explicit host-level projection."
        )


def flow_key(flow: dict[str, Any]) -> tuple[int, int, int]:
    return (int(flow["collective_id"]), int(flow["channel_id"]), int(flow["flow_id"]))


def flow_label(key: tuple[int, int, int]) -> str:
    collective_id, channel_id, flow_id = key
    return f"collective={collective_id},channel={channel_id},flow={flow_id}"


def compute_levels(flows: list[dict[str, Any]]) -> dict[tuple[int, int, int], int]:
    flow_by_key = {flow_key(flow): flow for flow in flows}
    children: dict[tuple[int, int, int], set[tuple[int, int, int]]] = defaultdict(set)
    indegree: dict[tuple[int, int, int], int] = {key: 0 for key in flow_by_key}

    for key, flow in flow_by_key.items():
        collective_id, channel_id, _ = key
        for parent_id in flow.get("parent_flow_id", []):
            parent_key = (collective_id, channel_id, int(parent_id))
            if parent_key not in flow_by_key:
                raise ValueError(
                    f"{flow_label(key)}: parent flow id {parent_id} is missing in the same collective/channel"
                )
            if key not in children[parent_key]:
                children[parent_key].add(key)
                indegree[key] += 1

    queue = deque(sorted(key for key, degree in indegree.items() if degree == 0))
    levels = {key: 0 for key in queue}
    visited = 0

    while queue:
        current = queue.popleft()
        visited += 1
        for child in sorted(children[current]):
            levels[child] = max(levels.get(child, 0), levels[current] + 1)
            indegree[child] -= 1
            if indegree[child] == 0:
                queue.append(child)

    if visited != len(flow_by_key):
        cyclic = sorted(key for key, degree in indegree.items() if degree > 0)
        sample = ", ".join(flow_label(key) for key in cyclic[:5])
        raise ValueError(f"flow dependency graph has a cycle or unresolved edge, sample: {sample}")
    return levels


def assign_phases(
    flows: list[dict[str, Any]],
    levels: dict[tuple[int, int, int], int],
    phase_mode: str,
    sequential_collectives: bool,
) -> tuple[dict[tuple[int, int, int], int], dict[int, set[int]]]:
    phase_by_key: dict[tuple[int, int, int], int] = {}
    deps_by_phase: dict[int, set[int]] = defaultdict(set)
    sorted_flows = sorted(flows, key=lambda flow: (int(flow["collective_id"]), levels[flow_key(flow)], int(flow["channel_id"]), int(flow["flow_id"])))

    next_phase = 0
    collective_level_phase: dict[tuple[int, int], int] = {}
    current_collective: int | None = None

    for flow in sorted_flows:
        key = flow_key(flow)
        collective_id = key[0]
        if current_collective is None:
            current_collective = collective_id
        elif current_collective != collective_id:
            current_collective = collective_id

        if phase_mode == "flow":
            phase = next_phase
            next_phase += 1
        else:
            level_key = (collective_id, levels[key])
            if level_key not in collective_level_phase:
                collective_level_phase[level_key] = next_phase
                next_phase += 1
            phase = collective_level_phase[level_key]
        phase_by_key[key] = phase

    flow_by_key = {flow_key(flow): flow for flow in flows}
    for key, flow in flow_by_key.items():
        phase = phase_by_key[key]
        for parent_id in flow.get("parent_flow_id", []):
            parent_key = (key[0], key[1], int(parent_id))
            parent_phase = phase_by_key[parent_key]
            if parent_phase != phase:
                deps_by_phase[phase].add(parent_phase)

    if sequential_collectives:
        collectives = sorted({int(flow["collective_id"]) for flow in flows})
        for previous, current in zip(collectives, collectives[1:]):
            previous_last = max(
                phase_by_key[flow_key(flow)]
                for flow in flows
                if int(flow["collective_id"]) == previous
            )
            current_first_level = min(
                levels[flow_key(flow)]
                for flow in flows
                if int(flow["collective_id"]) == current
            )
            for flow in flows:
                key = flow_key(flow)
                if int(flow["collective_id"]) == current and levels[key] == current_first_level:
                    deps_by_phase[phase_by_key[key]].add(previous_last)

    return phase_by_key, deps_by_phase


def convert(
    flows: list[dict[str, Any]],
    output_path: Path,
    rank_map: dict[int, int],
    host_count: int | None,
    phase_mode: str,
    sequential_collectives: bool,
    op_type: str,
    priority: int,
    delay: str,
) -> dict[str, int]:
    levels = compute_levels(flows)
    phase_by_key, deps_by_phase = assign_phases(
        flows=flows,
        levels=levels,
        phase_mode=phase_mode,
        sequential_collectives=sequential_collectives,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    bytes_by_src: dict[int, int] = defaultdict(int)
    max_phase = 0

    sorted_flows = sorted(
        flows,
        key=lambda flow: (
            phase_by_key[flow_key(flow)],
            int(flow["collective_id"]),
            int(flow["channel_id"]),
            int(flow["flow_id"]),
        ),
    )
    with output_path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(TRAFFIC_HEADER)
        for task_id, flow in enumerate(sorted_flows):
            key = flow_key(flow)
            source = map_endpoint(int(flow["src"]), rank_map)
            dest = map_endpoint(int(flow["dst"]), rank_map)
            label = flow_label(key)
            validate_endpoint(source, host_count, label, "sourceNode")
            validate_endpoint(dest, host_count, label, "destNode")
            if source == dest:
                raise ValueError(f"{label}: sourceNode equals destNode after rank mapping")
            data_size = int(flow["flow_size"])
            if data_size <= 0:
                raise ValueError(f"{label}: flow_size must be positive")
            phase = phase_by_key[key]
            max_phase = max(max_phase, phase)
            depend_on = " ".join(str(dep) for dep in sorted(deps_by_phase.get(phase, set())))
            writer.writerow([task_id, source, dest, data_size, op_type, priority, delay, phase, depend_on])
            bytes_by_src[source] += data_size

    return {
        "tasks": len(sorted_flows),
        "phases": max_phase + 1 if sorted_flows else 0,
        "collectives": len({int(flow["collective_id"]) for flow in flows}),
        "sources": len(bytes_by_src),
        "total_bytes": sum(bytes_by_src.values()),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert SimAI AS_FLOW_EXPORT JSONL to ns-3-ub traffic.csv"
    )
    parser.add_argument("input", type=Path, help="SimAI flow JSONL exported with AS_FLOW_EXPORT")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output traffic.csv path")
    parser.add_argument(
        "--rank-map",
        type=Path,
        help="Optional CSV with simai_rank,ub_node columns. Defaults to identity mapping.",
    )
    parser.add_argument(
        "--host-count",
        type=int,
        help="Validate mapped endpoints are ns-3-ub host IDs in 0..host_count-1.",
    )
    parser.add_argument(
        "--phase-mode",
        choices=["level", "flow"],
        default="level",
        help="level groups DAG levels into phases; flow gives every flow a phase.",
    )
    parser.add_argument(
        "--allow-overlap-collectives",
        action="store_true",
        help="Do not add dependencies between successive SimAI collectives.",
    )
    parser.add_argument("--op-type", default="URMA_WRITE", help="traffic.csv opType")
    parser.add_argument("--priority", type=int, default=7, help="traffic.csv priority")
    parser.add_argument("--delay", default="0ns", help="traffic.csv delay")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    rank_map = read_rank_map(args.rank_map)
    flows = read_jsonl(args.input)
    stats = convert(
        flows=flows,
        output_path=args.output,
        rank_map=rank_map,
        host_count=args.host_count,
        phase_mode=args.phase_mode,
        sequential_collectives=not args.allow_overlap_collectives,
        op_type=args.op_type,
        priority=args.priority,
        delay=args.delay,
    )
    print(
        "traffic.csv written: "
        f"{args.output} "
        f"tasks={stats['tasks']} phases={stats['phases']} "
        f"collectives={stats['collectives']} total_bytes={stats['total_bytes']}"
    )


if __name__ == "__main__":
    main()
