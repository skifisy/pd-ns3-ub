#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Compare request metrics and per-flow FCT across TE/TOE variants and baselines.

Example:
  python3 stat_request_compare_v10.py --case /data/CASE \
      --ecmp-dir /data/ecmp \
      --te-dir /data/transit_010 --te-dir /data/transit_020 \
      --toe-dir /data/slack_020 --toe-dir /data/slack_050 \
      --eps2-dir /data/eps_2layer \
      --data-size-divisor 100000 --num-layers 78 --out-dir /data/compare

Common inputs:
  CASE/task_dag.csv, CASE/traffic.csv (or --mapping), CASE/requests_debug.csv
Repeat --te-dir or --toe-dir once per tuning directory. When a family has
multiple directory inputs, each run is named te_<directory> or toe_<directory>.
Duplicate automatic names get numeric suffixes. For explicit names, repeat
--te NAME=DIR_OR_CSV or --toe NAME=DIR_OR_CSV.
DIR contains output/task_statistics.csv or task_statistics.csv.
With zero or one directory per family, --*-statistics supplies or overrides
the original run. With multiple directories, it adds a separate original run.
Select at least two runs. All runs must use the common traffic taskId mapping.
Prefill read/write times use observed network flows in task_statistics.csv.
The traffic.csv + DAG path provides their taskIds without a mapping file.
Outputs (exactly two CSVs):
  request_comparison.csv: one row/request with timing metrics for each run.
  summary_comparison.csv: one row per run, TTFT P50/P90/P99/P999,
      mean TPS, mean logical-read throughput, flow FCT quantiles.

Definitions:
  TTFT_ms = (earliest final-layer PREFIX_STORE_WRITE start
             - earliest PREFIX_READ start across layers) / 1000.
      Both starts are in microseconds, from task_statistics.csv for the same
      request. Each endpoint requires valid starts for its expected shards.
      TTFT is blank if either endpoint is unavailable. A started write can
      contribute while its completion is still pending.
  decode_load_compute_ms = (L-R)/1000, where R=min start of STORAGE_TO_D
      and L=max start of last-token DECODE_WRITE shards, or of the single
      request-final output write in the current generator.
  TPS = output_tokens * 1000 / decode_load_compute_ms.
  completion_time_ms = (max completion - min start) / 1000, among ALL
      expected request network flows, valid ONLY if all are completed.
  logical_read_throughput_gbps = (PREFIX_READ + STORAGE_TO_D DAG bytes)*8
      / (completion_time_ms * 1e6). Logical bytes are UNSCALED; this is
      not physical-link throughput when traffic sizes were scaled.
  FCT_ms = (taskCompletesTime(us) - taskStartTime(us))/1000 for EACH
      individually completed flow. FCT percentiles use taskIds completed
      in all selected runs; no request aggregation for FCT.

Summary request metrics each use their own common-valid request subset.
Exact percentiles use linear interpolation at (n-1)*p. FCT percentiles
use disk-based external sorting with bounded RAM; no numpy dependencies.
Traffic taskIds include local TIMERs (Prefill and dynamically inserted Decode
waits). They retain their index positions but are excluded from ALL network
metrics, expected/completed-flow counts and FCT intersections. Statistics may
include or omit TIMER rows. Network rows are streamed against the DAG's exact
TP-rank/uint32-chunk expansion; TIMERs never consume that expansion.
Both legacy per-token writes and current request-final writes are validated.
Each Decode Compute TIMER is one whole-model token step, not a network flow.
Task-to-request mapping/index is built only once (4 bytes per traffic task).
Only standard library modules are required.
"""
import argparse
from array import array
from collections import Counter
import csv
import heapq
import json
import math
import mmap
import os
from pathlib import Path
import re
import struct
import sys
import tempfile

VERSION = "v15-multi-dir-prefill-ttft-v10-20261113"
# Source snapshot: https://github.com/skifisy/pd-ns3-ub/commit/634eb5557a93ee68e7ddd6c6397c867af4923e1b
# Compared against tools/generate_trace/mooncake_{trace_to_dag,dag_to_traffic}_v15.py.
# Reserve the all-ones index for TIMERs. Network indexes pack a request slot and flags.
TIMER_CODE = (1 << 32) - 1
MAPPING_TIMER_TYPES = frozenset({
    "P_BATCH_BEGIN", "PREFILL_LAYER", "DECODE_READ_COMPUTE_WAIT",
    "DECODE_LAYER", "DECODE_STEP_COMPUTE",
})
U32_MAX = (1 << 32) - 1
SCHEMA = "v14-domain-serial-batch-tp-logical"
OPS = {"PREFIX_READ": "URMA_READ", "STORAGE_TO_D": "URMA_READ",
       "PREFIX_STORE_WRITE": "URMA_WRITE", "DECODE_WRITE": "URMA_WRITE"}
LOAD, FIRST, LAST = 1, 2, 4
PREFILL_READ, PREFILL_FINAL_WRITE = 8, 16
FLAG_BITS = 5
FLAG_MASK = (1 << FLAG_BITS) - 1
LEGACY_LABELS = {"ecmp": "ECMP", "te": "TE", "toe": "TOE", "eps2": "EPS-2L"}
COMMON_FIELDS = ("request_id", "prefill_read_bytes", "decode_read_bytes", "output_tokens")
SCHEME_FIELDS = ("ttft_ms", "decode_load_compute_ms", "tps",
                 "completion_time_ms", "throughput_gbps",
                 "prefill_first_read_start_ms", "prefill_final_write_start_ms")
QUANTILES = (("p50", .5), ("p90", .9), ("p99", .99), ("p999", .999))
SUMMARY_FIELDS = (
    "scheme", "common_valid_ttft_requests", "common_valid_tps_requests",
    "common_valid_throughput_requests", "common_valid_completion_requests",
    "valid_fct_flows", "common_valid_fct_flows",
    "ttft_p50_ms", "ttft_p90_ms", "ttft_p99_ms", "ttft_p999_ms",
    "avg_tps", "avg_throughput_gbps",
    "fct_p50_ms", "fct_p90_ms", "fct_p99_ms", "fct_p999_ms",
)


def csv_header(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        header = [x.strip() for x in next(csv.reader(stream), [])]
    if not header:
        raise ValueError(f"空文件：{path}")
    return header


def csv_rows(path, required):
    with Path(path).open(encoding="utf-8-sig", newline="", buffering=1024 * 1024) as stream:
        reader = csv.reader(stream)
        try:
            header = [x.strip() for x in next(reader)]
        except StopIteration:
            raise ValueError(f"空文件：{path}") from None
        for col in required:
            if header.count(col) != 1:
                raise ValueError(f"{path}：缺少或重复的列 {col}")
        indexes = [header.index(col) for col in required]
        for row in reader:
            if not row:
                continue
            if len(row) != len(header):
                raise ValueError(f"{path} 第 {reader.line_num} 行列数不符")
            yield tuple(row[i].strip() for i in indexes)


def nonnegative_int(text, label):
    if not text or not text.isascii() or not text.isdigit():
        raise ValueError(f"{label} 必须是非负整数，实际为 {text!r}")
    return int(text)


def load_request_metadata(path):
    requests, seen, batches = {}, set(), {}
    cols = ["request_id", "phase", "output_length", "p_batch_id"]
    for rid, phase, output, batch_id in csv_rows(path, cols):
        rid = nonnegative_int(rid, "request_id")
        if rid in seen:
            raise ValueError(f"requests_debug 的请求编号重复：{rid}")
        seen.add(rid)
        if phase.upper() == "WARMUP":
            continue
        if phase.upper() != "FORMAL":
            raise ValueError(f"requests_debug 未知 phase：{phase!r}")
        batch_id = nonnegative_int(batch_id, "requests_debug.p_batch_id")
        batch = batches.setdefault(batch_id, dict(batch_id=batch_id, layer_count=None))
        requests[rid] = dict(
            request_id=rid,
            output_tokens=nonnegative_int(output, "output_length"), dag_write_tokens=0,
            decode_write_mode=None,
            expected=[0, 0, 0], expected_flows=0,
            expected_prefill_reads=0, expected_prefill_final_writes=0,
            prefill_read_bytes=0, decode_read_bytes=0, prefill_batch=batch,
        )
    return requests


def original_expansion(dag, requests, num_layers=None):
    """Validate v15 DAG ordering and emit precisely its TP/uint32 flow expansion."""
    cols = ["task_id", "request_id", "task_class", "task_type", "execution_schema",
            "bytes", "tp_members", "tp_degree", "decode_token_index", "batch_id", "layer_index"]
    header = csv_header(dag)
    write_metadata = [key for key in ("block_index", "kv_extent_kind") if key in header]
    has_count = "layer_count" in header
    if not has_count and num_layers is None:
        last_layer = -1
        for task_type, layer in csv_rows(dag, ["task_type", "layer_index"]):
            if task_type in {"PREFILL_LAYER", "P_LAYER_START", "P_LAYER_READY"}:
                last_layer = max(last_layer, nonnegative_int(layer, "P compute layer_index"))
        if last_layer < 0:
            raise ValueError("紧凑 DAG 无 P 层计算记录；请用 --num-layers 指定模型层数")
        num_layers = last_layer + 1
    previous = -1
    required = cols + (["layer_count"] if has_count else []) + write_metadata
    for row in csv_rows(dag, required):
        tid, rid, kind, stage, schema, total, raw, degree, token, batch_id, layer = row[:11]
        layer_count = row[11] if has_count else str(num_layers)
        tid = nonnegative_int(tid, "DAG task_id")
        if tid <= previous:
            raise ValueError("DAG task_id 必须严格递增；不能使用重排后的 DAG")
        previous = tid
        if schema != SCHEMA:
            raise ValueError(f"仅支持逻辑 TP DAG：{SCHEMA}；实际 {schema!r}")
        if kind.upper() != "NETWORK":
            continue
        rid = nonnegative_int(rid, f"NETWORK task {tid} request_id")
        if stage not in OPS:
            raise ValueError(f"不支持的网络任务类型：{stage}")
        if rid not in requests:
            raise ValueError(f"DAG NETWORK 请求 {rid} 不在 requests_debug 的正式请求中")
        state = requests[rid]
        logical_bytes = nonnegative_int(total, "logical bytes")
        if stage == "PREFIX_READ":
            state["prefill_read_bytes"] += logical_bytes
        elif stage == "STORAGE_TO_D":
            state["decode_read_bytes"] += logical_bytes
        flags = 0
        if stage in {"PREFIX_READ", "PREFIX_STORE_WRITE"}:
            if nonnegative_int(batch_id, "P batch_id") != state["prefill_batch"]["batch_id"]:
                raise ValueError(f"请求 {rid} 的 P batch 与 requests_debug.p_batch_id 不匹配")
            count = nonnegative_int(layer_count, "P layer_count")
            index = nonnegative_int(layer, "P layer_index")
            if count < 1 or index >= count or (num_layers is not None and count != num_layers):
                raise ValueError(f"DAG task {tid} 的 P 层编号非法")
            batch = state["prefill_batch"]
            if batch["layer_count"] not in (None, count):
                raise ValueError("同一 P batch 模型层数不一致")
            batch["layer_count"] = count
            if stage == "PREFIX_READ":
                flags = PREFILL_READ
            elif index == count - 1:
                flags = PREFILL_FINAL_WRITE
        if stage == "STORAGE_TO_D":
            if state["output_tokens"] == 0:
                raise ValueError(f"请求 {rid} output_length=0 却有 Decode 读取")
            if nonnegative_int(token, "STORAGE_TO_D decode_token_index") != 0:
                raise ValueError("当前模型只支持首个 Decode token 加载输入 KV")
            flags = LOAD
        elif stage == "DECODE_WRITE":
            token = nonnegative_int(token, "DECODE_WRITE decode_token_index")
            metadata = dict(zip(write_metadata, row[len(cols) + int(has_count):]))
            final_write = (metadata.get("block_index") == "-1" or
                           metadata.get("kv_extent_kind") == "all_output_tokens_all_layers")
            if final_write:
                # Compact v9 DAGs use block_index=-1; full DAGs also carry
                # kv_extent_kind. Never infer aggregation just from a missing
                # token prefix, which could instead be a truncated old DAG.
                if (state["dag_write_tokens"] != 0 or state["decode_write_mode"] is not None or
                        state["output_tokens"] == 0 or token != state["output_tokens"] - 1 or
                        layer != "-1" or
                        ("block_index" in metadata and metadata["block_index"] != "-1") or
                        metadata.get("kv_extent_kind", "") not in ("", "all_output_tokens_all_layers")):
                    raise ValueError(f"请求 {rid} 的统一 Decode 写回重复、混用或与末 token/范围不符")
                state["decode_write_mode"] = "request-final"
                state["dag_write_tokens"] = state["output_tokens"]
            else:
                if (state["decode_write_mode"] not in (None, "per-token") or
                        token != state["dag_write_tokens"] or token >= state["output_tokens"]):
                    raise ValueError(f"请求 {rid} 的 Decode token 索引不连续、重复或超出 output_length")
                state["decode_write_mode"] = "per-token"
                state["dag_write_tokens"] += 1
            if token == 0:
                flags |= FIRST
            if token == state["output_tokens"] - 1:
                flags |= LAST
        members = json.loads(raw)
        degree = nonnegative_int(degree, "tp_degree")
        if not isinstance(members, list) or not members or degree < 1:
            raise ValueError(f"DAG task {tid} TP 成员为空或 TP 大小不合法")
        size_sum, previous_rank = 0, -1
        for member in members:
            if (not isinstance(member, list) or len(member) != 6 or
                    any(type(x) is not int for x in member)):
                raise ValueError(f"DAG task {tid} TP 成员格式错误")
            rank, src, sp, dst, dp, size = member
            if min(src, sp, dst, dp) < 0 or size <= 0 or not previous_rank < rank < degree:
                raise ValueError(f"DAG task {tid} TP 成员值或排列错误")
            previous_rank = rank
            size_sum += size
            op = OPS[stage]
            if op == "URMA_READ":
                src, dst = dst, src
            remaining = size
            while remaining:
                chunk = min(remaining, U32_MAX)
                yield (rid, src, dst, op, chunk, flags, stage, str(token), batch_id, layer,
                       tid, rank, (size - remaining) // U32_MAX, (size + U32_MAX - 1) // U32_MAX)
                remaining -= chunk
        if size_sum != logical_bytes:
            raise ValueError(f"DAG task {tid} TP 字节合计不等于 bytes")
    for state in requests.values():
        if state["dag_write_tokens"] != state["output_tokens"]:
            raise ValueError(f"请求 {state['request_id']} 的 DAG Decode 写 token 数与 output_length 不符")


def build_index(args, requests, index_path):
    """Index EVERY traffic taskId; only NETWORK rows consume DAG expansion.

    A Decode wait can appear before any TP member, and a cached wait may not
    emit another row. Its count/position cannot be inferred from DAG task
    counts. Read actual traffic/mapping order instead, preserving TIMER holes.
    """
    states = list(requests.values())
    slots = {s["request_id"]: i for i, s in enumerate(states)}
    if len(states) >= (1 << (32 - FLAG_BITS)):
        raise ValueError("请求数量超出保留 TIMER 标记后的 4 字节任务索引范围")
    expansion = original_expansion(args.task_dag, requests, args.num_layers)
    cols = (["taskId", "request_id", "task_type", "decode_token_index", "batch_id", "layer_index"]
            if args.mapping else ["taskId", "sourceNode", "destNode", "opType", "dataSize(Byte)"])
    header = set(csv_header(args.mapping)) if args.mapping else set()
    # The repository's full mapping also carries these exact identity fields.
    # Accept legacy mappings without them; validate every available field.
    optional = [key for key in ("dag_task_id", "tp_rank", "chunk_index", "chunk_count")
                if key in header]
    first, task_count, flow_count, timer_count = None, 0, 0, 0
    timer_types = Counter()
    with index_path.open("wb", buffering=1024 * 1024) as out:
        buf = array("I")
        if buf.itemsize != 4:
            raise ValueError("当前平台无法创建 4 字节 unsigned int 索引")
        for values in csv_rows(args.mapping or args.traffic, cols + optional):
            row = values[:len(cols)]
            tid = nonnegative_int(row[0], "traffic/mapping taskId")
            if tid > U32_MAX:
                raise ValueError(f"taskId={tid} 超出转换器 uint32 范围")
            if first is None:
                first = tid
            if tid != first + task_count:
                raise ValueError("traffic/mapping taskId 必须连续递增，不能重排或删行")
            if args.mapping:
                stage = row[2]
                is_timer = stage in MAPPING_TIMER_TYPES
                if stage not in OPS and not is_timer:
                    raise ValueError(f"taskId={tid} mapping 未知任务类型：{stage!r}")
                if is_timer:
                    if stage == "DECODE_READ_COMPUTE_WAIT":
                        rid = nonnegative_int(row[1], "Decode TIMER request_id")
                        if rid not in requests:
                            raise ValueError(f"Decode TIMER 请求 {rid} 不在正式请求中")
                    elif row[1]:
                        raise ValueError(f"taskId={tid} 计算/批次 TIMER 的 request_id 应为空")
                    timer_types[stage] += 1
            else:
                src_actual = nonnegative_int(row[1], "sourceNode")
                dst_actual = nonnegative_int(row[2], "destNode")
                actual_size = nonnegative_int(row[4], "dataSize(Byte)")
                is_timer = row[3] == "TIMER"
                if is_timer and (src_actual != dst_actual or actual_size != 0):
                    raise ValueError(f"taskId={tid} TIMER 必须为同节点、零字节的本地任务")
            if is_timer:
                code = TIMER_CODE
                timer_count += 1
            else:
                try:
                    (rid, src, dst, op, size, flags, stage, token, batch_id, layer,
                     dag_tid, rank, chunk_index, chunk_count) = next(expansion)
                except StopIteration:
                    raise ValueError(f"taskId={tid}：traffic/mapping 网络流比 DAG 展开结果多") from None
                if args.mapping:
                    if nonnegative_int(row[1], "mapping request_id") != rid:
                        raise ValueError(f"taskId={tid} 的 mapping 请求归属与 DAG 不匹配")
                    if tuple(row[2:6]) != (stage, token, batch_id, layer):
                        raise ValueError(f"taskId={tid} mapping 类型/token/batch/层编号与 DAG 不匹配")
                    expected_ids = dict(dag_task_id=dag_tid, tp_rank=rank,
                                        chunk_index=chunk_index, chunk_count=chunk_count)
                    for key, value in zip(optional, values[len(cols):]):
                        if nonnegative_int(value, f"mapping {key}") != expected_ids[key]:
                            raise ValueError(f"taskId={tid} mapping {key} 与 DAG 展开不匹配")
                else:
                    actual = (src_actual, dst_actual, row[3], actual_size)
                    expected = (src, dst, op, 1 + (size - 1) // args.data_size_divisor)
                    if actual != expected:
                        raise ValueError(f"taskId={tid} 与 DAG 不匹配：traffic={actual}，预期={expected}；"
                                         "请核对数据来源及 --data-size-divisor")
                state = requests[rid]
                state["expected_flows"] += 1
                if flags == PREFILL_READ:
                    state["expected_prefill_reads"] += 1
                elif flags == PREFILL_FINAL_WRITE:
                    state["expected_prefill_final_writes"] += 1
                for i, flag in enumerate((LOAD, FIRST, LAST)):
                    if flags & flag:
                        state["expected"][i] += 1
                code = (slots[rid] << FLAG_BITS) | flags
                flow_count += 1
            buf.append(code)
            task_count += 1
            if len(buf) >= 262144:
                buf.tofile(out)
                buf = array("I")
            if args.progress_every and task_count % args.progress_every == 0:
                print(f"建立公共任务索引：{task_count:,} 条任务，"
                      f"{flow_count:,} 条网络流，{timer_count:,} 条 TIMER",
                      file=sys.stderr, flush=True)
        buf.tofile(out)
        try:
            next(expansion)
        except StopIteration:
            pass
        else:
            raise ValueError("traffic/mapping 网络流比 DAG 展开结果少")
    print(f"公共索引：任务 {task_count:,}，网络流 {flow_count:,}，"
          f"排除 TIMER {timer_count:,}" +
          (f"（{dict(timer_types)}）" if timer_types else ""), file=sys.stderr)
    print(f"Decode 写回模式：{dict(Counter(s['decode_write_mode'] or 'no-output' for s in states))}",
          file=sys.stderr)
    return states, first, task_count, flow_count


def new_scheme_states(base_states):
    return [dict(observed=[0, 0, 0], completed_flows=0, statistics_rows=0,
                 first_flow_start=None, last_flow_finish=None,
                 read_start=None, first_ready=None, last_ready=None,
                 started_prefill_reads=0, started_prefill_final_writes=0,
                 prefill_read_start=None, prefill_final_write_start=None)
            for _ in base_states]


def scan_scheme(name, stat_path, base_states, first, count, flow_count, index_path, fct_path, progress_every):
    states = new_scheme_states(base_states)
    seen = bytearray((count + 7) // 8)
    valid_fct = bytearray((count + 7) // 8)
    counters = Counter()
    with fct_path.open("w+b") as fct_file, index_path.open("rb") as index_file:
        fct_file.truncate(count * 8)
        fct = mmap.mmap(fct_file.fileno(), count * 8, access=mmap.ACCESS_WRITE) if count else None
        lookup = mmap.mmap(index_file.fileno(), 0, access=mmap.ACCESS_READ) if count else None
        try:
            for tid, start, end in csv_rows(stat_path,
                                            ["taskId", "taskStartTime(us)", "taskCompletesTime(us)"]):
                tid = nonnegative_int(tid, f"{name} statistics taskId")
                pos = tid - first if first is not None else -1
                if not 0 <= pos < count:
                    raise ValueError(f"{name} taskId={tid} 不在公共任务索引范围内")
                byte, bit = pos >> 3, 1 << (pos & 7)
                if seen[byte] & bit:
                    raise ValueError(f"{name} statistics taskId={tid} 重复")
                seen[byte] |= bit
                counters["total_rows"] += 1
                if progress_every and counters["total_rows"] % progress_every == 0:
                    print(f"扫描 {name.upper()}：{counters['total_rows']:,} 条任务记录",
                          file=sys.stderr, flush=True)
                code = struct.unpack_from("=I", lookup, pos * 4)[0]
                if code == TIMER_CODE:
                    counters["timer_rows"] += 1
                    continue
                state, flags = states[code >> FLAG_BITS], code & FLAG_MASK
                state["statistics_rows"] += 1
                counters["rows"] += 1
                try:
                    st, fin = float(start), float(end)
                    valid = (math.isfinite(st) and math.isfinite(fin) and st >= 0 and fin >= 0
                             and ((fin == 0 and st > 0) or (fin > 0 and fin >= st)))
                except ValueError:
                    valid = False
                if not valid:
                    counters["invalid_time_rows"] += 1
                    continue
                # Start-based metrics can use pending flows. FCT requires completion.
                if fin == 0:
                    counters["started_unfinished"] += 1
                else:
                    state["completed_flows"] += 1
                    counters["completed_flows"] += 1
                    valid_fct[byte] |= bit
                    struct.pack_into("=d", fct, pos * 8, (fin - st) / 1000.0)
                if flags == PREFILL_READ:
                    state["started_prefill_reads"] += 1
                    old = state["prefill_read_start"]
                    state["prefill_read_start"] = st if old is None else min(old, st)
                elif flags == PREFILL_FINAL_WRITE:
                    state["started_prefill_final_writes"] += 1
                    old = state["prefill_final_write_start"]
                    state["prefill_final_write_start"] = st if old is None else min(old, st)
                old = state["first_flow_start"]
                state["first_flow_start"] = st if old is None else min(old, st)
                if fin > 0:
                    old = state["last_flow_finish"]
                    state["last_flow_finish"] = fin if old is None else max(old, fin)
                for i, flag in enumerate((LOAD, FIRST, LAST)):
                    if flags & flag:
                        state["observed"][i] += 1
                if flags & LOAD:
                    old = state["read_start"]
                    state["read_start"] = st if old is None else min(old, st)
                if flags & FIRST:
                    old = state["first_ready"]
                    state["first_ready"] = st if old is None else max(old, st)
                if flags & LAST:
                    old = state["last_ready"]
                    state["last_ready"] = st if old is None else max(old, st)
        finally:
            if lookup is not None:
                lookup.close()
            if fct is not None:
                fct.flush()
                fct.close()
    counters["missing_rows"] = flow_count - counters["rows"]
    counters["missing_timer_rows"] = count - flow_count - counters["timer_rows"]
    return states, valid_fct, counters


def compute_scheme_metrics(name, base_states, states):
    results = {}
    counters = Counter()
    for meta, state in zip(base_states, states):
        rid = meta["request_id"]
        result = {field: None for field in SCHEME_FIELDS}
        read_start = state["prefill_read_start"]
        write_start = state["prefill_final_write_start"]
        read_observed = read_start is not None and (
            state["started_prefill_reads"] == meta["expected_prefill_reads"])
        write_observed = write_start is not None and (
            state["started_prefill_final_writes"] == meta["expected_prefill_final_writes"])
        if read_observed:
            result["prefill_first_read_start_ms"] = read_start / 1000.0
        if write_observed:
            result["prefill_final_write_start_ms"] = write_start / 1000.0
        if read_observed and write_observed and write_start >= read_start:
            result["ttft_ms"] = (write_start - read_start) / 1000.0
        if meta["output_tokens"] > 0:
            ready = [expected > 0 and expected == observed
                     for expected, observed in zip(meta["expected"], state["observed"])]
            if ready[0] and ready[2]:
                if state["last_ready"] < state["read_start"]:
                    raise ValueError(f"{name} 请求 {rid} 末 token 写早于首次 D 读")
                if ready[1] and state["last_ready"] < state["first_ready"]:
                    raise ValueError(f"{name} 请求 {rid} 末 token 写早于首 token 写")
                duration = (state["last_ready"] - state["read_start"]) / 1000.0
                result["decode_load_compute_ms"] = duration
                if duration > 0:
                    result["tps"] = meta["output_tokens"] * 1000.0 / duration
        if (meta["expected_flows"] > 0 and
                state["completed_flows"] == meta["expected_flows"]):
            start, finish = state["first_flow_start"], state["last_flow_finish"]
            if start is None or finish is None or finish < start:
                raise ValueError(f"{name} 请求 {rid} 的 Completion 时间无效")
            completion = (finish - start) / 1000.0
            result["completion_time_ms"] = completion
            if completion > 0:
                total_read_bytes = meta["prefill_read_bytes"] + meta["decode_read_bytes"]
                result["throughput_gbps"] = total_read_bytes * 8.0 / (completion * 1e6)
        for field, value in result.items():
            if value is not None:
                counters[f"valid_{field}"] += 1
        results[rid] = result
    return results, counters


def linear_percentiles(values):
    if not values:
        return {name: None for name, _ in QUANTILES}
    values.sort()
    n = len(values)
    out = {}
    for name, p in QUANTILES:
        position = (n - 1) * p
        lo = int(math.floor(position))
        hi = int(math.ceil(position))
        weight = position - lo
        out[name] = values[lo] * (1 - weight) + values[hi] * weight
    return out


def read_sorted_binary(path, chunk_size=16384):
    """Yield native-endian doubles sequentially, bounded memory per sorted run."""
    with path.open("rb") as stream:
        while True:
            buf = array("d")
            data = stream.read(chunk_size * 8)
            if not data:
                break
            if len(data) % 8:
                raise ValueError(f"损坏的 FCT 排序临时文件：{path}")
            buf.frombytes(data)
            yield from buf


def sorted_run_percentiles(paths, n):
    if n == 0:
        return {name: None for name, _ in QUANTILES}
    targets = {}
    for name, p in QUANTILES:
        at = (n - 1) * p
        targets[name] = (int(math.floor(at)), int(math.ceil(at)), at % 1)
    wanted = {i for lo, hi, _ in targets.values() for i in (lo, hi)}
    found = {}
    if not paths:
        raise ValueError("FCT 记录数非零但排序临时文件为空")
    if len(paths) == 1:
        stream = read_sorted_binary(paths[0])
    else:
        stream = heapq.merge(*(read_sorted_binary(path) for path in paths))
    try:
        for i, fct in enumerate(stream):
            if i in wanted:
                found[i] = fct
            if i >= max(wanted):
                break
    finally:
        stream.close() if hasattr(stream, "close") else None
    if len(found) != len(wanted):
        raise ValueError("FCT 排序记录数不足，无法计算分位数")
    return {name: found[lo] * (1 - w) + found[hi] * w
            for name, (lo, hi, w) in targets.items()}


def common_fct_percentiles(fct_paths, common_mask, task_count, temp_dir, chunk_size):
    """Sort one run at a time on the shared set of completed network taskIds."""
    if not common_mask:
        return 0, {name: sorted_run_percentiles([], 0) for name in fct_paths}
    bitmap = common_mask.to_bytes((task_count + 7) // 8, "little")
    popcounts = [bin(byte).count("1") for byte in range(256)]
    common_count = sum(popcounts[byte] for byte in bitmap)
    output = {}
    for name, fct_path in fct_paths.items():
        with tempfile.TemporaryDirectory(prefix="fct_sort_", dir=temp_dir) as sort_dir:
            paths, values = [], []

            def flush_run():
                values.sort()
                path = Path(sort_dir) / f"{len(paths):05d}.bin"
                with path.open("wb", buffering=1024 * 1024) as out:
                    array("d", values).tofile(out)
                paths.append(path)
                values.clear()

            with fct_path.open("rb") as stream, mmap.mmap(
                stream.fileno(), 0, access=mmap.ACCESS_READ
            ) as fct:
                for byte_idx, mask in enumerate(bitmap):
                    while mask:
                        bit = mask & -mask
                        pos = (byte_idx << 3) + bit.bit_length() - 1
                        values.append(struct.unpack_from("=d", fct, pos * 8)[0])
                        if len(values) >= chunk_size:
                            flush_run()
                        mask ^= bit
            if values:
                flush_run()
            output[name] = sorted_run_percentiles(paths, common_count)
        fct_path.unlink()
    return common_count, output


def fmt(value):
    if value is None:
        return ""
    if not math.isfinite(value):
        raise ValueError("计算产生无穷或 NaN 值")
    return f"{value:.9f}".rstrip("0").rstrip(".")


def write_csv_temp(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8-sig", newline="",
                                     dir=path.parent, prefix=f".{path.stem}_",
                                     suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            writer = csv.DictWriter(stream, fieldnames=header, extrasaction="raise")
            writer.writeheader()
            writer.writerows(rows)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    return temporary


def make_detail_rows(base_states, metrics):
    for meta in sorted(base_states, key=lambda x: x["request_id"]):
        rid = meta["request_id"]
        row = {k: meta[k] for k in COMMON_FIELDS}
        for scheme, results in metrics.items():
            for field in SCHEME_FIELDS:
                row[f"{scheme}_{field}"] = fmt(results[rid][field])
        yield row


def make_summary_rows(base_states, metrics, labels, fct_quantiles, valid_fct_counts, common_flow_count):
    # Use a different common-valid request set per metric, so an incomplete
    # trailing WRITE cannot invalidate otherwise available TTFT/TPS.
    request_ids = [meta["request_id"] for meta in base_states]
    common = {}
    for field in ("ttft_ms", "tps", "throughput_gbps", "completion_time_ms"):
        common[field] = [rid for rid in request_ids if all(
            results[rid][field] is not None for results in metrics.values())]
    for scheme, label in labels.items():
        ttft = linear_percentiles([metrics[scheme][rid]["ttft_ms"] for rid in common["ttft_ms"]])
        tps_values = [metrics[scheme][rid]["tps"] for rid in common["tps"]]
        th_values = [metrics[scheme][rid]["throughput_gbps"]
                     for rid in common["throughput_gbps"]]
        row = dict(
            scheme=label,
            common_valid_ttft_requests=len(common["ttft_ms"]),
            common_valid_tps_requests=len(common["tps"]),
            common_valid_throughput_requests=len(common["throughput_gbps"]),
            common_valid_completion_requests=len(common["completion_time_ms"]),
            valid_fct_flows=valid_fct_counts[scheme],
            common_valid_fct_flows=common_flow_count,
            avg_tps=fmt(math.fsum(tps_values) / len(tps_values)) if tps_values else "",
            avg_throughput_gbps=fmt(math.fsum(th_values) / len(th_values)) if th_values else "",
        )
        for tag, _ in QUANTILES:
            row[f"ttft_{tag}_ms"] = fmt(ttft[tag])
            row[f"fct_{tag}_ms"] = fmt(fct_quantiles[scheme][tag])
        yield row


def resolve_stat_path(directory, override, scheme):
    if override is not None:
        return override.resolve()
    directory = directory.resolve()
    nested = directory / "output" / "task_statistics.csv"
    direct = directory / "task_statistics.csv"
    if nested.is_file():
        return nested
    if direct.is_file():
        return direct
    raise ValueError(f"找不到 {scheme} 统计文件：{nested} 或 {direct}")


def resolve_runs(args, parser):
    paths, labels = {}, {}
    for scheme, label in LEGACY_LABELS.items():
        directories = getattr(args, f"{scheme}_dir")
        override = getattr(args, f"{scheme}_statistics")
        if scheme not in ("te", "toe"):
            directories = [directories] if directories is not None else []
        if override is not None or len(directories) == 1:
            directory = directories[0] if len(directories) == 1 else None
            paths[scheme] = resolve_stat_path(directory, override, label)
            labels[scheme] = label

        named = {}
        for spec in getattr(args, scheme, ()):
            name, sep, raw_path = spec.partition("=")
            if not sep or not raw_path.strip() or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", name):
                parser.error(f"--{scheme} 需要 NAME=DIR_OR_CSV。NAME 以字母或数字开头，"
                             "最多 64 个字符，可用字母、数字、点、下划线和连字符")
            key = f"{scheme}_{name}"
            if key in named:
                parser.error(f"--{scheme} 版本名重复：{name}")
            path = Path(raw_path).resolve()
            named[key] = path if path.is_file() else resolve_stat_path(path, None, key)

        if len(directories) > 1:
            for directory in directories:
                name = re.sub(r"[^A-Za-z0-9._-]+", "_", directory.name).strip("._-")[:64] or "run"
                key = f"{scheme}_{name}"
                suffix = 2
                while key in paths or key in named:
                    ending = f"_{suffix}"
                    key = f"{scheme}_{name[:64 - len(ending)]}{ending}"
                    suffix += 1
                paths[key] = resolve_stat_path(directory, None, key)
                labels[key] = key

        paths.update(named)
        labels.update((key, key) for key in named)
    if len(paths) < 2:
        parser.error("请至少指定两个对比结果，例如 --te-dir /data/te_a --te-dir /data/te_b")
    return paths, labels


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("case_dir", nargs="?", type=Path, help="公共 CASE 目录（兼容原脚本的位置参数）")
    p.add_argument("--case", type=Path, help="公共 CASE 目录")
    p.add_argument("--task-dag", type=Path)
    p.add_argument("--traffic", type=Path)
    p.add_argument("--mapping", type=Path, help="v15 完整 mapping；替代 traffic.csv")
    p.add_argument("--requests-debug", type=Path)
    p.add_argument("--ecmp-dir", type=Path, help="ECMP 结果目录，含 output/task_statistics.csv")
    for scheme in ("te", "toe"):
        p.add_argument(f"--{scheme}-dir", type=Path, action="append", default=[], metavar="DIR",
                       help=f"添加 {LEGACY_LABELS[scheme]} 结果目录，可重复指定")
    for scheme in ("ecmp", "te", "toe"):
        p.add_argument(f"--{scheme}-statistics", type=Path,
                       help=f"直接指定 {LEGACY_LABELS[scheme]} 的 statistics CSV")
    for scheme in ("te", "toe"):
        p.add_argument(f"--{scheme}", action="append", default=[], metavar="NAME=DIR_OR_CSV",
                       help=f"添加一个具名 {LEGACY_LABELS[scheme]} 版本，可重复指定")
    p.add_argument("--eps2-dir", "--eps-2layer-dir", "--two-layer-eps-dir",
                   dest="eps2_dir", type=Path,
                   help="两层 EPS 结果目录，含 output/task_statistics.csv")
    p.add_argument("--eps2-statistics", "--eps-2layer-statistics",
                   dest="eps2_statistics", type=Path,
                   help="覆盖两层 EPS 的 task_statistics.csv 路径")
    p.add_argument("--out-dir", type=Path, help="两个对比 CSV 的输出目录，默认 CASE/output/compare")
    p.add_argument("--data-size-divisor", type=int, default=1)
    p.add_argument("--num-layers", type=int)
    p.add_argument("--work-dir", type=Path, help="大型临时文件目录（建议有足够空闲空间的 /mnt 路径）")
    p.add_argument("--fct-sort-chunk", type=int, default=500000,
                   help="每个方案一次在内存排序的 FCT 数量，默认 500000")
    p.add_argument("--progress-every", type=int, default=1000000)
    p.add_argument("--version", action="version", version=VERSION)
    args = p.parse_args()
    # Native dependOnPhases / DAG depends_on fields can exceed Python's 128 KiB
    # CSV default in large batches, even though we only select other columns.
    limit = sys.maxsize
    while True:
        try:
            csv.field_size_limit(limit)
            break
        except OverflowError:
            limit //= 10
    if args.case is not None and args.case_dir is not None and args.case.resolve() != args.case_dir.resolve():
        p.error("位置参数 CASE 与 --case 不能指定不同目录")
    case = args.case or args.case_dir
    if case is None:
        p.error("需要 CASE 路径：--case DIR 或位置参数 DIR")
    if (args.data_size_divisor <= 0 or args.fct_sort_chunk <= 0 or
            args.progress_every < 0 or (args.num_layers is not None and args.num_layers <= 0)):
        p.error("divisor、sort-chunk、num-layers 必须 > 0；progress-every 必须 >= 0")
    case = case.resolve()
    args.task_dag = (args.task_dag or case / "task_dag.csv").resolve()
    args.traffic = (args.traffic or case / "traffic.csv").resolve()
    args.requests_debug = (args.requests_debug or case / "requests_debug.csv").resolve()
    out_dir = (args.out_dir or case / "output" / "compare").resolve()
    out_detail = out_dir / "request_comparison.csv"
    out_summary = out_dir / "summary_comparison.csv"
    stats, labels = resolve_runs(args, p)
    if args.mapping is not None:
        args.mapping = args.mapping.resolve()
    inputs = [args.task_dag, args.requests_debug, args.mapping or args.traffic, *stats.values()]
    for path in inputs:
        if not path.is_file():
            raise ValueError(f"输入文件不存在：{path}")
    if out_detail in inputs or out_summary in inputs or out_detail == out_summary:
        p.error("对比输出不能覆盖输入文件")
    fingerprints = {path: (path.stat().st_size, path.stat().st_mtime_ns) for path in inputs}
    out_dir.mkdir(parents=True, exist_ok=True)
    work = (args.work_dir or out_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)
    requests = load_request_metadata(args.requests_debug)
    with tempfile.TemporaryDirectory(prefix="request_compare_", dir=work) as tmp:
        temp_dir = Path(tmp)
        index_path = temp_dir / "flow_request_index.bin"
        base_states, first, task_count, flow_count = build_index(args, requests, index_path)
        metrics, fct_files, valid_fct_counts = {}, {}, {}
        common_fct_mask = None
        for scheme, stat_path in stats.items():
            print(f"{labels[scheme]} 统计文件 {stat_path}", file=sys.stderr, flush=True)
            fct_path = temp_dir / f"{scheme}_fct_ms.bin"
            scheme_states, bitmap, counters = scan_scheme(
                scheme, stat_path, base_states, first, task_count, flow_count,
                index_path, fct_path, args.progress_every,
            )
            metrics[scheme], metric_counts = compute_scheme_metrics(scheme, base_states, scheme_states)
            print(f"{labels[scheme]} TTFT 有效请求 {metric_counts['valid_ttft_ms']:,}；"
                  f"首次 Prefill Read 时间有效 {metric_counts['valid_prefill_first_read_start_ms']:,}，"
                  f"末层 Write 开始时间有效 {metric_counts['valid_prefill_final_write_start_ms']:,}",
                  file=sys.stderr)
            mask = int.from_bytes(bitmap, "little")
            common_fct_mask = mask if common_fct_mask is None else common_fct_mask & mask
            fct_files[scheme] = fct_path
            valid_fct_counts[scheme] = counters["completed_flows"]
            del scheme_states, bitmap, mask
            print(f"{labels[scheme]}：完成流 {counters['completed_flows']:,} / {flow_count:,}；"
                  f"无效记录 {counters['invalid_time_rows']:,}，"
                  f"已开始未完成 {counters['started_unfinished']:,}，"
                  f"缺失网络流 {counters['missing_rows']:,}，"
                  f"已排除 TIMER 记录 {counters['timer_rows']:,}", file=sys.stderr)
        common_flow_count, fct_quantiles = common_fct_percentiles(
            fct_files, common_fct_mask, task_count, temp_dir, args.fct_sort_chunk,
        )
        detail_fields = list(COMMON_FIELDS) + [f"{s}_{field}"
                                                for s in stats for field in SCHEME_FIELDS]
        tmp_detail = tmp_summary = None
        try:
            tmp_detail = write_csv_temp(out_detail, detail_fields, make_detail_rows(base_states, metrics))
            tmp_summary = write_csv_temp(out_summary, SUMMARY_FIELDS, make_summary_rows(
                base_states, metrics, labels, fct_quantiles, valid_fct_counts,
                common_flow_count,
            ))
            for path in inputs:
                if fingerprints[path] != (path.stat().st_size, path.stat().st_mtime_ns):
                    raise ValueError(f"统计期间输入发生变化：{path}")
            os.replace(tmp_detail, out_detail)
            tmp_detail = None
            os.replace(tmp_summary, out_summary)
            tmp_summary = None
        finally:
            if tmp_detail is not None:
                tmp_detail.unlink(missing_ok=True)
            if tmp_summary is not None:
                tmp_summary.unlink(missing_ok=True)
    print(f"正式 Request：{len(base_states):,}；预期网络流：{flow_count:,}；"
          f"{len(stats)} 个结果共同完成网络流：{common_flow_count:,}")
    print(f"Request 明细：{out_detail}")
    print(f"统计对比：{out_summary}")
    print("TTFT 为同一请求末层 Prefill Write 最早启动减去首次 Prefill Read 启动，"
          "缺少观测则留空；FCT 是逐流 finish-start；"
          "吞吐为未缩放逻辑读字节数 / Request 网络流时间跨度。")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, csv.Error, OverflowError) as exc:
        print(f"统计失败：{exc}", file=sys.stderr)
        sys.exit(1)
