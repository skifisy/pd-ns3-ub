"""V15 logical TP DAG -> native nine-column ns-3-ub traffic.

Prefill emits zero-byte TIMER records for batch-start gates and layer Compute.
Read and Write each use a separate Batch+Layer phase.
Prefill preserves all required predecessor phases
(AND semantics). Decode keeps offline batches and request-wave/per-rank
grouping for writes. Every input Read waits only for its own Prefill final phase.
All initial Reads of a Decode batch share one completion phase, across requests,
layers, blocks and TP ranks. Decode Compute stays in the logical DAG but is
folded into downstream network delay; no Decode TIMER rows are emitted.
All required predecessor phases are retained. Unequal accumulated lags require
conservative one-delay padding (or rejection with --timing-policy error).
Decode Read delay
is storage latency only; batch-start forecasts are not replay constraints.

Logical TP rows expand using tp_members; all physical rank flows are retained.
The nine-column traffic.csv format is unchanged. TIMER is local control work
and never enters the network-send path.
traffic_debug.csv mirrors every traffic row with an extra request_id column;
batch-owned TIMERs have an empty request_id.
"""

import argparse
from array import array
from contextlib import ExitStack
import csv
import heapq
import json
import os
import shutil
import re
import sqlite3
import sys
import tempfile
import time
from collections import Counter, OrderedDict
from decimal import Decimal, InvalidOperation, ROUND_CEILING
from pathlib import Path

UINT32_MAX = (1 << 32) - 1
SCHEMA = "v12-revised-stable-worker-unlimited"
V14_SCHEMA = "v14-domain-serial-batch-tp-sharded"
LOGICAL_SCHEMA = "v14-domain-serial-batch-tp-logical"
VERSION = "v15-prefill-traffic-debug-decode-folded-compute-20261109-v10"

PREFILL_TASK_TYPES = frozenset({
    "P_BATCH_BEGIN", "P_LAYER_START", "PREFILL_LAYER", "P_LAYER_READY",
    "PREFIX_READ", "PREFIX_STORE_WRITE", "P_COMPUTE_DONE", "P_DONE",
    "INPUT_BLOCK_READY",
})
DECODE_TASK_TYPES = frozenset({
    "D_BATCH_BEGIN", "D_LAYER_START", "DECODE_LAYER", "D_LAYER_READY",
    "STORAGE_TO_D", "DECODE_WRITE", "D_COMPUTE_DONE", "D_WRITES_DONE", "DECODE_STEP_COMPUTE", "D_STEP_START",
})


def validate_prefill_metadata(row):
    """Reject incomplete Prefill metadata before expansion can prune its AND gate."""
    stage = row.get("task_type", "").strip()
    batch_type = (row.get("batch_type") or "").strip().upper()
    location = f"physical line {row['_line']}, task {row.get('task_id')} ({stage})"
    if stage in PREFILL_TASK_TYPES and batch_type != "PREFILL":
        raise ValueError(
            f"{location}: expected batch_type=PREFILL, got {batch_type!r}; "
            "regenerate task_dag.csv with the updated V15 generator. "
            "Prefill must not fall back to the Decode timing/grouping policy"
        )
    if batch_type != "PREFILL":
        return
    try:
        integer(row.get("batch_id"), "Prefill batch_id")
        if stage in {"PREFIX_READ", "PREFIX_STORE_WRITE", "PREFILL_LAYER"}:
            integer(row.get("layer_index"), "Prefill layer_index")
        if stage == "PREFIX_STORE_WRITE":
            integer(row.get("request_id"), "Prefill request_id")
            integer(row.get("block_index"), "Prefill block_index")
        if stage in {"P_BATCH_BEGIN", "PREFILL_LAYER"}:
            # CSV node '0' is a valid execution context.
            node = row.get("src_node") or row.get("p_node")
            integer(node, "Prefill TIMER sourceNode (src_node/p_node)")
    except ValueError as exc:
        raise ValueError(f"{location}: {exc}; regenerate task_dag.csv") from None


def validate_decode_metadata(row):
    stage = row.get("task_type", "").strip()
    batch_type = (row.get("batch_type") or "").strip().upper()
    location = f"physical line {row['_line']}, task {row.get('task_id')} ({stage})"
    if stage in DECODE_TASK_TYPES and batch_type != "DECODE":
        raise ValueError(f"{location}: expected batch_type=DECODE; regenerate task_dag.csv")
    if batch_type != "DECODE":
        return
    try:
        integer(row.get("batch_id"), "Decode batch_id")
        if stage in {"DECODE_LAYER", "STORAGE_TO_D"}:
            integer(row.get("layer_index"), "Decode layer_index")
        if stage in {"DECODE_LAYER", "DECODE_STEP_COMPUTE"}:
            integer(row.get("src_node") or row.get("d_node"), "Decode DAG sourceNode (src_node/d_node)")
        if stage == "DECODE_STEP_COMPUTE" and str(row.get("layer_index")) != "-1":
            raise ValueError("whole-step Decode Compute must have layer_index=-1")
    except ValueError as exc:
        raise ValueError(f"{location}: {exc}; regenerate task_dag.csv with the v8 generator") from None


class Progress:
    """Throttled standard-library progress bar; status goes only to stderr."""

    def __init__(self, label, total, unit="req", enabled=True, interval=0.5, stream=None):
        if not 0 < float(interval) < float("inf"):
            raise ValueError("progress interval must be finite and positive")
        self.label, self.total, self.unit = label, max(0, int(total)), unit
        self.enabled = bool(enabled)
        self.stream = sys.stderr if stream is None else stream
        self.tty = bool(getattr(self.stream, "isatty", lambda: False)())
        # Redirected logs get occasional complete lines instead of carriage returns.
        self.interval = float(interval) if self.tty else max(5.0, float(interval))
        self.started = time.monotonic()
        self.last_update, self.last_width, self.current, self.detail = 0.0, 0, 0, ""

    @staticmethod
    def duration(seconds):
        if seconds is None:
            return "--:--:--"
        seconds = max(0, int(seconds))
        hours, remainder = divmod(seconds, 3600)
        minutes, seconds = divmod(remainder, 60)
        return f"{hours:02d}:{minutes:02d}:{seconds:02d}"

    def update(self, completed, detail="", force=False, status=""):
        self.current = min(self.total, max(0, int(completed)))
        self.detail = detail
        if not self.enabled:
            return
        now = time.monotonic()
        if not force and now - self.last_update < self.interval:
            return
        elapsed = max(0.0, now - self.started)
        speed = self.current / elapsed if elapsed > 0 else 0.0
        fraction = self.current / self.total if self.total else 1.0
        eta = (self.total - self.current) / speed if speed > 0 else None
        if self.current == self.total:
            eta = 0.0
        filled = min(24, int(fraction * 24))
        bar = "#" * filled + "-" * (24 - filled)
        if self.unit == "bytes":
            scale, suffix = ((1048576, "MiB") if self.total >= 1048576
                             else (1024, "KiB") if self.total >= 1024 else (1, "B"))
            count = f"{self.current / scale:.1f}/{self.total / scale:.1f} {suffix}"
            rate = f"{speed / scale:.1f} {suffix}/s"
        else:
            count = f"{self.current:,}/{self.total:,} {self.unit}"
            rate = f"{speed:.1f} {self.unit}/s"
        line = (f"{self.label:<11} [{bar}] {fraction * 100:6.2f}% | {count}"
                f" | {rate} | elapsed {self.duration(elapsed)} | ETA {self.duration(eta)}")
        if detail:
            line += " | " + detail
        if status:
            line += " | " + status
        if self.tty:
            columns = max(40, shutil.get_terminal_size(fallback=(80, 24)).columns)
            width = min(24, max(8, columns // 10))
            filled = min(width, int(fraction * width))
            bar = "#" * filled + "-" * (width - filled)
            suffix = " | " + status if status else ""
            line = (f"{self.label:<11} [{bar}] {fraction * 100:6.2f}%"
                    f" | {count} | ETA {self.duration(eta)}")
            if len(line + suffix) >= columns:
                line = f"{self.label} {fraction * 100:.2f}% | {count} | ETA {self.duration(eta)}"
            for extra in (detail, rate, "elapsed " + self.duration(elapsed)):
                if extra and len(line + " | " + extra + suffix) < columns:
                    line += " | " + extra
            line = line[:max(0, columns - 1 - len(suffix))] + suffix
            padding = max(0, min(self.last_width, columns - 1) - len(line))
            self.stream.write("\r" + line + " " * padding)
            if status:
                self.stream.write("\n")
            self.last_width = len(line)
        else:
            self.stream.write(line + "\n")
        self.stream.flush()
        self.last_update = now

    def __enter__(self):
        self.update(0, force=True)
        return self

    def __exit__(self, exc_type, exc, traceback):
        # Never fake 100% if processing stopped, including --max-requests reads.
        self.update(self.current, self.detail, force=True,
                    status="done" if exc_type is None else "stopped")
        return False

TYPE_TO_OP = {
    "PREFIX_READ": "URMA_READ",
    "PREFIX_STORE_WRITE": "URMA_WRITE",
    "STORAGE_TO_D": "URMA_READ",
    "DECODE_WRITE": "URMA_WRITE",
}
OUTPUT_FIELDS = [
    "taskId", "sourceNode", "destNode", "dataSize(Byte)", "opType",
    "priority", "delay", "phaseId", "dependOnPhases",
]
DEBUG_OUTPUT_FIELDS = OUTPUT_FIELDS + ["request_id"]
V14_MAPPING_FIELDS = ["batch_id", "batch_size", "batch_type", "request_ids", "batch_step_index", "tp_degree", "tp_group_id", "service_domain", "tp_rank", "kv_extent_kind", "kv_shard_offset_bytes", "kv_shard_bytes", "kv_shard_layer_ranges"]

MAPPING_FIELDS = [
    "taskId", "dag_task_id", "request_id", "task_type", "layer_index",
    "decode_token_index", "block_index", "chunk_index", "chunk_count",
    "src_port", "dst_port", "phaseId", "dependOnPhases", "delay", "network_wave",
]


class NativeTrafficWriter:
    """Write only compiler-validated native records, byte-identical to csv.

    IDs, bytes and priority are integers, opType is a fixed enum, delay is an
    integer plus us/ns, and dependencies contain only integers and spaces.
    None of these fields can contain commas, quotes or line breaks. Avoid
    CSV's repeated quoting scan of dependency fields that can exceed 600 KiB.
    Arbitrary user strings and mapping CSVs still use the standard writer.
    """
    def __init__(self, stream, debug_stream=None):
        self.write = stream.write
        self.debug_write = debug_stream.write if debug_stream is not None else None

    def writeheader(self):
        self.write(",".join(OUTPUT_FIELDS) + "\r\n")
        if self.debug_write:
            self.debug_write(",".join(DEBUG_OUTPUT_FIELDS) + "\r\n")

    def writerow(self, row, request_id=""):
        # request_id is a validated integer or empty for batch-owned work.
        # Reuse the native prefix and dependency string without copying a
        # potentially very large dependency field into a second whole row.
        prefix = (f"{row['taskId']},{row['sourceNode']},{row['destNode']},"
                  f"{row['dataSize(Byte)']},{row['opType']},{row['priority']},"
                  f"{row['delay']},{row['phaseId']},")
        self.write(prefix)
        self.write(row['dependOnPhases'])
        self.write("\r\n")
        if self.debug_write:
            self.debug_write(prefix)
            self.debug_write(row['dependOnPhases'])
            self.debug_write(f",{request_id}\r\n")


def csv_limit():
    limit = sys.maxsize
    while True:
        try:
            csv.field_size_limit(limit)
            return
        except OverflowError:
            limit //= 10


def integer(value, label, default=None):
    if value in (None, "") and default is not None:
        return default
    try:
        result = int(value)
    except (TypeError, ValueError):
        raise ValueError(f"{label}: invalid integer {value!r}") from None
    if result < 0:
        raise ValueError(f"{label}: must be nonnegative")
    return result


def time_ns(value, label, default=0):
    if value in (None, ""):
        return default
    try:
        value = Decimal(str(value))
        if not value.is_finite() or value < 0:
            raise InvalidOperation
        return int((value * 1000).to_integral_value(rounding=ROUND_CEILING))
    except (InvalidOperation, ValueError):
        raise ValueError(f"{label}: invalid nonnegative microseconds") from None


def delay_text(ns):
    return f"{ns // 1000}us" if ns % 1000 == 0 else f"{ns}ns"


def split_u32(size):
    while size:
        chunk = min(size, UINT32_MAX)
        yield chunk
        size -= chunk


def data_size_divisor(args):
    divisor = getattr(args, "data_size_divisor", 1)
    if isinstance(divisor, bool) or not isinstance(divisor, int) or divisor < 1:
        raise ValueError("data size divisor must be a positive integer")
    return divisor


def ub_endpoints(row, stage):
    # Retained from V12: READ source is the reader/initiator; payload travels
    # from the remote ASU (destNode) to the reader (sourceNode).
    src = integer(row["src_node"], "src_node")
    dst = integer(row["dst_node"], "dst_node")
    if src > UINT32_MAX or dst > UINT32_MAX:
        raise ValueError("node ID exceeds uint32")
    if TYPE_TO_OP[stage] == "URMA_READ":
        return dst, src, row.get("dst_port", ""), row.get("src_port", "")
    return src, dst, row.get("src_port", ""), row.get("dst_port", "")


MAPPING_FIELDS += V14_MAPPING_FIELDS


class CountedTextLines:
    """Count CSV input bytes, including quoted multiline records and a BOM."""

    def __init__(self, stream, enabled=True):
        self.stream, self.enabled = stream, enabled
        self.processed_bytes = (3 if enabled and stream.buffer.peek(3).startswith(b"\xef\xbb\xbf") else 0)

    def __iter__(self):
        return self

    def __next__(self):
        line = next(self.stream)
        if self.enabled:
            self.processed_bytes += len(line.encode("utf-8"))
        return line


def request_groups(reader, counted_lines=None):
    current, rows = None, []
    for row in reader:
        row["_line"] = reader.line_num
        if counted_lines is not None:
            row["_processed_bytes"] = counted_lines.processed_bytes
        request = integer(row["request_id"], "request_id")
        if current is not None and request != current:
            yield current, rows
            rows = []
        current = request
        rows.append(row)
    if current is not None:
        yield current, rows


class Compiler:
    def __init__(self, db, args):
        self.db, self.args = db, args
        self.data_size_divisor = data_size_divisor(args)
        self.next_phase, self.next_task = args.start_phase_id, args.start_task_id
        self.last_dag_id = -1
        self.counts = Counter()
        self.examples = []
        self.max_phase_examples = []
        self.progress = None
        # Bounded caches keep recent task forms in RAM while SQLite retains
        # the complete DAG, including old cross-request dependencies.
        self.task_cache = OrderedDict()
        self.phase_cache = OrderedDict()
        self.prune_cache = OrderedDict()
        self.prune_cache_terms = 0
        self.phase_base = args.start_phase_id
        self.phase_gate_ids = array("Q")
        self.phase_delays = array("Q")
        self.phase_floors = array("Q")
        self.parent_ids = OrderedDict()
        self.parent_ids_weight = 0
        self.parents_cache = OrderedDict()
        self.parents_cache_weight = 0
        self.next_parent_id = 1
        self.last_parent_vector = None
        self.last_parent_gid = None
        self.cache_limit = 8192
        self.known_requests = OrderedDict()
        self.last_out_form = None
        self.share_phases = getattr(args, "phase_grouping", "request-wave") == "request-wave"
        self.eager_max_phase = False
        self.shared_groups = OrderedDict()
        self.shared_groups_weight = 0
        self.sealed_phases = bytearray()
        self.prefill_ready_cache = OrderedDict()
        db.executescript(
            "CREATE TABLE tasks(id INTEGER PRIMARY KEY, form TEXT NOT NULL);"
            "CREATE TABLE phases(id INTEGER PRIMARY KEY, parents TEXT NOT NULL,"
            " delay INTEGER NOT NULL, floor INTEGER NOT NULL);"
            "CREATE TABLE requests(id INTEGER PRIMARY KEY);"
            "CREATE TABLE prefill_ready(request INTEGER PRIMARY KEY, task INTEGER NOT NULL);"
            "CREATE INDEX prefill_ready_task ON prefill_ready(task);"
            "CREATE TABLE parent_gates(id INTEGER PRIMARY KEY, parents TEXT NOT NULL);"
            "CREATE TABLE shared_groups(owner TEXT, wave TEXT, parents TEXT, form TEXT NOT NULL,"
            " PRIMARY KEY(owner, wave, parents)) WITHOUT ROWID;"
        )

    def save_shared_group(self, key, group):
        record = dict(group, parents=list(group["parents"]),
                      delays=sorted(group["delays"]), types=sorted(group["types"]))
        record.pop("_parent_set", None)
        self.db.execute("INSERT OR REPLACE INTO shared_groups VALUES (?, ?, ?, ?)",
                        (key[0], key[1], json.dumps(key[2]), json.dumps(record)))

    def cache_shared_group(self, key, group):
        if key not in self.shared_groups:
            self.shared_groups_weight += len(key[2]) + 1
        self.shared_groups[key] = group
        self.shared_groups.move_to_end(key)
        while (len(self.shared_groups) > 8192 or
               self.shared_groups_weight > 131072 and len(self.shared_groups) > 1):
            old_key, old = self.shared_groups.popitem(last=False)
            self.shared_groups_weight -= len(old_key[2]) + 1
            self.save_shared_group(old_key, old)

    def shared_group(self, key):
        group = self.shared_groups.get(key)
        if group is not None:
            self.shared_groups.move_to_end(key)
        else:
            record = self.db.execute(
                "SELECT form FROM shared_groups WHERE owner=? AND wave=? AND parents=?",
                (key[0], key[1], json.dumps(key[2]))).fetchone()
            if record is not None:
                group = json.loads(record[0])
                group["parents"] = tuple(group["parents"])
                group["delays"] = set(group["delays"])
                group["types"] = set(group["types"])
                group["_parent_set"] = set(group["parents"])
                self.cache_shared_group(key, group)
        if group is not None and self.sealed_phases[group["pid"] - self.phase_base]:
            return None
        return group

    def prefill_shared_group(self, key):
        """Return an open exact-AND group; never silently split a sealed group."""
        group = self.shared_group(key)
        if group is not None:
            return group
        exists = (key in self.shared_groups or
                  self.db.execute(
                      "SELECT 1 FROM shared_groups WHERE owner=? AND wave=? AND parents=?",
                      (key[0], key[1], json.dumps(key[2]))).fetchone() is not None)
        if exists:
            raise ValueError(
                "Shared phase group received another member after a dependent task "
                "consumed the phase; check DAG row ordering"
            )
        return None

    def prefill_network_group_key(self, row, request, stage):
        if row.get("batch_type", "").strip().upper() != "PREFILL":
            return None
        batch = integer(row.get("batch_id"), "Prefill batch_id")
        layer = integer(row.get("layer_index"), "Prefill layer_index")
        if stage == "PREFIX_READ":
            return ("prefill", f"batch:{batch}:read:{layer}", ())
        if stage == "PREFIX_STORE_WRITE":
            return ("prefill", f"batch:{batch}:write:{layer}", ())
        return None

    def prefill_gate(self, root, terms, local_delay):
        """Map a Prefill gate to all parent phases plus only its local delay."""
        parents = tuple(sorted(terms))
        parent_floor = max((self.phase_floor(pid) for pid in parents), default=0)
        delay = max(0, root - parent_floor) + local_delay
        floor = max(root, parent_floor) + local_delay
        return parents, delay, floor

    def update_prefill_shared_group(self, key, group, parents, delay, floor, stage):
        old_parents = tuple(group["parents"])
        parent_set = group.setdefault("_parent_set", set(old_parents))
        added = [pid for pid in parents if pid not in parent_set]
        if added:
            parent_set.update(added)
            new_parents = tuple(sorted(parent_set))
        else:
            new_parents = old_parents
        new_floor = max(group["floor"], floor)
        parents_changed = new_parents != old_parents
        floor_changed = new_floor != group["floor"]
        was_mixed = len(group["delays"]) > 1
        group["delays"].add(delay)
        group["types"].add(stage)
        group["max_delay"] = max(group["max_delay"], delay)
        group["parents"] = new_parents
        group["floor"] = new_floor
        if parents_changed or floor_changed:
            parent_ref = self.register_phase(group["pid"], new_parents, 0, new_floor)
            self.db.execute("UPDATE phases SET parents=?, delay=0, floor=? WHERE id=?",
                            (parent_ref, new_floor, group["pid"]))
            self.phase_cache.pop(group["pid"], None)
            self.prune_cache.clear()
            self.prune_cache_terms = 0
        if not was_mixed and len(group["delays"]) > 1:
            self.counts["mixed_delay_phases"] += 1
        self.cache_shared_group(key, group)
        return group

    def seal_terms(self, row, terms):
        if ((self.share_phases or row.get("batch_type", "").strip().upper() == "PREFILL")
                and row.get("execution_schema") in {V14_SCHEMA, LOGICAL_SCHEMA}):
            for pid, _ in terms:
                self.sealed_phases[pid - self.phase_base] = 1

    def flush_shared_groups(self):
        for key, group in self.shared_groups.items():
            self.save_shared_group(key, group)
        self.db.commit()

    def remember_parents(self, gid, parents):
        if gid in self.parents_cache:
            self.parents_cache.move_to_end(gid)
            return
        self.parents_cache[gid] = parents
        self.parents_cache_weight += len(parents)
        while len(self.parents_cache) > 8192 or self.parents_cache_weight > 131072 and len(self.parents_cache) > 1:
            _, old = self.parents_cache.popitem(last=False)
            self.parents_cache_weight -= len(old)

    def register_phase(self, pid, parents, delay, floor):
        """Intern parent vectors; retain only 24 bytes of scalar metadata per phase."""
        parents = tuple(parents)
        gid = self.last_parent_gid if parents is self.last_parent_vector else self.parent_ids.get(parents)
        if gid is None:
            gid = self.next_parent_id
            self.next_parent_id += 1
            self.db.execute("INSERT INTO parent_gates VALUES (?, ?)", (gid, json.dumps(parents)))
            self.parent_ids[parents] = gid
            self.parent_ids_weight += len(parents)
            while len(self.parent_ids) > 8192 or self.parent_ids_weight > 131072 and len(self.parent_ids) > 1:
                key, _ = self.parent_ids.popitem(last=False)
                self.parent_ids_weight -= len(key)
        else:
            if parents is not self.last_parent_vector and parents in self.parent_ids:
                self.parent_ids.move_to_end(parents)
        self.last_parent_vector, self.last_parent_gid = parents, gid
        self.remember_parents(gid, parents)
        index = pid - self.phase_base
        if index == len(self.phase_gate_ids):
            self.phase_gate_ids.append(gid)
            self.phase_delays.append(delay)
            self.phase_floors.append(floor)
            self.sealed_phases.append(0)
        elif 0 <= index < len(self.phase_gate_ids):
            self.phase_gate_ids[index] = gid
            self.phase_delays[index] = delay
            self.phase_floors[index] = floor
        else:
            raise ValueError("phase IDs must be allocated contiguously")
        return "@" + str(gid)

    def phase_floor(self, pid):
        index = pid - self.phase_base
        return self.phase_floors[index] if 0 <= index < len(self.phase_floors) else self.phase(pid)[2]

    def phase(self, pid):
        index = pid - self.phase_base
        if 0 <= index < len(self.phase_gate_ids):
            gid = self.phase_gate_ids[index]
            parents = self.parents_cache.get(gid)
            if parents is None:
                record = self.db.execute("SELECT parents FROM parent_gates WHERE id=?", (gid,)).fetchone()
                parents = tuple(json.loads(record[0]))
                self.remember_parents(gid, parents)
            else:
                self.parents_cache.move_to_end(gid)
            return parents, self.phase_delays[index], self.phase_floors[index]
        cached = self.phase_cache.get(pid)
        if cached is not None:
            self.phase_cache.move_to_end(pid)
            return cached
        record = self.db.execute(
            "SELECT parents, delay, floor FROM phases WHERE id=?", (pid,)
        ).fetchone()
        if record is None:
            raise ValueError(f"internal error: unknown network phase {pid}")
        if record[0].startswith("@"):
            raw = self.db.execute("SELECT parents FROM parent_gates WHERE id=?", (int(record[0][1:]),)).fetchone()[0]
        else:
            raw = record[0]
        value = tuple(json.loads(raw)), record[1], record[2]
        self.phase_cache[pid] = value
        if len(self.phase_cache) > self.cache_limit:
            self.phase_cache.popitem(last=False)
        return value

    def remember_task(self, tid, form):
        self.task_cache[tid] = form
        self.task_cache.move_to_end(tid)
        if len(self.task_cache) > self.cache_limit:
            self.task_cache.popitem(last=False)

    def task_form(self, tid):
        cached = self.task_cache.get(tid)
        if cached is not None:
            self.task_cache.move_to_end(tid)
            return cached
        record = self.db.execute("SELECT form FROM tasks WHERE id=?", (tid,)).fetchone()
        if record is None:
            return None
        form = json.loads(record[0])
        self.remember_task(tid, form)
        return form

    def prune(self, terms):
        """Exact weighted dominance, one descending pass over the ancestor DAG.

        Original rule: q dominates p if lag(q)+longest_delay(q->p)>=lag(p).
        Phase IDs are topological. At p, all larger sources have propagated
        their maximum threshold already. Inject p's own lag only AFTER the
        domination test, so a phase can never dominate itself.
        Dominated sources may still propagate: any source dominating them
        also dominates everything they would remove, by transitivity.
        """
        terms = dict(terms)
        if len(terms) < 2:
            return terms
        ordered = sorted(terms)
        key = None
        if len(terms) <= 8192:
            shift = min(terms.values())
            key = tuple((pid, terms[pid] - shift) for pid in ordered)
            kept = self.prune_cache.get(key)
            if kept is not None:
                self.prune_cache.move_to_end(key)
                return {pid: terms[pid] for pid in kept}
        cutoff = ordered[0]
        heap = [-pid for pid in ordered]
        heapq.heapify(heap)
        queued = set(ordered)
        incoming = {}
        kept = {}
        gate_best = {}
        while heap:
            node = -heapq.heappop(heap)
            threshold = incoming.get(node, -1)
            own = terms.get(node)
            if own is not None:
                if threshold < own:
                    kept[node] = own
                threshold = max(threshold, own)
            if node <= cutoff:
                continue
            parents, delay, _ = self.phase(node)
            if not parents or parents[-1] < cutoff:
                continue
            # Sibling rank phases often have identical gates. Their largest
            # threshold alone covers the same parent updates.
            index = node - self.phase_base
            gate = (self.phase_gate_ids[index], delay) if 0 <= index < len(self.phase_gate_ids) else (parents, delay)
            if threshold <= gate_best.get(gate, -1):
                continue
            gate_best[gate] = threshold
            candidate = threshold + delay
            for parent in parents:
                if parent >= cutoff and candidate > incoming.get(parent, -1):
                    incoming[parent] = candidate
                    if parent not in queued:
                        queued.add(parent)
                        heapq.heappush(heap, -parent)
        if key is not None:
            ids = tuple(sorted(kept))
            self.prune_cache[key] = ids
            self.prune_cache_terms += len(key) + len(ids)
            while self.prune_cache_terms > 131072 or len(self.prune_cache) > 2048:
                old_key, old_ids = self.prune_cache.popitem(last=False)
                self.prune_cache_terms -= len(old_key) + len(old_ids)
        return kept

    def decode_predecessor_terms(self, tid, terms):
        # Match input Read's existing final-Prefill-frontier convention only
        # for a direct P_DONE predecessor. Never compact previous Decode
        # Compute: its weighted Read dependencies must all survive folding.
        if self.db.execute("SELECT 1 FROM prefill_ready WHERE task=?", (tid,)).fetchone():
            frontier = max(terms, default=None, key=lambda item: item[0])
            return [frontier] if frontier is not None else []
        return terms

    def incoming(self, row):
        row["_incoming_pruned"] = False
        decode_begin = row.get("task_type") == "D_BATCH_BEGIN"
        release = time_ns(row["release_time_us"], "release_time_us")
        # The offline scheduler's dispatch forecast must not become another
        # wait after real Prefill/Decode completion. Request arrival gates
        # remain enforced by the preceding Prefill tasks.
        root = 0 if decode_begin else release
        terms = {}
        deps = row.get("depends_on", "").strip()
        missing = []
        values = list(filter(None, re.split(r"[;,\s]+", deps)))
        if (row.get("task_type") == "STORAGE_TO_D" and
                row.get("batch_type", "").strip().upper() == "DECODE"):
            request = integer(row.get("request_id"), "Decode request_id")
            expected = self.prefill_ready_cache.get(request)
            if expected is None:
                found = self.db.execute("SELECT task FROM prefill_ready WHERE request=?", (request,)).fetchone()
                expected = found[0] if found is not None else None
            else:
                self.prefill_ready_cache.move_to_end(request)
            if expected is None or [integer(value, "depends_on") for value in values] != [expected]:
                raise ValueError("Every Decode input Read must depend directly on its own P_DONE; "
                                 "regenerate task_dag.csv with the v8 generator")
        if row.get("_immutable_phase_graph") is True and len(values) == 1:
            tid = integer(values[0], "depends_on")
            found = self.task_form(tid)
            if found is not None:
                # Stored V14 local forms are already normalized; rank-group
                # aliases consist of independent sibling phases. A uniform
                # local/read lag shift preserves weighted dominance exactly.
                self.seal_terms(row, found[1])
                if decode_begin:
                    # Folded Compute may refer to several independent Read
                    # phases with different accumulated compute lags.
                    return 0, dict(self.decode_predecessor_terms(tid, found[1]))
                row["_incoming_pruned"] = True
                return max(root, found[0]), dict(found[1])
        for value in values:
            tid = integer(value, "depends_on")
            found = self.task_form(tid)
            if found is None:
                missing.append(tid)
                continue
            dep_root, dep_terms = found
            self.seal_terms(row, dep_terms)
            if decode_begin:
                dep_terms = self.decode_predecessor_terms(tid, dep_terms)
            else:
                root = max(root, dep_root)
            for pid, lag in dep_terms:
                terms[pid] = max(terms.get(pid, 0), lag)
        if missing:
            raise ValueError(
                f"missing/forward dependency task IDs {missing[:20]}; use a complete, "
                "topologically ordered DAG; do not drop cross-request dependencies"
            )
        # normalize() prunes once after adding the local duration/read lag.
        # A uniform lag shift preserves every dominance comparison, so doing
        # the identical traversal here as well only doubled the work.
        return root, terms

    def decode_gate(self, row, terms, local_delay=0):
        """Fold Compute lags into network delay, retaining AND predecessors."""
        if row.get("task_type") == "STORAGE_TO_D":
            # Every layer Read points directly at its request's P_DONE.
            # Its last phase is the final-layer
            # Write (or final Compute if the request has no new KV to write).
            if terms:
                selected = max(terms)
                terms = {selected: terms[selected]}
        else:
            terms = self.prune(terms)
        parents = tuple(sorted(terms))
        if not parents:
            raise ValueError("Decode task needs a completion dependency; regenerate task_dag.csv")
        lags = set(terms.values())
        delay = max(lags) + local_delay
        approximate = len(lags) > 1
        if approximate:
            # Nine-column traffic has only one delay after the AND barrier:
            # max(F_i + lag_i) cannot in general equal max(F_i) + delay.
            # Keep every phase, pad conservatively, and expose the approximation.
            # --max-phase must not silently discard a Decode predecessor.
            detail = dict(request_id=row["request_id"], dag_task_id=row["task_id"],
                          task_type=row["task_type"],
                          dependency_lags={str(p): delay_text(terms[p]) for p in parents},
                          native_delay=delay_text(delay),
                          reason="folded Decode compute has unequal predecessor lags")
            if self.args.timing_policy == "error":
                raise ValueError("native one-delay format cannot represent folded Decode exactly: "
                                 + json.dumps(detail) + "; use --timing-policy conservative")
            self.counts["decode_padded_network_tasks"] += 1
            if len(self.examples) < 12:
                self.examples.append(detail)
        floor = max(self.phase_floor(pid) for pid in parents) + delay
        return parents, delay, floor, approximate

    def decode_read_gate(self, row, terms):
        """Storage-only Read delay; these Reads still depend on their own P_DONE."""
        configured = (row.get("storage_read_latency_us", "150")
                      if self.args.storage_read_latency_us is None
                      else self.args.storage_read_latency_us)
        read_ns = time_ns(configured, "storage read latency")
        self.counts["decode_storage_only_read_tasks"] += 1
        return self.decode_gate(row, terms, read_ns)

    def normalize(self, root, terms, already_pruned=False):
        if self.eager_max_phase:
            # Phase IDs are topological: the largest ID cannot be removed by
            # weighted ancestor pruning. max-phase later selects exactly it.
            # Uniform lag shifts and merges preserve this maximum and its lag,
            # so lower phase terms need not survive intermediate local forms.
            if terms:
                selected = max(terms)
                lag = terms[selected]
                if len(terms) > 1:
                    self.counts["max_phase_compacted_forms"] += 1
                    self.counts["max_phase_compacted_terms"] += len(terms) - 1
                terms = {selected: lag}
                if root <= self.phase_floor(selected) + lag:
                    root = 0
            return root, terms
        if not already_pruned:
            terms = self.prune(terms)
        if self.args.timing_policy == "max-phase":
            # Only the retained phase may cover an absolute release floor.
            # In particular, discard artificial zero-origin compute floors
            # already covered by this phase's own accumulated compute lag.
            if terms:
                selected = max(terms)
                if root <= self.phase_floor(selected) + terms[selected]:
                    root = 0
            return root, terms
        if terms and any(self.phase_floor(p) + lag >= root for p, lag in terms.items()):
            root = 0
        return root, terms

    def native_gate(self, root, terms, row):
        prepared = row.get("_native_inputs")
        prepared = prepared if isinstance(prepared, tuple) else None
        parents = prepared[0] if prepared is not None else sorted(terms)
        if not parents:
            return [], root, root, False
        if self.args.timing_policy == "max-phase":
            selected = parents[-1]
            selected_lag = terms[selected]
            parent_floor = prepared[1] if prepared is not None else self.phase_floor(selected)
            # Use the selected phase's lag, NEVER the maximum lag across phases.
            # A request's absolute release time remains a lower bound. The
            # one-delay format pads only its uncovered part relative to this
            # selected phase's known earliest completion (no network forecast).
            delay = max(selected_lag, max(0, root - parent_floor))
            approximate = root > parent_floor + selected_lag
            if len(parents) > 1:
                self.counts["max_phase_reduced_tasks"] += 1
                self.counts["discarded_phase_dependencies"] += len(parents) - 1
                if len(self.max_phase_examples) < 12:
                    self.max_phase_examples.append({
                        "request_id": row["request_id"], "dag_task_id": row["task_id"],
                        "task_type": row["task_type"],
                        "dependency_lags": {str(p): delay_text(terms[p]) for p in parents},
                        "selected_phase": selected,
                        "selected_phase_lag": delay_text(selected_lag),
                        "discarded_phases": parents[:-1],
                        "absolute_floor": delay_text(root),
                        "native_delay": delay_text(delay),
                    })
            if approximate and len(self.examples) < 12:
                self.examples.append({
                    "request_id": row["request_id"], "dag_task_id": row["task_id"],
                    "task_type": row["task_type"], "selected_phase": selected,
                    "selected_phase_lag": delay_text(selected_lag),
                    "absolute_floor": delay_text(root), "native_delay": delay_text(delay),
                    "reason": "absolute release floor padding relative to selected phase",
                })
            return [selected], delay, parent_floor + delay, approximate
        if prepared is not None:
            parent_floor, common, mixed = prepared[1:]
        else:
            lags = set(terms.values())
            common = max(lags)
            mixed = len(lags) > 1
            parent_floor = max(self.phase_floor(p) for p in parents)
        # An absolute release floor cannot be added as an absolute timestamp
        # after dependencies. Pad only the part not covered by known floors.
        pad = max(0, root - parent_floor)
        delay = max(common, pad)
        approximate = mixed or root > 0
        if approximate and (self.args.timing_policy == "error" or len(self.examples) < 12):
            detail = {
                "request_id": row["request_id"], "dag_task_id": row["task_id"],
                "task_type": row["task_type"], "phaseId": self.next_phase,
                "dependency_lags": {str(p): delay_text(terms[p]) for p in parents},
                "absolute_floor": delay_text(root), "native_delay": delay_text(delay),
            }
            if self.args.timing_policy == "error":
                raise ValueError(
                    "native one-delay format cannot represent this gate exactly: "
                    + json.dumps(detail, ensure_ascii=False)
                    + "; use --timing-policy conservative to explicitly allow later starts"
                )
            if len(self.examples) < 12:
                self.examples.append(detail)
        return parents, delay, parent_floor + delay, approximate

    def write_timer_record(self, row, request, dag_task_id, phase_id, parents,
                           delay, writer, mapping):
        if self.next_task > UINT32_MAX:
            raise ValueError("traffic task ID exceeds uint32")
        node_value = row.get("src_node") or row.get("p_node") or row.get("d_node")
        if node_value in (None, ""):
            raise ValueError("TIMER needs a valid sourceNode (p_node/src_node/d_node)")
        node = integer(node_value, "TIMER sourceNode")
        dependency_text = " ".join(map(str, parents))
        record = dict(zip(OUTPUT_FIELDS, [
            self.next_task, node, node, 0, "TIMER", self.args.priority,
            delay_text(delay), phase_id, dependency_text,
        ]))
        if isinstance(writer, NativeTrafficWriter):
            writer.writerow(record, request_id=request)
        else:
            writer.writerow(record)
        if mapping:
            mapping.writerow(dict(zip(MAPPING_FIELDS, [
                self.next_task, row.get("_logical_task_id", dag_task_id),
                request, row.get("task_type", ""), row.get("layer_index", ""),
                row.get("decode_token_index", ""), row.get("block_index", ""),
                0, 1, "", "", phase_id, dependency_text, delay_text(delay),
                row.get("network_wave", ""),
            ] + [row.get(key, "") for key in V14_MAPPING_FIELDS])))
        self.next_task += 1
        self.counts["timer_rows"] += 1

    def convert_request(self, request, rows, schema, writer, mapping):
        if schema == V14_SCHEMA:
            if request != "":
                if request not in self.known_requests:
                    self.counts["requests"] += self.db.execute("INSERT OR IGNORE INTO requests VALUES (?)", (request,)).rowcount
                    self.known_requests[request] = None
                    if len(self.known_requests) > 8192:
                        self.known_requests.popitem(last=False)
                else:
                    self.known_requests.move_to_end(request)
        else:
            try:
                self.db.execute("INSERT INTO requests VALUES (?)", (request,))
            except sqlite3.IntegrityError:
                raise ValueError(f"request {request} appears in multiple file regions") from None
        groups = {}
        current_wave, closed_waves, wave_phases = None, set(), {}
        for row in rows:
            try:
                tid = integer(row["task_id"], "task_id")
                if tid <= self.last_dag_id:
                    raise ValueError("task IDs must be strictly increasing (topological order)")
                self.last_dag_id = tid
                if row.get("execution_schema") != schema:
                    raise ValueError("this converter needs the matching revised DAG; regenerate with the matching trace generator")
                kind, stage = row["task_class"].strip().upper(), row["task_type"].strip()
                is_prefill = row.get("batch_type", "").strip().upper() == "PREFILL"
                is_decode = row.get("batch_type", "").strip().upper() == "DECODE"
                is_decode_read = schema == V14_SCHEMA and stage == "STORAGE_TO_D"
                ephemeral = kind == "NETWORK" and row.get("_ephemeral_task") is True
                for key, allowed in (("dependency_mode", {"", "ALL"}),
                                     ("dependency_event", {"", "COMPLETE"})):
                    if row.get(key, "").strip().upper() not in allowed:
                        raise ValueError(f"unsupported {key}: {row[key]}")
                size = integer(row["bytes"], "bytes", 0)
                duration = time_ns(row["duration_us"], "duration_us")
                prepared = row.get("_prepared_terms")
                prepared = prepared if isinstance(prepared, tuple) else None
                root, terms = prepared if prepared is not None else self.incoming(row)
                if kind == "NETWORK":
                    if stage not in TYPE_TO_OP:
                        raise ValueError(f"unsupported network task type: {stage}")
                    if size == 0 or duration:
                        raise ValueError("NETWORK needs positive bytes and duration_us=0")
                    wave = row.get("network_wave", "").strip()
                    if not wave:
                        raise ValueError("network_wave is required for network tasks")
                    if wave != current_wave:
                        if wave in closed_waves:
                            raise ValueError("network waves must be contiguous within a request")
                        if current_wave is not None:
                            closed_waves.add(current_wave)
                        current_wave = wave
                    explicit_group_key = self.prefill_network_group_key(row, request, stage) if is_prefill else None
                    if is_decode_read:
                        batch = integer(row.get("batch_id"), "Decode batch_id")
                        explicit_group_key = ("decode", f"batch:{batch}:input-read-all", ())
                    if is_prefill:
                        if TYPE_TO_OP[stage] == "URMA_READ":
                            configured = (row.get("storage_read_latency_us", "150")
                                          if self.args.storage_read_latency_us is None
                                          else self.args.storage_read_latency_us)
                            read_ns = time_ns(configured, "storage read latency")
                        else:
                            read_ns = 0
                        parents, delay, floor = self.prefill_gate(
                            root, terms, read_ns if TYPE_TO_OP[stage] == "URMA_READ" else 0
                        )
                        approximate = False
                    elif is_decode_read:
                        parents, delay, floor, approximate = self.decode_read_gate(
                            row, terms
                        )
                    elif is_decode:
                        parents, delay, floor, approximate = self.decode_gate(row, terms)
                    else:
                        if prepared is None and TYPE_TO_OP[stage] == "URMA_READ":
                            configured = (row.get("storage_read_latency_us", "150")
                                          if self.args.storage_read_latency_us is None
                                          else self.args.storage_read_latency_us)
                            read_ns = time_ns(configured, "storage read latency")
                            root += read_ns
                            terms = {p: lag + read_ns for p, lag in terms.items()}
                        if prepared is None:
                            root, terms = self.normalize(root, terms, row.get("_incoming_pruned") is True)
                        parents, delay, floor, approximate = self.native_gate(root, terms, row)
                    streaming_shared = (explicit_group_key is not None or
                                        schema == V14_SCHEMA and self.share_phases)
                    signature = (wave, tuple(parents))
                    shared_key = None
                    if explicit_group_key is not None:
                        shared_key = explicit_group_key
                        group = self.prefill_shared_group(shared_key)
                    elif streaming_shared:
                        owner = (f"request:{request}" if request != "" else
                                 f"logical:{row['_logical_task_id']}" if "_logical_task_id" in row else f"task:{tid}")
                        shared_key = (owner, wave, tuple(parents))
                        group = self.shared_group(shared_key)
                    else:
                        if wave_phases and any(wave_phases.get(p) == wave for p in parents):
                            raise ValueError("a wave cannot depend on its own completion barrier")
                        group = groups.get(signature)
                    if group is None:
                        pid = self.next_phase
                        if pid > UINT32_MAX:
                            raise ValueError("phase ID exceeds uint32")
                        phase_delay = 0 if explicit_group_key is not None else delay
                        parent_ref = self.register_phase(pid, parents, phase_delay, floor)
                        if not ephemeral:
                            self.db.execute("INSERT INTO phases VALUES (?, ?, ?, ?)",
                                            (pid, parent_ref, phase_delay, floor))
                        self.next_phase += 1
                        group = dict(pid=pid, parents=tuple(parents), max_delay=delay,
                                     phase_delay=phase_delay, floor=floor,
                                     delays={delay}, types={stage}, approximate=approximate)
                        if explicit_group_key is not None:
                            group["_parent_set"] = set(parents)
                            self.cache_shared_group(shared_key, group)
                        elif streaming_shared:
                            self.cache_shared_group(shared_key, group)
                        else:
                            groups[signature] = group
                        wave_phases[pid] = wave
                        self.counts["network_phases"] += 1
                        if is_decode_read:
                            self.counts["decode_input_read_phases"] += 1
                        self.counts["conservative_phases"] += approximate
                    pid = group["pid"]
                    if parents and parents[-1] >= pid:
                        raise ValueError("phase dependencies must precede their completion phase")
                    if explicit_group_key is not None:
                        self.update_prefill_shared_group(
                            shared_key, group, parents, delay, floor, stage
                        )
                    else:
                        was_mixed = len(group["delays"]) > 1
                        was_multiple_types = len(group["types"]) > 1
                        if len(group["delays"]) < 2:
                            group["delays"].add(delay)
                        group["types"].add(stage)
                        if streaming_shared:
                            self.counts["mixed_delay_phases"] += not was_mixed and len(group["delays"]) > 1
                            self.counts["read_write_shared_phases"] += not was_multiple_types and len(group["types"]) > 1
                        if delay > group["max_delay"] or floor > group["floor"]:
                            group["max_delay"] = max(group["max_delay"], delay)
                            group["floor"] = max(group["floor"], floor)
                            self.register_phase(pid, group["parents"], group["max_delay"], group["floor"])
                            self.db.execute("UPDATE phases SET delay=?, floor=? WHERE id=?",
                                            (group["max_delay"], group["floor"], pid))
                            self.phase_cache.pop(pid, None)
                            self.prune_cache.clear()
                            self.prune_cache_terms = 0
                        if approximate and not group["approximate"]:
                            self.counts["conservative_phases"] += 1
                            group["approximate"] = True
                    source, dest, src_port, dst_port = ub_endpoints(row, stage)
                    # Native ns-3-ub tokenizes this CSV field on whitespace.
                    # Empty means no dependency; signed -1 is INVALID.
                    dep_text = (row["_dependency_text"] if prepared is not None and not is_decode
                                else " ".join(map(str, parents)))
                    chunk_count = (size + UINT32_MAX - 1) // UINT32_MAX
                    divisor, emitted_bytes = self.data_size_divisor, 0
                    for chunk_index, chunk in enumerate(split_u32(size)):
                        if self.next_task > UINT32_MAX:
                            raise ValueError("traffic task ID exceeds uint32")
                        # Scale the ORIGINAL traffic row, not the logical task
                        # or unsplit rank bytes. This exactly retains all rows
                        # and phases, including tiny final uint32 chunks.
                        if divisor != 1:
                            chunk = 1 + (chunk - 1) // divisor
                        emitted_bytes += chunk
                        record = dict(zip(OUTPUT_FIELDS, [
                            self.next_task, source, dest, chunk, TYPE_TO_OP[stage],
                            self.args.priority, delay_text(delay), pid, dep_text,
                        ]))
                        if isinstance(writer, csv.DictWriter) and writer.fieldnames == OUTPUT_FIELDS:
                            writer.writer.writerow([record[key] for key in OUTPUT_FIELDS])
                        elif isinstance(writer, NativeTrafficWriter):
                            writer.writerow(record, request_id=request)
                        else:
                            writer.writerow(record)
                        if mapping:
                            mapping.writerow(dict(zip(MAPPING_FIELDS, [
                                self.next_task, row.get("_logical_task_id", tid), request, stage, row.get("layer_index", ""),
                                row.get("decode_token_index", ""), row.get("block_index", ""),
                                chunk_index, chunk_count, src_port, dst_port, pid, dep_text,
                                delay_text(delay), wave,
                            ] + [row.get(key, "") for key in V14_MAPPING_FIELDS])))
                        self.next_task += 1
                        self.counts["traffic_rows"] += 1
                    self.counts[stage] += 1
                    self.counts["network_bytes"] += size
                    self.counts["traffic_bytes"] += emitted_bytes
                    # Unknown actual network duration stays with ns-3-ub.
                    out_form = [0, [[pid, 0]]]
                elif kind in {"COMPUTE", "TIMER", "GATE"}:
                    if size:
                        raise ValueError("non-network task must have bytes=0")
                    prefill_timer = is_prefill and (
                        (kind == "COMPUTE" and stage == "PREFILL_LAYER") or
                        (kind == "TIMER" and stage == "P_BATCH_BEGIN"))
                    if prefill_timer:
                        parents, timer_delay, floor = self.prefill_gate(root, terms, duration)
                        pid = self.next_phase
                        if pid > UINT32_MAX:
                            raise ValueError("phase ID exceeds uint32")
                        phase_delay = duration if kind == "COMPUTE" else 0
                        parent_ref = self.register_phase(pid, parents, phase_delay, floor)
                        if not ephemeral:
                            self.db.execute("INSERT INTO phases VALUES (?, ?, ?, ?)",
                                            (pid, parent_ref, phase_delay, floor))
                        self.next_phase += 1
                        self.write_timer_record(
                            row, request, tid, pid, parents, timer_delay, writer, mapping
                        )
                        out_form = [0, [[pid, 0]]]
                        if stage == "PREFILL_LAYER":
                            self.counts["prefill_compute_phases"] += 1
                        else:
                            self.counts["prefill_control_phases"] += 1
                    else:
                        root += duration
                        terms = {p: lag + duration for p, lag in terms.items()}
                        if stage == "D_BATCH_BEGIN":
                            self.counts["decode_batch_forecast_releases_ignored"] += 1
                        elif is_decode:
                            terms = self.prune(terms)
                            if kind == "COMPUTE":
                                self.counts["folded_decode_compute_tasks"] += 1
                                self.counts["folded_decode_compute_ns"] += duration
                        elif not is_prefill:
                            root, terms = self.normalize(
                                root, terms, row.get("_incoming_pruned") is True
                            )
                        out_form = [root, sorted(terms.items())]
                        self.counts["folded_local_tasks"] += 1
                else:
                    raise ValueError(f"unsupported task_class: {kind}")
                self.last_out_form = out_form
                if is_prefill and stage == "P_DONE":
                    self.db.execute("INSERT OR REPLACE INTO prefill_ready VALUES (?, ?)", (request, tid))
                    self.prefill_ready_cache[request] = tid
                    self.prefill_ready_cache.move_to_end(request)
                    while len(self.prefill_ready_cache) > 8192:
                        self.prefill_ready_cache.popitem(last=False)
                if not ephemeral:
                    self.db.execute("INSERT INTO tasks VALUES (?, ?)", (tid, json.dumps(out_form)))
                    self.remember_task(tid, out_form)
                self.counts["dag_rows"] += 1
                if self.progress is not None:
                    self.progress.update(row["_processed_bytes"],
                                         f"{self.counts['dag_rows']:,} rows | {self.counts['traffic_rows']:,} flows")
            except (ValueError, KeyError) as exc:
                raise ValueError(f"physical line {row['_line']}, task {row.get('task_id')}: {exc}") from None
        if schema != V14_SCHEMA:
            self.counts["requests"] += 1
        for group in groups.values():
            self.counts["mixed_delay_phases"] += len(group["delays"]) > 1
            self.counts["read_write_shared_phases"] += len(group["types"]) > 1
        if schema != V14_SCHEMA or self.counts["dag_rows"] % 10000 == 0:
            self.db.commit()


class LogicalTpExpander:
    """Stream exact rank flows; disk-backed logical-to-expanded ALL gates.

    A network logical ID resolves to an ALL-completion alias of its rank
    phases, deduplicated when ranks share a phase. Each rank is compiled
    separately, retaining integer byte sizes and physical flow order.
    """
    def __init__(self, compiler):
        self.compiler = compiler
        self.db = compiler.db
        self.db.execute("CREATE TABLE logical_aliases(id INTEGER PRIMARY KEY, expanded INTEGER NOT NULL)")
        self.cache = OrderedDict()
        self.next_id = 0
        self.last_logical = -1
        self.rows = 0
        self.prepare_cache = OrderedDict()
        self.prepare_cache_weight = 0

    def resolve(self, tid):
        if tid in self.cache:
            self.cache.move_to_end(tid)
            return self.cache[tid]
        found = self.db.execute("SELECT expanded FROM logical_aliases WHERE id=?", (tid,)).fetchone()
        if found is None:
            raise ValueError(f"missing/forward logical dependency {tid}")
        return found[0]

    def compile(self, row, writer, mapping):
        tid = integer(row["task_id"], "logical task_id")
        if tid <= self.last_logical:
            raise ValueError("logical task IDs must be strictly increasing")
        self.last_logical = tid
        deps = [integer(value, "logical depends_on")
                for value in filter(None, re.split(r"[;,\s]+", row["depends_on"]))]
        expanded_deps = ";".join(str(self.resolve(dep)) for dep in deps)
        degree = integer(row["tp_degree"] or "1", "tp_degree", 1)
        if degree < 1:
            raise ValueError("tp_degree must be positive")
        kind = row["task_class"].strip().upper()
        members = []
        if kind == "NETWORK":
            members = json.loads(row["tp_members"])
            if not isinstance(members, list) or not members:
                raise ValueError("logical NETWORK needs a nonempty tp_members array")
            ranks = []
            total = 0
            for item in members:
                if not isinstance(item, list) or len(item) != 6:
                    raise ValueError("tp_members entries must be [rank,src_node,src_port,dst_node,dst_port,bytes]")
                if any(type(value) is not int for value in item):
                    raise ValueError("tp_members values must be integers")
                rank, src, sp, dst, dp, size = item
                if not 0 <= rank < degree or min(src, sp, dst, dp) < 0 or size <= 0:
                    raise ValueError("invalid TP member rank, endpoint or byte size")
                ranks.append(rank)
                total += size
            if ranks != sorted(set(ranks)):
                raise ValueError("TP member ranks must be unique and increasing")
            if total != integer(row["bytes"], "logical bytes", 1):
                raise ValueError("TP member bytes do not sum to logical bytes")
        elif row.get("tp_members", "").strip():
            raise ValueError("non-network tasks must not contain tp_members")
        request = row["request_id"].strip()
        if request:
            request = integer(request, "request_id")
        prepared_terms = native_inputs = dependency_text = None
        if members:
            read_ns = 0
            if TYPE_TO_OP.get(row["task_type"]) == "URMA_READ":
                configured = (row.get("storage_read_latency_us", "150")
                              if self.compiler.args.storage_read_latency_us is None
                              else self.compiler.args.storage_read_latency_us)
                read_ns = time_ns(configured, "storage read latency")
            is_prefill = row.get("batch_type", "").strip().upper() == "PREFILL"
            is_decode = row.get("batch_type", "").strip().upper() == "DECODE"
            key = (expanded_deps, time_ns(row["release_time_us"], "release_time_us"),
                   read_ns, is_prefill, is_decode)
            cached = self.prepare_cache.get(key)
            if cached is not None:
                self.prepare_cache.move_to_end(key)
                prepared_terms, native_inputs, dependency_text = cached
            else:
                incoming_row = dict(row, depends_on=expanded_deps)
                root, terms = self.compiler.incoming(incoming_row)
                if is_prefill or is_decode:
                    parents = tuple(sorted(terms))
                    prepared_terms = (root, terms)
                    dependency_text = " ".join(map(str, parents))
                else:
                    root += read_ns
                    if read_ns:
                        terms = {pid: lag + read_ns for pid, lag in terms.items()}
                    root, terms = self.compiler.normalize(
                        root, terms, incoming_row.get("_incoming_pruned") is True
                    )
                    prepared_terms = (root, terms)
                    parents = tuple(sorted(terms))
                    if self.compiler.args.timing_policy == "max-phase":
                        floor = self.compiler.phase_floor(parents[-1]) if parents else 0
                        common, mixed = 0, False
                    else:
                        floor = max((self.compiler.phase_floor(pid) for pid in parents), default=0)
                        lags = set(terms.values())
                        common, mixed = max(lags, default=0), len(lags) > 1
                    native_inputs = (parents, floor, common, mixed)
                    dependency_text = (str(parents[-1]) if parents and self.compiler.args.timing_policy == "max-phase"
                                       else " ".join(map(str, parents)))
                self.prepare_cache[key] = (prepared_terms, native_inputs, dependency_text)
                self.prepare_cache_weight += len(terms)
                while len(self.prepare_cache) > 128 or self.prepare_cache_weight > 262144 and len(self.prepare_cache) > 1:
                    _, old = self.prepare_cache.popitem(last=False)
                    self.prepare_cache_weight -= len(old[0][1])
        forms = []
        for member in members or [None]:
            expanded = dict(row, execution_schema=V14_SCHEMA,
                            task_id=str(self.next_id), depends_on=expanded_deps,
                            _logical_task_id=tid)
            if member is not None:
                rank, src, sp, dst, dp, size = member
                expanded.update(tp_rank=str(rank), src_node=str(src), src_port=str(sp),
                                dst_node=str(dst), dst_port=str(dp), bytes=str(size))
                expanded["_prepared_terms"] = prepared_terms
                if native_inputs is not None:
                    expanded["_native_inputs"] = native_inputs
                expanded["_dependency_text"] = dependency_text
                # These synthetic rank IDs are never referenced by logical
                # successors; only the ALL-completion alias is addressable.
                expanded["_ephemeral_task"] = True
            self.compiler.convert_request(request, [expanded], V14_SCHEMA, writer, mapping)
            forms.append(self.compiler.last_out_form)
            alias = self.next_id
            self.next_id += 1
        if kind == "NETWORK":
            # Sibling phases share the same incoming gate; none can dominate
            # another. The group completion is their exact ALL form.
            combined = {}
            for root, terms in forms:
                for pid, lag in terms:
                    combined[pid] = max(combined.get(pid, 0), lag)
            form = [0, sorted(combined.items())]
            alias = self.next_id
            self.next_id += 1
            self.db.execute("INSERT INTO tasks VALUES (?, ?)", (alias, json.dumps(form)))
            self.compiler.remember_task(alias, form)
            self.compiler.last_dag_id = alias
        self.db.execute("INSERT INTO logical_aliases VALUES (?, ?)", (tid, alias))
        self.cache[tid] = alias
        if len(self.cache) > 8192:
            self.cache.popitem(last=False)
        self.rows += 1


def convert(args):
    csv_limit()
    divisor = data_size_divisor(args)
    source = Path(args.task_dag).expanduser().resolve()
    output = (Path(args.output) if args.output else Path(args.out_dir or ".") / args.traffic_name).resolve()
    debug_path = (Path(args.debug_output).expanduser().resolve()
                  if getattr(args, "debug_output", None) else output.with_name("traffic_debug.csv"))
    mapping_path = Path(args.mapping_output).resolve() if args.mapping_output else None
    summary_path = Path(args.summary_output).resolve() if args.summary_output else None
    paths = [p for p in (output, debug_path, mapping_path, summary_path) if p is not None]
    if source in paths or len(set(paths)) != len(paths):
        raise ValueError("input, traffic, debug, mapping and summary paths must be distinct")
    if not 0 <= args.priority <= 15:
        raise ValueError("priority must be between 0 and 15")
    if not 1 <= args.start_phase_id <= UINT32_MAX or not 0 <= args.start_task_id <= UINT32_MAX:
        raise ValueError("invalid starting phase/task ID")
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-dag-", dir=output.parent) as temp_dir, ExitStack() as work:
        temp = Path(temp_dir)
        database = temp / "tasks.sqlite"
        if getattr(args, "work_dir", None):
            work_root = Path(args.work_dir).expanduser().resolve()
            work_root.mkdir(parents=True, exist_ok=True)
            database = Path(work.enter_context(tempfile.TemporaryDirectory(
                prefix="mooncake-sqlite-", dir=work_root))) / "tasks.sqlite"
        with sqlite3.connect(database) as db:
            db.execute("PRAGMA journal_mode=OFF")
            db.execute("PRAGMA synchronous=OFF")
            db.execute("PRAGMA cache_size=-16384")
            compiler = Compiler(db, args)
            mapping_file = (temp / "mapping.csv").open("w", encoding="utf-8", newline="") if mapping_path else None
            try:
                mapping = csv.DictWriter(mapping_file, MAPPING_FIELDS) if mapping_file else None
                if mapping:
                    mapping.writeheader()
                with source.open(encoding="utf-8-sig", newline="") as fin, \
                        (temp / "traffic.csv").open("w", encoding="utf-8", newline="", buffering=1024 * 1024) as fout, \
                        (temp / "traffic_debug.csv").open("w", encoding="utf-8", newline="", buffering=1024 * 1024) as fdebug, \
                        Progress("DAG convert", source.stat().st_size, unit="bytes",
                                 enabled=not getattr(args, "no_progress", False),
                                 interval=getattr(args, "progress_interval", 0.5)) as progress:
                    compiler.progress = progress
                    counted_lines = CountedTextLines(fin, enabled=progress.enabled)
                    reader = csv.DictReader(counted_lines)
                    required = {"task_id", "request_id", "task_class", "task_type", "depends_on",
                                "release_time_us", "duration_us", "src_node", "dst_node", "bytes",
                                "execution_schema", "network_wave", "storage_read_latency_us"}
                    if not reader.fieldnames or not required.issubset(reader.fieldnames):
                        raise ValueError(f"missing DAG columns: {sorted(required - set(reader.fieldnames or []))}")
                    schema = SCHEMA
                    writer = NativeTrafficWriter(fout, fdebug)
                    writer.writeheader()
                    first = next(reader, None)
                    schema = first.get("execution_schema") if first else V14_SCHEMA
                    if schema not in {SCHEMA, V14_SCHEMA, LOGICAL_SCHEMA}:
                        raise ValueError(f"unsupported execution_schema: {schema!r}")
                    expander = LogicalTpExpander(compiler) if schema == LOGICAL_SCHEMA else None
                    compiler.eager_max_phase = (schema in {V14_SCHEMA, LOGICAL_SCHEMA}
                                               and compiler.share_phases
                                               and args.timing_policy == "max-phase")
                    if expander and not {"tp_members", "tp_degree"}.issubset(reader.fieldnames):
                        raise ValueError("logical TP DAG requires tp_members and tp_degree")
                    if schema in {V14_SCHEMA, LOGICAL_SCHEMA}:
                        row = first
                        while row is not None:
                            row["_line"] = reader.line_num
                            row["_processed_bytes"] = counted_lines.processed_bytes
                            validate_prefill_metadata(row)
                            validate_decode_metadata(row)
                            request = row["request_id"].strip()
                            if request:
                                request = integer(request, "request_id")
                            # Per-rank phases are immutable. Shared phases are
                            # sealed in incoming() before any form is consumed,
                            # so single-predecessor forms stay stable here too.
                            row["_immutable_phase_graph"] = True
                            if expander:
                                if row.get("execution_schema") != LOGICAL_SCHEMA:
                                    raise ValueError("mixed execution schemas in logical TP DAG")
                                expander.compile(row, writer, mapping)
                            else:
                                compiler.convert_request(request, [row], schema, writer, mapping)
                            row = next(reader, None)
                    else:
                        fin.seek(0)
                        counted_lines = CountedTextLines(fin, enabled=progress.enabled)
                        reader = csv.DictReader(counted_lines)
                        for request, rows in request_groups(reader, counted_lines):
                            compiler.convert_request(request, rows, schema, writer, mapping)
                    progress.update(counted_lines.processed_bytes,
                                    f"{compiler.counts['dag_rows']:,} rows | {compiler.counts['traffic_rows']:,} flows")
                compiler.flush_shared_groups()
                summary = dict(compiler.counts)
                if expander:
                    summary["logical_dag_rows"] = expander.rows
                summary.update({
                    "converter_version": VERSION,
                    "input": str(source), "output": str(output), "dag_schema": schema,
                    "debug_output": str(debug_path),
                    "debug_request_id_policy": "DAG request_id; empty for batch-owned TIMERs",
                    "input_columns": reader.fieldnames,
                    "total_rows": compiler.counts["traffic_rows"] + compiler.counts["timer_rows"],
                    "prefill_batches": compiler.counts["prefill_control_phases"],
                    "prefill_metadata_validation": "required before phase pruning/TP expansion",
                    "timing_policy": args.timing_policy,
                    "phase_grouping_policy": ("legacy-request-wave" if schema == SCHEMA else
                                              "request-wave" if compiler.share_phases else "per-rank"),
                    "prefill_phase_grouping_policy": (
                        "Read=Batch+Layer; Compute=Batch+Layer TIMER; "
                        "Write=Batch+Layer (all requests, blocks, TP ranks and chunks)"
                    ),
                    "decode_phase_grouping_policy": (
                        "Read=Batch+all input layers/requests/blocks/TP ranks; Write="
                        + ("request-wave" if compiler.share_phases else "per-rank")
                    ),
                    "prefill_dependency_policy": (
                        "all required predecessor phases (AND); no max-phase pruning"
                    ),
                    "decode_dependency_policy": (
                        "every Read waits only for its request's Prefill final phase; "
                        "whole-step Compute folds into downstream network delay; "
                        "retain all weighted predecessor phases without max-phase pruning; "
                        "unequal lags are conservatively padded or rejected by timing-policy=error"
                    ),
                    "decode_compute_phases": 0,
                    "decode_step_compute_phases": 0,
                    "decode_layer_compute_phases": 0,
                    "decode_compute_representation": "network dependency delay; no Decode TIMER records",
                    "decode_read_delay_policy": "storage latency only; no inter-layer Read/Compute gating",
                    "decode_prefetch_policy": "read all input KV after own Prefill, before whole-step Compute",
                    "decode_batch_release_policy": "ignore offline D_BATCH_BEGIN release forecasts; keep completion dependencies",
                    "max_phase_form_compaction": compiler.eager_max_phase,
                    "data_size_divisor": divisor,
                    "data_size_rounding": "ceil per original uint32 traffic chunk; minimum 1 byte",
                    "traffic_bytes": compiler.counts["traffic_bytes"],
                    "network_bytes": compiler.counts["network_bytes"],
                    "dependency_policy": (
                        "Prefill: exact AND; Decode: AND with folded compute delays; "
                        "every Decode Read uses its request's final Prefill phase"
                    ),
                    "storage_read_latency": ("from DAG rows" if args.storage_read_latency_us is None
                                             else delay_text(time_ns(args.storage_read_latency_us, "storage latency"))),
                    "conservative_examples": compiler.examples,
                    "max_phase_examples": compiler.max_phase_examples,
                    "phase_grouping": (
                        "Prefill: Read=Batch+Layer, Compute=Batch+Layer TIMER, "
                        "Write=Batch+Layer; Decode Read=Batch+all input; Decode Write="
                        + ("request-wave" if compiler.share_phases else "per-rank")
                    ),
                    "group_completion": "all traffic/control rows in a phase must finish",
                    "approximation": (
                        "Prefill keeps explicit AND dependency sets; absolute release-floor padding is conservative; "
                        "Decode batch-start forecast releases are ignored; Read delay is storage latency only; "
                        "Decode computation is folded into network delays without TIMERs; "
                        "unequal predecessor lags use conservative padding or fail under timing-policy=error"
                    ),
                })
                if summary_path:
                    (temp / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            finally:
                if mapping_file:
                    mapping_file.close()
            # Publish only after the whole DAG has passed validation.
            os.replace(temp / "traffic.csv", output)
            os.replace(temp / "traffic_debug.csv", debug_path)
            if mapping_path:
                os.replace(temp / "mapping.csv", mapping_path)
            if summary_path:
                os.replace(temp / "summary.json", summary_path)
    print(f"Wrote {output}: {summary.get('traffic_rows', 0)} READ/WRITE rows + "
          f"{summary.get('timer_rows', 0)} TIMER rows, "
          f"{summary.get('network_phases', 0)} network phases, "
          f"{summary.get('prefill_compute_phases', 0) + summary.get('prefill_control_phases', 0)} "
          f"Prefill TIMER phases, {summary.get('folded_decode_compute_tasks', 0)} "
          f"folded Decode Compute tasks (no Decode TIMERs), {summary.get('requests', 0)} requests")
    print(f"Wrote {debug_path}: same rows plus request_id (empty for batch-owned TIMERs)")
    if divisor != 1:
        print(f"Data sizes: ceil(each original traffic row / {divisor}), minimum 1 byte; "
              f"{summary['network_bytes']} -> {summary['traffic_bytes']} payload bytes")
    if summary.get("max_phase_reduced_tasks", 0):
        print(f"NOTE: {summary['max_phase_reduced_tasks']} network tasks kept only their largest predecessor "
              f"phaseId; {summary['discarded_phase_dependencies']} other phase dependencies were dropped.", file=sys.stderr)
    if summary.get("max_phase_compacted_forms", 0):
        print(f"NOTE: {summary['max_phase_compacted_forms']} local dependency forms were compacted "
              f"to their largest phaseId under max-phase; "
              f"{summary['max_phase_compacted_terms']} lower-phase entries were discarded early.", file=sys.stderr)
    if summary.get("conservative_phases", 0) and args.timing_policy == "max-phase":
        print(f"NOTE: {summary['conservative_phases']} phases use padding to preserve absolute release floors "
              "relative to the selected predecessor; see --summary-output for details.", file=sys.stderr)
    elif summary.get("conservative_phases", 0):
        print(f"WARNING: {summary['conservative_phases']} phases use conservative common delay; "
              "starts can be later than the weighted DAG gate. "
              "Use --summary-output for details or --timing-policy error to reject.", file=sys.stderr)
    if summary.get("mixed_delay_phases", 0):
        print(f"NOTE: {summary['mixed_delay_phases']} shared phases contain different per-flow delays; "
              "downstream tasks wait for all members. This is the configured barrier approximation.", file=sys.stderr)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", action="version", version=VERSION)
    parser.add_argument("task_dag")
    parser.add_argument("--output", help="traffic output path (takes precedence over --out-dir)")
    parser.add_argument("--out-dir", help="output directory; default current directory")
    parser.add_argument("--work-dir", help="optional local directory for temporary SQLite; traffic stays beside the output for atomic publication")
    parser.add_argument("--traffic-name", default="traffic.csv")
    parser.add_argument("--debug-output", help="debug CSV path; default traffic_debug.csv beside traffic output; "
                        "same rows plus request_id, empty for batch-owned TIMERs")
    parser.add_argument("--data-size-divisor", "--divisor", type=int, default=1,
                        help="divide each original traffic row's bytes by this positive integer, "
                             "rounding up to at least 1 byte; default 1 (no scaling)")
    parser.add_argument("--mapping-output", help="optional offline DAG/traffic mapping; not a simulator input")
    parser.add_argument("--summary-output", help="optional offline conversion/approximation report")
    parser.add_argument("--priority", type=int, default=7)
    parser.add_argument("--start-phase-id", type=int, default=1)
    parser.add_argument("--start-task-id", type=int, default=0)
    parser.add_argument("--storage-read-latency-us", "--storage-latency-us", default=None,
                        help="override each ASU read's DAG latency; default uses the DAG configuration")
    parser.add_argument("--phase-grouping", choices=("request-wave", "per-rank"), default="request-wave",
                        help="Decode Write grouping: request-wave (default) or per-rank; "
                             "Decode input Reads always share a Batch completion phase; "
                             "Prefill uses separate Batch+Layer Read and Write phases")
    parser.add_argument("--timing-policy", type=lambda value: value.replace("_", "-"),
                        choices=("max-phase", "conservative", "error"), default="max-phase",
                        help="legacy DAG timing policy: max-phase, conservative, or error; "
                             "V15 Prefill and Decode preserve exact AND dependencies and use explicit "
                             "Compute TIMERs; Decode reads all input before whole-step Compute")
    parser.add_argument("--no-progress", action="store_true", help="disable progress display")
    parser.add_argument("--progress-interval", type=float, default=0.5,
                        help="terminal progress refresh interval in seconds (default 0.5)")
    args = parser.parse_args()
    try:
        convert(args)
    except (ValueError, OSError, csv.Error, sqlite3.Error) as exc:
        print(f"Conversion failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
