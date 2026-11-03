"""V15-compatible logical TP DAG -> native nine-column ns-3-ub traffic.

This is the V14.11 conversion algorithm, unchanged except this version label.
V15 changes per-engine scheduling upstream and retains the logical DAG schema.

Logical TP rows expand using tp_members; all physical rank flows are retained.

No simulator patch, control task, comment record, or predicted network timing.
Default 'max-phase' retains only the largest predecessor phaseId.
Default phase grouping shares a phase within one request and network wave
when the retained dependency phase sets match. A phase is sealed before any
successor consumes it; later matching flows start a fresh group. ALL rows in
a shared phase must finish. --phase-grouping per-rank restores separate phases.
V14 streams tasks in topological order; batch tasks may have no request ID.
Every uint32 chunk of a physical rank flow shares its completion phase.
Token-level DECODE_WRITE bytes are already TP-sharded and are not expanded.
Optional --data-size-divisor scales each emitted uint32 chunk, rounding up
to at least one byte. Scaling happens AFTER TP expansion and chunk splitting;
it preserves the two-step converter-then-scaler row count and dependencies.

The default 'max-phase' keeps only the largest predecessor phaseId and that
phase's own accumulated delay. It intentionally drops other phase waits:
the largest phaseId does NOT necessarily finish last.
'conservative' retains all predecessor phases with a conservative common
delay; 'error' rejects unequal-delay or absolute-floor approximations.
Same-wave flows with identical retained dependency phase sets may share a
phase and have DIFFERENT delays. ALL members must finish to complete a phase.
The matching revised generator's DAG schema is unchanged by file renaming.
Performance revision: compact native shared max-phase forms early; inherit
already-pruned single-predecessor forms after sealing shared source phases;
short-circuit release-floor checks; write validated native fields without
rescanning dependency text for CSV quoting. Legacy shared-phase inputs retain
their complete weighted-pruning path.
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
VERSION = "v15-v14.11-converter-unchanged-max-phase-request-wave-20261027"

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
    def __init__(self, stream):
        self.write = stream.write

    def writeheader(self):
        self.write(",".join(OUTPUT_FIELDS) + "\r\n")

    def writerow(self, row):
        self.write(f"{row['taskId']},{row['sourceNode']},{row['destNode']},"
                   f"{row['dataSize(Byte)']},{row['opType']},{row['priority']},"
                   f"{row['delay']},{row['phaseId']},")
        self.write(row['dependOnPhases'])
        self.write("\r\n")


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
        db.executescript(
            "CREATE TABLE tasks(id INTEGER PRIMARY KEY, form TEXT NOT NULL);"
            "CREATE TABLE phases(id INTEGER PRIMARY KEY, parents TEXT NOT NULL,"
            " delay INTEGER NOT NULL, floor INTEGER NOT NULL);"
            "CREATE TABLE requests(id INTEGER PRIMARY KEY);"
            "CREATE TABLE parent_gates(id INTEGER PRIMARY KEY, parents TEXT NOT NULL);"
            "CREATE TABLE shared_groups(owner TEXT, wave TEXT, parents TEXT, form TEXT NOT NULL,"
            " PRIMARY KEY(owner, wave, parents)) WITHOUT ROWID;"
        )

    def save_shared_group(self, key, group):
        record = dict(group, parents=list(group["parents"]),
                      delays=sorted(group["delays"]), types=sorted(group["types"]))
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
                self.cache_shared_group(key, group)
        if group is not None and self.sealed_phases[group["pid"] - self.phase_base]:
            return None
        return group

    def seal_terms(self, row, terms):
        if self.share_phases and row.get("execution_schema") in {V14_SCHEMA, LOGICAL_SCHEMA}:
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
            if self.share_phases:
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

    def incoming(self, row):
        row["_incoming_pruned"] = False
        root = time_ns(row["release_time_us"], "release_time_us")
        terms = {}
        deps = row.get("depends_on", "").strip()
        missing = []
        values = list(filter(None, re.split(r"[;,\s]+", deps)))
        if row.get("_immutable_phase_graph") is True and len(values) == 1:
            tid = integer(values[0], "depends_on")
            found = self.task_form(tid)
            if found is not None:
                # Stored V14 local forms are already normalized; rank-group
                # aliases consist of independent sibling phases. A uniform
                # local/read lag shift preserves weighted dominance exactly.
                self.seal_terms(row, found[1])
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
                    streaming_shared = schema == V14_SCHEMA and self.share_phases
                    if not streaming_shared and wave_phases and any(wave_phases.get(p) == wave for p in parents):
                        raise ValueError("a wave cannot depend on its own completion barrier")
                    signature = (wave, tuple(parents))
                    shared_key = None
                    if streaming_shared:
                        owner = (f"request:{request}" if request != "" else
                                 f"logical:{row['_logical_task_id']}" if "_logical_task_id" in row else f"task:{tid}")
                        shared_key = (owner, wave, tuple(parents))
                        group = self.shared_group(shared_key)
                    else:
                        group = groups.get(signature)
                    if group is None:
                        pid = self.next_phase
                        if pid > UINT32_MAX:
                            raise ValueError("phase ID exceeds uint32")
                        parent_ref = self.register_phase(pid, parents, delay, floor)
                        if not ephemeral:
                            self.db.execute("INSERT INTO phases VALUES (?, ?, ?, ?)",
                                            (pid, parent_ref, delay, floor))
                        self.next_phase += 1
                        group = dict(pid=pid, parents=tuple(parents), max_delay=delay,
                                     floor=floor, delays={delay}, types={stage}, approximate=approximate)
                        if streaming_shared:
                            self.cache_shared_group(shared_key, group)
                        else:
                            groups[signature] = group
                        wave_phases[pid] = wave
                        self.counts["network_phases"] += 1
                        self.counts["conservative_phases"] += approximate
                    pid = group["pid"]
                    if parents and parents[-1] >= pid:
                        raise ValueError("phase dependencies must precede their completion phase")
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
                        # This row changes only this phase's own record.
                        # Cached parents' records remain valid.
                        self.phase_cache.pop(pid, None)
                        # Legacy shared phases can gain a larger delay.
                        # Cached dominance results must then be recomputed.
                        self.prune_cache.clear()
                        self.prune_cache_terms = 0
                    if approximate and not group["approximate"]:
                        self.counts["conservative_phases"] += 1
                        group["approximate"] = True
                    source, dest, src_port, dst_port = ub_endpoints(row, stage)
                    # Native ns-3-ub tokenizes this CSV field on whitespace.
                    # Empty means no dependency; signed -1 is INVALID.
                    dep_text = row["_dependency_text"] if prepared is not None else " ".join(map(str, parents))
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
                    root += duration
                    terms = {p: lag + duration for p, lag in terms.items()}
                    root, terms = self.normalize(root, terms, row.get("_incoming_pruned") is True)
                    out_form = [root, sorted(terms.items())]
                    self.counts["folded_local_tasks"] += 1
                else:
                    raise ValueError(f"unsupported task_class: {kind}")
                self.last_out_form = out_form
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
            key = (expanded_deps, time_ns(row["release_time_us"], "release_time_us"), read_ns)
            cached = self.prepare_cache.get(key)
            if cached is not None:
                self.prepare_cache.move_to_end(key)
                prepared_terms, native_inputs, dependency_text = cached
            else:
                incoming_row = dict(row, depends_on=expanded_deps)
                root, terms = self.compiler.incoming(incoming_row)
                root += read_ns
                if read_ns:
                    terms = {pid: lag + read_ns for pid, lag in terms.items()}
                root, terms = self.compiler.normalize(root, terms, incoming_row.get("_incoming_pruned") is True)
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
                # Predecessor phases are sealed on consumption in shared
                # mode, or immutable in per-rank mode. Cached gates stay valid.
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
    mapping_path = Path(args.mapping_output).resolve() if args.mapping_output else None
    summary_path = Path(args.summary_output).resolve() if args.summary_output else None
    paths = [p for p in (output, mapping_path, summary_path) if p is not None]
    if source in paths or len(set(paths)) != len(paths):
        raise ValueError("input, traffic, mapping and summary paths must be distinct")
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
                    writer = NativeTrafficWriter(fout)
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
                    "input": str(source), "output": str(output), "dag_schema": schema,
                    "timing_policy": args.timing_policy,
                    "phase_grouping_policy": ("legacy-request-wave" if schema == SCHEMA else
                                              "request-wave" if compiler.share_phases else "per-rank"),
                    "max_phase_form_compaction": compiler.eager_max_phase,
                    "data_size_divisor": divisor,
                    "data_size_rounding": "ceil per original uint32 traffic chunk; minimum 1 byte",
                    "traffic_bytes": compiler.counts["traffic_bytes"],
                    "network_bytes": compiler.counts["network_bytes"],
                    "dependency_policy": ("largest phaseId only" if args.timing_policy == "max-phase"
                                          else "all predecessor phases"),
                    "storage_read_latency": ("from DAG rows" if args.storage_read_latency_us is None
                                             else delay_text(time_ns(args.storage_read_latency_us, "storage latency"))),
                    "conservative_examples": compiler.examples,
                    "max_phase_examples": compiler.max_phase_examples,
                    "phase_grouping": ("same request/wave and retained dependency phase set; seal before successor use; per-flow delays may differ"
                                       if compiler.share_phases and schema in {V14_SCHEMA, LOGICAL_SCHEMA} else
                                       "one phase per expanded TP rank flow; uint32 chunks share completion" if schema == LOGICAL_SCHEMA else
                                       "one phase per DAG network task; uint32 chunks share completion" if schema == V14_SCHEMA else
                                       "same request/wave and retained dependency phase set; per-flow delays may differ"),
                    "group_completion": "all READ/WRITE rows in a phase must finish",
                    "approximation": (
                        "unselected predecessor phases are not waited for; largest phaseId need not finish last; "
                        "absolute release floors use conservative padding; shared completion barriers can delay tasks"
                        if args.timing_policy == "max-phase" else
                        "shared completion barriers can delay downstream tasks; compute overlap is approximate"),
                })
                if summary_path:
                    (temp / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            finally:
                if mapping_file:
                    mapping_file.close()
            # Publish only after the whole DAG has passed validation.
            os.replace(temp / "traffic.csv", output)
            if mapping_path:
                os.replace(temp / "mapping.csv", mapping_path)
            if summary_path:
                os.replace(temp / "summary.json", summary_path)
    print(f"Wrote {output}: {summary.get('traffic_rows', 0)} READ/WRITE rows, "
          f"{summary.get('network_phases', 0)} phases, {summary.get('requests', 0)} requests")
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
                        help="request-wave (default): share phases within a request/wave with identical retained dependencies; "
                             "per-rank: restore separate V14 rank phases")
    parser.add_argument("--timing-policy", type=lambda value: value.replace("_", "-"),
                        choices=("max-phase", "conservative", "error"), default="max-phase",
                        help="max-phase: keep largest predecessor phaseId and its own delay; "
                             "conservative: retain all phases with safe common delay; "
                             "error: reject nonrepresentable common delays")
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
