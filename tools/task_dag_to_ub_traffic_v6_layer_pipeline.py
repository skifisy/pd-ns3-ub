#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Convert Mooncake Store-mediated PD task_dag.csv (v5) to native ns-3-UB traffic.csv.

Confirmed ns-3-UB semantics:
- Tasks wait for all dependOnPhases to complete.
- Once a task becomes ready, UbTrafficGen schedules it after its own `delay`.
- Therefore COMPUTE/TIMER DAG nodes can be folded into the delay of the next
  NETWORK phase.

v5 DAG:
    PREFIX_READ
        -> PREFILL
        -> PREFIX_STORE_WRITE
        -> PREFIX_STORE_COMMIT
        -> STORAGE_TO_D
        -> DECODE
        -> DECODE_WRITE

Native encoding:
    Phase R: PREFIX_READ
    Phase W: PREFIX_STORE_WRITE depends on R, delay=PREFILL duration
    Phase D: STORAGE_TO_D depends on W, delay=STORE_COMMIT duration
    Phase O: DECODE_WRITE depends on D, delay=DECODE duration

Cold-start/no-hit requests have no R phase. Their W tasks are initial tasks with
absolute delay = request arrival + PREFILL duration.

Important: UbTrafficGen stores dataSize in uint32_t. Any network task larger
than UINT32_MAX is split into multiple traffic.csv rows in the same phase.
"""

import argparse
import csv
import sys
from collections import defaultdict

UINT32_MAX = (1 << 32) - 1

TYPE_TO_OP = {
    "PREFIX_READ": "URMA_READ",
    "PREFIX_STORE_WRITE": "URMA_WRITE",
    "STORAGE_TO_D": "URMA_READ",
    "DECODE_WRITE": "URMA_WRITE",
}

STAGE_ORDER = [
    "PREFIX_READ",
    "PREFIX_STORE_WRITE",
    "STORAGE_TO_D",
    "DECODE_WRITE",
]

OUTPUT_FIELDS = [
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


def configure_csv_field_limit():
    """Raise Python csv's per-field limit for large dependency lists.

    Layer-pipelined DAGs can legitimately contain very long `depends_on`
    fields (for example, a commit barrier waiting on many per-layer writes).
    Python's csv module defaults to only 131072 bytes per field, which is too
    small for large Mooncake requests.
    """
    limit = sys.maxsize
    while True:
        try:
            csv.field_size_limit(limit)
            return limit
        except OverflowError:
            # Some Python/C-library combinations expose a smaller C long.
            limit //= 10


def iter_request_groups(path):
    """Stream task_dag.csv one request at a time.

    The v6 DAG generator emits all tasks for one request contiguously.
    Streaming avoids keeping the full layer-expanded DAG (and the full
    converted traffic) in memory.
    """
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        if not reader.fieldnames:
            raise ValueError("task_dag.csv is empty or missing a header")

        required = {
            "task_id", "request_id", "task_class", "task_type",
            "release_time_us", "duration_us", "src_node", "dst_node", "bytes"
        }
        missing = required - set(reader.fieldnames)
        if missing:
            raise ValueError(f"task_dag.csv missing columns: {sorted(missing)}")

        current_req = None
        current_rows = []
        finished = set()

        for row in reader:
            req_id = int(row["request_id"])

            if current_req is None:
                current_req = req_id

            if req_id != current_req:
                finished.add(current_req)
                yield current_req, current_rows

                if req_id in finished:
                    raise ValueError(
                        "task_dag.csv is not grouped by request_id; "
                        f"request {req_id} appears in multiple regions"
                    )

                current_req = req_id
                current_rows = []

            current_rows.append(row)

        if current_req is not None:
            yield current_req, current_rows


def intval(v, default=0):
    if v is None or v == "":
        return default
    return int(v)


def us_str(us):
    return f"{int(us)}us"


def split_u32(n):
    n = int(n)
    if n < 0:
        raise ValueError(f"negative data size: {n}")
    chunks = []
    while n > UINT32_MAX:
        chunks.append(UINT32_MAX)
        n -= UINT32_MAX
    if n:
        chunks.append(n)
    return chunks


def group_by_request(rows):
    d = defaultdict(list)
    for r in rows:
        d[int(r["request_id"])].append(r)
    return d


def tasks_of_type(rows, task_type):
    return [r for r in rows if r["task_type"] == task_type]


def one_task(rows, task_type):
    found = tasks_of_type(rows, task_type)
    if len(found) > 1:
        raise ValueError(
            f"request {rows[0]['request_id']}: expected <=1 {task_type}, got {len(found)}"
        )
    return found[0] if found else None


def validate_rows(rows):
    required = {
        "task_id", "request_id", "task_class", "task_type",
        "release_time_us", "duration_us", "src_node", "dst_node", "bytes"
    }
    if not rows:
        raise ValueError("task_dag.csv is empty")
    missing = required - set(rows[0].keys())
    if missing:
        raise ValueError(f"task_dag.csv missing columns: {sorted(missing)}")


def convert_request(req_id, rows, next_phase_id, next_task_id, priority, storage_latency_us):
    by_type = {t: tasks_of_type(rows, t) for t in STAGE_ORDER}

    prefill_layers = sorted(
        tasks_of_type(rows, "PREFILL_LAYER"),
        key=lambda r: intval(r.get("layer_index", 0)),
    )
    commit = one_task(rows, "PREFIX_STORE_COMMIT")
    decode = one_task(rows, "DECODE")

    prefill_us = sum(intval(r["duration_us"]) for r in prefill_layers)
    commit_us = intval(commit["duration_us"]) if commit else 0
    decode_us = intval(decode["duration_us"]) if decode else 0

    phase = {}
    for stage in STAGE_ORDER:
        if by_type[stage]:
            phase[stage] = next_phase_id
            next_phase_id += 1

    out = []

    def emit_stage(
        stage,
        dependency_phase=None,
        delay_us=0,
        preserve_release=False,
        per_row_delay_field=None,
    ):
        nonlocal next_task_id
        dep = "" if dependency_phase is None else str(dependency_phase)

        for src in by_type[stage]:
            if preserve_release:
                row_delay_us = intval(src["release_time_us"])
            elif per_row_delay_field is not None:
                row_delay_us = intval(src.get(per_row_delay_field, 0))
            else:
                row_delay_us = int(delay_us)

            for chunk in split_u32(intval(src["bytes"])):
                out.append({
                    "taskId": next_task_id,
                    "sourceNode": intval(src["src_node"]),
                    "destNode": intval(src["dst_node"]),
                    "dataSize(Byte)": chunk,
                    "opType": TYPE_TO_OP[stage],
                    "priority": priority,
                    "delay": us_str(row_delay_us),
                    "phaseId": phase[stage],
                    "dependOnPhases": dep,
                })
                next_task_id += 1

    # R: Storage -> P. release_time_us in v5 already equals arrival + storage latency.
    if by_type["PREFIX_READ"]:
        emit_stage("PREFIX_READ", preserve_release=True)

    # W: P -> Storage full-prefix write, split by model layer.
    #
    # Partial-hit/all-hit request:
    #   wait for the entire PREFIX_READ phase, then each layer starts after its
    #   own cumulative layer_ready_offset_us.
    #
    # Cold/no-hit request:
    #   each task_dag row already stores absolute release_time_us =
    #   arrival + cumulative layer compute time.
    #
    # All layer writes stay in ONE native phase.  Therefore downstream
    # STORAGE_TO_D still waits until every layer write completes, but the 78
    # writes themselves are staggered and overlap with ongoing compute.
    if by_type["PREFIX_STORE_WRITE"]:
        if by_type["PREFIX_READ"]:
            emit_stage(
                "PREFIX_STORE_WRITE",
                dependency_phase=phase["PREFIX_READ"],
                per_row_delay_field="layer_ready_offset_us",
            )
        else:
            emit_stage(
                "PREFIX_STORE_WRITE",
                dependency_phase=None,
                preserve_release=True,
            )

    # D: Storage -> Decode node after all full-prefix writes + storage commit delay.
    if by_type["STORAGE_TO_D"]:
        if "PREFIX_STORE_WRITE" not in phase:
            raise ValueError(
                f"request {req_id}: STORAGE_TO_D exists without PREFIX_STORE_WRITE"
            )
        emit_stage(
            "STORAGE_TO_D",
            dependency_phase=phase["PREFIX_STORE_WRITE"],
            delay_us=commit_us if commit is not None else storage_latency_us,
        )

    # O: Decode KV write after all Storage->D flows + Decode compute delay.
    if by_type["DECODE_WRITE"]:
        if "STORAGE_TO_D" not in phase:
            raise ValueError(
                f"request {req_id}: DECODE_WRITE exists without STORAGE_TO_D"
            )
        emit_stage(
            "DECODE_WRITE",
            dependency_phase=phase["STORAGE_TO_D"],
            delay_us=decode_us,
        )

    debug = {
        "request_id": req_id,
        "read_phase": phase.get("PREFIX_READ", ""),
        "write_phase": phase.get("PREFIX_STORE_WRITE", ""),
        "storage_to_d_phase": phase.get("STORAGE_TO_D", ""),
        "decode_write_phase": phase.get("DECODE_WRITE", ""),
        "prefill_delay_us": prefill_us,
        "prefill_layer_count": len(prefill_layers),
        "max_layer_ready_offset_us": max(
            (intval(r.get("layer_ready_offset_us", 0))
             for r in by_type["PREFIX_STORE_WRITE"]),
            default=0,
        ),
        "commit_delay_us": commit_us if commit is not None else storage_latency_us,
        "decode_delay_us": decode_us,
        "ub_network_tasks": len(out),
    }

    return out, next_phase_id, next_task_id, debug


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("task_dag")
    ap.add_argument("--output", default="traffic.csv")
    ap.add_argument("--debug-output", default="traffic_phase_debug.csv")
    ap.add_argument("--priority", type=int, default=7)
    ap.add_argument("--start-phase-id", type=int, default=1)
    ap.add_argument("--start-task-id", type=int, default=0)
    ap.add_argument("--storage-latency-us", type=int, default=150)
    args = ap.parse_args()

    csv_limit = configure_csv_field_limit()

    next_phase = args.start_phase_id
    next_task = args.start_task_id
    request_count = 0
    network_task_count = 0

    debug_file = None
    try:
        with open(args.output, "w", newline="", encoding="utf-8") as fout:
            out_writer = csv.DictWriter(fout, fieldnames=OUTPUT_FIELDS)
            out_writer.writeheader()

            debug_writer = None
            if args.debug_output:
                debug_file = open(
                    args.debug_output, "w", newline="", encoding="utf-8"
                )

            for req_id, req_rows in iter_request_groups(args.task_dag):
                # Header validity is already checked by iter_request_groups; this
                # preserves the existing row-level validation contract too.
                validate_rows(req_rows)

                converted, next_phase, next_task, dbg = convert_request(
                    req_id,
                    req_rows,
                    next_phase,
                    next_task,
                    args.priority,
                    args.storage_latency_us,
                )

                out_writer.writerows(converted)
                request_count += 1
                network_task_count += len(converted)

                if debug_file is not None:
                    if debug_writer is None:
                        debug_writer = csv.DictWriter(
                            debug_file, fieldnames=list(dbg.keys())
                        )
                        debug_writer.writeheader()
                    debug_writer.writerow(dbg)
    finally:
        if debug_file is not None:
            debug_file.close()

    if request_count == 0:
        raise ValueError("task_dag.csv contains no task rows")

    print("=" * 88)
    print("Mooncake layer-pipelined task_dag.csv -> native ns-3-UB traffic.csv")
    print("=" * 88)
    print(f"CSV field limit    : {csv_limit:,} bytes")
    print(f"Requests converted : {request_count:,}")
    print(f"UB network tasks   : {network_task_count:,}")
    print(f"Phase IDs used     : {next_phase - args.start_phase_id:,}")
    print(f"Output             : {args.output}")
    if args.debug_output:
        print(f"Phase debug        : {args.debug_output}")
    print("=" * 88)


if __name__ == "__main__":
    main()
