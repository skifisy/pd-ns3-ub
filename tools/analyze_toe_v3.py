#!/usr/bin/env python3
"""Single-file ECMP/ToE analysis and result summarizer.

Default mode feeds task summary accumulators while writing task comparisons;
it never rereads task_compare.csv. Small result tables are read for rendering. Use
--summary-only to summarize an existing analysis directory without re-reading
raw traces. Python 3.9+, standard library only.

Examples:
  python3 analyze_toe_v3.py --baseline /data/ecmp --toe /data/toe --output /data/result
  python3 analyze_toe_v3.py --summary-only /data/result --max-lines 100
  python3 analyze_toe_v3.py --demand-only /data/result --demand-file ToE /data/toe/toe/toe_demand.csv
"""
# Embedded summary implementation.
"""Summarize analyze_toe_v1.py outputs for sharing with an AI assistant.

Python 3.9+, standard library only. Does not change any input CSV/JSON/log.
The generated report is capped at 1500 physical lines.

Examples:
  python3 summarize_toe_v3.py /path/to/toe_analysis_v1
  python3 summarize_toe_v3.py --input /path/to/results --output toe_for_ai.txt
  python3 summarize_toe_v3.py --input /path/to/run_a --input /path/to/run_b

Default output: FIRST_INPUT/toe_ai_summary_v3.txt. Strict report limits:
1500 lines, 120000 characters. Use --max-lines to lower the line limit.
Representative tasks are selected by absolute FCT increase/decrease, delayed
completion, and observed pause exposure. Task dictionaries are streamed using
bounded sample heaps. Exact focus quantiles retain numeric FCT arrays and
group indices, not all task dictionaries. Missing ECMP P99 metadata requires
one preliminary pass. Topology changes are retained per case/epoch.

In summary-only mode, --include-link-samples scans task_link_usage.csv.
Integrated mode queries selected TaskIds from the existing database instead.
The default avoids this extra potentially large scan. NPU/ASU/leaf/port
observations already included in task_compare.csv are retained in the report.
v3 writes topology_group_changes_v3.csv, topology_group_totals_v3.csv,
task_focus_summary.csv and task_focus_samples.csv.
Leaf roles: P=37..48, D=49..52, S=53..56. Group changes are undirected,
not traffic measurements or complete topology capacity snapshots.
"""
import argparse
import csv
import heapq
import json
import math
import os
import sys
import time
from collections import Counter, defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path
from array import array

SUMMARY_VERSION = "v3.2.0"
HARD_MAX_LINES = 1500
ROLE_GROUPS = ("P↔P", "P↔D", "P↔S", "D↔S")
ROLE_RANGES = (("P", 37, 48), ("D", 49, 52), ("S", 53, 56))
GROUP_METRICS = ("pair_change_events", "added_circuits", "removed_circuits", "net_delta_circuits",
                 "abs_delta_circuits", "added_capacity_gbps", "removed_capacity_gbps",
                 "net_delta_capacity_gbps", "reversal_circuits", "rapid_reversal_pairs")
FOCUS_TAGS = ("TAIL_ENTER", "TAIL_EXIT", "BASELINE_TAIL_REGRESSED", "BASELINE_TAIL_IMPROVED",
              "EXPANDED_REGRESSION", "REDUCED_REGRESSION", "NO_PAUSE_REGRESSION", "FINISH_DELAY",
              "FCT_WORST", "FCT_BEST") + tuple("ROLE_%s_%s" % (g, k) for g in ROLE_GROUPS for k in ("WORST", "BEST"))
FILES = ["run_comparison_summary.csv", "fct_group_summary.csv", "reconfiguration_events.csv",
         "topology_changes.csv", "hotspot_effect_summary.csv", "event_task_cohorts.csv",
         "task_compare.csv", "task_link_usage.csv", "analysis_summary.json", "analysis.log"]
NULLS = {"", "na", "n/a", "nan", "none", "null", "-"}
ESCAPES = {ord(c): "\\u%04x" % ord(c) for c in "\n\r\v\f\x1c\x1d\x1e\x85\u2028\u2029"}


def clean(value, limit=200):
    text = str(value).translate(ESCAPES)
    if len(text) > limit:
        text = text[:max(0, limit - 25)] + "…[truncated:%d chars]" % len(text)
    return text


def number(value):
    if value is None or str(value).strip().lower() in NULLS:
        return None
    try:
        value = float(value)
        return value if math.isfinite(value) else None
    except (ValueError, TypeError):
        return None


def flag(value):
    return str(value).strip().lower() in ("1", "true", "yes")


def fmt(value):
    x = number(value)
    if x is not None:
        return format(x, ".8g")
    if value is None or str(value).strip().lower() in NULLS:
        return "NA"
    return clean(value, 100)


def fields(row, mapping):
    return " ".join(label + "=" + fmt(row.get(key)) for label, key in mapping)


def rows(source, name):
    path = source.path / name
    if not path.is_file():
        source.counts[name] = None
        return
    count = 0
    with path.open(newline="", encoding="utf-8-sig") as f:
        r = csv.DictReader(f)
        if not r.fieldnames:
            raise ValueError("missing CSV header: %s" % path)
        r.fieldnames = [k.strip() for k in r.fieldnames]
        if len(set(r.fieldnames)) != len(r.fieldnames):
            raise ValueError("duplicate CSV header: %s" % path)
        for row in r:
            if None in row:
                raise ValueError("extra CSV cells at %s:%d" % (path, r.line_num))
            if not any(v for v in row.values()):
                continue
            count += 1
            yield row
    source.counts[name] = count


class Top:
    def __init__(self, limit):
        self.limit, self.heap, self.seq = limit, [], 0

    def add(self, score, value):
        if score is None or not math.isfinite(score):
            return
        item = (score, self.seq, value)
        self.seq += 1
        if len(self.heap) < self.limit:
            heapq.heappush(self.heap, item)
        elif score > self.heap[0][0]:
            heapq.heapreplace(self.heap, item)

    def values(self):
        return [v for score, seq, v in sorted(self.heap, reverse=True)]


@dataclass
class Aggregate:
    n: int = 0
    base: float = 0
    toe: float = 0
    delta: float = 0
    positive_delta: float = 0
    negative_delta: float = 0
    regressed: int = 0
    improved: int = 0
    unchanged: int = 0
    overlap_n: int = 0
    direct_n: int = 0
    max_delta: float = 0
    # Compensated sums reduce error across millions of tasks.
    compensation: dict = field(default_factory=dict)

    def add_sum(self, name, value):
        y = value - self.compensation.get(name, 0)
        total = getattr(self, name)
        new = total + y
        self.compensation[name] = (new - total) - y
        setattr(self, name, new)

    def add(self, b, t, d, epsilon, pause, direct):
        self.n += 1
        for name, value in [("base", b), ("toe", t), ("delta", d),
                            ("positive_delta", max(0, d)), ("negative_delta", min(0, d))]:
            self.add_sum(name, value)
        self.regressed += int(d > epsilon)
        self.improved += int(d < -epsilon)
        self.unchanged += int(abs(d) <= epsilon)
        self.overlap_n += int(pause is not None and pause > 0)
        self.direct_n += int(direct is not None and direct > 0)
        self.max_delta = max(self.max_delta, d)

    def describe(self):
        return ("N=%d meanFCT(us)=%s→%s gain(%%)=%s sumDelta(us)=%s "
                "regress/improve/equal=%d/%d/%d overlap/directPaused=%d/%d") % (
            self.n, fmt(self.base / self.n) if self.n else "NA", fmt(self.toe / self.n) if self.n else "NA",
            fmt(-self.delta / self.base * 100) if self.base else "NA", fmt(self.delta),
            self.regressed, self.improved, self.unchanged, self.overlap_n, self.direct_n)


class SummaryCase:
    def __init__(self, name, args):
        self.name, self.run, self.groups = name, {}, {}
        self.cap, self.warnings = {}, []
        self.statuses, self.hot_conclusions, self.event_statuses = Counter(), Counter(), Counter()
        self.totals = Aggregate()
        self.task_rows = self.invalid_tasks = self.inconsistent_delta = 0
        self.path_observed = Counter()
        self.host_groups, self.leaf_groups, self.topology_pairs = {}, {}, {}
        self.skipped_groups = Counter()
        self.task_heaps = {k: Top(args.task_top) for k in ("FCT_WORST", "FCT_BEST", "FINISH_DELAY", "PAUSE_OVERLAP_WORST", "DIRECT_PAUSE_WORST")}
        self.event_heaps = {k: Top(args.event_top) for k in ("pause", "latest", "earliest", "problem")}
        self.hot_heaps = {k: Top(args.top) for k in ("regression", "benefit", "missing_evidence")}
        self.selected_events, self.cohorts, self.samples = {}, {}, {}
        self.cost_sum = self.gain_sum = 0
        self.cost_n = self.metadata_partial = 0
        self.topology_group_events, self.topology_seen, self.topology_expected = {}, {}, {}
        self.topology_group_totals = defaultdict(Counter)
        self.topology_checks = Counter()
        self.focus_threshold, self.focus_threshold_source = None, "unknown"
        self.focus_b, self.focus_t = array("d"), array("d")
        self.focus_d, self.focus_flags = array("d"), array("B")
        self.focus_size, self.focus_start, self.focus_finish = array("d"), array("d"), array("d")
        self.focus_meta_flags = array("H")
        self.focus_stats, self.focus_heaps, self.focus_counts = {}, {k: Top(args.focus_top) for k in FOCUS_TAGS}, Counter()

    def choose_events(self, limit):
        candidates = {}
        for bucket in self.event_heaps.values():
            for row in bucket.values():
                candidates[row.get("epoch", "?")] = row
        def rank(r):
            status = r.get("event_status", "")
            problem = int(status not in ("applied", "no_change") or (number(r.get("degree_delta_max")) or 0) > 0)
            return problem, number(r.get("pause_seconds")) or 0, number(r.get("epoch")) or 0
        picked = sorted(candidates.values(), key=rank, reverse=True)[:limit]
        self.selected_events = {r.get("epoch", "?"): r for r in picked}


@dataclass
class Source:
    path: Path
    label: str
    meta: dict = field(default_factory=dict)
    counts: dict = field(default_factory=dict)
    cases: dict = field(default_factory=dict)
    warnings: list = field(default_factory=list)
    log_lines: deque = field(default_factory=lambda: deque(maxlen=20))
    log_warning_count: int = 0
    epsilon: float = .001

    def case(self, name, args):
        name = name or "UNSPECIFIED_CASE"
        if name not in self.cases:
            self.cases[name] = SummaryCase(name, args)
            for item in self.meta.get("cases", []):
                if item.get("name") == name:
                    self.cases[name].cap = item.get("capabilities", {})
                    self.cases[name].warnings = item.get("warnings", [])
                    break
        return self.cases[name]


def required_int(row, key, negative=False):
    value = number(row.get(key))
    if value is None or not value.is_integer() or (value < 0 and not negative):
        raise ValueError("topology row: invalid %s=%r" % (key, row.get(key)))
    return int(value)


def leaf_role(node):
    return next((role for role, lo, hi in ROLE_RANGES if lo <= node <= hi), "UNKNOWN")


def role_group(a, b):
    roles = (leaf_role(a), leaf_role(b))
    if "UNKNOWN" in roles:
        return "UNKNOWN"
    x, y = sorted(roles, key=("P", "D", "S").index)
    return x + "↔" + y


def group_order(group):
    order = ROLE_GROUPS + ("D↔D", "S↔S", "UNKNOWN")
    return order.index(group)


def add_topology_group(case, row, args):
    a, b = required_int(row, "leaf_a"), required_int(row, "leaf_b")
    epoch = str(required_int(row, "epoch"))
    for name in ("circuits_before", "circuits_after", "delta_circuits", "capacity_before_gbps",
                 "capacity_after_gbps", "delta_capacity_gbps", "reversal_circuits", "applied_seconds"):
        raw = row.get(name)
        if raw is not None and str(raw).strip().lower() not in NULLS and number(raw) is None:
            raise ValueError("topology row: invalid numeric %s=%r" % (name, raw))
    if a == b:
        raise ValueError("topology row: self-loop at leaf %d" % a)
    before, after = [required_int(row, k) if number(row.get(k)) is not None else None
                     for k in ("circuits_before", "circuits_after")]
    if number(row.get("delta_circuits")) is None:
        if before is None or after is None:
            raise ValueError("topology row: require delta_circuits or circuits_before/after")
        delta = after - before
    else:
        delta = required_int(row, "delta_circuits", negative=True)
    if before is not None and after is not None and delta != after - before:
        raise ValueError("topology row: delta_circuits differs from circuits_after-before")
    cap = number(row.get("delta_capacity_gbps"))
    cb, ca = number(row.get("capacity_before_gbps")), number(row.get("capacity_after_gbps"))
    if cap is not None and cb is not None and ca is not None and not math.isclose(cap, ca - cb, abs_tol=1e-6):
        raise ValueError("topology row: inconsistent capacity delta")
    derived = cap is None and (cb is None or ca is None)
    if cap is None:
        cap = ca - cb if cb is not None and ca is not None else delta * args.circuit_gbps
    if (delta == 0 and abs(cap) > 1e-6) or (delta != 0 and cap * delta <= 0):
        raise ValueError("topology row: capacity delta and circuit delta disagree")
    reverse = number(row.get("reversal_circuits"))
    rapid = None if str(row.get("rapid_reversal", "")).lower() in NULLS else int(flag(row.get("rapid_reversal")))
    if reverse is not None and (not reverse.is_integer() or reverse < 0 or reverse > abs(delta)):
        raise ValueError("topology row: reversal_circuits outside 0..abs(delta)")
    if reverse is not None:
        reverse = int(reverse)
    when = number(row.get("applied_seconds"))
    key = (epoch, min(a, b), max(a, b))
    signature = (before, after, delta, cap, reverse, rapid, when)
    if key in case.topology_seen:
        if case.topology_seen[key] != signature:
            raise ValueError("conflicting duplicate topology pair: %s epoch=%s pair=%d-%d" % (case.name, *key))
        case.topology_checks["duplicate_pair_rows_ignored"] += 1
        return False
    case.topology_seen[key] = signature
    row["epoch"], row["delta_circuits"] = epoch, str(delta)
    event = case.topology_group_events.setdefault(epoch, {"groups": defaultdict(Counter), "time": when})
    if when is not None and event["time"] is not None and not math.isclose(when, event["time"], abs_tol=1e-6):
        raise ValueError("inconsistent topology applied_seconds in epoch " + epoch)
    if event["time"] is None:
        event["time"] = when
    if delta == 0:
        case.topology_checks["zero_delta_rows"] += 1
        return True
    group = role_group(a, b)
    if group == "UNKNOWN":
        case.topology_checks["unknown_role_pair_rows"] += 1
    case.topology_checks["capacity_rows_derived_from_circuit_gbps"] += int(derived)
    values = dict(pair_change_events=1, added_circuits=max(delta, 0), removed_circuits=max(-delta, 0),
                  net_delta_circuits=delta, abs_delta_circuits=abs(delta), added_capacity_gbps=max(cap, 0),
                  removed_capacity_gbps=max(-cap, 0), net_delta_capacity_gbps=cap,
                  reversal_circuits=reverse or 0, rapid_reversal_pairs=rapid or 0,
                  missing_reversal_rows=int(reverse is None), missing_rapid_rows=int(rapid is None))
    event["groups"][group].update(values)
    case.topology_group_totals[group].update(values)
    return True


def finish_topology_groups(source):
    for case in source.cases.values():
        for epoch, expected in case.topology_expected.items():
            if expected.get("event_status") == "applied" and epoch not in case.topology_group_events:
                case.topology_group_events[epoch] = {"groups": {}, "time": number(expected.get("applied_seconds")),
                                                     "coverage": "missing_topology_rows"}
        for epoch, event in case.topology_group_events.items():
            if event.get("coverage") == "missing_topology_rows":
                continue
            expected, checks = case.topology_expected.get(epoch, {}), []
            if event["time"] is None:
                event["time"] = number(expected.get("applied_seconds"))
            for key in ("added_circuits", "removed_circuits"):
                value = number(expected.get(key))
                if value is not None:
                    checks.append(value == sum(g[key] for g in event["groups"].values()))
            n = number(expected.get("changed_leaf_pairs"))
            if n is not None:
                checks.append(n == sum(g["pair_change_events"] for g in event["groups"].values()))
            event["coverage"] = "partial_or_mismatched" if False in checks else "matched_event_counts" if checks else "not_verified"
        bad = Counter(e["coverage"] for e in case.topology_group_events.values()
                      if e["coverage"] in ("missing_topology_rows", "partial_or_mismatched"))
        if bad:
            case.warnings.append("topology role-group coverage=" + json.dumps(bad, separators=(",", ":")))


def topology_groups(groups):
    return sorted(set(ROLE_GROUPS) | set(groups), key=group_order)


def group_values(values, missing=False):
    result = {key: None if missing else values.get(key, 0) for key in GROUP_METRICS}
    for key, missing_key in (("reversal_circuits", "missing_reversal_rows"), ("rapid_reversal_pairs", "missing_rapid_rows")):
        if values.get(missing_key):
            result[key] = None
    return result


def group_description(group, values, missing=False):
    v = group_values(values, missing)
    return ("group=%s add=%s remove=%s net=%s abs=%s pairs=%s netGbps=%s reversal=%s rapid=%s" %
            (group, *(fmt(v[k]) for k in ("added_circuits", "removed_circuits", "net_delta_circuits", "abs_delta_circuits",
                                          "pair_change_events", "net_delta_capacity_gbps", "reversal_circuits", "rapid_reversal_pairs"))))


def load_group_totals(source, args):
    if hasattr(source, "topology_states") and not (args.topology_snapshots or args.initial_topology):
        for case in source.cases.values():
            for epoch, event in case.topology_group_events.items():
                state = source.topology_states.get((case.name, int(epoch)))
                for key in ("before", "after"):
                    allocation = getattr(state, key, None)
                    event[key + "_totals"] = None if allocation is None else Counter()
                    if allocation is not None:
                        for (a, b), n in allocation.items():
                            event[key + "_totals"][role_group(a, b)] += n
                event["group_total_source"] = "parsed_analysis_topologies" if state and state.before is not None and state.after is not None else "unknown_missing_initial_or_snapshot"
        return
    def supplied(options, case, name):
        targets = (source.label + "/" + case.name, case.name)
        explicit = [Path(path) for target, path in options or [] if target in targets]
        if len(explicit) > 1:
            raise ValueError("duplicate topology override for " + case.name)
        if explicit:
            if not explicit[0].is_file(): raise ValueError("topology file not found: " + str(explicit[0]))
            return explicit[0]
        meta = next((c for c in source.meta.get("cases", []) if c.get("name") == case.name), {})
        raw = meta.get("files", {}).get(name)
        if raw:
            path = Path(raw)
            if not path.is_absolute(): path = source.path/path
            if path.is_file(): return path
        local = source.path/("toe_topology.csv" if name == "toe_topology" else "topology.csv")
        return local if len(source.cases) == 1 and local.is_file() else None
    def read_allocations(path, case, physical=False):
        snapshots, edges = defaultdict(dict), defaultdict(set)
        with path.open(newline="", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            fields = set(reader.fieldnames or [])
            # Some analyzer manifests point `files.toe_topology` at a physical
            # port-level topology file. Detect that format from its columns;
            # otherwise its rows have no `circuits` field and used to fail with
            # the opaque error `invalid circuits=None`.
            has_ports = any(k in fields for k in ("portId1", "port_a", "port1")) and any(
                k in fields for k in ("portId2", "port_b", "port2"))
            count_columns = ("circuits", "circuit_count", "link_count", "num_links", "links", "count")
            has_count = any(k in fields for k in count_columns)
            physical = physical or (has_ports and not has_count)
            for row_no, r in enumerate(reader, 2):
                if r.get("case_name") and r["case_name"] != case.name: continue
                a = next((r[k] for k in ("leaf_a", "nodeId1", "leaf_u", "node_a", "src_leaf") if r.get(k) is not None), None)
                b = next((r[k] for k in ("leaf_b", "nodeId2", "leaf_v", "node_b", "dst_leaf") if r.get(k) is not None), None)
                try:
                    pair = tuple(sorted((required_int({"a": a}, "a"), required_int({"b": b}, "b"))))
                except ValueError as exc:
                    raise ValueError("topology file %s row %d: invalid endpoints (%s, %s): %s" % (path, row_no, a, b, exc)) from exc
                raw = next((r[k] for k in count_columns if r.get(k) not in (None, "")), None)
                epoch = required_int({"epoch": r.get("epoch") or "0"}, "epoch")
                phase = (r.get("status") or r.get("phase") or ("initial" if epoch == 0 else "applied")).lower()
                if phase not in ("initial", "current", "before", "applied", "after"): continue
                key = epoch, phase
                row_port_a = next((r[k] for k in ("portId1", "port_a", "port1") if r.get(k) not in (None, "")), None)
                row_port_b = next((r[k] for k in ("portId2", "port_b", "port2") if r.get(k) not in (None, "")), None)
                if raw is None and (physical or (row_port_a is not None and row_port_b is not None)):
                    if any(leaf_role(n) == "UNKNOWN" for n in pair): continue
                    port_a, port_b = row_port_a, row_port_b
                    if port_a is None or port_b is None:
                        raise ValueError("physical topology file %s row %d requires port columns (portId1/portId2 or port_a/port_b)" % (path, row_no))
                    edge = tuple(sorted(((pair[0], port_a), (pair[1], port_b))))
                    if edge not in edges[key]:
                        edges[key].add(edge); snapshots[key][pair] = snapshots[key].get(pair, 0) + 1
                    continue
                if raw is None:
                    raise ValueError("topology file %s row %d has no circuits/link_count column; physical format requires port IDs" % (path, row_no))
                try:
                    n = required_int({"circuits": raw}, "circuits")
                except ValueError as exc:
                    raise ValueError("topology file %s row %d: invalid circuits=%r" % (path, row_no, raw)) from exc
                if pair in snapshots[key] and snapshots[key][pair] != n: raise ValueError("conflicting snapshot pair in " + str(path))
                snapshots[key][pair] = n
        return snapshots
    def totals(state):
        if state is None: return None
        result = Counter()
        for (a, b), n in state.items(): result[role_group(a, b)] += n
        return result
    for case in source.cases.values():
        full = supplied(args.topology_snapshots, case, "toe_topology")
        initial = supplied(args.initial_topology, case, "topology")
        snapshots = read_allocations(full, case) if full else {}
        state = None
        if initial:
            initial_rows = read_allocations(initial, case, physical=True)
            state = initial_rows.get((0, "initial"), initial_rows.get((0, "current"), initial_rows.get((0, "applied"))))
        state = snapshots.get((0, "initial"), snapshots.get((0, "current"), snapshots.get((0, "applied"), state)))
        mode = args.topology_log_format or source.meta.get("parameters", {}).get("topology_log_format", "full")
        epochs = sorted(set(map(int, case.topology_group_events)) | {e for e, phase in snapshots if e > 0 and phase in ("applied", "after")})
        for ep in epochs:
            event = case.topology_group_events.get(str(ep))
            before = snapshots.get((ep, "before"), snapshots.get((ep, "current"), state)) if mode == "full" else state
            explicit_after = snapshots.get((ep, "applied"), snapshots.get((ep, "after")))
            after, origin = None, "unknown_missing_initial_or_snapshot"
            if explicit_after is not None:
                after = explicit_after.copy() if mode == "full" else before.copy() if before is not None else None
                if mode == "delta" and after is not None: after.update(explicit_after)
                origin = "full_applied_snapshot" if mode == "full" else "initial_plus_delta_snapshots"
            elif event and before is not None and event["coverage"] not in ("missing_topology_rows", "partial_or_mismatched"):
                after = before.copy()
                for (epoch, a, b), signature in case.topology_seen.items():
                    if int(epoch) != ep: continue
                    old, new, delta = signature[:3]
                    if old is not None and after.get((a, b), 0) != old:
                        after = None; break
                    value = after.get((a, b), 0) + delta
                    if value < 0: after = None; break
                    after[a, b] = value
                origin = "initial_plus_changes" if after is not None else "unknown_pair_state_mismatch"
            if event:
                if before is None and after is not None and event["coverage"] not in ("missing_topology_rows", "partial_or_mismatched"):
                    before = after.copy()
                    for (epoch, a, b), signature in case.topology_seen.items():
                        if int(epoch) != ep: continue
                        value = before.get((a, b), 0) - signature[2]
                        if value < 0 or (signature[0] is not None and value != signature[0]): before = None; break
                        before[a, b] = value
                    origin = "applied_snapshot_minus_changes" if before is not None else "applied_snapshot_only"
                event["before_totals"], event["after_totals"] = totals(before), totals(after)
                event["group_total_source"] = origin
                if before is not None and after is not None:
                    mismatch = any(event["after_totals"][g] - event["before_totals"][g] != event["groups"].get(g, {}).get("net_delta_circuits", 0)
                                   for g in set(event["before_totals"]) | set(event["after_totals"]) | set(event["groups"]))
                    if mismatch:
                        event["coverage"] = "partial_or_mismatched"
                        case.warnings.append("epoch=%s group totals and observed changes disagree" % ep)
            state = after


def write_topology_groups(sources, directory):
    directory.mkdir(parents=True, exist_ok=True)
    common = ["source_label", "source_path", "case_name"]
    paths = []
    for kind in ("changes", "totals"):
        path = directory / ("topology_group_%s_v3.csv" % kind)
        specific = ["epoch", "applied_seconds"] if kind == "changes" else ["epochs_with_changes", "epochs_with_rows"]
        temp = path.with_name(path.name + ".tmp.%d" % os.getpid())
        try:
            with temp.open("w", newline="", encoding="utf-8-sig") as f:
                absolute = ["circuits_before", "circuits_after", "group_total_source"] if kind == "changes" else []
                writer = csv.DictWriter(f, fieldnames=common + specific + ["leaf_group", "coverage"] + absolute + list(GROUP_METRICS))
                writer.writeheader()
                for source in sources:
                    for case in source.cases.values():
                        base = dict(source_label=source.label, source_path=str(source.path), case_name=case.name)
                        events = sorted(case.topology_group_events.items(), key=lambda x: int(x[0]))
                        if kind == "changes":
                            for epoch, event in events:
                                for group in topology_groups(event["groups"]):
                                    missing = event["coverage"] == "missing_topology_rows"
                                    writer.writerow(dict(base, epoch=epoch, applied_seconds=event["time"], leaf_group=group,
                                        coverage=event["coverage"], circuits_before=event["before_totals"].get(group, 0) if event.get("before_totals") is not None else None,
                                        circuits_after=event["after_totals"].get(group, 0) if event.get("after_totals") is not None else None,
                                        group_total_source=event.get("group_total_source", "unknown"), **group_values(event["groups"].get(group, {}), missing)))
                        elif events:
                            bad = any(e["coverage"] in ("missing_topology_rows", "partial_or_mismatched") for _, e in events)
                            coverage = "partial_or_mismatched" if bad else "matched_event_counts" if all(e["coverage"] == "matched_event_counts" for _, e in events) else "not_verified"
                            for group in topology_groups(case.topology_group_totals):
                                writer.writerow(dict(base, leaf_group=group, coverage=coverage,
                                    epochs_with_changes=sum(e["groups"].get(group, {}).get("pair_change_events", 0) > 0 for _, e in events),
                                    epochs_with_rows=sum(e["coverage"] != "missing_topology_rows" for _, e in events),
                                    **group_values(case.topology_group_totals.get(group, {}), all(e["coverage"] == "missing_topology_rows" for _, e in events))))
            os.replace(temp, path)
        finally:
            if temp.exists():
                temp.unlink()
        paths.append(path)
    return paths


def percentile(values, q):
    if not values:
        return None
    rank = (len(values) - 1) * q
    lo, hi = math.floor(rank), math.ceil(rank)
    return values[lo] + (values[hi] - values[lo]) * (rank - lo)


def prepare_focus_thresholds(source, args):
    for case in source.cases.values():
        if not args.recompute_focus_p99:
            q = number(case.groups.get("all", {}).get("p99_fct_us_ecmp"))
            if q is None:
                q = number(case.run.get("p99_fct_us_ecmp"))
            if q is not None and q >= 0:
                case.focus_threshold, case.focus_threshold_source = q, "paired_summary_verified_after_scan"
    if source.cases and all(c.focus_threshold is not None for c in source.cases.values()):
        return
    values = defaultdict(lambda: array("d"))
    print("[%s] computing missing ECMP P99 thresholds from paired task FCTs" % source.label, file=sys.stderr)
    for row in rows(source, "task_compare.csv"):
        if row.get("comparison_status") != "paired_completed":
            continue
        b, t = number(row.get("fct_us_ecmp")), number(row.get("fct_us_toe"))
        if b is None or t is None or b < 0 or t < 0:
            continue
        case = source.case(row.get("case_name"), args)
        if case.focus_threshold is None:
            values[case.name].append(b)
    for name, sequence in values.items():
        case = source.cases[name]
        case.focus_threshold = percentile(sorted(sequence), .99)
        case.focus_threshold_source = "computed_paired_linear"


@dataclass
class FocusStats:
    agg: Aggregate = field(default_factory=Aggregate)
    indices: object = field(default_factory=lambda: array("I"))
    counts: Counter = field(default_factory=Counter)
    qs: dict = field(default_factory=dict)


def observed_role(row, endpoint, side):
    available = row.get("path_data_available_" + side)
    if available is not None and str(available).lower() not in NULLS and not flag(available):
        return "UNKNOWN"
    nodes = []
    one = number(row.get(endpoint + "_node_id_" + side))
    if one is not None:
        if not one.is_integer():
            return "UNKNOWN"
        nodes.append(int(one))
    raw = row.get(endpoint + "_node_ids_" + side)
    if raw and str(raw).lower() not in NULLS:
        try:
            many = json.loads(raw)
            if not isinstance(many, list) or any(number(x) is None or not number(x).is_integer() for x in many):
                return "UNKNOWN"
            nodes.extend(int(x) for x in many)
        except (ValueError, TypeError):
            return "UNKNOWN"
    roles = {leaf_role(n) for n in nodes}
    return "UNKNOWN" if not roles or "UNKNOWN" in roles else next(iter(roles)) if len(roles) == 1 else "MIXED"


def task_role_group(row, side):
    roles = [observed_role(row, k, side) for k in ("leaf1", "leaf2")]
    if "UNKNOWN" in roles:
        return "UNKNOWN"
    if "MIXED" in roles:
        return "MIXED"
    return "↔".join(sorted(roles, key=("P", "D", "S").index))


def focus_epoch(row, key):
    if row.get("topology_epoch_data_available_toe") not in (None, "") and not flag(row.get("topology_epoch_data_available_toe")):
        return None
    value = number(row.get(key))
    return int(value) if value is not None and value >= 0 and value.is_integer() else None


def add_focus_task(case, row, args, epsilon, record):
    b, t = number(row["fct_us_ecmp"]), number(row["fct_us_toe"])
    d, threshold = number(row.get("delta_fct_us")), case.focus_threshold
    if d is None:
        d = t - b
    tail_b, tail_t = (b >= threshold, t >= threshold) if threshold is not None else (None, None)
    tail = "unknown" if threshold is None else "tail_stay" if tail_b and tail_t else "tail_exit" if tail_b else "tail_enter" if tail_t else "normal_stay"
    role = task_role_group(row, "toe")
    capacity = row.get("capacity_effect_vs_initial", "unknown")
    if capacity not in ("increased", "decreased", "mixed", "unchanged"):
        capacity = "unknown"
    epoch, end_epoch = [focus_epoch(row, k) for k in ("topology_epoch_start_toe", "topology_epoch_finish_toe")]
    for kind in ("start", "finish"):
        e, v = number(row.get(kind + "_us_ecmp")), number(row.get(kind + "_us_toe"))
        if e is not None and v is not None:
            row["delta_" + kind + "_us"] = str(v - e)
    row.update(delta_fct_us=str(d), improvement_pct=str(-d / b * 100) if b else "",
               _focus_role=role, _focus_capacity=capacity, _focus_tail=tail, _focus_epoch=epoch,
               _focus_tail_ecmp=tail_b, _focus_tail_toe=tail_t, _focus_record=record,
               _focus_cross_epoch=None if epoch is None or end_epoch is None else int(epoch != end_epoch))
    index = len(case.focus_b)
    case.focus_b.append(b); case.focus_t.append(t)
    pause, direct = number(row.get("pause_overlap_us")), number(row.get("direct_pause_us"))
    case.focus_d.append(d)
    case.focus_flags.append(int(pause is not None and pause > 0) | (int(direct is not None and direct > 0) << 1))
    size = number(row.get("data_size_bytes"))
    start, finish = number(row.get("delta_start_us")), number(row.get("delta_finish_us"))
    selflags = int(pause is not None) | (int(direct is not None) << 1) | (int(start is not None) << 2) | (int(finish is not None) << 3)
    selflags |= int(row["_focus_cross_epoch"] is not None) << 4
    selflags |= (row["_focus_cross_epoch"] or 0) << 5
    selflags |= int(size is not None and size >= 0) << 6
    selflags |= ("tail_enter", "tail_exit", "tail_stay", "normal_stay", "unknown").index(tail) << 7
    case.focus_meta_flags.append(selflags)
    case.focus_size.append(size if size is not None and size >= 0 else 0)
    case.focus_start.append(start or 0); case.focus_finish.append(finish or 0)
    keys = [("all", "all"), ("tail_transition", tail), ("role_group", role),
            ("op_type", row.get("op_type") or "UNKNOWN"), ("size_group", row.get("size_group") or "UNKNOWN"),
            ("capacity_effect", capacity), ("epoch_role", "epoch=%s;group=%s" % (epoch if epoch is not None else "UNKNOWN", role))]
    if pause is not None:
        keys.append(("pause_exposure", "pause_time_overlap" if pause > 0 else "no_pause_time_overlap"))
    if direct is not None and direct > 0:
        keys.append(("pause_exposure", "directly_paused_observed"))
    for key in keys:
        if key not in case.focus_stats and len(case.focus_stats) >= args.focus_max_groups:
            case.skipped_groups["focus_" + key[0] + "_merged_to_OVERFLOW"] += 1
            key = key[0], "OVERFLOW"
        stats = case.focus_stats.get(key)
        if stats is None:
            stats = case.focus_stats[key] = FocusStats()
        stats.indices.append(index)
    scores = {}
    if tail == "tail_enter": scores["TAIL_ENTER"] = d
    if tail == "tail_exit": scores["TAIL_EXIT"] = -d
    if tail_b and d > epsilon: scores["BASELINE_TAIL_REGRESSED"] = d
    if tail_b and d < -epsilon: scores["BASELINE_TAIL_IMPROVED"] = -d
    if d > epsilon:
        scores["FCT_WORST"] = d
        if pause == 0: scores["NO_PAUSE_REGRESSION"] = d
        if capacity == "increased": scores["EXPANDED_REGRESSION"] = d
        if capacity == "decreased": scores["REDUCED_REGRESSION"] = d
        if role in ROLE_GROUPS: scores["ROLE_%s_WORST" % role] = d
    if d < -epsilon:
        scores["FCT_BEST"] = -d
        if role in ROLE_GROUPS: scores["ROLE_%s_BEST" % role] = -d
    if finish is not None and finish > epsilon: scores["FINISH_DELAY"] = finish
    for tag, score in scores.items():
        case.focus_heaps[tag].add(score, row)


def finalize_focus(case, epsilon):
    if case.focus_stats:
        for group in ROLE_GROUPS:
            case.focus_stats.setdefault(("role_group", group), FocusStats())
    reused = defaultdict(list)
    for stats in case.focus_stats.values():
        key = (len(stats.indices), stats.indices[0] if stats.indices else -1, stats.indices[-1] if stats.indices else -1)
        same = next((other for other in reused[key] if other.indices == stats.indices), None)
        if same is not None:
            stats.qs = dict(same.qs)
            stats.agg = same.agg
            stats.counts = same.counts
            continue
        stats.agg.n = len(stats.indices)
        for side, values in (("ecmp", case.focus_b), ("toe", case.focus_t)):
            ordered = sorted(values[i] for i in stats.indices)
            setattr(stats.agg, "base" if side == "ecmp" else "toe", math.fsum(ordered))
            stats.qs.update({"p%d_fct_us_%s" % (q, side): percentile(ordered, q / 100) for q in (50, 95, 99)})
            stats.qs["max_fct_us_" + side] = ordered[-1] if ordered else None
        del ordered
        agg = stats.agg
        agg.delta = math.fsum(case.focus_d[i] for i in stats.indices)
        agg.positive_delta = math.fsum(case.focus_d[i] for i in stats.indices if case.focus_d[i] > 0)
        agg.negative_delta = math.fsum(case.focus_d[i] for i in stats.indices if case.focus_d[i] < 0)
        stats.counts["logical_payload_bytes"] = math.fsum(case.focus_size[i] for i in stats.indices)
        stats.counts["sum_start_delta_us"] = math.fsum(case.focus_start[i] for i in stats.indices)
        stats.counts["sum_finish_delta_us"] = math.fsum(case.focus_finish[i] for i in stats.indices)
        known_counts = [0] * 7
        tail_counts = [0] * 5
        for i in stats.indices:
            delta, flags = case.focus_d[i], case.focus_flags[i]
            agg.regressed += int(delta > epsilon)
            agg.improved += int(delta < -epsilon)
            agg.unchanged += int(abs(delta) <= epsilon)
            agg.overlap_n += flags & 1
            agg.direct_n += (flags >> 1) & 1
            agg.max_delta = max(agg.max_delta, delta)
            metadata = case.focus_meta_flags[i]
            for bit in range(7):
                known_counts[bit] += (metadata >> bit) & 1
            tail_counts[metadata >> 7] += 1
        stats.counts.update(dict(zip(("pause_known_tasks", "direct_pause_known_tasks", "start_delta_known_tasks", "finish_delta_known_tasks",
                                "cross_epoch_known_tasks", "cross_epoch_tasks", "logical_bytes_known_tasks"), known_counts)))
        stats.counts.update(dict(zip(("tail_enter_tasks", "tail_exit_tasks", "tail_stay_tasks", "normal_stay_tasks", "unknown_tasks"), tail_counts)))
        reused[key].append(stats)
    overall = case.focus_stats.get(("all", "all"))
    if overall and case.focus_threshold is not None:
        actual = overall.qs["p99_fct_us_ecmp"]
        if abs(actual - case.focus_threshold) > max(epsilon, abs(actual) * 1e-9):
            raise ValueError("ECMP P99 metadata differs from task CSV for %s; rerun with --recompute-focus-p99" % case.name)
    for values in (case.focus_b, case.focus_t, case.focus_d, case.focus_flags, case.focus_size, case.focus_start, case.focus_finish, case.focus_meta_flags):
        del values[:]
    for stats in case.focus_stats.values():
        del stats.indices[:]


def choose_focus_samples(sources, args):
    buckets, candidates = [], {}
    for tag in FOCUS_TAGS:
        for source in sources:
            for case in source.cases.values():
                bucket = []
                for row in case.focus_heaps[tag].values():
                    tid = row.get("task_id") or "record:%s" % row["_focus_record"]
                    key = source.label, case.name, tid
                    item = candidates.setdefault(key, [source, case, row, []])
                    item[3].append(tag); bucket.append(key)
                buckets.append(bucket)
    picked, seen = [], set()
    for key in interleave(buckets):
        if key not in seen:
            seen.add(key); picked.append(candidates[key])
            if len(picked) >= args.focus_max_samples:
                break
    return picked


def focus_record(source, case, dimension, value, stats):
    a, c, n = stats.agg, stats.counts, stats.agg.n
    row = dict(source_label=source.label, source_path=str(source.path), case_name=getattr(case, "name", ""),
        dimension=dimension, group_value=value, paired_tasks=n, baseline_p99_threshold_us=getattr(case, "focus_threshold", None),
        threshold_source=getattr(case, "focus_threshold_source", "unknown"), mean_fct_us_ecmp=a.base/n if n else None,
        mean_fct_us_toe=a.toe/n if n else None, mean_delta_fct_us=a.delta/n if n else None,
        improvement_pct=-a.delta/a.base*100 if a.base else None, regressed_tasks=a.regressed,
        improved_tasks=a.improved, unchanged_tasks=a.unchanged, sum_fct_increase_us=a.positive_delta,
        sum_fct_reduction_us=-a.negative_delta, net_delta_fct_us=a.delta, pause_overlap_tasks=a.overlap_n,
        directly_paused_observed_tasks=a.direct_n, grouping_basis="ToE_start_epoch;observed_ToE_endpoint_roles" if dimension == "epoch_role" else "observed_ToE_endpoint_roles" if dimension == "role_group" else dimension)
    for key in ("tail_enter_tasks", "tail_exit_tasks", "tail_stay_tasks", "normal_stay_tasks", "unknown_tasks",
                "logical_bytes_known_tasks", "logical_payload_bytes", "pause_known_tasks", "direct_pause_known_tasks",
                "start_delta_known_tasks", "sum_start_delta_us", "finish_delta_known_tasks", "sum_finish_delta_us",
                "cross_epoch_tasks", "cross_epoch_known_tasks"):
        row[key] = c[key]
    for side in ("ecmp", "toe"):
        for q in (95, 99):
            key = "p%d_fct_us_%s" % (q, side)
            row[key] = stats.qs.get(key)
    return row


def write_focus_tables(sources, selected, directory):
    directory.mkdir(parents=True, exist_ok=True)
    path_fields = [name + "_" + side for side in ("ecmp", "toe") for name in (
        "npu_port_id", "npu_port_ids", "asu_port_id", "asu_port_ids", "leaf1_node_id", "leaf1_node_ids",
        "leaf2_node_id", "leaf2_node_ids", "leaf1_host_port_id", "leaf1_host_port_ids", "leaf2_host_port_id",
        "leaf2_host_port_ids", "leaf1_network_port_id", "leaf1_network_port_ids", "leaf2_network_port_id", "leaf2_network_port_ids")]
    sample_fields = ["source_label", "source_path", "case_name", "task_id", "source_record_number", "focus_tags",
        "op_type", "size_group", "data_size_bytes", "npu_node_id", "asu_node_id", "role_group_ecmp", "role_group_toe",
        "baseline_p99_threshold_us", "baseline_tail", "toe_tail_same_threshold", "tail_transition",
        "fct_us_ecmp", "fct_us_toe", "delta_fct_us", "improvement_pct", "start_us_ecmp", "finish_us_ecmp",
        "start_us_toe", "finish_us_toe", "delta_start_us", "delta_finish_us", "topology_epoch_start_toe",
        "topology_epoch_finish_toe", "cross_epoch", "pause_overlap_us", "direct_pause_us", "capacity_effect_vs_initial",
        "path_data_available_ecmp", "path_data_available_toe"] + path_fields
    def summaries():
        for source in sources:
            for case in source.cases.values():
                for (dimension, value), stats in sorted(case.focus_stats.items()):
                    yield focus_record(source, case, dimension, value, stats)
    def samples():
        for source, case, row, tags in selected:
            out = {k: row.get(k) for k in sample_fields}
            out.update(source_label=source.label, source_path=str(source.path), case_name=case.name,
                source_record_number=row["_focus_record"], focus_tags=";".join(tags),
                role_group_ecmp=task_role_group(row, "ecmp"), role_group_toe=row["_focus_role"],
                baseline_p99_threshold_us=case.focus_threshold, baseline_tail=row["_focus_tail_ecmp"],
                toe_tail_same_threshold=row["_focus_tail_toe"], tail_transition=row["_focus_tail"],
                cross_epoch=row["_focus_cross_epoch"], capacity_effect_vs_initial=row["_focus_capacity"])
            yield out
    paths = []
    for name, records in (("task_focus_summary.csv", summaries()), ("task_focus_samples.csv", samples())):
        path = directory/name
        temp = path.with_name(path.name + ".tmp.%d" % os.getpid())
        try:
            first = next(records, None)
            header = list(first) if first is not None else sample_fields if "samples" in name else list(focus_record(Source(Path("."), ""), None, "", "", FocusStats()))
            with temp.open("w", newline="", encoding="utf-8-sig") as f:
                w = csv.DictWriter(f, fieldnames=header); w.writeheader()
                if first is not None: w.writerow(first)
                w.writerows(records)
            os.replace(temp, path)
        finally:
            if temp.exists(): temp.unlink()
        paths.append(path)
    return paths


def scan_small_files(source, args):
    meta = source.path / "analysis_summary.json"
    if meta.exists():
        source.meta = json.loads(meta.read_text(encoding="utf-8-sig"))
        if not isinstance(source.meta, dict):
            raise ValueError("analysis_summary.json must be an object")
    else:
        source.warnings.append("analysis_summary.json missing: version/coverage/parameters not verified")
    for item in source.meta.get("cases", []):
        if item.get("name") in source.cases:
            case = source.cases[item["name"]]
            case.cap, case.warnings = item.get("capabilities", {}), list(item.get("warnings", []))
    epsilon = args.epsilon_us if args.epsilon_us is not None else number(source.meta.get("parameters", {}).get("epsilon_us"))
    source.epsilon = .001 if epsilon is None else epsilon
    if source.epsilon < 0 or not math.isfinite(source.epsilon):
        raise ValueError("invalid FCT tolerance")
    for row in rows(source, "run_comparison_summary.csv"):
        case = source.case(row.get("case_name"), args)
        if case.run:
            raise ValueError("duplicate case in run_comparison_summary.csv: %s" % case.name)
        case.run = row
    for row in rows(source, "fct_group_summary.csv"):
        source.case(row.get("case_name"), args).groups[row.get("group", "?")] = row
    for row in rows(source, "reconfiguration_events.csv"):
        case = source.case(row.get("case_name"), args)
        status = row.get("event_status", "?")
        case.event_statuses[status] += 1
        case.topology_expected[str(required_int(row, "epoch"))] = row
        epoch = number(row.get("epoch")) or 0
        case.event_heaps["pause"].add(number(row.get("pause_seconds")) or 0, row)
        case.event_heaps["earliest"].add(-epoch, row)
        case.event_heaps["latest"].add(epoch, row)
        case.event_heaps["problem"].add(1000000 if status not in ("applied", "no_change") else
                                      (number(row.get("degree_delta_max")) or 0), row)
    for case in source.cases.values():
        case.choose_events(args.event_top)
    for row in rows(source, "event_task_cohorts.csv"):
        case = source.case(row.get("case_name"), args)
        if row.get("epoch") in case.selected_events:
            case.cohorts[(row.get("epoch"), row.get("cohort"))] = row
    for row in rows(source, "topology_changes.csv"):
        case = source.case(row.get("case_name"), args)
        if not add_topology_group(case, row, args):
            continue
        pair = tuple(str(x) for x in sorted((required_int(row, "leaf_a"), required_int(row, "leaf_b"))))
        if pair not in case.topology_pairs and len(case.topology_pairs) >= args.max_groups:
            case.skipped_groups["topology_rows"] += 1
            continue
        agg = case.topology_pairs.setdefault(pair, dict(n=0, moved_abs=0, reversed=0, rapid=0, last=None, latest_delta=None))
        agg["n"] += 1
        agg["moved_abs"] += abs(number(row.get("delta_circuits")) or 0)
        agg["reversed"] += number(row.get("reversal_circuits")) or 0
        agg["rapid"] += int(flag(row.get("rapid_reversal")))
        agg["last"], agg["latest_delta"] = row.get("epoch"), row.get("delta_circuits")
    finish_topology_groups(source)
    load_group_totals(source, args)
    for row in rows(source, "hotspot_effect_summary.csv"):
        case = source.case(row.get("case_name"), args)
        conclusion = row.get("conclusion", "?")
        case.hot_conclusions[conclusion] += 1
        gain = number(row.get("post_mean_fct_improvement_pct"))
        n = number(row.get("post_paired_tasks")) or 0
        b, t = number(row.get("post_mean_fct_us_ecmp")), number(row.get("post_mean_fct_us_toe"))
        impact = (t - b) * n if b is not None and t is not None else 0
        if gain is not None and gain < 0:
            case.hot_heaps["regression"].add(max(0, impact), row)
        if gain is not None and gain > 0:
            case.hot_heaps["benefit"].add(max(0, -impact), row)
        missing = int(number(row.get("pre_queue_bytes_p95")) is None) + int(number(row.get("post_queue_bytes_p95")) is None) + int(gain is None)
        if missing:
            case.hot_heaps["missing_evidence"].add(missing * 1000000 + n, row)
    log = source.path / "analysis.log"
    if log.exists():
        with log.open(encoding="utf-8", errors="replace") as f:
            for line in f:
                if "WARNING" in line or "ERROR" in line or "analysis_failed" in line:
                    source.log_warning_count += 1
                    source.log_lines.append(line.strip())


def add_group(case, bucket, key, args, values):
    if key not in bucket and len(bucket) >= args.max_groups:
        case.skipped_groups["host_task_rows" if bucket is case.host_groups else "leaf_task_rows"] += 1
        return
    bucket.setdefault(key, Aggregate()).add(*values)


def consume_task(source, args, row, count):
    case = source.case(row.get("case_name"), args)
    case.task_rows += 1
    status = row.get("comparison_status", "unknown")
    case.statuses[status] += 1
    if status != "paired_completed":
        return
    b, t, d = [number(row.get(k)) for k in ("fct_us_ecmp", "fct_us_toe", "delta_fct_us")]
    if b is None or t is None or b < 0 or t < 0:
        case.invalid_tasks += 1
        return
    if d is None:
        d = t - b
    if abs((t - b) - d) > max(source.epsilon, 1e-9 * max(abs(t), abs(b), 1)):
        case.inconsistent_delta += 1
        d = t - b
        row = dict(row, delta_fct_us=str(d), improvement_pct=str(-d / b * 100) if b > 0 else "")
    pause, direct = number(row.get("pause_overlap_us")), number(row.get("direct_pause_us"))
    values = b, t, d, source.epsilon, pause, direct
    case.totals.add(*values)
    add_focus_task(case, row, args, source.epsilon, count)
    case.metadata_partial += int(row.get("metadata_verification") != "full_traffic_row")
    for side in ("ecmp", "toe"):
        case.path_observed[side] += int(flag(row.get("path_data_available_" + side)))
    npu, asu = row.get("npu_node_id"), row.get("asu_node_id")
    if number(npu) is not None and number(asu) is not None:
        add_group(case, case.host_groups, (npu, asu), args, values)
    l1, l2 = row.get("leaf1_node_id_toe"), row.get("leaf2_node_id_toe")
    if number(l1) is not None and number(l2) is not None:
        add_group(case, case.leaf_groups, (l1, l2), args, values)
    else:
        case.skipped_groups["leaf_missing_or_multiple"] += 1
    if d > source.epsilon:
        case.task_heaps["FCT_WORST"].add(d, row)
        if pause is not None and pause > 0:
            case.task_heaps["PAUSE_OVERLAP_WORST"].add(d, row)
        if direct is not None and direct > 0:
            case.task_heaps["DIRECT_PAUSE_WORST"].add(d, row)
    if d < -source.epsilon:
        case.task_heaps["FCT_BEST"].add(-d, row)
    late = number(row.get("delta_finish_us"))
    if late is not None and late > source.epsilon:
        case.task_heaps["FINISH_DELAY"].add(late, row)
    cost = number(row.get("reconfiguration_execution_cost_us"))
    gain = number(row.get("ideal_topology_gain_us"))
    if flag(row.get("ideal_plan_verified")) and cost is not None and gain is not None:
        case.cost_n += 1; case.cost_sum += cost; case.gain_sum += gain
    if count % 500000 == 0:
        print("[%s] task rows accumulated=%d" % (source.label, count), file=sys.stderr)


def finish_task_stream(source, args):
    for case in source.cases.values():
        finalize_focus(case, source.epsilon)
        declared = number(case.run.get("paired_tasks"))
        if source.counts.get("task_compare.csv") is not None and declared is not None and declared != case.totals.n:
            case.warnings.append("paired task count differs: run_summary=%s vs valid task rows=%d (possibly partial CSV)" % (fmt(declared), case.totals.n))
        for tag, bucket in case.task_heaps.items():
            for row in bucket.values():
                tid = row.get("task_id", "?")
                sample = case.samples.setdefault(tid, {"row": row, "tags": [], "links": []})
                sample["tags"].append(tag)



def scan_tasks(source, args):
    prepare_focus_thresholds(source, args)
    for count, row in enumerate(rows(source, "task_compare.csv"), 1):
        consume_task(source, args, row, count)
    finish_task_stream(source, args)


def scan_link_samples(source, args):
    if not args.include_link_samples:
        return
    for row in rows(source, "task_link_usage.csv"):
        observed_case = row.get("case_name")
        baseline = source.meta.get("baseline") or next((c.run.get("baseline_name") for c in source.cases.values() if c.run.get("baseline_name")), "ECMP")
        targets = source.cases.values() if observed_case == baseline else [source.cases.get(observed_case)]
        for case in targets:
            if case is None:
                continue
            sample = case.samples.get(row.get("task_id"))
            if sample is not None:
                counts = sample.setdefault("link_counts", Counter())
                if counts[observed_case] < args.links_per_task:
                    sample["links"].append(row)
                    counts[observed_case] += 1


class Report:
    def __init__(self, args):
        self.args, self.sections = args, []

    def section(self, name, core=False):
        s = {"name": name, "blocks": [], "core": core}
        self.sections.append(s)
        return s

    def add(self, section, *lines):
        section["blocks"].append([clean(x, self.args.line_width) for x in lines])

    def render(self):
        # Reserve the section headings, blank lines, and four footer lines first.
        fixed_lines = 2 * len(self.sections) + 4
        fixed_chars = sum(len(s["name"]) + 7 for s in self.sections) + 1600
        line_budget = self.args.max_lines - fixed_lines
        char_budget = self.args.max_chars - fixed_chars
        selected = {s["name"]: [] for s in self.sections}
        picked = Counter()
        def take(s, index):
            nonlocal line_budget, char_budget
            block = s["blocks"][index]
            size = sum(len(x) + 1 for x in block)
            if len(block) > line_budget or size > char_budget:
                return False
            selected[s["name"]].append((index, block))
            line_budget -= len(block); char_budget -= size; picked[s["name"]] += 1
            return True
        for s in self.sections:
            if s["core"]:
                for i in range(len(s["blocks"])):
                    take(s, i)
        optional = [s for s in self.sections if not s["core"]]
        for i in range(max((len(s["blocks"]) for s in optional), default=0)):
            for s in optional:
                if i < len(s["blocks"]):
                    take(s, i)
        lines = []
        for s in self.sections:
            lines.append("## " + clean(s["name"], self.args.line_width))
            for i, block in sorted(selected[s["name"]]):
                lines.extend(block)
            lines.append("")
        omitted = {s["name"]: len(s["blocks"]) - picked[s["name"]] for s in self.sections
                   if len(s["blocks"]) > picked[s["name"]]}
        lines += ["SUMMARY_BUDGET omitted_blocks=" + clean(json.dumps(omitted, ensure_ascii=False), self.args.line_width),
                  "省略的是展示记录，扫描计数仍覆盖全部已读取行；超出分组上限的任务另行标记。",
                  "请结合完成率、输入一致性和数据缺失分析；暂停重叠、端口集合与热点相关性不能直接证明因果。"]
        lines.append("SUMMARY_LIMIT lines=%d/%d chars=%d/%d version=%s" % (
            len(lines) + 1, self.args.max_lines, len("\n".join(lines)), self.args.max_chars, SUMMARY_VERSION))
        for _ in range(4):
            actual_chars = len("\n".join(lines)) + 1
            updated = "SUMMARY_LIMIT lines=%d/%d chars=%d/%d version=%s" % (
                len(lines), self.args.max_lines, actual_chars, self.args.max_chars, SUMMARY_VERSION)
            if updated == lines[-1]:
                break
            lines[-1] = updated
        text = "\n".join(lines) + "\n"
        if len(text.splitlines()) > self.args.max_lines or len(text) > self.args.max_chars:
            raise ValueError("internal report budget failure")
        return text


def path_description(row, suffix):
    def v(name):
        one = row.get(name + "_" + suffix)
        many = row.get(name + "s_" + suffix)
        value = one if number(one) is not None else many
        return clean(value, 85) if value and str(value).lower() not in NULLS else "NA"
    return ("%s observations: available=%s NPUports=%s ASUports=%s "
            "L1(nodes/host/net)=%s/%s/%s L2(nodes/host/net)=%s/%s/%s") % (
        suffix.upper(), fmt(row.get("path_data_available_" + suffix)), v("npu_port_id"), v("asu_port_id"),
        v("leaf1_node_id"), v("leaf1_host_port_id"), v("leaf1_network_port_id"),
        v("leaf2_node_id"), v("leaf2_host_port_id"), v("leaf2_network_port_id"))


def interleave(groups):
    for i in range(max((len(g) for g in groups), default=0)):
        for g in groups:
            if i < len(g):
                yield g[i]


def make_report(sources, args, focus_samples=None):
    report = Report(args)
    intro = report.section("1. 口径与输入", True)
    report.add(intro, "ToE AI summary " + SUMMARY_VERSION + "; 本摘要最多1500行；基于任务累计统计与拓扑状态生成，不重新运行仿真。",
               "FCT/任务时间单位=us；重配时间单位=s；吞吐=Gbps；队列/待发送量=bytes；改善百分比正值表示更快。",
               "NA=未知/缺失，不能当作零。分位数来自原始汇总表；不会用Top任务样本重新估算P99。",
               "逐task比较仅使用 paired_completed；样本按绝对FCT差/完成推迟筛选，样本不代表整体分布。",
               "pause_overlap仅是时间暴露；direct_pause需实际记录；独立执行代价仅来自已验证的ideal回放对照。")
    states = []
    for source in sources:
        report.add(intro, "input=%s path=%s analyzer=%s baseline=%s epsilon_us=%s" % (
            source.label, source.path, source.meta.get("script_version", "NA"), source.meta.get("baseline", "NA"), fmt(source.epsilon)),
            "CSV rows=" + json.dumps(source.counts, ensure_ascii=False, separators=(",", ":")))
        if source.meta.get("pipeline"):
            mode = "integrated_row_accumulation" if hasattr(source, "topology_states") else "summary_only_existing_csv"
            report.add(intro, "report_mode=" + mode + "; original_analysis_pipeline=" + json.dumps(source.meta["pipeline"], ensure_ascii=False, separators=(",", ":")),
                       "stage_seconds=" + json.dumps(source.meta.get("stage_seconds", {}), ensure_ascii=False, separators=(",", ":")))
        states.extend((source, case) for case in source.cases.values())
    def impact(item):
        c = item[1]
        return max(abs(c.totals.delta), c.totals.positive_delta, -c.totals.negative_delta) if c.totals.n else abs(number(c.run.get("mean_fct_improvement_pct")) or 0)
    states.sort(key=impact, reverse=True)
    skipped_cases = max(0, len(states) - args.max_cases)
    states = states[:args.max_cases]
    report.add(intro, "cases_total=%d cases_selected=%d cases_omitted=%d; 跨输入目录的同名case不会合并。" %
               (sum(len(s.cases) for s in sources), len(states), skipped_cases))
    report.add(intro, "默认仅选Top样本；event_top=%d task_top/category=%d top/group=%d link_samples_scan=%s；全量统计另列。" %
               (args.event_top, args.task_top, args.top, "enabled" if args.include_link_samples else "off"))
    overall = report.section("2. 总体效果与独立重配代价", True)
    coverage = report.section("3. 数据覆盖、异常与可比性", True)
    grouping = report.section("4. FCT分组")
    events = report.section("5. 重配事件")
    cohorts = report.section("6. 事件前后任务阶段")
    role_changes = report.section("7. Leaf角色分组的链路变化")
    topology = report.section("8. 拓扑反复变化")
    hotspots = report.section("9. 热点缓解证据")
    groups = report.section("10. 端点与单一Leaf对的累计影响")
    tasks = report.section("11. 代表性任务与实际端口观察")
    focus = report.section("12. 重点任务统计与样本")
    logs = report.section("13. 输入日志告警")
    focus_samples = choose_focus_samples(sources, args) if focus_samples is None else focus_samples
    report.add(intro, "重点任务：两次实验均使用ECMP P99阈值；退出尾部/新进入尾部按同一阈值分类；分组分位数由全部有效配对任务精确计算。",
               "task_focus_summary.csv=完整分组统计；task_focus_samples.csv=每类前%d、全局去重最多%d个任务；逻辑数据量不是端口实际吞吐。" % (args.focus_top, args.focus_max_samples))
    report.add(intro, "Leaf角色：P=37–48，D=49–52，S=53–56；P↔P/P↔D/P↔S/D↔S均按无向连接统计，镜像重复行只计一次。",
               "分组统计的是变更记录：add=增加，remove=减少，net=增加-减少，abs=增加+减少；不代表全组总容量或实际流量。",
               "分组CSV保存全部输入/case/epoch，不受摘要预算或Top样本限制；D↔D、S↔S及UNKNOWN有记录时另列，避免遗漏。")
    per_section = {s["name"]: [] for s in (grouping, events, cohorts, role_changes, topology, hotspots, groups, tasks, focus)}
    for source, case in states:
        tag = clean(source.label + "/" + case.name, 180)
        r, all_group = case.run, case.groups.get("all", {})
        report.add(overall, tag + " " + fields(r, [("planned", "planned_tasks"), ("completed", "completed_tasks"),
            ("completion", "completion_rate"), ("paired", "paired_tasks"), ("meanE", "mean_fct_us_ecmp"),
            ("meanT", "mean_fct_us_toe"), ("gain%", "mean_fct_improvement_pct")]),
            tag + " " + fields(all_group or r, [("p50E", "p50_fct_us_ecmp"), ("p50T", "p50_fct_us_toe"),
            ("p95E", "p95_fct_us_ecmp"), ("p95T", "p95_fct_us_toe"), ("p99E", "p99_fct_us_ecmp"), ("p99T", "p99_fct_us_toe")]))
        report.add(overall, tag + " " + fields(r, [("decisions", "decision_count"), ("appliedChanges", "actual_reconfiguration_count"),
            ("pendingDrain", "pending_draining_count"), ("observedDecisionGap(s)", "decision_interval_mean_seconds"),
            ("pauseUnion(s)", "pause_timeline_union_seconds"), ("removed", "removed_circuits_total"),
            ("reverse", "reversal_circuits_total"), ("rapidReversePairs", "rapid_pair_reversal_count")]))
        if case.task_rows:
            task_label = " streamed task aggregation: " if hasattr(source, "topology_states") else " full task scan: "
            report.add(overall, tag + task_label + case.totals.describe() +
                       " status_counts=" + json.dumps(case.statuses, separators=(",", ":")))
        if case.cost_n:
            report.add(overall, tag + " verified ideal: N=%d meanTopologyGain(us)=%s meanExecutionCost(us)=%s" % (
                case.cost_n, fmt(case.gain_sum / case.cost_n), fmt(case.cost_sum / case.cost_n)))
        else:
            report.add(overall, tag + " 独立重配执行代价=NA（无可用且验证通过的逐任务ideal对照）；不从暂停重叠推算。")
        report.add(coverage, tag + " " + fields(r, [("sameTrafficSHA", "traffic_sha256_equal"), ("sameInitialTopologySHA", "initial_topology_sha256_equal"),
            ("pathTasksE", "path_observed_task_count_ecmp"), ("pathTasksT", "path_observed_task_count_toe"),
            ("pauseTimelineComplete", "pause_timeline_complete"), ("directPauseAvailable", "direct_task_pause_available"), ("idealVerified", "ideal_plan_verified")]),
            tag + " task_rows=%d invalid_paired=%d delta_inconsistent_recomputed=%d partial_metadata=%d skipped_groups=%s" % (
                case.task_rows, case.invalid_tasks, case.inconsistent_delta, case.metadata_partial, json.dumps(case.skipped_groups, separators=(",", ":"))))
        report.add(coverage, tag + " events_csv_rows=%d event_samples_selected=%d task_samples_selected=%d" %
                   (sum(case.event_statuses.values()), len(case.selected_events), len(case.samples)))
        for msg in case.warnings[:args.warning_top]:
            report.add(coverage, tag + " WARNING " + clean(msg, 600))
        if len(case.warnings) > args.warning_top:
            report.add(coverage, tag + " additional_warnings=%d" % (len(case.warnings) - args.warning_top))
        for key in ("unknown_peer_events", "metadata_mismatched_tasks", "l1_pair_events", "l1_pair_tagged_events", "port_events", "port_tagged_events"):
            if key in case.cap:
                report.add(coverage, tag + " " + key + "=" + fmt(case.cap[key]))
        local = defaultdict(list)
        local[focus["name"]].append([tag + " focus_threshold_us=" + fmt(case.focus_threshold) + " source=" + case.focus_threshold_source])
        important = [(key, value) for key, value in case.focus_stats.items() if key[0] in ("all", "tail_transition", "role_group")]
        other = sorted(((key, value) for key, value in case.focus_stats.items() if key[0] not in ("all", "tail_transition", "role_group")),
                       key=lambda x: abs(x[1].agg.delta), reverse=True)[:args.top]
        for (dimension, value), stats in important + other:
            local[focus["name"]].append([tag + " focus " + clean(dimension + "=" + value, 130) + " " + stats.agg.describe() +
                " p99E=" + fmt(stats.qs.get("p99_fct_us_ecmp")) + " p99T=" + fmt(stats.qs.get("p99_fct_us_toe"))])
        chosen = [item for item in focus_samples if item[0] is source and item[1] is case]
        local[focus["name"]].append([tag + " selected_focus_tasks=%d; CSV含全部所选任务，文本仅展示最多%d个。" % (len(chosen), args.task_top)])
        for _, _, row, tags in chosen[:args.task_top]:
            local[focus["name"]].append([tag + " focus_task=" + clean(row.get("task_id", "?"), 50) + " tags=" + clean(";".join(tags), 350) +
                " tail=" + row["_focus_tail"] + " role=" + row["_focus_role"] + " " + fields(row, [("FCTE", "fct_us_ecmp"),
                ("FCTT", "fct_us_toe"), ("delta", "delta_fct_us"), ("finishDelta", "delta_finish_us"), ("capacity", "_focus_capacity")])])
        role_events = sorted(case.topology_group_events.items(), key=lambda x: int(x[0]))
        report.add(coverage, tag + " topology_group_checks=" + json.dumps(case.topology_checks, separators=(",", ":")) +
                   " grouped_epochs=%d" % len(role_events))
        if role_events:
            incomplete = any(e["coverage"] in ("missing_topology_rows", "partial_or_mismatched") for _, e in role_events)
            no_rows = all(e["coverage"] == "missing_topology_rows" for _, e in role_events)
            totals_coverage = "partial_or_mismatched" if incomplete else "matched_event_counts" if all(e["coverage"] == "matched_event_counts" for _, e in role_events) else "not_verified"
            for group in topology_groups(case.topology_group_totals):
                values = case.topology_group_totals.get(group, {})
                local[role_changes["name"]].append([tag + " role_total coverage=" + totals_coverage + " " +
                    group_description(group, values, no_rows) + " epochsChanged=%d" %
                    sum(e["groups"].get(group, {}).get("pair_change_events", 0) > 0 for _, e in role_events)])
            picked = sorted(role_events, key=lambda x: sum(g["abs_delta_circuits"] for g in x[1]["groups"].values()), reverse=True)[:args.event_top]
            local[role_changes["name"]].append([tag + " role_epochs total=%d displayed=%d; CSV含全部epoch。" % (len(role_events), len(picked))])
            for epoch, event in sorted(picked, key=lambda x: int(x[0])):
                block = [tag + " role_epoch=" + epoch + " apply(s)=" + fmt(event["time"]) + " coverage=" + event["coverage"]]
                for group in topology_groups(event["groups"]):
                    block.append(tag + " epoch=" + epoch + " " + group_description(group, event["groups"].get(group, {}),
                        event["coverage"] == "missing_topology_rows") + " beforeLinks=" +
                        fmt(event["before_totals"].get(group, 0) if event.get("before_totals") is not None else None) + " afterLinks=" +
                        fmt(event["after_totals"].get(group, 0) if event.get("after_totals") is not None else None))
                local[role_changes["name"]].append(block)
        else:
            local[role_changes["name"]].append([tag + " 分组变更=NA（无可用拓扑变更记录）；不能解释为各组均无变化。"])
        selected_groups = sorted(case.groups.items(), key=lambda x: (0 if x[0] in ("all", "small", "large") else 1, x[0]))
        for name, row in selected_groups[:max(args.top, 16)]:
            local[grouping["name"]].append([tag + " group=" + clean(name, 100) + " " + fields(row, [
                ("N", "paired_tasks"), ("meanE", "mean_fct_us_ecmp"), ("meanT", "mean_fct_us_toe"), ("gain%", "mean_fct_improvement_pct"),
                ("p95E", "p95_fct_us_ecmp"), ("p95T", "p95_fct_us_toe"), ("p99E", "p99_fct_us_ecmp"), ("p99T", "p99_fct_us_toe"),
                ("regress", "regressed_tasks"), ("improve", "improved_tasks"), ("equal", "unchanged_tasks")])])
        event_rows = sorted(case.selected_events.values(), key=lambda x: number(x.get("epoch")) or 0)
        for row in event_rows:
            ep = row.get("epoch", "?")
            local[events["name"]].append([tag + " epoch=" + ep + " " + fields(row, [
                ("status", "event_status"), ("apply(s)", "applied_seconds"), ("pause(s)", "pause_seconds"), ("drain(s)", "drain_seconds"),
                ("scope", "pause_scope"), ("TPs", "paused_tp_count"), ("removed", "removed_circuits"), ("added", "added_circuits"),
                ("preserved", "preserved_circuits"), ("reconnected", "reconnected_circuits"), ("unpausedBytes", "unpaused_tx_bytes"),
                ("returnTo", "returned_to_epoch"), ("life(s)", "target_lifetime_seconds"), ("minMLU", "min_mlu"), ("bound", "applied_mlu_bound"),
                ("degreeDelta", "degree_delta_max")])])
            for phase in ("pre", "pause", "recovery", "post"):
                cohort = case.cohorts.get((ep, phase))
                if cohort:
                    local[cohorts["name"]].append([tag + " epoch=" + ep + " cohort=" + phase + " " + fields(cohort, [
                        ("N", "paired_tasks"), ("meanE", "mean_fct_us_ecmp"), ("meanT", "mean_fct_us_toe"), ("gain%", "mean_fct_improvement_pct"),
                        ("overlapN", "pause_overlap_task_count"), ("directPauseN", "directly_paused_observed_task_count")])])
        local[topology["name"]].append([tag + " event_status_counts=" + json.dumps(case.event_statuses, separators=(",", ":"))])
        for pair, agg in sorted(case.topology_pairs.items(), key=lambda x: (x[1]["reversed"], x[1]["moved_abs"]), reverse=True)[:args.top]:
            local[topology["name"]].append([tag + " pair=%s-%s group=%s changes=%d absDelta=%s reversal=%s rapid=%d lastEpoch=%s latestDelta=%s" % (
                pair[0], pair[1], role_group(int(pair[0]), int(pair[1])), agg["n"], fmt(agg["moved_abs"]), fmt(agg["reversed"]), agg["rapid"], agg["last"], agg["latest_delta"])])
        local[hotspots["name"]].append([tag + " conclusion_counts=" + json.dumps(case.hot_conclusions, separators=(",", ":"))])
        picked_hot = {}
        for kind, bucket in case.hot_heaps.items():
            for row in bucket.values():
                key = row.get("epoch"), row.get("src_leaf"), row.get("dst_leaf")
                if key not in picked_hot:
                    picked_hot[key] = row
        for row in list(picked_hot.values())[:args.top * 2]:
            local[hotspots["name"]].append([tag + " hot epoch=" + row.get("epoch", "?") + " pair=" + row.get("src_leaf", "?") + "→" + row.get("dst_leaf", "?") + " " + fields(row, [
                ("circuitsE", "circuits_before"), ("circuitsT", "circuits_after"), ("queueP95pre", "pre_queue_bytes_p95"),
                ("queueP95post", "post_queue_bytes_p95"), ("qSamplesPre", "pre_queue_sample_count"), ("qSamplesPost", "post_queue_sample_count"),
                ("physUtilPre", "pre_physical_utilization"), ("physUtilPost", "post_physical_utilization"),
                ("N", "post_paired_tasks"), ("gain%", "post_mean_fct_improvement_pct"), ("conclusion", "conclusion")]),
                tag + " hot traffic " + fields(row, [("logicalPre", "pre_logical_gbps"), ("logicalPost", "post_logical_gbps"),
                ("logicalEPre", "pre_logical_gbps_ecmp"), ("logicalEPost", "post_logical_gbps_ecmp"), ("backlogPre", "pre_backlog_bytes_mean"),
                ("backlogPost", "post_backlog_bytes_mean"), ("srcUtil", "pre_source_utilization_mean"), ("dstUtil", "pre_destination_utilization_mean")])])
        for kind, bucket in [("NPU-ASU", case.host_groups), ("single_observed_NPUleaf-ASUleaf", case.leaf_groups)]:
            worst = sorted(bucket.items(), key=lambda x: x[1].positive_delta, reverse=True)[:args.top]
            best = sorted(bucket.items(), key=lambda x: x[1].negative_delta)[:max(1, args.top // 2)]
            seen = set()
            for pair, agg in worst + best:
                if pair in seen:
                    continue
                seen.add(pair)
                local[groups["name"]].append([tag + " " + kind + "=%s→%s " % pair + agg.describe()])
        for sample in case.samples.values():
            row, tags = sample["row"], sample["tags"]
            tid = clean(row.get("task_id", "?"), 40)
            block = [tag + " task=" + tid + " tags=" + ",".join(tags) + " " + fields(row, [
                ("op", "op_type"), ("size", "data_size_bytes"), ("NPU", "npu_node_id"), ("ASU", "asu_node_id"),
                ("FCTE", "fct_us_ecmp"), ("FCTT", "fct_us_toe"), ("delta", "delta_fct_us"), ("gain%", "improvement_pct"),
                ("startDelta", "delta_start_us"), ("finishDelta", "delta_finish_us"), ("epoch0", "topology_epoch_start_toe"),
                ("epoch1", "topology_epoch_finish_toe"), ("overlap", "pause_overlap_us"), ("directPause", "direct_pause_us"), ("capacity", "capacity_effect_vs_initial")]),
                tag + " task=" + tid + " times " + fields(row, [("startE", "start_us_ecmp"), ("finishE", "finish_us_ecmp"),
                ("startT", "start_us_toe"), ("finishT", "finish_us_toe")]),
                tag + " task=" + tid + " " + path_description(row, "ecmp"),
                tag + " task=" + tid + " " + path_description(row, "toe")]
            for link in sample["links"]:
                block.append(tag + " task=" + tid + " actual_link " + fields(link, [("case", "case_name"), ("epoch", "epoch"),
                    ("a", "src_node"), ("ap", "src_port"), ("b", "dst_node"), ("bp", "dst_port"), ("bytes", "tx_bytes"), ("mapped", "peer_mapping_available")]))
            local[tasks["name"]].append(block)
        for name in per_section:
            per_section[name].append(local[name])
    for section in (grouping, events, cohorts, role_changes, topology, hotspots, groups, tasks, focus):
        for block in interleave(per_section[section["name"]]):
            report.add(section, *block)
    for source in sources:
        for msg in source.warnings:
            report.add(coverage, source.label + " WARNING " + clean(msg, 600))
        report.add(logs, source.label + " warning/error log_lines=%d last_shown=%d" % (source.log_warning_count, len(source.log_lines)))
        for msg in source.log_lines:
            report.add(logs, source.label + " " + clean(msg, 800))
    report.add(coverage, "单一Leaf分组只含单值且实际可观察的Leaf；多Leaf/缺记录任务不会被强行分配。分组有max_groups上限，溢出明确标记。",
               "节点/端口数组是观测集合，不是完整逐包路径，也不能把不同数组逐项配对。",
               "逐事件cohort可能重叠，不能把其FCT差值相加作为总重配代价。热点判定只是相关证据；需求自然下降和端侧瓶颈仍需核查。")
    if any(getattr(source, "demand_summary", []) for source in sources):
        demand_section = report.section("14. 求解器需求矩阵与重配")
        report.add(demand_section, "热力图索引=demand_heatmaps.html；原始数值=demand_matrices.csv；每轮概览=demand_epoch_summary.csv。",
                   "需求为决策时的求解器输入，不是拓扑生效时刻的瞬时流量；逐OD历史峰值可能来自不同窗口。矩阵保留方向，缺失单元为NA。")
        for source, case in states:
            selected = [row for row in getattr(source,"demand_summary",[]) if row["case_name"]==case.name]
            selected = [row for row in selected if args.demand_heatmaps=="all" or row["event_status"] in ("applied","applied_change_unknown")]
            report.add(demand_section, "%s/%s reconfiguration_matrices=%d missing=%d" % (source.label,case.name,len(selected),sum(row["coverage"]=="missing_solver_demand_log" for row in selected)))
            for row in selected[:args.event_top]:
                report.add(demand_section, "%s/%s demand " % (source.label,case.name)+fields(row,[("epoch","epoch"),("decision(s)","decision_seconds"),("applied(s)","applied_seconds"),("coverage","coverage"),("knownSum(Gbps)","total_known_demand_gbps"),("maxSrc","max_src_leaf"),("maxDst","max_dst_leaf"),("maxOD(Gbps)","max_pair_demand_gbps")]))
    return report.render()


def parser():
    p = argparse.ArgumentParser(description="Summarize ToE result CSVs into at most 1500 lines (%s)." % SUMMARY_VERSION)
    p.add_argument("analysis_dir", nargs="?", type=Path, help="existing analysis directory (use --summary-only)")
    p.add_argument("--input", action="append", type=Path, help="repeat for several independent result directories")
    p.add_argument("--output", type=Path, help="default FIRST_INPUT/toe_ai_summary_v3.txt")
    p.add_argument("--group-output-dir", type=Path, help="group CSV directory; default summary output directory")
    p.add_argument("--circuit-gbps", type=float, default=400, help="fallback capacity per circuit when input capacity columns are missing")
    p.add_argument("--focus-top", type=int, default=20, help="task candidates per focus category")
    p.add_argument("--focus-max-samples", type=int, default=200, help="global deduplicated focus task limit")
    p.add_argument("--focus-max-groups", type=int, default=5000, help="focus group limit; excess groups merge into an explicit OVERFLOW bucket")
    p.add_argument("--recompute-focus-p99", action="store_true", help="recompute ECMP P99 instead of using paired summary metadata")
    p.add_argument("--topology-snapshots", nargs=2, action="append", metavar=("CASE", "CSV"), help="full/delta toe_topology snapshot file; CASE may be SOURCE/CASE")
    p.add_argument("--initial-topology", nargs=2, action="append", metavar=("CASE", "CSV"), help="initial topology.csv or full leaf_a/leaf_b/circuits allocation")
    p.add_argument("--topology-log-format", choices=("full", "delta"), help="snapshot format; default analyzer metadata, otherwise full")
    p.add_argument("--max-lines", type=int, default=1500, help="hard cap, 80..1500, including headers and footer")
    p.add_argument("--max-chars", type=int, default=120000, help="additional character cap; minimum 4000")
    p.add_argument("--line-width", type=int, default=1000, help="maximum characters per physical line, minimum 300")
    p.add_argument("--top", type=int, default=10, help="bounded hotspot/topology/group samples per case")
    p.add_argument("--task-top", type=int, default=8, help="tasks per sampling category; duplicates combined")
    p.add_argument("--event-top", type=int, default=30, help="max event samples per case; problems/long pauses prioritized")
    p.add_argument("--max-cases", type=int, default=20)
    p.add_argument("--max-groups", type=int, default=5000, help="distinct endpoint/leaf/topology groups per case")
    p.add_argument("--warning-top", type=int, default=6)
    p.add_argument("--epsilon-us", type=float, help="override FCT equality tolerance; otherwise inherit analyzer metadata")
    p.add_argument("--include-link-samples", action="store_true", help="selected task links: database lookup in combined mode, CSV scan in summary-only mode")
    p.add_argument("--links-per-task", type=int, default=4)
    p.add_argument("--demand-file", nargs=2, action="append", metavar=("CASE", "CSV"), help="exact solver-demand log; CASE may be SOURCE/CASE")
    p.add_argument("--demand-heatmaps", choices=("changes", "all", "off"), default="changes", help="draw actual reconfigurations (default), all decisions, or no images")
    p.add_argument("--stdout", action="store_true", help="also print the exact bounded summary to stdout")
    return p


def validate_summary_args(args):
    if not 80 <= args.max_lines <= HARD_MAX_LINES:
        raise ValueError("--max-lines must be 80..1500")
    if args.max_chars < 4000 or args.line_width < 300:
        raise ValueError("--max-chars >=4000 and --line-width >=300 are required")
    if not math.isfinite(args.circuit_gbps) or args.circuit_gbps <= 0:
        raise ValueError("--circuit-gbps must be finite and positive")
    for name in ("top", "task_top", "event_top", "max_cases", "max_groups", "warning_top", "links_per_task", "focus_top", "focus_max_samples", "focus_max_groups"):
        if not 1 <= getattr(args, name) <= 10000:
            raise ValueError("invalid --" + name.replace("_", "-"))


def write_summary_outputs(sources, args, started):
    roots = [source.path for source in sources]
    targets = {c.name for s in sources for c in s.cases.values()} | {s.label + "/" + c.name for s in sources for c in s.cases.values()}
    if any(target not in targets for target, _ in (args.topology_snapshots or []) + (args.initial_topology or []) + (args.demand_file or [])):
        raise ValueError("topology/demand override CASE does not match an input case")
    output = (args.output or roots[0] / "toe_ai_summary_v3.txt").resolve()
    if output.suffix.lower() not in (".txt", ".md", ".log"):
        raise ValueError("summary output must have .txt/.md/.log extension")
    if output in {root / name for root in roots for name in FILES}:
        raise ValueError("output cannot overwrite an input file")
    selected_focus = choose_focus_samples(sources, args)
    demand_paths = write_demand_outputs(sources, args, (args.group_output_dir or output.parent).resolve())
    text = make_report(sources, args, selected_focus)
    group_paths = write_topology_groups(sources, (args.group_output_dir or output.parent).resolve())
    group_paths += write_focus_tables(sources, selected_focus, (args.group_output_dir or output.parent).resolve())
    group_paths += demand_paths
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.name + ".tmp.%d" % os.getpid())
    try:
        with temp.open("w", encoding="utf-8", newline="") as f:
            f.write(text)
        os.replace(temp, output)
    finally:
        if temp.exists():
            temp.unlink()
    if args.stdout:
        sys.stdout.write(text)
    for path in group_paths:
        print("[OK] group CSV: %s" % path, file=sys.stderr)
    print("[OK] %s | lines=%d/%d chars=%d/%d elapsed=%.2fs" % (
        output, len(text.splitlines()), args.max_lines, len(text), args.max_chars, time.monotonic() - started), file=sys.stderr)
    return 0


def summary_main(argv=None):
    args = parser().parse_args(argv)
    started = time.monotonic()
    try:
        validate_summary_args(args)
        roots = list(args.input or []) + ([args.analysis_dir] if args.analysis_dir else [])
        if not roots:
            raise ValueError("provide analysis_dir or --input DIR")
        roots = [p.resolve() for p in roots]
        if len(set(roots)) != len(roots):
            raise ValueError("duplicate input directory")
        if len(roots) > 1 and any("/" not in target for target, _ in (args.topology_snapshots or []) + (args.initial_topology or []) + (args.demand_file or [])):
            raise ValueError("multiple inputs require SOURCE/CASE for topology overrides")
        sources, labels = [], Counter(p.name for p in roots)
        for i, root in enumerate(roots):
            if not root.is_dir() or not any((root / f).is_file() for f in FILES if f.endswith(".csv")):
                raise ValueError("no ToE analysis CSVs found in %s" % root)
            label = root.name + ("#%d" % (i + 1) if labels[root.name] > 1 else "")
            source = Source(root, label)
            print("Reading %s" % root, file=sys.stderr)
            scan_small_files(source, args)
            scan_tasks(source, args)
            scan_link_samples(source, args)
            sources.append(source)
        return write_summary_outputs(sources, args, started)
    except (ValueError, OSError, csv.Error, json.JSONDecodeError) as exc:
        print("[ERROR] %s" % exc, file=sys.stderr)
        return 2

# Embedded analysis implementation.
"""Offline ns-3-UB ToE analysis. Python 3.9+, standard library only.

ECMP needs no ToE logs. Missing observations stay unknown. Task timings and
trace observations are streamed into SQLite, not retained in Python lists.
"""
import argparse
import bisect
import csv
import hashlib
import json
import logging
import math
import re
import sqlite3
import sys
import time
from collections import defaultdict
from contextlib import contextmanager
from dataclasses import dataclass, field
from decimal import Decimal, InvalidOperation, ROUND_HALF_EVEN
from pathlib import Path

ANALYZER_VERSION = "v3.0.0"
NULLS = {"", "na", "n/a", "nan", "none", "null", "-"}


def norm(s):
    return re.sub(r"[^a-z0-9]", "", str(s).lower())


class Row:
    def __init__(self, raw):
        self.raw = raw
        self.d = {norm(k): str(v).strip() for k, v in raw.items() if k is not None and v is not None}

    def get(self, *names, default=None):
        for name in names:
            value = self.d.get(norm(name))
            if value is not None and value.lower() not in NULLS:
                return value
        return default

    def integer(self, *names, default=None):
        value = self.get(*names)
        if value is None:
            return default
        decimal = Decimal(value)
        if not decimal.is_finite() or decimal != decimal.to_integral_value():
            raise ValueError("expected integer for %s: %r" % (names, value))
        return int(decimal)

    def number(self, *names, default=None):
        value = self.get(*names)
        if value is None:
            return default
        value = float(value)
        if not math.isfinite(value):
            raise ValueError("non-finite number for %s" % (names,))
        return value


def ns(value, unit="us"):
    if value is None or str(value).strip().lower() in NULLS:
        return None
    x = Decimal(str(value))
    if not x.is_finite() or x < 0:
        return None
    factor = {"s": 10**9, "ms": 10**6, "us": 1000, "ns": 1}[unit]
    return int((x * factor).to_integral_value(rounding=ROUND_HALF_EVEN))


def row_time(r):
    for unit, names in [("ns", ("time_ns",)), ("us", ("time_us", "time(us)", "timestamp_us")),
                        ("s", ("time_seconds", "time_s", "seconds"))]:
        v = r.get(*names)
        if v is not None:
            return ns(v, unit)
    return None


def micro(x):
    return None if x is None else x / 1000.0


def seconds(x):
    return None if x is None else x / 1e9


def csv_rows(path):
    with path.open(newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        if not reader.fieldnames:
            raise ValueError("missing CSV header: %s" % path)
        if len({norm(x) for x in reader.fieldnames}) != len(reader.fieldnames):
            raise ValueError("duplicate/ambiguous CSV columns: %s" % path)
        for line, raw in enumerate(reader, 2):
            if None in raw:
                raise ValueError("%s:%d: too many CSV cells" % (path, line))
            if not any(v for v in raw.values()):
                continue
            try:
                yield Row(raw)
            except (ValueError, InvalidOperation) as exc:
                raise ValueError("%s:%d: %s" % (path, line, exc)) from exc


@contextmanager
def writer(path, fields):
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        yield w


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def find(case, name):
    if name in case.file_cache:
        return case.file_cache[name]
    if name.startswith("toe_") and case.toe_log_dir is not None:
        candidates = [case.toe_log_dir / name]
    else:
        directories = ([case.path / "toe", case.path / "output" / "toe"] if name.startswith("toe_") else [])
        directories += [case.path / "output", case.path, case.path / "runlog"]
        candidates = [directory / name for directory in directories]
    case.searched_paths[name] = [str(path) for path in candidates]
    existing = list(dict.fromkeys(path.resolve() for path in candidates if path.is_file()))
    if len(existing) > 1 and len({digest(path) for path in existing}) > 1:
        raise ValueError("ambiguous %s for %s: %s; use --toe-log-dir CASE DIR to select ToE logs" % (name, case.name, existing))
    path = existing[0] if existing else None
    case.file_cache[name] = path
    if path:
        logging.info("resolved_input case=%s file=%s path=%s", case.name, name, path)
    return path


def warn(case, message):
    case.warnings.append(message)
    logging.warning("case=%s %s", case.name, message)


@dataclass
class Event:
    epoch: int
    solved: int = None
    draining: int = None
    applied: int = None
    pause_start: int = None
    pause_end: int = None
    drain_ns: int = None
    pause_ns: int = None
    changed: int = None
    min_mlu: float = None
    mlu_bound: float = None
    current_mlu: float = None
    optimal: int = None
    scope: str = "unknown"
    paused_tps: int = None
    preserved: int = None
    reconnected: int = None
    unpaused_bytes: int = None
    before: dict = None
    after: dict = None
    delta: dict = field(default_factory=dict)
    returned_to_epoch: int = None
    circuit_source: str = "unavailable"
    summary_applied: bool = False

    def zones(self, args, next_pause=None):
        start, end = self.pause_start, self.pause_end
        if start is None or end is None:
            return []
        post0 = end + ns(args.recovery_us)
        post1 = post0 + ns(args.event_window_us)
        if next_pause is not None:
            post1 = min(post1, next_pause)
        return [("pre", max(0, start - ns(args.event_window_us)), start),
                ("pause", start, end), ("recovery", end, post0),
                ("post", post0, max(post0, post1))]


@dataclass
class Case:
    id: int
    name: str
    path: Path
    is_baseline: bool = False
    files: dict = field(default_factory=dict)
    warnings: list = field(default_factory=list)
    capabilities: dict = field(default_factory=dict)
    events: dict = field(default_factory=dict)
    nodes: dict = field(default_factory=dict)
    links: dict = field(default_factory=dict)
    leaves: set = field(default_factory=set)
    initial: dict = None
    snapshots: dict = field(default_factory=dict)
    snapshot_times: list = field(default_factory=list)
    snapshot_epochs: list = field(default_factory=list)
    physical: list = field(default_factory=list)
    physical_times: list = field(default_factory=list)
    metric_pairs: dict = field(default_factory=dict)
    port_files: set = field(default_factory=set)
    first_apply: int = None
    duration_ns: int = None
    demand_matrices: dict = field(default_factory=dict)
    toe_log_dir: Path = None
    file_cache: dict = field(default_factory=dict)
    searched_paths: dict = field(default_factory=dict)

    def epoch_at(self, t):
        i = bisect.bisect_right(self.snapshot_times, t)
        return self.snapshot_epochs[i - 1] if i else 0

    def peer_at(self, node, port, t):
        initial = self.links.get((node, port))
        if initial is None:
            return None
        # Host attachment is static in this ToE implementation.
        if node not in self.leaves or initial[0] not in self.leaves:
            if self.nodes.get(node) != "SWITCH" or self.nodes.get(initial[0]) != "SWITCH":
                return initial
        if self.is_baseline:
            return initial
        if self.first_apply is None and self.capabilities.get("toe_summary"):
            return initial
        if self.first_apply is not None and t < self.first_apply:
            return initial
        i = bisect.bisect_right(self.physical_times, t)
        if i:
            # A later epoch without a usable physical snapshot invalidates the
            # earlier snapshot; keeping it would silently assign the old peer.
            epoch_i = bisect.bisect_right(self.snapshot_times, t)
            if epoch_i and self.physical_times[i - 1] < self.snapshot_times[epoch_i - 1]:
                return None
            return self.physical[i - 1].get((node, port))
        # Never apply a static OCS peer map after an unobserved mutation.
        return None


SCHEMA = """
CREATE TABLE tasks(case_id INTEGER,tid INTEGER,src INTEGER,dst INTEGER,size INTEGER,
 op TEXT,phase TEXT,meta_hash TEXT,start_ns INTEGER,finish_ns INTEGER,
 stats_present INTEGER DEFAULT 0,attr_conflict INTEGER DEFAULT 0,
 PRIMARY KEY(case_id,tid));
CREATE TABLE stats_seen(case_id INTEGER,tid INTEGER,PRIMARY KEY(case_id,tid));
CREATE TABLE obs(case_id INTEGER,tid INTEGER,epoch INTEGER,kind TEXT,
 a INTEGER,ap INTEGER,b INTEGER,bp INTEGER,bytes INTEGER,packets INTEGER,
 first_ns INTEGER,last_ns INTEGER,source TEXT,
 PRIMARY KEY(case_id,tid,epoch,kind,a,ap,b,bp));
CREATE TABLE features(case_id INTEGER,tid INTEGER,data TEXT,PRIMARY KEY(case_id,tid));
CREATE TABLE direct_pause(case_id INTEGER,tid INTEGER,epoch INTEGER,
 start_ns INTEGER,end_ns INTEGER,duration_ns INTEGER);
CREATE TABLE comparisons(case_id INTEGER,tid INTEGER,status TEXT,op TEXT,size_group TEXT,
 base_ns INTEGER,toe_ns INTEGER,delta_ns INTEGER,pause_overlap_ns INTEGER,
 direct_pause_ns INTEGER,capacity_effect TEXT,start_ns INTEGER,finish_ns INTEGER,
 PRIMARY KEY(case_id,tid));
CREATE TABLE hot(case_id INTEGER,target_case INTEGER,epoch INTEGER,a INTEGER,b INTEGER,
 phase TEXT,kind TEXT,bytes INTEGER,packets INTEGER,
 PRIMARY KEY(case_id,target_case,epoch,a,b,phase,kind));
CREATE TABLE samples(case_id INTEGER,target_case INTEGER,epoch INTEGER,a INTEGER,b INTEGER,
 phase TEXT,metric TEXT,value REAL);
"""


def setup_database(path):
    con = sqlite3.connect(path)
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA synchronous=NORMAL")
    con.execute("PRAGMA temp_store=FILE")
    con.execute("PRAGMA cache_size=-65536")
    con.executescript(SCHEMA)
    return con


def meta(r):
    return (r.integer("taskId", "task_id"), r.integer("sourceNode", "sourceNodeId", "src_node", "source_node"),
            r.integer("destNode", "destNodeId", "dst_node", "dest_node"),
            r.integer("dataSize(Byte)", "size_bytes", "data_size_bytes", "bytes"),
            r.get("opType", "op_type"), r.get("phaseId", "phase_id"))


def load_tasks(con, case):
    traffic = find(case, "traffic.csv")
    stats = find(case, "task_statistics.csv")
    if stats is None:
        raise ValueError("case=%s: task_statistics.csv is required (output/ or case root)" % case.name)
    case.files["task_statistics"] = str(stats)
    if traffic:
        case.files["traffic"] = str(traffic)
        case.capabilities["traffic_sha256"] = digest(traffic)
        batch = []
        for count, r in enumerate(csv_rows(traffic), 1):
            vals = meta(r)
            if vals[0] is None:
                raise ValueError("%s: taskId required" % traffic)
            h = hashlib.sha256(json.dumps(r.d, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            batch.append((case.id,) + vals + (h,))
            if len(batch) >= 10000:
                con.executemany("INSERT INTO tasks(case_id,tid,src,dst,size,op,phase,meta_hash) VALUES(?,?,?,?,?,?,?,?)", batch)
                batch.clear()
            if count % 500000 == 0:
                logging.info("case=%s traffic_rows=%d", case.name, count)
        con.executemany("INSERT INTO tasks(case_id,tid,src,dst,size,op,phase,meta_hash) VALUES(?,?,?,?,?,?,?,?)", batch)
    else:
        warn(case, "traffic.csv missing; full task/dependency equality cannot be verified")
    batch = []
    seen = []
    valid = 0
    count = 0
    sql = """INSERT INTO tasks(case_id,tid,src,dst,size,op,phase,start_ns,finish_ns,stats_present)
    VALUES(?,?,?,?,?,?,?,?,?,1) ON CONFLICT(case_id,tid) DO UPDATE SET
    attr_conflict=CASE WHEN
      (excluded.src IS NOT NULL AND tasks.src IS NOT NULL AND excluded.src!=tasks.src) OR
      (excluded.dst IS NOT NULL AND tasks.dst IS NOT NULL AND excluded.dst!=tasks.dst) OR
      (excluded.size IS NOT NULL AND tasks.size IS NOT NULL AND excluded.size!=tasks.size) OR
      (excluded.op IS NOT NULL AND tasks.op IS NOT NULL AND excluded.op!=tasks.op)
      THEN 1 ELSE tasks.attr_conflict END,
    src=COALESCE(tasks.src,excluded.src),dst=COALESCE(tasks.dst,excluded.dst),
    size=COALESCE(tasks.size,excluded.size),op=COALESCE(tasks.op,excluded.op),
    phase=COALESCE(tasks.phase,excluded.phase),start_ns=excluded.start_ns,
    finish_ns=excluded.finish_ns,stats_present=1"""
    for count, r in enumerate(csv_rows(stats), 1):
        if count == 1 and (not any(norm(k) in r.d for k in ("taskStartTime(us)", "task_start_us", "start_us")) or
                           not any(norm(k) in r.d for k in ("taskCompletesTime(us)", "taskCompleteTime(us)", "finish_us", "task_finish_us"))):
            raise ValueError("%s: taskStartTime(us) and taskCompletesTime(us) are required; packet timestamps are not required" % stats)
        vals = meta(r)
        if vals[0] is None:
            raise ValueError("%s: taskId required" % stats)
        start = ns(r.get("taskStartTime(us)", "task_start_us", "start_us"))
        finish = ns(r.get("taskCompletesTime(us)", "taskCompleteTime(us)", "finish_us", "task_finish_us"))
        if start is not None and finish is not None and finish >= start:
            valid += 1
        seen.append((case.id, vals[0]))
        batch.append((case.id,) + vals + (start, finish))
        if len(batch) >= 10000:
            con.executemany("INSERT INTO stats_seen VALUES(?,?)", seen)
            con.executemany(sql, batch)
            batch.clear(); seen.clear()
        if count % 500000 == 0:
            logging.info("case=%s task_statistics_rows=%d", case.name, count)
    con.executemany("INSERT INTO stats_seen VALUES(?,?)", seen)
    con.executemany(sql, batch)
    con.commit()
    case.duration_ns = con.execute("SELECT MAX(finish_ns) FROM tasks WHERE case_id=?", (case.id,)).fetchone()[0]
    case.capabilities.update(task_statistics_rows=count, valid_completed_tasks=valid)
    if not valid:
        warn(case, "no valid completed task timings; verify taskStartTime(us)/taskCompletesTime(us) columns")


def parse_range(text):
    a, sep, b = text.partition("..")
    return range(int(a), int(b) + 1) if sep else [int(a)]


def load_network(case):
    path = find(case, "node.csv")
    if path:
        for r in csv_rows(path):
            ids = r.get("nodeId", "node_id")
            typ = r.get("nodeType", "node_type", default="UNKNOWN").upper()
            if ids is None:
                raise ValueError("%s: nodeId required" % path)
            for node in parse_range(ids):
                case.nodes[node] = typ
        case.files["node"] = str(path)
    path = find(case, "topology.csv")
    if path:
        for r in csv_rows(path):
            a, ap = r.integer("nodeId1", "src_node", "src"), r.integer("portId1", "src_port")
            b, bp = r.integer("nodeId2", "dst_node", "dst"), r.integer("portId2", "dst_port")
            if None in (a, ap, b, bp):
                raise ValueError("%s: nodeId1/portId1/nodeId2/portId2 required" % path)
            for key, val in [((a, ap), (b, bp)), ((b, bp), (a, ap))]:
                if key in case.links:
                    raise ValueError("%s: duplicate port %s" % (path, key))
                case.links[key] = val
            if case.nodes.get(a) == "SWITCH" and case.nodes.get(b) != "SWITCH":
                case.leaves.add(a)
            if case.nodes.get(b) == "SWITCH" and case.nodes.get(a) != "SWITCH":
                case.leaves.add(b)
        case.files["topology"] = str(path)
    initial = defaultdict(int)
    for (a, ap), (b, bp) in case.links.items():
        if a in case.leaves and b in case.leaves and a < b:
            initial[(a, b)] += 1
    case.initial = dict(initial) if initial else None
    case.snapshots[0] = case.initial
    case.capabilities["network_mapping"] = bool(case.nodes and case.links)
    if not case.capabilities["network_mapping"]:
        warn(case, "node.csv/topology.csv unavailable; actual host/leaf port attribution will be partial")


def leaf_pair(r):
    a = r.integer("leaf_a", "leaf1", "leaf_u", "src_leaf", "src_l1", "node_a", "nodeId1", "i")
    b = r.integer("leaf_b", "leaf2", "leaf_v", "dst_leaf", "dst_l1", "node_b", "nodeId2", "j")
    return None if a is None or b is None else tuple(sorted((a, b)))


def topology_signature(snapshot):
    if snapshot is None:
        return None
    return hashlib.sha256(json.dumps(sorted((a, b, n) for (a, b), n in snapshot.items() if n),
                                     separators=(",", ":")).encode()).hexdigest()


def load_toe(case, args):
    if case.is_baseline:
        case.capabilities["toe_summary"] = False
        return
    path = find(case, "toe_summary.csv")
    case.capabilities["toe_summary"] = path is not None
    if path:
        case.files["toe_summary"] = str(path)
        for r in csv_rows(path):
            epoch = r.integer("epoch")
            if epoch is None:
                raise ValueError("%s: epoch required" % path)
            ev = case.events.setdefault(epoch, Event(epoch))
            status = r.get("status", default="").lower()
            t = row_time(r)
            if status in ("solved", "draining", "applied"):
                setattr(ev, status, t)
            ev.changed = r.integer("changed_circuits", "changed_links", default=ev.changed)
            ev.min_mlu = r.number("min_mlu", default=ev.min_mlu)
            ev.mlu_bound = r.number("applied_mlu_bound", default=ev.mlu_bound)
            ev.current_mlu = r.number("current_mlu", "old_mlu", default=ev.current_mlu)
            ev.optimal = r.integer("solver_optimal", default=ev.optimal)
            if status == "applied":
                ev.summary_applied = True
                ev.pause_ns = ns(r.get("pause_seconds"), "s")
                ev.drain_ns = ns(r.get("drain_seconds"), "s")
    else:
        warn(case, "toe_summary.csv missing; decision count and pause timeline may be unavailable")

    path = find(case, "toe_reconfiguration.csv")
    if path:
        case.files["toe_reconfiguration"] = str(path)
        for r in csv_rows(path):
            epoch = r.integer("epoch")
            if epoch is None:
                raise ValueError("%s: epoch required" % path)
            ev = case.events.setdefault(epoch, Event(epoch))
            ev.scope = r.get("pause_scope", "scope", "effective_scope", "reconfiguration_scope", default=ev.scope)
            ev.paused_tps = r.integer("paused_tp_count", "paused_tp_channels", "paused_tps", "paused_tp", default=ev.paused_tps)
            ev.preserved = r.integer("preserved_circuits", "retained_circuits", "kept_circuits", default=ev.preserved)
            ev.reconnected = r.integer("reconnected_circuits", "reconnected_links", "rewired_circuits", default=ev.reconnected)
            ev.unpaused_bytes = r.integer("unpaused_tx_bytes", "unpaused_new_payload_bytes", "unpaused_bytes", "unpaused_payload_bytes", default=ev.unpaused_bytes)
            for attribute, fields in [("pause_start", ("pause_start_us", "pause_start_time_us")),
                                      ("pause_end", ("resume_us", "pause_end_us", "resume_time_us"))]:
                val = ns(r.get(*fields))
                if val is not None:
                    setattr(ev, attribute, val)
            for attribute, fields in [("pause_start", ("pause_start_seconds", "pause_started_seconds")),
                                      ("pause_end", ("resume_seconds", "pause_end_seconds"))]:
                val = ns(r.get(*fields), "s")
                if val is not None:
                    setattr(ev, attribute, val)
            status = r.get("status", default="").lower()
            if status == "applied" and ev.applied is None:
                ev.applied = row_time(r)
            if status == "applied":
                ev.summary_applied = True
            if r.get("pause_seconds") is not None:
                ev.pause_ns = ns(r.get("pause_seconds"), "s")
            if r.get("drain_seconds") is not None:
                ev.drain_ns = ns(r.get("drain_seconds"), "s")

    groups = defaultdict(dict)
    times = {}
    path = find(case, "toe_topology.csv")
    if path:
        case.files["toe_topology"] = str(path)
        for r in csv_rows(path):
            pair = leaf_pair(r)
            count = r.integer("circuits", "circuit_count", "link_count", "num_links", "links", "count")
            epoch = r.integer("epoch", default=0)
            status = r.get("status", "phase", default="applied").lower()
            if pair is None or count is None:
                raise ValueError("%s: require leaf_a/leaf_b/circuits (aliases supported)" % path)
            if count < 0 or pair[0] == pair[1]:
                raise ValueError("%s: invalid circuit allocation %s=%s" % (path, pair, count))
            key = (epoch, status)
            if pair in groups[key] and groups[key][pair] != count:
                raise ValueError("%s: inconsistent duplicate pair at epoch %s" % (path, epoch))
            groups[key][pair] = count
            times[key] = row_time(r)
        for key in [(0, "initial"), (0, "applied"), (0, "current")]:
            if key in groups:
                case.initial = groups[key].copy()
                break
    else:
        warn(case, "toe_topology.csv missing; capacity-change analysis unavailable unless initial/applied snapshots are supplied")
    case.snapshots[0] = case.initial
    previous = case.initial
    seen = {}
    if previous is not None:
        seen[topology_signature(previous)] = 0
    all_epochs = sorted(set(case.events) | {e for e, s in groups if e > 0})
    for epoch in all_epochs:
        ev = case.events.setdefault(epoch, Event(epoch))
        applied = groups.get((epoch, "applied"))
        if applied is not None and ev.applied is None:
            ev.applied = times.get((epoch, "applied"))
        if applied is None and ev.applied is not None:
            applied = groups.get((epoch, "target"))
            if applied is not None:
                ev.circuit_source = "target_confirmed_by_applied_event"
        elif applied is not None:
            ev.circuit_source = "applied_snapshot"
        if ev.applied is not None and applied is not None:
            after = applied.copy() if args.topology_log_format == "full" else dict(previous or {})
            if args.topology_log_format == "delta":
                after.update(applied)
            ev.before, ev.after = previous, after
            if previous is not None:
                ev.delta = {p: after.get(p, 0) - previous.get(p, 0)
                            for p in set(previous) | set(after)
                            if after.get(p, 0) != previous.get(p, 0)}
            signature = topology_signature(after)
            ev.returned_to_epoch = seen.get(signature) if ev.delta else None
            seen.setdefault(signature, epoch)
            previous = after
            case.snapshots[epoch] = after
    for ev in case.events.values():
        if ev.pause_end is None and ev.applied is not None and ev.pause_ns is not None:
            ev.pause_end = ev.applied
        if ev.pause_start is None and ev.pause_end is not None and ev.pause_ns is not None:
            ev.pause_start = ev.pause_end - ev.pause_ns
        if ev.pause_start is None:
            ev.pause_start = ev.draining
        if ev.pause_end is None and ev.draining is not None:
            ev.pause_end = ev.applied
        if ev.pause_start is not None and ev.pause_end is not None:
            if ev.pause_end < ev.pause_start:
                raise ValueError("case=%s epoch=%d: reversed pause interval" % (case.name, ev.epoch))
        if is_reconfiguration(ev) or (ev.applied is not None and ev.after is not None and ev.before is None):
            case.snapshot_times.append(ev.applied)
            case.snapshot_epochs.append(ev.epoch)
    ordered = sorted(zip(case.snapshot_times, case.snapshot_epochs))
    case.snapshot_times = [x[0] for x in ordered]
    case.snapshot_epochs = [x[1] for x in ordered]
    case.first_apply = case.snapshot_times[0] if case.snapshot_times else None
    case.capabilities["capacity_snapshots"] = bool(groups and previous is not None)
    case.capabilities["reconfiguration_count_complete"] = bool(case.capabilities.get("toe_summary")) or (
        bool(case.events) and all(ev.before is not None for ev in case.events.values() if ev.applied is not None))
    if any(e.draining is not None and e.applied is None for e in case.events.values()):
        warn(case, "one or more events remain draining without applied; target is not treated as active topology")

    path = find(case, "toe_circuits.csv")
    physical_groups = defaultdict(dict)
    if path:
        case.files["toe_circuits"] = str(path)
        for r in csv_rows(path):
            epoch = r.integer("epoch", default=0)
            status = r.get("status", "phase", default="applied").lower()
            if status not in ("applied", "initial"):
                continue
            a = r.integer("leaf_a", "node_a", "nodeId1", "leaf1", "src_leaf")
            b = r.integer("leaf_b", "node_b", "nodeId2", "leaf2", "dst_leaf")
            ap = r.integer("port_a", "portId1", "leaf1_port", "src_port")
            bp = r.integer("port_b", "portId2", "leaf2_port", "dst_port")
            if None in (a, ap, b, bp):
                warn(case, "toe_circuits.csv has unsupported endpoint columns; dynamic peer attribution disabled")
                physical_groups.clear()
                break
            m = physical_groups[epoch]
            for key, val in [((a, ap), (b, bp)), ((b, bp), (a, ap))]:
                if key in m and m[key] != val:
                    raise ValueError("%s: duplicate physical port at epoch %d" % (path, epoch))
                m[key] = val
        for t, epoch in ordered:
            mapping = physical_groups.get(epoch)
            if not mapping:
                continue
            counts = defaultdict(int)
            for (a, ap), (b, bp) in mapping.items():
                if a < b:
                    counts[(a, b)] += 1
            expected = case.snapshots.get(epoch)
            if expected is not None and {p: n for p, n in expected.items() if n} != dict(counts):
                warn(case, "epoch=%d physical snapshot incomplete or disagrees with logical allocation; ignored" % epoch)
                continue
            full = {k: v for k, v in case.links.items()
                    if not (k[0] in case.leaves and v[0] in case.leaves)}
            full.update(mapping)
            case.physical_times.append(t)
            case.physical.append(full)
    case.capabilities["dynamic_physical_snapshots"] = bool(case.physical)
    case.capabilities["pause_timeline_complete"] = bool(case.capabilities.get("toe_summary")) and all(
        (ev.pause_start is not None and ev.pause_end is not None) if is_reconfiguration(ev) else
        not (ev.changed is not None and ev.changed > 0 and ev.applied is None)
        for ev in case.events.values())
    if case.first_apply is not None and not case.physical:
        warn(case, "no usable applied toe_circuits.csv; switched OCS peers are left unknown after the first reconfiguration")


def load_direct_pause(con, case):
    path = find(case, "task_toe_exposure.csv")
    case.capabilities["direct_task_pause"] = False
    if not path:
        return
    count = 0
    for r in csv_rows(path):
        tid = r.integer("task_id", "taskId")
        epoch = r.integer("epoch", default=-1)
        start, end = ns(r.get("pause_start_us")), ns(r.get("pause_end_us", "resume_us"))
        duration = ns(r.get("direct_pause_us", "blocked_by_toe_us"))
        if tid is None:
            raise ValueError("%s: task_id required" % path)
        if start is not None and end is not None:
            if end < start:
                raise ValueError("%s: reversed task pause" % path)
            duration = end - start
        elif duration is None:
            continue
        con.execute("INSERT INTO direct_pause VALUES(?,?,?,?,?,?)", (case.id, tid, epoch, start, end, duration))
        count += 1
    case.capabilities["direct_task_pause"] = bool(count)
    case.files["task_toe_exposure"] = str(path)
    if not count:
        warn(case, "task_toe_exposure.csv contains no direct_pause_us or exact task pause intervals")
    con.commit()


class Zones:
    def __init__(self, cases, args):
        self.by_target = {}
        self.lookup = {}
        self.indices = {}
        for case in cases:
            if case.is_baseline:
                continue
            events = sorted((e for e in case.events.values() if e.pause_start is not None and e.pause_end is not None),
                            key=lambda e: e.pause_start)
            intervals = []
            for i, ev in enumerate(events):
                nxt = events[i + 1].pause_start if i + 1 < len(events) else None
                for phase, start, end in ev.zones(args, nxt):
                    if case.duration_ns is not None:
                        end = min(end, case.duration_ns)
                    end = max(start, end)
                    self.lookup[(case.id, ev.epoch, phase)] = (start, end)
                    if end > start:
                        intervals.append((start, end, ev.epoch, phase))
            intervals.sort()
            self.by_target[case.id] = intervals
            max_end = 0
            prefix = []
            for start, end, epoch, phase in intervals:
                max_end = max(max_end, end)
                prefix.append(max_end)
            self.indices[case.id] = ([x[0] for x in intervals], prefix)

    def matches(self, case, t, bin_end=None):
        targets = self.by_target if case.is_baseline else {case.id: self.by_target.get(case.id, [])}
        for target, intervals in targets.items():
            starts, ends = self.indices.get(target, ([], []))
            lower, upper = bisect.bisect_right(ends, t), bisect.bisect_right(starts, t)
            for start, end, epoch, phase in intervals[lower:upper]:
                if start <= t < end and (bin_end is None or bin_end <= end):
                    yield target, epoch, phase


OBS_UPSERT = """INSERT INTO obs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(case_id,tid,epoch,kind,a,ap,b,bp)
 DO UPDATE SET bytes=bytes+excluded.bytes,packets=packets+excluded.packets,
 first_ns=MIN(first_ns,excluded.first_ns),last_ns=MAX(last_ns,excluded.last_ns)"""
HOT_UPSERT = """INSERT INTO hot VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(case_id,target_case,epoch,a,b,phase,kind)
 DO UPDATE SET bytes=bytes+excluded.bytes,packets=packets+excluded.packets"""


class Buffer:
    def __init__(self, con, zones, case):
        self.con, self.zones, self.case = con, zones, case
        self.obs, self.hot = {}, {}

    def observation(self, tid, t, kind, a, ap, b, bp, size, source, packets=1, last=None, epoch=None):
        if tid is None:
            return
        epoch = self.case.epoch_at(t) if epoch is None else epoch
        key = (self.case.id, tid, epoch, kind, a, ap, b, bp)
        val = self.obs.setdefault(key, [0, 0, t, t if last is None else last, source])
        val[0] += size; val[1] += packets
        val[2] = min(val[2], t); val[3] = max(val[3], t if last is None else last)
        if len(self.obs) >= 30000:
            self.flush()

    def traffic(self, t, kind, a, b, size, packets=1, bin_end=None):
        for target, epoch, phase in self.zones.matches(self.case, t, bin_end):
            key = (self.case.id, target, epoch, a, b, phase, kind)
            val = self.hot.setdefault(key, [0, 0])
            val[0] += size; val[1] += packets
        if len(self.hot) >= 30000:
            self.flush()

    def flush(self):
        self.con.executemany(OBS_UPSERT, (key + tuple(val) for key, val in self.obs.items()))
        self.con.executemany(HOT_UPSERT, (key + tuple(val) for key, val in self.hot.items()))
        self.obs.clear(); self.hot.clear()
        self.con.commit()


TX_RE = re.compile(rb'^\[\s*([0-9eE+\-.]+)us\]\s+Port Tx,\s*port ID:\s*(\d+)\s+PacketSize:\s*(\d+)')
TASK_RE = re.compile(rb'TaskId:\s*(\d+|NA)')
PAIR_RE = re.compile(rb'^\[\s*([0-9eE+\-.]+)us\]\s+L1 Pair Tx,\s*SrcL1:\s*(\d+)\s+DstL1:\s*(\d+)\s+PacketUid:\s*\d+\s+PacketSize:\s*(\d+)\s+TaskId:\s*(\d+|NA)')
PORT_NAME_RE = re.compile(r'PortTrace_node_(\d+)_port_(\d+)\.tr$')


def scan_traces(con, case, zones, args):
    case.capabilities.update(port_trace=False, tagged_port_trace=False, l1_pair_trace=False,
                             logical_metrics=False, physical_metrics=False)
    if args.trace_mode == "off":
        warn(case, "raw trace scanning disabled; actual task paths may be unavailable")
        return
    buf = Buffer(con, zones, case)
    runlog = case.path / "runlog"
    pair_paths = sorted(runlog.glob("L1PairTrace*.tr")) if runlog.exists() else []
    total, tagged, malformed = 0, 0, 0
    for path in pair_paths:
        with path.open("rb", buffering=1024 * 1024) as f:
            for line in f:
                if b"L1 Pair Tx" not in line:
                    continue
                m = PAIR_RE.match(line)
                if not m:
                    malformed += 1
                    continue
                t, a, b, size = ns(m[1].decode()), int(m[2]), int(m[3]), int(m[4])
                tid = None if m[5] == b"NA" else int(m[5])
                total += 1; tagged += int(tid is not None)
                buf.observation(tid, t, "L1_PAIR", a, -1, b, -1, size, "L1PairTrace")
                buf.traffic(t, "logical", a, b, size)
                if total % 1000000 == 0:
                    logging.info("case=%s L1PairTrace_events=%d", case.name, total)
        case.files.setdefault("l1_pair_traces", []).append(str(path))
    case.capabilities.update(l1_pair_trace=bool(total), logical_metrics=bool(total),
                             l1_pair_events=total, l1_pair_tagged_events=tagged)
    if total:
        case.metric_pairs["logical"] = None  # Complete raw stream, including zero-traffic pairs.
    if malformed:
        warn(case, "%d malformed L1 Pair Tx records skipped" % malformed)
    port_paths = sorted(runlog.glob("PortTrace_node_*_port_*.tr")) if runlog.exists() else []
    total, tagged, unknown, malformed = 0, 0, 0, 0
    for path in port_paths:
        name = PORT_NAME_RE.match(path.name)
        if not name:
            continue
        node, expected_port = int(name[1]), int(name[2])
        case.port_files.add((node, expected_port))
        with path.open("rb", buffering=1024 * 1024) as f:
            for line in f:
                if b"Port Tx" not in line:
                    continue
                m = TX_RE.match(line)
                if not m or int(m[2]) != expected_port:
                    malformed += 1
                    continue
                t, size = ns(m[1].decode()), int(m[3])
                tag = TASK_RE.search(line)
                tid = int(tag[1]) if tag and tag[1] != b"NA" else None
                total += 1; tagged += int(tid is not None)
                peer = case.peer_at(node, expected_port, t)
                b, bp = peer if peer is not None else (-1, -1)
                is_host = peer is not None and (case.nodes.get(node) != "SWITCH" or case.nodes.get(b) != "SWITCH")
                buf.observation(tid, t, "HOST" if is_host else "SWITCH", node, expected_port,
                                b, bp, size, "PortTrace")
                if peer is None:
                    unknown += 1
                elif node in case.leaves and b in case.leaves:
                    buf.traffic(t, "physical", node, b, size)
                if total % 1000000 == 0:
                    logging.info("case=%s PortTrace_events=%d unknown_peer_events=%d", case.name, total, unknown)
    buf.flush()
    case.capabilities.update(port_trace=bool(total), tagged_port_trace=bool(tagged),
                             physical_metrics=bool(total and case.initial is not None),
                             port_events=total, port_tagged_events=tagged, unknown_peer_events=unknown)
    if total and case.initial is not None:
        case.metric_pairs["physical"] = None
    if malformed:
        warn(case, "%d malformed/mismatched Port Tx records skipped" % malformed)
    if not tagged and total:
        warn(case, "PortTrace has no usable TaskId; cannot assign physical ports to individual tasks")
    if not tagged and not case.capabilities["l1_pair_tagged_events"]:
        warn(case, "no task-attributed path observations; task FCT comparison remains available")


def import_path_csv(con, case, zones):
    path = find(case, "task_path_usage.csv")
    if not path:
        return False
    buf = Buffer(con, zones, case)
    count = 0
    for r in csv_rows(path):
        tid = r.integer("task_id", "taskId")
        a, b = r.integer("src_leaf", "src_l1"), r.integer("dst_leaf", "dst_l1")
        first = ns(r.get("first_tx_us", "time_us"))
        last = ns(r.get("last_tx_us"))
        size = r.integer("tx_bytes", "bytes")
        packets = r.integer("packet_count", "packets", default=0)
        if None in (tid, a, b, first, size):
            raise ValueError("%s: task_id/src_leaf/dst_leaf/first_tx_us/tx_bytes required" % path)
        epoch = r.integer("epoch", default=case.epoch_at(first))
        if last is not None and last < first:
            raise ValueError("%s: reversed task path interval" % path)
        if case.epoch_at(first) != case.epoch_at(last if last is not None else first):
            raise ValueError("%s: a path row spans topology epochs; split it into epoch rows" % path)
        buf.observation(tid, first, "L1_PAIR", a, -1, b, -1, size, "task_path_usage.csv",
                        packets, last, epoch)
        # Ports are actual observations only if the normalized input explicitly
        # supplies them. They are never selected from candidate topology lanes.
        for prefix in ("npu", "asu"):
            dev = r.integer(prefix + "_node_id")
            port = r.integer(prefix + "_port_id")
            leaf_prefix = "leaf1" if prefix == "npu" else "leaf2"
            leaf = r.integer(leaf_prefix + "_node_id")
            hp = r.integer(leaf_prefix + "_host_port_id")
            if None not in (dev, port, leaf, hp):
                buf.observation(tid, first, "HOST", dev, port, leaf, hp, size,
                                "task_path_usage.csv", packets, last, epoch)
        count += 1
    buf.flush()
    case.files["task_path_usage"] = str(path)
    case.capabilities["normalized_task_path_rows"] = count
    return bool(count)


def import_metrics(con, case, zones, args):
    # Use precomputed streams only when corresponding raw metrics weren't read.
    paths = [("logical", case.path / "output/l1_pair_hotspots/l1_pair_timeseries.csv"),
             ("physical", case.path / "output/switch_hotspots/switch_bundle_timeseries.csv")]
    for kind, path in paths:
        if not path.exists() or kind in case.metric_pairs:
            continue
        if kind == "physical" and not case.is_baseline and case.first_apply is not None:
            warn(case, "static switch_bundle_timeseries.csv not used for dynamic OCS peer attribution; use raw PortTrace + applied circuit snapshots")
            continue
        buf = Buffer(con, zones, case)
        known_pairs = set()
        chosen_window = args.metrics_window_us
        count = 0
        for r in csv_rows(path):
            width = r.integer("window_us")
            if chosen_window is None:
                chosen_window = width
            if width != chosen_window:
                continue
            a = r.integer("src_l1", "src_switch", "src_leaf")
            b = r.integer("dst_l1", "dst_switch", "dst_leaf")
            start, end = ns(r.get("window_start_us")), ns(r.get("window_end_us"))
            size = r.integer("tx_bytes")
            if None in (a, b, start, end, size):
                raise ValueError("%s: window_start_us/window_end_us/src/dst/tx_bytes required" % path)
            if kind == "physical" and not (a in case.leaves and b in case.leaves):
                continue
            known_pairs.add((a, b))
            buf.traffic(start, kind, a, b, size, r.integer("packet_count", default=0), end)
            count += 1
        buf.flush()
        if count:
            case.metric_pairs[kind] = known_pairs
            case.capabilities[kind + "_metrics"] = True
            case.files[kind + "_timeseries"] = str(path)
            case.capabilities[kind + "_metrics_window_us"] = chosen_window
            warn(case, "%s precomputed series covers %d observed pairs; unlisted pairs stay unavailable, not zero" % (kind, len(known_pairs)))
    path = find(case, "pair_metrics.csv")
    if path:
        count = 0
        for r in csv_rows(path):
            t = row_time(r)
            a, b = r.integer("src_leaf", "src_l1"), r.integer("dst_leaf", "dst_l1")
            if None in (t, a, b):
                raise ValueError("%s: time_us/src_leaf/dst_leaf required" % path)
            for target, epoch, phase in zones.matches(case, t):
                for metric in ("queue_bytes", "backlog_bytes", "source_utilization", "destination_utilization", "predicted_demand_gbps"):
                    value = r.number(metric)
                    if value is not None:
                        con.execute("INSERT INTO samples VALUES(?,?,?,?,?,?,?,?)",
                                    (case.id, target, epoch, a, b, phase, metric, value))
                        count += 1
        case.files["pair_metrics"] = str(path)
        case.capabilities["pair_metric_samples"] = count
    con.commit()


def encode_set(values):
    return json.dumps(sorted(values), separators=(",", ":"))


def build_features(con, case, args):
    fields = ["npu_port_id", "asu_port_id", "leaf1_node_id", "leaf2_node_id",
              "leaf1_host_port_id", "leaf2_host_port_id", "leaf1_network_port_id", "leaf2_network_port_id"]
    sql = """SELECT o.tid,o.epoch,o.kind,o.a,o.ap,o.b,o.bp,o.bytes,o.source,t.src,t.dst
      FROM obs o LEFT JOIN tasks t ON t.case_id=o.case_id AND t.tid=o.tid
      WHERE o.case_id=? ORDER BY o.tid,o.epoch,o.kind"""
    current = None
    rows = []
    batch = []
    host_leaves = defaultdict(set)
    for (node, port), (peer, peer_port) in case.links.items():
        if peer in case.leaves and case.nodes.get(node) != "SWITCH":
            host_leaves[node].add(peer)

    def finish(tid, observations):
        sets = {f: set() for f in fields}
        npu, asu = observations[0][9:11]
        if args.endpoint_convention == "dest-is-npu":
            npu, asu = asu, npu
        pairs, sources, epochs = set(), set(), set()
        links_by_epoch = defaultdict(set)
        capacity_deltas = []
        for _, ep, kind, a, ap, b, bp, size, source, _, _ in observations:
            epochs.add(ep); sources.add(source)
            if kind == "HOST":
                for dev, dp, leaf, hp in [(a, ap, b, bp), (b, bp, a, ap)]:
                    if dev == npu and leaf in case.leaves:
                        sets["npu_port_id"].add(dp); sets["leaf1_node_id"].add(leaf); sets["leaf1_host_port_id"].add(hp)
                    if dev == asu and leaf in case.leaves:
                        sets["asu_port_id"].add(dp); sets["leaf2_node_id"].add(leaf); sets["leaf2_host_port_id"].add(hp)
            if kind == "L1_PAIR":
                pairs.add((a, b))
                # Actual L1 tags + host attachment distinguish sides even for
                # READ's reversed data direction. Never guess a host port.
                for leaf in (a, b):
                    if leaf in host_leaves[npu]:
                        sets["leaf1_node_id"].add(leaf)
                    if leaf in host_leaves[asu]:
                        sets["leaf2_node_id"].add(leaf)
                initial, active = case.initial, case.snapshots.get(ep)
                pair = tuple(sorted((a, b)))
                if initial is not None and active is not None and (pair in initial or pair in active):
                    capacity_deltas.append(active.get(pair, 0) - initial.get(pair, 0))
            if kind == "SWITCH" and b >= 0:
                links_by_epoch[ep].add((a, ap, b, bp))
        for _, ep, kind, a, ap, b, bp, size, source, _, _ in observations:
            if kind != "SWITCH":
                continue
            for leaf, port in [(a, ap), (b, bp)]:
                if leaf in sets["leaf1_node_id"] and port >= 0:
                    sets["leaf1_network_port_id"].add(port)
                if leaf in sets["leaf2_node_id"] and port >= 0:
                    sets["leaf2_network_port_id"].add(port)
        effect = "unknown"
        if capacity_deltas:
            plus, minus = any(x > 0 for x in capacity_deltas), any(x < 0 for x in capacity_deltas)
            effect = "mixed" if plus and minus else "increased" if plus else "decreased" if minus else "unchanged"
        link_sets = list(links_by_epoch.values())
        data = {f: (next(iter(s)) if len(s) == 1 else None) for f, s in sets.items()}
        data.update({f + "s": sorted(s) for f, s in sets.items()})
        data.update(path_data_available=1, path_data_source=";".join(sorted(sources)),
                    observed_leaf_pair_count=len(pairs), observed_link_count=sum(len(x) for x in link_sets),
                    observed_epochs=sorted(epochs), capacity_effect_vs_initial=effect,
                    observed_link_set_changed_across_epochs=(int(any(x != link_sets[0] for x in link_sets[1:]))
                                                             if len(link_sets) > 1 else None))
        return (case.id, tid, json.dumps(data, separators=(",", ":")))

    for row in con.execute(sql, (case.id,)):
        if current is not None and row[0] != current:
            batch.append(finish(current, rows)); rows = []
            if len(batch) >= 10000:
                con.executemany("INSERT INTO features VALUES(?,?,?)", batch); batch.clear()
        current = row[0]; rows.append(row)
    if current is not None:
        batch.append(finish(current, rows))
    con.executemany("INSERT INTO features VALUES(?,?,?)", batch)
    con.commit()


def merged_intervals(intervals):
    result = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if result and start <= result[-1][1]:
            result[-1] = (result[-1][0], max(result[-1][1], end))
        else:
            result.append((start, end))
    return result


def overlap(start, end, intervals, ends):
    if start is None or end is None or end < start:
        return None
    total = 0
    i = bisect.bisect_right(ends, start)
    while i < len(intervals) and intervals[i][0] < end:
        a, b = intervals[i]
        total += max(0, min(end, b) - max(start, a))
        i += 1
    return total


def prepare_direct_totals(con):
    con.execute("CREATE TABLE direct_totals(case_id INTEGER,tid INTEGER,duration_ns INTEGER,PRIMARY KEY(case_id,tid))")
    current, intervals, scalars = None, [], []
    def flush(key):
        if key is not None:
            total = sum(b - a for a, b in merged_intervals(intervals)) + sum(scalars)
            con.execute("INSERT INTO direct_totals VALUES(?,?,?)", key + (total,))
    for c, tid, epoch, start, end, duration in con.execute("SELECT * FROM direct_pause ORDER BY case_id,tid,epoch"):
        key = (c, tid)
        if key != current:
            flush(current); intervals, scalars = [], []
        current = key
        if start is not None and end is not None:
            intervals.append((start, end))
        else:
            scalars.append(duration)
    flush(current)
    con.commit()


def mismatch_sql():
    return """(b.attr_conflict=1 OR t.attr_conflict=1 OR
     (b.meta_hash IS NOT NULL AND t.meta_hash IS NOT NULL AND b.meta_hash!=t.meta_hash) OR
     (b.src IS NOT NULL AND t.src IS NOT NULL AND b.src!=t.src) OR
     (b.dst IS NOT NULL AND t.dst IS NOT NULL AND b.dst!=t.dst) OR
     (b.size IS NOT NULL AND t.size IS NOT NULL AND b.size!=t.size) OR
     (b.op IS NOT NULL AND t.op IS NOT NULL AND b.op!=t.op))"""


def verify_matching(con, baseline, cases, args):
    for case in cases:
        bad = con.execute("SELECT COUNT(*) FROM tasks b JOIN tasks t ON b.tid=t.tid "
                          "WHERE b.case_id=? AND t.case_id=? AND " + mismatch_sql(),
                          (baseline.id, case.id)).fetchone()[0]
        case.capabilities["metadata_mismatched_tasks"] = bad
        if bad and not args.allow_traffic_mismatch:
            raise ValueError("case=%s: %d task metadata/dependency mismatches with baseline; use identical traffic.csv. "
                             "--allow-traffic-mismatch excludes mismatched tasks from paired metrics" % (case.name, bad))
        if bad:
            warn(case, "%d task metadata mismatches excluded from paired FCT metrics" % bad)
        if baseline.capabilities.get("traffic_sha256") and case.capabilities.get("traffic_sha256"):
            if baseline.capabilities["traffic_sha256"] != case.capabilities["traffic_sha256"] and not bad:
                warn(case, "traffic.csv bytes differ, but canonical per-task rows match (e.g. row order/line endings)")


def compare_fields():
    fields = ["case_name", "task_id", "source_node_id", "dest_node_id", "npu_node_id", "asu_node_id",
              "op_type", "data_direction", "data_source_node_id", "data_dest_node_id", "data_size_bytes", "phase_id", "size_group", "comparison_status", "metadata_verification",
              "start_us_ecmp", "finish_us_ecmp", "start_us_toe", "finish_us_toe",
              "fct_us_ecmp", "fct_us_toe", "delta_fct_us", "improvement_pct", "change_class",
              "delta_start_us", "delta_finish_us", "topology_epoch_start_toe", "topology_epoch_finish_toe", "topology_epoch_data_available_toe",
              "pause_overlap_us", "direct_pause_us", "capacity_effect_vs_initial",
              "fct_us_ideal", "ideal_plan_verified", "ideal_topology_gain_us", "reconfiguration_execution_cost_us"]
    for suffix in ("ecmp", "toe"):
        for name in ("npu_port_id", "asu_port_id", "leaf1_node_id", "leaf2_node_id", "leaf1_host_port_id",
                     "leaf2_host_port_id", "leaf1_network_port_id", "leaf2_network_port_id"):
            fields += [name + "_" + suffix, name + "s_" + suffix]
        fields += ["path_data_available_" + suffix, "path_data_source_" + suffix,
                   "observed_leaf_pair_count_" + suffix, "observed_link_count_" + suffix,
                   "observed_link_set_changed_across_epochs_" + suffix]
    return fields


def valid_timing(start, finish):
    return start is not None and finish is not None and finish >= start


def ideal_verified(case, ideal):
    if ideal is None or case.initial is None or ideal.initial is None:
        return False
    if case.initial != ideal.initial or case.snapshot_times != ideal.snapshot_times:
        return False
    if len(case.snapshot_epochs) != len(ideal.snapshot_epochs):
        return False
    return bool(case.snapshot_epochs) and all(
        case.snapshots.get(a) is not None and case.snapshots.get(a) == ideal.snapshots.get(b)
        for a, b in zip(case.snapshot_epochs, ideal.snapshot_epochs))


def write_task_comparisons(con, baseline, cases, ideals, output, args, consumer=None):
    epsilon = ns(args.epsilon_us)
    insert = "INSERT INTO comparisons VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
    with writer(output / "task_compare.csv", compare_fields()) as w:
        for case in cases:
            intervals = merged_intervals([(e.pause_start, e.pause_end) for e in case.events.values()
                                          if e.pause_start is not None and e.pause_end is not None])
            ends = [b for a, b in intervals]
            ideal = ideals.get(case.name)
            verified_ideal = ideal_verified(case, ideal)
            case.capabilities["ideal_plan_verified"] = verified_ideal
            if ideal and not verified_ideal:
                warn(case, "ideal run's initial topology/applied snapshots/timestamps cannot be verified equal; execution-cost columns stay blank")
            ideal_id = ideal.id if ideal else -1
            sql = """WITH ids AS (SELECT tid FROM tasks WHERE case_id=? UNION SELECT tid FROM tasks WHERE case_id=?)
            SELECT ids.tid,b.src AS bsrc,b.dst AS bdst,b.size AS bsize,b.op AS bop,b.phase AS bphase,
             b.start_ns AS bs,b.finish_ns AS bf,b.meta_hash AS bh,b.attr_conflict AS bc,
             t.src AS tsrc,t.dst AS tdst,t.size AS tsize,t.op AS top,t.phase AS tphase,
             t.start_ns AS ts,t.finish_ns AS tf,t.meta_hash AS th,t.attr_conflict AS tc,
             fb.data AS bfeatures,ft.data AS tfeatures,d.duration_ns AS direct,
             i.start_ns AS is_,i.finish_ns AS if_,i.src AS isrc,i.dst AS idst,i.size AS isize,i.op AS iop,
             i.meta_hash AS ih,i.attr_conflict AS ic,b.tid AS bid,t.tid AS toeid
            FROM ids LEFT JOIN tasks b ON b.case_id=? AND b.tid=ids.tid
             LEFT JOIN tasks t ON t.case_id=? AND t.tid=ids.tid
             LEFT JOIN features fb ON fb.case_id=b.case_id AND fb.tid=b.tid
             LEFT JOIN features ft ON ft.case_id=t.case_id AND ft.tid=t.tid
             LEFT JOIN direct_totals d ON d.case_id=t.case_id AND d.tid=t.tid
             LEFT JOIN tasks i ON i.case_id=? AND i.tid=ids.tid ORDER BY ids.tid"""
            cur = con.execute(sql, (baseline.id, case.id, baseline.id, case.id, ideal_id))
            names = [d[0] for d in cur.description]
            batch = []
            counts = defaultdict(int)
            cost_count, total_cost, total_gain = 0, 0, 0
            for count, values in enumerate(cur, 1):
                r = dict(zip(names, values))
                bs, bf, ts, tf = r["bs"], r["bf"], r["ts"], r["tf"]
                bvalid, tvalid = valid_timing(bs, bf), valid_timing(ts, tf)
                bad = bool(r["bc"] or r["tc"] or (r["bh"] is not None and r["th"] is not None and r["bh"] != r["th"]))
                for left, right in (("bsrc", "tsrc"), ("bdst", "tdst"), ("bsize", "tsize"), ("bop", "top")):
                    bad |= r[left] is not None and r[right] is not None and r[left] != r[right]
                status = ("metadata_mismatch" if bad else "missing_ecmp_task" if r["bid"] is None else
                          "missing_toe_task" if r["toeid"] is None else "paired_completed" if bvalid and tvalid else
                          "toe_incomplete_or_invalid" if bvalid else "ecmp_incomplete_or_invalid" if tvalid else "both_incomplete_or_invalid")
                base_fct, toe_fct = (bf - bs if bvalid else None), (tf - ts if tvalid else None)
                delta = toe_fct - base_fct if status == "paired_completed" else None
                counts[status] += 1
                src = r["tsrc"] if r["tsrc"] is not None else r["bsrc"]
                dst = r["tdst"] if r["tdst"] is not None else r["bdst"]
                size = r["tsize"] if r["tsize"] is not None else r["bsize"]
                op = r["top"] or r["bop"] or "UNKNOWN"
                group = "unknown" if size is None else "small" if size <= args.size_threshold_bytes else "large"
                npu, asu = (src, dst) if args.endpoint_convention == "source-is-npu" else (dst, src)
                feat = json.loads(r["tfeatures"]) if r["tfeatures"] else {}
                pause = overlap(ts, tf, intervals, ends) if tvalid and case.capabilities.get("pause_timeline_complete") else None
                # Known run-level absence of a direct task pause observation is
                # not evidence that the task's blocked time was zero.
                direct = r["direct"]
                effect = feat.get("capacity_effect_vs_initial", "unknown")
                iv = valid_timing(r["is_"], r["if_"])
                ideal_fct = r["if_"] - r["is_"] if iv else None
                ideal_match = not r["ic"] and all(r[x] is None or r[y] is None or r[x] == r[y]
                                                  for x, y in [("tsrc", "isrc"), ("tdst", "idst"), ("tsize", "isize"), ("top", "iop"), ("th", "ih")])
                cost_ok = verified_ideal and ideal_match and iv and status == "paired_completed"
                if cost_ok:
                    cost_count += 1
                    total_cost += toe_fct - ideal_fct
                    total_gain += base_fct - ideal_fct
                read = op.upper() in ("URMA_READ", "MEM_LOAD")
                known_op = op.upper() in ("URMA_READ", "MEM_LOAD", "URMA_WRITE", "MEM_STORE")
                data_src, data_dst = (dst, src) if read else (src, dst)
                row = dict(case_name=case.name, task_id=r["tid"], source_node_id=src, dest_node_id=dst,
                           npu_node_id=npu, asu_node_id=asu, op_type=op,
                           data_direction=("npu_to_asu_payload" if data_src == npu else "asu_to_npu_payload") if known_op else "unknown",
                           data_source_node_id=data_src if known_op else None, data_dest_node_id=data_dst if known_op else None, data_size_bytes=size,
                           phase_id=r["tphase"] or r["bphase"], size_group=group, comparison_status=status,
                           metadata_verification="full_traffic_row" if r["bh"] and r["th"] else "partial_attributes_only",
                           start_us_ecmp=micro(bs), finish_us_ecmp=micro(bf), start_us_toe=micro(ts), finish_us_toe=micro(tf),
                           fct_us_ecmp=micro(base_fct), fct_us_toe=micro(toe_fct), delta_fct_us=micro(delta),
                           improvement_pct=(-delta / base_fct * 100 if delta is not None and base_fct > 0 else None),
                           change_class=("improved" if delta < -epsilon else "regressed" if delta > epsilon else "unchanged") if delta is not None else "unpaired",
                           delta_start_us=micro(ts - bs) if ts is not None and bs is not None else None,
                           delta_finish_us=micro(tf - bf) if tf is not None and bf is not None else None,
                           topology_epoch_start_toe=case.epoch_at(ts) if ts is not None and (case.capabilities.get("toe_summary") or case.capabilities.get("capacity_snapshots")) else None,
                           topology_epoch_finish_toe=case.epoch_at(tf) if tf is not None and (case.capabilities.get("toe_summary") or case.capabilities.get("capacity_snapshots")) else None,
                           topology_epoch_data_available_toe=int(bool(case.capabilities.get("toe_summary") or case.capabilities.get("capacity_snapshots"))),
                           pause_overlap_us=micro(pause), direct_pause_us=micro(direct), capacity_effect_vs_initial=effect,
                           fct_us_ideal=micro(ideal_fct), ideal_plan_verified=int(verified_ideal),
                           ideal_topology_gain_us=micro(base_fct - ideal_fct) if cost_ok else None,
                           reconfiguration_execution_cost_us=micro(toe_fct - ideal_fct) if cost_ok else None)
                for suffix, raw in [("ecmp", r["bfeatures"]), ("toe", r["tfeatures"])]:
                    features = json.loads(raw) if raw else {"path_data_available": 0, "path_data_source": "unavailable"}
                    for key, value in features.items():
                        row[key + "_" + suffix] = json.dumps(value, separators=(",", ":")) if isinstance(value, list) else value
                w.writerow(row)
                if consumer is not None:
                    consumer({key: "" if value is None else str(value) for key, value in row.items()})
                batch.append((case.id, r["tid"], status, op, group, base_fct, toe_fct, delta, pause, direct, effect, ts, tf))
                if len(batch) >= 10000:
                    con.executemany(insert, batch); batch.clear()
                if count % 500000 == 0:
                    logging.info("case=%s task_compare_rows=%d", case.name, count)
            con.executemany(insert, batch)
            con.commit()
            case.capabilities["comparison_status_counts"] = dict(counts)
            case.capabilities.update(ideal_paired_tasks=cost_count,
                ideal_mean_topology_gain_us=micro(total_gain / cost_count) if cost_count else None,
                ideal_mean_execution_cost_us=micro(total_cost / cost_count) if cost_count else None)
            logging.info("case=%s paired_counts=%s", case.name, dict(counts))


def write_observations(con, cases, output):
    names = {c.id: c.name for c in cases}
    with writer(output / "task_path_usage.csv", ["case_name", "task_id", "epoch", "src_leaf", "dst_leaf", "tx_bytes",
                                                "packet_count", "first_tx_us", "last_tx_us", "source", "path_semantics"]) as paths, \
         writer(output / "task_link_usage.csv", ["case_name", "task_id", "epoch", "link_kind", "src_node", "src_port",
                                                "dst_node", "dst_port", "tx_bytes", "packet_count", "first_tx_us", "last_tx_us",
                                                "source", "peer_mapping_available"]) as links:
        for c, tid, ep, kind, a, ap, b, bp, size, packets, first, last, source in con.execute("SELECT * FROM obs ORDER BY case_id,tid,epoch"):
            if c not in names:
                continue
            if kind == "L1_PAIR":
                paths.writerow(dict(case_name=names[c], task_id=tid, epoch=ep, src_leaf=a, dst_leaf=b, tx_bytes=size,
                                    packet_count=packets, first_tx_us=micro(first), last_tx_us=micro(last), source=source,
                                    path_semantics="observed_ingress_egress_leaf_pair_not_full_packet_path"))
            else:
                links.writerow(dict(case_name=names[c], task_id=tid, epoch=ep, link_kind=kind,
                                    src_node=a, src_port=ap, dst_node=b if b >= 0 else None, dst_port=bp if bp >= 0 else None,
                                    tx_bytes=size, packet_count=packets, first_tx_us=micro(first), last_tx_us=micro(last),
                                    source=source, peer_mapping_available=int(b >= 0)))


def quantiles(con, sql, params, count):
    if not count:
        return {"p50": None, "p95": None, "p99": None, "max": None}
    ranks = {name: (count - 1) * q for name, q in [("p50", .5), ("p95", .95), ("p99", .99), ("max", 1)]}
    needed = sorted({i for rank in ranks.values() for i in [math.floor(rank), math.ceil(rank)]})
    # sql already orders its first column. ROW_NUMBER counts inside SQLite and
    # only seven requested numeric values cross the Python/SQLite boundary.
    query = "WITH ordered(value) AS (%s), ranked AS (SELECT value,ROW_NUMBER() OVER (ORDER BY value)-1 AS i FROM ordered) SELECT i,value FROM ranked WHERE i IN (%s)" % (sql.rsplit(" ORDER BY ", 1)[0], ",".join("?" for _ in needed))
    selected = dict(con.execute(query, tuple(params) + tuple(needed)))
    return {name: selected[math.floor(rank)] * (1 - (rank % 1)) + selected[math.ceil(rank)] * (rank % 1)
            for name, rank in ranks.items()}


def group_summary(con, case, where="1", params=()):
    args = (case.id,) + tuple(params)
    predicate = "case_id=? AND status='paired_completed' AND " + where
    count, bmean, tmean, dmean = con.execute("SELECT COUNT(*),AVG(base_ns),AVG(toe_ns),AVG(delta_ns) FROM comparisons WHERE " + predicate, args).fetchone()
    result = dict(paired_tasks=count, mean_fct_us_ecmp=micro(bmean), mean_fct_us_toe=micro(tmean), mean_delta_fct_us=micro(dmean),
                  mean_fct_improvement_pct=((bmean - tmean) / bmean * 100 if bmean and tmean is not None else None))
    for column, suffix in [("base_ns", "ecmp"), ("toe_ns", "toe")]:
        qs = quantiles(con, "SELECT %s FROM comparisons WHERE %s ORDER BY %s" % (column, predicate, column), args, count)
        for name, value in qs.items():
            result[name + "_fct_us_" + suffix] = micro(value)
    return result


def accumulated_group_summary(accumulator, label):
    if label == "all":
        key = "all", "all"
    elif label in ("small", "large"):
        key = "size_group", label
    elif label.startswith("op:"):
        key = "op_type", label[3:]
    elif label.startswith("capacity:"):
        key = "capacity_effect", label[9:]
    else:
        key = "pause_exposure", label
    stats = accumulator.focus_stats.get(key)
    if stats is None and accumulator.skipped_groups.get("focus_" + key[0] + "_merged_to_OVERFLOW"):
        return None  # The explicit focus-group limit must not truncate legacy statistics.
    stats = stats or FocusStats()
    agg, n = stats.agg, stats.agg.n
    result = dict(paired_tasks=n, mean_fct_us_ecmp=agg.base/n if n else None,
        mean_fct_us_toe=agg.toe/n if n else None, mean_delta_fct_us=agg.delta/n if n else None,
        mean_fct_improvement_pct=-agg.delta/agg.base*100 if agg.base else None,
        improved_tasks=agg.improved, regressed_tasks=agg.regressed, unchanged_tasks=agg.unchanged)
    for side in ("ecmp", "toe"):
        for q in ("p50", "p95", "p99", "max"):
            result[q + "_fct_us_" + side] = stats.qs.get(q + "_fct_us_" + side)
    return result


def write_group_summary(con, cases, output, args, accumulators=None):
    fields = ["case_name", "group", "paired_tasks", "mean_fct_us_ecmp", "mean_fct_us_toe", "mean_delta_fct_us", "mean_fct_improvement_pct"]
    for name in ("p50", "p95", "p99", "max"):
        fields += [name + "_fct_us_ecmp", name + "_fct_us_toe"]
    fields += ["improved_tasks", "regressed_tasks", "unchanged_tasks"]
    all_summaries = {}
    with writer(output / "fct_group_summary.csv", fields) as w:
        for case in cases:
            ops = [r[0] for r in con.execute("SELECT DISTINCT op FROM comparisons WHERE case_id=?", (case.id,))]
            groups = [("all", "1", ()), ("small", "size_group='small'", ()), ("large", "size_group='large'", ()),
                      ("pause_time_overlap", "pause_overlap_ns>0", ()), ("no_pause_time_overlap", "pause_overlap_ns=0", ()),
                      ("directly_paused_observed", "direct_pause_ns>0", ())]
            groups += [("op:" + op, "op=?", (op,)) for op in ops]
            groups += [("capacity:" + group, "capacity_effect=?", (group,)) for group in ("increased", "decreased", "mixed", "unchanged", "unknown")]
            for label, where, params in groups:
                result = accumulated_group_summary(accumulators[case.name], label) if accumulators is not None else None
                if result is None:
                    result = group_summary(con, case, where, params)
                    pred = "case_id=? AND status='paired_completed' AND " + where
                    epsilon = ns(args.epsilon_us)
                    improved, regressed, unchanged = con.execute("SELECT SUM(delta_ns<?),SUM(delta_ns>?),SUM(ABS(delta_ns)<=?) FROM comparisons WHERE " + pred,
                        (-epsilon, epsilon, epsilon, case.id) + params).fetchone()
                    result.update(improved_tasks=improved or 0, regressed_tasks=regressed or 0, unchanged_tasks=unchanged or 0)
                result.update(case_name=case.name, group=label)
                w.writerow(result)
                if label == "all":
                    all_summaries[case.id] = result
                    logging.info("case=%s mean_fct_improvement_pct=%s paired_tasks=%d", case.name, result["mean_fct_improvement_pct"], result["paired_tasks"])
    return all_summaries


def is_reconfiguration(ev):
    if ev.applied is None:
        return False
    if ev.changed is not None:
        return ev.changed > 0 or bool(ev.delta)
    if ev.before is not None and ev.after is not None:
        return bool(ev.delta) or bool(ev.reconnected)
    return ev.summary_applied


def write_events(cases, output, args):
    event_fields = ["case_name", "epoch", "event_status", "solved_seconds", "draining_seconds", "applied_seconds",
                    "pause_start_seconds", "resume_seconds", "pause_seconds", "drain_seconds", "pause_scope",
                    "paused_tp_count", "unpaused_tx_bytes", "reported_changed_circuits", "removed_circuits",
                    "added_circuits", "changed_leaf_pairs", "preserved_circuits", "reconnected_circuits",
                    "returned_to_epoch", "target_lifetime_seconds", "min_mlu", "applied_mlu_bound", "current_mlu",
                    "solver_optimal", "capacity_snapshot_source", "degree_delta_max"]
    change_fields = ["case_name", "epoch", "applied_seconds", "leaf_a", "leaf_b", "circuits_before", "circuits_after",
                     "delta_circuits", "capacity_before_gbps", "capacity_after_gbps", "delta_capacity_gbps",
                     "previous_pair_change_epoch", "seconds_since_previous_pair_change", "reversal_circuits",
                     "rapid_reversal", "capacity_snapshot_source"]
    with writer(output / "reconfiguration_events.csv", event_fields) as ew, \
         writer(output / "topology_changes.csv", change_fields) as cw:
        for case in cases:
            previous_changes = {}
            total_moved, reversals, rapid = 0, 0, 0
            actual = [e for e in case.events.values() if is_reconfiguration(e)]
            actual.sort(key=lambda e: e.applied)
            next_apply = {e.epoch: actual[i + 1].applied if i + 1 < len(actual) else case.duration_ns
                          for i, e in enumerate(actual)}
            for ev in sorted(case.events.values(), key=lambda e: e.epoch):
                removed = sum(-v for v in ev.delta.values() if v < 0) if ev.before is not None and ev.after is not None else None
                added = sum(v for v in ev.delta.values() if v > 0) if ev.before is not None and ev.after is not None else None
                total_moved += removed or 0
                deg_before, deg_after = defaultdict(int), defaultdict(int)
                for snapshot, degrees in [(ev.before, deg_before), (ev.after, deg_after)]:
                    for (a, b), n in (snapshot or {}).items():
                        degrees[a] += n; degrees[b] += n
                degree_delta = max((abs(deg_after[n] - deg_before[n]) for n in set(deg_before) | set(deg_after)), default=0)
                if ev.before is not None and ev.after is not None and degree_delta:
                    warn(case, "epoch=%d leaf degree changed by up to %d; check topology log completeness/format" % (ev.epoch, degree_delta))
                event_status = ("applied" if ev.applied is not None and ev in actual else
                                "applied_change_unknown" if ev.applied is not None and ev.before is None and ev.changed is None else
                                "no_change" if ev.changed == 0 else "draining_not_applied" if ev.draining is not None else "solved_only")
                lifetime = (next_apply.get(ev.epoch) - ev.applied if ev.epoch in next_apply and next_apply[ev.epoch] is not None else None)
                ew.writerow(dict(case_name=case.name, epoch=ev.epoch, event_status=event_status,
                    solved_seconds=seconds(ev.solved), draining_seconds=seconds(ev.draining), applied_seconds=seconds(ev.applied),
                    pause_start_seconds=seconds(ev.pause_start), resume_seconds=seconds(ev.pause_end),
                    pause_seconds=seconds(ev.pause_ns if ev.pause_ns is not None else
                                          ev.pause_end - ev.pause_start if ev.pause_end is not None and ev.pause_start is not None else None),
                    drain_seconds=seconds(ev.drain_ns), pause_scope=ev.scope, paused_tp_count=ev.paused_tps,
                    unpaused_tx_bytes=ev.unpaused_bytes, reported_changed_circuits=ev.changed,
                    removed_circuits=removed, added_circuits=added, changed_leaf_pairs=len(ev.delta) if ev.before is not None else None,
                    preserved_circuits=ev.preserved, reconnected_circuits=ev.reconnected, returned_to_epoch=ev.returned_to_epoch,
                    target_lifetime_seconds=seconds(lifetime), min_mlu=ev.min_mlu, applied_mlu_bound=ev.mlu_bound,
                    current_mlu=ev.current_mlu, solver_optimal=ev.optimal,
                    capacity_snapshot_source=ev.circuit_source, degree_delta_max=degree_delta if ev.before is not None and ev.after is not None else None))
                for pair, delta in sorted(ev.delta.items()):
                    previous = previous_changes.get(pair)
                    reverse = min(abs(delta), abs(previous[2])) if previous and delta * previous[2] < 0 else 0
                    elapsed = ev.applied - previous[1] if previous and ev.applied is not None else None
                    is_rapid = int(reverse > 0 and elapsed is not None and elapsed <= ns(args.reversal_horizon_seconds, "s"))
                    reversals += reverse; rapid += is_rapid
                    cw.writerow(dict(case_name=case.name, epoch=ev.epoch, applied_seconds=seconds(ev.applied),
                        leaf_a=pair[0], leaf_b=pair[1], circuits_before=ev.before.get(pair, 0), circuits_after=ev.after.get(pair, 0),
                        delta_circuits=delta, capacity_before_gbps=ev.before.get(pair, 0) * args.circuit_gbps,
                        capacity_after_gbps=ev.after.get(pair, 0) * args.circuit_gbps, delta_capacity_gbps=delta * args.circuit_gbps,
                        previous_pair_change_epoch=previous[0] if previous else None,
                        seconds_since_previous_pair_change=seconds(elapsed), reversal_circuits=reverse,
                        rapid_reversal=is_rapid, capacity_snapshot_source=ev.circuit_source))
                    if ev.applied is not None:
                        previous_changes[pair] = (ev.epoch, ev.applied, delta)
            pauses = merged_intervals([(e.pause_start, e.pause_end) for e in case.events.values()
                                      if e.pause_start is not None and e.pause_end is not None])
            solved = sorted(e.solved for e in case.events.values() if e.solved is not None)
            has_timeline = case.capabilities.get("toe_summary") or bool(case.events)
            case.capabilities.update(decision_count=len(solved) if case.capabilities.get("toe_summary") else None,
                actual_reconfiguration_count=len(actual) if case.capabilities.get("reconfiguration_count_complete") else None,
                pending_draining_count=sum(e.draining is not None and e.applied is None for e in case.events.values()) if has_timeline else None,
                pause_timeline_union_seconds=sum(b - a for a, b in pauses) / 1e9 if pauses or case.capabilities.get("pause_timeline_complete") else None,
                removed_circuits_total=total_moved if case.capabilities.get("capacity_snapshots") else None,
                reversal_circuits_total=reversals if case.capabilities.get("capacity_snapshots") else None,
                rapid_pair_reversal_count=rapid if case.capabilities.get("capacity_snapshots") else None,
                decision_interval_mean_seconds=((solved[-1] - solved[0]) / (len(solved) - 1) / 1e9 if len(solved) > 1 else None))


def write_event_cohorts(con, cases, zones, output):
    fields = ["case_name", "epoch", "cohort", "cohort_start_us", "cohort_end_us", "paired_tasks",
              "mean_fct_us_ecmp", "mean_fct_us_toe", "mean_fct_improvement_pct", "mean_delta_fct_us",
              "pause_overlap_task_count", "directly_paused_observed_task_count", "scope_note"]
    with writer(output / "event_task_cohorts.csv", fields) as w:
        for case in cases:
            for ev in sorted(case.events.values(), key=lambda e: e.epoch):
                for phase in ("pre", "pause", "recovery", "post"):
                    interval = zones.lookup.get((case.id, ev.epoch, phase))
                    if interval is None:
                        continue
                    start, end = interval
                    count, bm, tm, dm, po, dp = con.execute("""SELECT COUNT(*),AVG(base_ns),AVG(toe_ns),AVG(delta_ns),
                      SUM(pause_overlap_ns>0),SUM(direct_pause_ns>0) FROM comparisons
                      WHERE case_id=? AND status='paired_completed' AND start_ns>=? AND start_ns<?""", (case.id, start, end)).fetchone()
                    w.writerow(dict(case_name=case.name, epoch=ev.epoch, cohort=phase,
                        cohort_start_us=micro(start), cohort_end_us=micro(end), paired_tasks=count,
                        mean_fct_us_ecmp=micro(bm), mean_fct_us_toe=micro(tm), mean_delta_fct_us=micro(dm),
                        mean_fct_improvement_pct=(bm - tm) / bm * 100 if bm else None,
                        pause_overlap_task_count=po or 0, directly_paused_observed_task_count=dp or 0,
                        scope_note="all_tasks_started_in_toe_interval; temporal_exposure_not_causal_cost"))


def available_metric(case, kind, a, b, interval):
    if kind not in case.metric_pairs or interval is None or interval[1] <= interval[0]:
        return False
    pairs = case.metric_pairs[kind]
    if pairs is not None and (a, b) not in pairs:
        return False
    if kind == "physical" and not case.is_baseline:
        start, end = interval
        if case.first_apply is None:
            if not case.capabilities.get("toe_summary"):
                return False
        elif end > case.first_apply:
            for t in [max(start, case.first_apply), end - 1] + [t for t in case.snapshot_times if start <= t < end]:
                i = bisect.bisect_right(case.physical_times, t)
                j = bisect.bisect_right(case.snapshot_times, t)
                if not i or (j and case.physical_times[i - 1] < case.snapshot_times[j - 1]):
                    return False
    if kind == "physical" and pairs is None:
        # Having some PortTrace files does not make an unrecorded lane zero.
        start, end = interval
        times = [start, end - 1] + [t for t in case.snapshot_times if start <= t < end]
        for t in times:
            if case.is_baseline or case.first_apply is None or t < case.first_apply:
                mapping = case.links
            else:
                i = bisect.bisect_right(case.physical_times, t)
                if not i:
                    return False
                mapping = case.physical[i - 1]
            expected = [key for key, peer in mapping.items() if key[0] == a and peer[0] == b]
            if any(key not in case.port_files for key in expected):
                return False
    return True


def capacity_integral(case, pair, interval, args):
    if case.initial is None or interval is None:
        return None
    start, end = interval
    boundaries = [start] + [t for t in case.snapshot_times if start < t < end] + [end]
    total = 0
    for a, b in zip(boundaries, boundaries[1:]):
        snapshot = case.snapshots.get(case.epoch_at(a))
        if snapshot is None:
            return None
        total += (b - a) * snapshot.get(tuple(sorted(pair)), 0) * args.circuit_gbps
    return total


def hot_rate(con, observed, target, epoch, a, b, phase, kind, zones, args):
    interval = zones.lookup.get((target.id, epoch, phase))
    if not available_metric(observed, kind, a, b, interval):
        return None, None, None
    start, end = interval
    width = observed.capabilities.get(kind + "_metrics_window_us")
    if width:
        width = ns(width)
        # Only complete bins are included; denominator must use the same bins.
        start = ((start + width - 1) // width) * width
        end = (end // width) * width
        if end <= start:
            return None, None, None
    size = con.execute("SELECT bytes FROM hot WHERE case_id=? AND target_case=? AND epoch=? AND a=? AND b=? AND phase=? AND kind=?",
                       (observed.id, target.id, epoch, a, b, phase, kind)).fetchone()
    num_bytes = size[0] if size else 0
    rate = num_bytes * 8 / (end - start)
    integral = capacity_integral(observed, (a, b), (start, end), args) if kind == "physical" else None
    utilization = num_bytes * 8 / integral if integral and integral > 0 else None
    return rate, utilization, num_bytes


def sample_stat(con, case, target, epoch, a, b, phase, metric):
    sql = "SELECT value FROM samples WHERE case_id=? AND target_case=? AND epoch=? AND a=? AND b=? AND phase=? AND metric=?"
    params = (case.id, target.id, epoch, a, b, phase, metric)
    count, mean = con.execute("SELECT COUNT(*),AVG(value) FROM (" + sql + ")", params).fetchone()
    values = quantiles(con, sql + " ORDER BY value", params, count)
    return count, mean, values["p95"]


def write_hotspots(con, baseline, cases, zones, output, args):
    fields = ["case_name", "epoch", "src_leaf", "dst_leaf", "circuits_before", "circuits_after", "delta_circuits",
              "pre_logical_gbps", "post_logical_gbps", "pre_logical_gbps_ecmp", "post_logical_gbps_ecmp",
              "pre_physical_gbps", "post_physical_gbps", "pre_physical_utilization", "post_physical_utilization",
              "pre_queue_sample_count", "post_queue_sample_count", "pre_queue_bytes_p95", "post_queue_bytes_p95",
              "pre_backlog_bytes_mean", "post_backlog_bytes_mean", "pre_source_utilization_mean", "pre_destination_utilization_mean",
              "predicted_demand_gbps_mean", "post_paired_tasks", "post_mean_fct_us_ecmp", "post_mean_fct_us_toe",
              "post_mean_fct_improvement_pct", "conclusion", "scope_note"]
    with writer(output / "hotspot_effect_summary.csv", fields) as w:
        for case in cases:
            for ev in sorted(case.events.values(), key=lambda e: e.epoch):
                for pair, delta in sorted(ev.delta.items()):
                    for a, b in [pair, pair[::-1]]:
                        row = dict(case_name=case.name, epoch=ev.epoch, src_leaf=a, dst_leaf=b,
                                   circuits_before=ev.before.get(pair, 0), circuits_after=ev.after.get(pair, 0), delta_circuits=delta)
                        for phase in ("pre", "post"):
                            logical, _, _ = hot_rate(con, case, case, ev.epoch, a, b, phase, "logical", zones, args)
                            base, _, _ = hot_rate(con, baseline, case, ev.epoch, a, b, phase, "logical", zones, args)
                            physical, util, _ = hot_rate(con, case, case, ev.epoch, a, b, phase, "physical", zones, args)
                            count, _, q95 = sample_stat(con, case, case, ev.epoch, a, b, phase, "queue_bytes")
                            _, backlog, _ = sample_stat(con, case, case, ev.epoch, a, b, phase, "backlog_bytes")
                            row.update({phase + "_logical_gbps": logical, phase + "_logical_gbps_ecmp": base,
                                        phase + "_physical_gbps": physical, phase + "_physical_utilization": util,
                                        phase + "_queue_sample_count": count, phase + "_queue_bytes_p95": q95,
                                        phase + "_backlog_bytes_mean": backlog})
                        for metric in ("source_utilization", "destination_utilization", "predicted_demand_gbps"):
                            _, avg, _ = sample_stat(con, case, case, ev.epoch, a, b, "pre", metric)
                            row[("predicted_demand_gbps_mean" if metric == "predicted_demand_gbps" else "pre_" + metric + "_mean")] = avg
                        interval = zones.lookup.get((case.id, ev.epoch, "post"))
                        n, bm, tm = 0, None, None
                        if interval is not None:
                            n, bm, tm = con.execute("""SELECT COUNT(*),AVG(c.base_ns),AVG(c.toe_ns) FROM comparisons c
                              WHERE c.case_id=? AND c.status='paired_completed' AND c.start_ns>=? AND c.start_ns<?
                              AND EXISTS (SELECT 1 FROM obs o WHERE o.case_id=c.case_id AND o.tid=c.tid
                               AND o.kind='L1_PAIR' AND o.a=? AND o.b=?)""", (case.id, interval[0], interval[1], a, b)).fetchone()
                        gain = (bm - tm) / bm * 100 if bm else None
                        row.update(post_paired_tasks=n, post_mean_fct_us_ecmp=micro(bm), post_mean_fct_us_toe=micro(tm),
                                   post_mean_fct_improvement_pct=gain)
                        preq, postq = row["pre_queue_bytes_p95"], row["post_queue_bytes_p95"]
                        if preq is not None and postq is not None and gain is not None:
                            conclusion = ("evidence_consistent_with_hotspot_relief" if delta > 0 and preq > 0 and postq < preq and gain > 0 else
                                          "possible_new_hotspot_after_capacity_removal" if delta < 0 and postq > preq and gain < 0 else
                                          "mixed_queue_and_task_evidence")
                        elif gain is not None:
                            conclusion = "partial_evidence_tasks_improved" if gain > 0 else "partial_evidence_tasks_regressed" if gain < 0 else "partial_evidence_tasks_unchanged"
                        else:
                            conclusion = "insufficient_evidence"
                        row.update(conclusion=conclusion, scope_note="logical=delivered_wire_bytes; physical=direct_link_bytes; post_tasks_selected_by_ToE_start_and_observed_pair; no_causal_claim")
                        w.writerow(row)


def write_run_summary(con, baseline, cases, summaries, output, args):
    fields = ["case_name", "baseline_name", "planned_tasks", "statistics_rows", "completed_tasks", "completion_rate",
              "paired_tasks", "mean_fct_us_ecmp", "mean_fct_us_toe", "mean_fct_improvement_pct", "p50_fct_us_toe", "p95_fct_us_toe", "p99_fct_us_toe",
              "decision_count", "actual_reconfiguration_count", "pending_draining_count", "decision_interval_mean_seconds",
              "pause_timeline_union_seconds", "removed_circuits_total", "reversal_circuits_total", "rapid_pair_reversal_count",
              "last_task_finish_seconds", "task_activity_span_seconds", "traffic_sha256_equal", "initial_topology_sha256_equal",
              "path_observed_task_count_ecmp", "path_observed_task_count_toe", "direct_task_pause_available", "pause_timeline_complete", "ideal_plan_verified"]
    fields += ["ideal_paired_tasks", "ideal_mean_topology_gain_us", "ideal_mean_execution_cost_us"]
    with writer(output / "run_comparison_summary.csv", fields) as w:
        for case in cases:
            planned, complete, earliest, latest = con.execute("""SELECT COUNT(*),SUM(start_ns IS NOT NULL AND finish_ns>=start_ns),MIN(start_ns),MAX(finish_ns)
              FROM tasks WHERE case_id=?""", (case.id,)).fetchone()
            bpaths = con.execute("SELECT COUNT(*) FROM features WHERE case_id=?", (baseline.id,)).fetchone()[0]
            tpaths = con.execute("SELECT COUNT(*) FROM features WHERE case_id=?", (case.id,)).fetchone()[0]
            topology_equal = None
            if "topology" in baseline.files and "topology" in case.files:
                topology_equal = int(digest(Path(baseline.files["topology"])) == digest(Path(case.files["topology"])))
                if not topology_equal:
                    warn(case, "initial topology files differ from ECMP; total FCT difference also includes initial-topology/routing differences")
            traffic_equal = None
            if baseline.capabilities.get("traffic_sha256") and case.capabilities.get("traffic_sha256"):
                traffic_equal = int(baseline.capabilities["traffic_sha256"] == case.capabilities["traffic_sha256"])
            row = dict(summaries[case.id])
            row.update(case_name=case.name, baseline_name=baseline.name, planned_tasks=planned,
                statistics_rows=case.capabilities["task_statistics_rows"], completed_tasks=complete or 0,
                completion_rate=(complete or 0) / planned if planned else None,
                last_task_finish_seconds=seconds(latest), task_activity_span_seconds=seconds(latest - earliest) if latest is not None and earliest is not None else None,
                traffic_sha256_equal=traffic_equal, initial_topology_sha256_equal=topology_equal,
                path_observed_task_count_ecmp=bpaths, path_observed_task_count_toe=tpaths,
                direct_task_pause_available=int(case.capabilities.get("direct_task_pause", False)),
                **{k: case.capabilities.get(k) for k in fields if k in case.capabilities})
            w.writerow(row)


# Exact solver-demand matrices and dependency-free SVG heatmaps.
def demand_override(args, source_label, case_name):
    matches = [Path(path).resolve() for target, path in args.demand_file or []
               if target in (case_name, source_label + "/" + case_name)]
    if len(matches) > 1:
        raise ValueError("duplicate --demand-file for " + case_name)
    if matches and not matches[0].is_file():
        raise ValueError("demand matrix file not found: " + str(matches[0]))
    return matches[0] if matches else None


def read_demand_file(path, case_name):
    matrices = {}
    leaves = tuple(range(37, 57))
    for record, row in enumerate(csv_rows(path), 2):
        if row.get("case_name") and row.get("case_name") != case_name:
            continue
        ep = row.integer("epoch")
        a, b = row.integer("src_leaf", "leaf_a"), row.integer("dst_leaf", "leaf_b")
        value, bps = row.number("demand_gbps"), row.number("demand_bps")
        if value is None and bps is not None:
            value = bps / 1e9
        if ep is None or ep < 0 or a not in leaves or b not in leaves or value is None or value < 0:
            raise ValueError("%s row %d: require epoch, Leaf IDs 37..56, and nonnegative demand_bps/demand_gbps" % (path, record))
        if bps is not None and (bps < 0 or not math.isclose(bps / 1e9, value, rel_tol=1e-9, abs_tol=1e-12)):
            raise ValueError("%s row %d: demand_bps and demand_gbps disagree" % (path, record))
        stamp = row.number("decision_seconds", "time_seconds")
        complete = row.integer("matrix_complete", default=0)
        if complete not in (0, 1):
            raise ValueError("%s row %d: matrix_complete must be 0 or 1" % (path, record))
        metadata = (stamp, complete, row.get("matrix_kind", default="provided_matrix"),
                    row.number("sample_window_seconds"), row.integer("history_windows"), row.integer("completed_window"))
        matrix = matrices.setdefault(ep, dict(metadata=metadata, cells={}, latest={}, peaks={}, latest_bytes={}, source_file=str(path)))
        if matrix["metadata"] != metadata:
            raise ValueError("%s epoch %d: inconsistent decision metadata" % (path, ep))
        pair = a, b
        if pair in matrix["cells"] and matrix["cells"][pair] != value:
            raise ValueError("%s epoch %d: conflicting duplicate pair %s" % (path, ep, pair))
        matrix["cells"][pair] = value
        for key, column in (("latest", "latest_gbps"), ("peaks", "peak_window_bytes"), ("latest_bytes", "latest_window_bytes")):
            item = row.number(column) if key == "latest" else row.integer(column)
            if item is not None and item < 0:
                raise ValueError("%s row %d: negative %s" % (path, record, column))
            if pair in matrix[key] and matrix[key][pair] != item:
                raise ValueError("%s epoch %d: conflicting duplicate %s" % (path, ep, column))
            matrix[key][pair] = item
    for ep, matrix in matrices.items():
        if matrix["metadata"][1] and len(matrix["cells"]) != len(leaves)**2:
            raise ValueError("%s epoch %d: matrix_complete=1 requires all 400 cells; found %d" % (path, ep, len(matrix["cells"])))
        matrix["coverage"] = "complete" if len(matrix["cells"]) == len(leaves)**2 else "partial_missing_cells"
    return matrices


def role_demand(cells):
    result = {}
    for src in ("P", "D", "S"):
        for dst in ("P", "D", "S"):
            values = [cells.get((a, b)) for a in range(37,57) for b in range(37,57)
                      if leaf_role(a) == src and leaf_role(b) == dst]
            result[src, dst] = math.fsum(values) if all(v is not None for v in values) else None
    return result


def heatmap_svg(cells, labels, title, subtitle, vmax, signed=False):
    from html import escape
    import textwrap
    count = len(labels)
    size = 25 if count > 3 else 84
    left = 78
    side = count * size
    width = left + side + 120
    title_lines = textwrap.wrap(title, max(24, int((width-32)/8))) or [""]
    subtitle_lines = textwrap.wrap(subtitle, max(35, int((width-32)/5.2))) or [""]
    footer_lines = textwrap.wrap("Linear scale shared across displayed epochs; hatched cells = NA; zero is measured/logged zero.", max(35, int((width-32)/5.2)))
    heading_end = 15 + len(title_lines)*19 + len(subtitle_lines)*13
    top = heading_end+58
    height = top+side+35+13*len(footer_lines)
    vmax = max(0, vmax)
    def color(value):
        if value is None:
            return "url(#missing)"
        ratio = min(1.0, abs(value)/vmax) if vmax else 0
        start = (248,250,252)
        end = (185,28,28) if signed and value < 0 else (29,78,216)
        return "#" + "".join("%02x" % round(a+(b-a)*ratio) for a,b in zip(start,end))
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (width,height,width,height),
        '<rect width="100%%" height="100%%" fill="white"/>'.replace('%%','%'),
        '<defs><pattern id="missing" width="6" height="6" patternUnits="userSpaceOnUse"><rect width="6" height="6" fill="#e5e7eb"/><path d="M0,6 L6,0" stroke="#9ca3af"/></pattern></defs>',
        '<g font-family="Arial,DejaVu Sans,sans-serif" fill="#111827">']
    for i,line in enumerate(title_lines):
        svg.append('<text x="16" y="%d" font-size="16" font-weight="bold">%s</text>' % (25+i*19,escape(line)))
    for i,line in enumerate(subtitle_lines):
        svg.append('<text x="16" y="%d" font-size="10">%s</text>' % (25+len(title_lines)*19+i*13,escape(line)))
    svg += ['<text x="%d" y="%d" font-size="11">Destination →</text>' % (left,top-35),
            '<text x="8" y="%d" font-size="11">Source ↓</text>' % (top-15)]
    for i, label in enumerate(labels):
        x, y = left + i*size, top + i*size
        svg.append('<text x="%d" y="%d" text-anchor="end" font-size="10">%s</text>' % (left-7,y+size*.62,escape(str(label))))
        svg.append('<text transform="translate(%g,%g) rotate(-55)" font-size="10">%s</text>' % (x+size*.4,top-8,escape(str(label))))
    for i, a in enumerate(labels):
        for j, b in enumerate(labels):
            value = cells.get((a,b))
            caption = "NA (missing observation)" if value is None else format(value, ".8g") + " Gbps"
            svg.append('<rect x="%g" y="%g" width="%g" height="%g" fill="%s" stroke="#d1d5db" stroke-width=".4"><title>%s → %s: %s</title></rect>' %
                       (left+j*size,top+i*size,size,size,color(value),escape(str(a)),escape(str(b)),escape(caption)))
            if count <= 3:
                svg.append('<text x="%g" y="%g" text-anchor="middle" font-size="12" fill="%s">%s</text>' %
                    (left+(j+.5)*size,top+(i+.55)*size,"white" if value is not None and vmax and abs(value)/vmax>.55 else "#111827",escape("NA" if value is None else format(value,".3g"))))
    if count == 20:
        for boundary in (12,16):
            offset=boundary*size
            svg.append('<path d="M%g,%g V%g M%g,%g H%g" stroke="#374151" stroke-width="2" fill="none"/>' %
                       (left+offset,top,top+side,left,top+offset,left+side))
    legend_x = left+side+24
    for i in range(101):
        value = vmax*(1-2*i/100) if signed else vmax*(1-i/100)
        svg.append('<rect x="%d" y="%g" width="15" height="2.3" fill="%s"/>' % (legend_x,top+i*2,color(value)))
    for pos, value in ((0,vmax),(100,0 if not signed else -vmax)):
        svg.append('<text x="%d" y="%d" font-size="10">%s</text>' % (legend_x+20,top+pos*2+5,escape(format(value,".3g"))))
    if signed:
        svg.append('<text x="%d" y="%d" font-size="10">0</text>' % (legend_x+20,top+105))
    svg.append('<text x="%d" y="%d" font-size="10">Gbps</text>' % (legend_x,top-10))
    for i,line in enumerate(footer_lines):
        svg.append('<text x="16" y="%d" font-size="10">%s</text>' % (top+side+24+i*13,escape(line)))
    svg.append('</g></svg>')
    return "\n".join(svg)


def write_demand_outputs(sources, args, directory):
    from html import escape
    directory.mkdir(parents=True,exist_ok=True)
    views, summaries, normalized = [], [], []
    leaves = tuple(range(37,57))
    provided_targets = {case.name for source in sources for case in source.cases.values()} | {source.label+"/"+case.name for source in sources for case in source.cases.values()}
    if any(target not in provided_targets for target,path in args.demand_file or []):
        raise ValueError("--demand-file CASE does not match input cases")
    for source in sources:
        for case in source.cases.values():
            explicit = demand_override(args,source.label,case.name)
            if hasattr(source,"demand_cases"):
                matrices = source.demand_cases.get(case.name,{})
            elif explicit:
                matrices = read_demand_file(explicit,case.name)
            else:
                stored = source.path/"demand_matrices.csv"
                raw = next((c.get("files",{}).get("toe_demand") for c in source.meta.get("cases",[]) if c.get("name")==case.name),None)
                path = stored if stored.is_file() else Path(raw) if raw and Path(raw).is_file() else None
                matrices = read_demand_file(path,case.name) if path else {}
            events = case.topology_expected
            epochs = sorted(set(matrices) | {int(ep) for ep,event in events.items() if event.get("event_status") in ("applied","applied_change_unknown")})
            previous = None
            for ep in epochs:
                matrix = matrices.get(ep)
                event = events.get(str(ep),{})
                status = event.get("event_status","unknown")
                stamp, complete, kind, window, history, completed = matrix["metadata"] if matrix else (None,0,"unavailable",None,None,None)
                solved = number(event.get("solved_seconds"))
                if solved is not None and stamp is not None and not math.isclose(solved,stamp,rel_tol=1e-9,abs_tol=1e-6):
                    raise ValueError("case=%s epoch=%d: demand decision time %s differs from solved time %s" % (case.name,ep,stamp,solved))
                cells = matrix["cells"] if matrix else {}
                maximum = max(cells,key=cells.get) if cells else None
                row = dict(source_label=source.label,case_name=case.name,epoch=ep,decision_seconds=stamp,
                    applied_seconds=event.get("applied_seconds"),event_status=status,
                    is_reconfiguration=1 if status=="applied" else 0 if status=="no_change" else None,
                    coverage=matrix["coverage"] if matrix else "missing_solver_demand_log",matrix_kind=kind,
                    known_cells=len(cells),expected_cells=400,total_known_demand_gbps=math.fsum(cells.values()) if matrix else None,
                    max_src_leaf=maximum[0] if maximum else None,max_dst_leaf=maximum[1] if maximum else None,
                    max_pair_demand_gbps=cells[maximum] if maximum else None,sample_window_seconds=window,history_windows=history,
                    source_file=matrix["source_file"] if matrix else "",demand_svg="",role_demand_svg="",demand_delta_svg="",delta_reference_epoch="")
                summaries.append(row)
                if matrix:
                    for (a,b), value in sorted(cells.items()):
                        normalized.append(dict(source_label=source.label,case_name=case.name,epoch=ep,decision_seconds=stamp,
                            src_leaf=a,dst_leaf=b,src_role=leaf_role(a),dst_role=leaf_role(b),demand_bps=value*1e9,demand_gbps=value,
                            latest_gbps=matrix["latest"].get((a,b)),peak_window_bytes=matrix["peaks"].get((a,b)),latest_window_bytes=matrix["latest_bytes"].get((a,b)),
                            sample_window_seconds=window,history_windows=history,completed_window=completed,matrix_complete=complete,matrix_kind=kind))
                selected = args.demand_heatmaps=="all" or (args.demand_heatmaps=="changes" and status in ("applied","applied_change_unknown"))
                if selected:
                    delta = {(a,b): cells[a,b]-previous[1][a,b] if (a,b) in cells and (a,b) in previous[1] else None for a in leaves for b in leaves} if previous else None
                    views.append((source,case,row,cells,delta))
                    if previous:
                        row["delta_reference_epoch"] = previous[0]
                    # An absent matrix at an intervening event breaks comparison continuity.
                    previous = (ep,cells) if matrix else None
    asset_dir = directory/"demand_heatmaps"
    asset_dir.mkdir(exist_ok=True)
    vmax = max((v for source,case,row,cells,delta in views for v in cells.values()),default=0)
    group_max = max((v for source,case,row,cells,delta in views if cells for v in role_demand(cells).values() if v is not None),default=0)
    delta_max = max((abs(v) for source,case,row,cells,delta in views if delta for v in delta.values() if v is not None),default=0)
    sections=[]
    for source,case,row,cells,delta in views:
        title = "%s / %s / epoch %s" % (source.label,case.name,row["epoch"])
        if not cells:
            sections.append('<section><h2>%s</h2><p>Missing solver-demand log. Exact historical solver inputs cannot be reconstructed from topology changes.</p></section>' % escape(title))
            continue
        token = hashlib.sha256((source.label+"\0"+case.name).encode()).hexdigest()[:12] + "_epoch_%s" % row["epoch"]
        subtitle = "Decision=%ss; applied=%ss; status=%s; %s; history=max over %s windows x %ss" % (fmt(row["decision_seconds"]),fmt(row["applied_seconds"]),row["event_status"],row["coverage"],fmt(row["history_windows"]),fmt(row["sample_window_seconds"]))
        labelled = {(leaf_role(a)+str(a),leaf_role(b)+str(b)):value for (a,b),value in cells.items()}
        labels = [leaf_role(a)+str(a) for a in leaves]
        demand_name=token+"_demand.svg";role_name=token+"_role_demand.svg"
        (asset_dir/demand_name).write_text(heatmap_svg(labelled,labels,title+" — Solver demand",subtitle,vmax),encoding="utf-8")
        (asset_dir/role_name).write_text(heatmap_svg(role_demand(cells),["P","D","S"],title+" — Role demand sum",subtitle,group_max),encoding="utf-8")
        row["demand_svg"]="demand_heatmaps/"+demand_name;row["role_demand_svg"]="demand_heatmaps/"+role_name
        images = '<figure><img src="%s" alt="Solver demand"/><figcaption>Rows: source; columns: destination. Exact values are saved in demand_matrices.csv.</figcaption></figure><figure><img src="%s" alt="Role demand"/></figure>' % (row["demand_svg"],row["role_demand_svg"])
        if delta is not None:
            delta_name=token+"_demand_delta.svg"
            labelled_delta={(leaf_role(a)+str(a),leaf_role(b)+str(b)):v for (a,b),v in delta.items()}
            (asset_dir/delta_name).write_text(heatmap_svg(labelled_delta,labels,title+" — Demand difference", "Current decision minus displayed reference epoch %s; blue=increase, red=decrease" % row["delta_reference_epoch"],delta_max,True),encoding="utf-8")
            row["demand_delta_svg"]="demand_heatmaps/"+delta_name
            images += '<figure><img src="%s" alt="Demand difference"/></figure>' % row["demand_delta_svg"]
        groups=case.topology_group_events.get(str(row["epoch"]),{})
        allocations=[]
        for group in ROLE_GROUPS:
            before=groups.get("before_totals");after=groups.get("after_totals")
            allocations.append('<tr><td>%s</td><td>%s</td><td>%s</td></tr>' % (escape(group),fmt(before.get(group,0)) if before is not None else "NA",fmt(after.get(group,0)) if after is not None else "NA"))
        sections.append('<section><h2>%s</h2><p>%s</p><div class="charts">%s</div><table><tr><th>Undirected circuit group</th><th>Before</th><th>After</th></tr>%s</table></section>' % (escape(title),escape(subtitle),images,"".join(allocations)))
    if args.demand_heatmaps == "off":
        sections=['<p>Heatmap generation is disabled. Numeric matrices and per-epoch coverage are exported as CSV.</p>']
    elif not sections:
        sections=['<p>No selected reconfiguration matrices. Missing toe_demand.csv cannot be replaced with an all-zero matrix. Use --demand-heatmaps all to inspect decision epochs without topology changes.</p>']
    for source in sources:
        source.demand_summary = [row for row in summaries if row["source_label"] == source.label]
    page='<!doctype html><html lang="en"><meta charset="utf-8"><title>ToE solver-demand heatmaps</title><style>body{font-family:Arial,sans-serif;margin:24px;color:#172033;background:#f5f7fb}section{background:white;padding:18px;margin:20px 0;border-radius:8px}.charts{display:flex;flex-wrap:wrap;gap:12px}figure{margin:0;max-width:100%%}img{max-width:100%%;height:auto}table{border-collapse:collapse;margin-top:15px}th,td{border:1px solid #ddd;padding:7px}p,figcaption{font-size:13px}</style><h1>ToE decision demand and circuit allocation</h1><p>Demand is directional solver input, not instantaneous applied-time traffic. Different OD peaks may come from different historical windows. All demand panels share a color scale; role panels and signed differences have their own shared scales. Hatched cells mean unavailable data. SVG files can be imported into PowerPoint.</p>%s</html>' % "\n".join(sections)
    paths=[]
    headers={"demand_matrices.csv":["source_label","case_name","epoch","decision_seconds","src_leaf","dst_leaf","src_role","dst_role","demand_bps","demand_gbps","latest_gbps","peak_window_bytes","latest_window_bytes","sample_window_seconds","history_windows","completed_window","matrix_complete","matrix_kind"],
             "demand_epoch_summary.csv":["source_label","case_name","epoch","decision_seconds","applied_seconds","event_status","is_reconfiguration","coverage","matrix_kind","known_cells","expected_cells","total_known_demand_gbps","max_src_leaf","max_dst_leaf","max_pair_demand_gbps","sample_window_seconds","history_windows","source_file","demand_svg","role_demand_svg","demand_delta_svg","delta_reference_epoch"]}
    for name, records in (("demand_matrices.csv",normalized),("demand_epoch_summary.csv",summaries)):
        path=directory/name
        with writer(path,headers[name]) as output:
            output.writerows(records)
        paths.append(path)
    index=directory/"demand_heatmaps.html";index.write_text(page,encoding="utf-8");paths.append(index)
    return paths


def demand_only_main(argv):
    cli=argparse.ArgumentParser(description="Draw solver-demand matrices without reading task or trace data.")
    cli.add_argument("analysis_dir",type=Path)
    cli.add_argument("--output",type=Path,help="heatmap output directory; default analysis_dir")
    cli.add_argument("--demand-file",nargs=2,action="append",metavar=("CASE","CSV"))
    cli.add_argument("--demand-heatmaps",choices=("changes","all","off"),default="changes")
    opts=cli.parse_args(argv)
    try:
        root=opts.analysis_dir.resolve()
        if not root.is_dir():raise ValueError("analysis directory not found: "+str(root))
        args=parser().parse_args([str(root)])
        args.demand_file=opts.demand_file;args.demand_heatmaps=opts.demand_heatmaps
        source=Source(root,root.name)
        scan_small_files(source,args)
        if not source.cases:
            for entry in source.meta.get("cases",[]):
                if entry.get("name")!=source.meta.get("baseline") and not entry.get("name","").startswith("Ideal:"):
                    source.case(entry["name"],args)
        for path in write_demand_outputs([source],args,(opts.output or root).resolve()):
            print("[OK] demand output: "+str(path))
        return 0
    except (ValueError,OSError,csv.Error,json.JSONDecodeError,InvalidOperation) as exc:
        print("[ERROR] "+str(exc),file=sys.stderr)
        return 2


def build_parser():
    p = argparse.ArgumentParser(description="Analyze and summarize ECMP vs ToE runs (%s), Python standard library only." % ANALYZER_VERSION)
    p.add_argument("--summary-only", action="store_true", help="summarize an existing analysis directory without reading raw traces")
    p.add_argument("--demand-only", action="store_true", help="render demand matrices from existing analysis metadata; reads no tasks or traces")
    p.add_argument("--baseline", "--ecmp-case", type=Path, required=True, help="ECMP case directory; no toe_* files required")
    p.add_argument("--baseline-name", default="ECMP")
    p.add_argument("--case", action="append", nargs=2, metavar=("NAME", "CASE_DIR"), help="repeat for ToE-30/ToE-60/ToE-120")
    p.add_argument("--toe", type=Path, help="single ToE case shorthand; name is ToE")
    p.add_argument("--toe-log-dir", nargs=2, action="append", metavar=("CASE", "DIR"), help="override ToE output directory; default CASE_DIR/toe")
    p.add_argument("--allow-missing-toe-logs", action="store_true", help="explicitly permit FCT-only analysis when ToE summary/topology logs are absent")
    p.add_argument("--ideal-case", action="append", nargs=2, metavar=("TOE_NAME", "IDEAL_DIR"), help="optional ideal-switch replay paired to a ToE case")
    p.add_argument("--output", type=Path, required=True, help="new or empty result directory")
    p.add_argument("--trace-mode", choices=("auto", "raw", "off"), default="auto", help="auto prefers normalized task_path_usage.csv; raw streams PortTrace/L1PairTrace; off skips raw logs")
    p.add_argument("--endpoint-convention", choices=("source-is-npu", "dest-is-npu"), default="source-is-npu", help="traffic source is the NPU request initiator, including READ/LOAD")
    p.add_argument("--epsilon-us", type=float, default=.001, help="FCT equality tolerance; default 1ns")
    p.add_argument("--size-threshold-bytes", type=int, default=336)
    p.add_argument("--event-window-us", type=float, default=1000000, help="pre/post observation duration, default 1s")
    p.add_argument("--recovery-us", type=float, default=100000, help="separate recovery interval, default 100ms")
    p.add_argument("--reversal-horizon-seconds", type=float, default=60)
    p.add_argument("--circuit-gbps", type=float, default=400)
    p.add_argument("--topology-log-format", choices=("full", "delta"), default="full", help="toe_topology snapshots: full allocation or changed-pair updates")
    p.add_argument("--metrics-window-us", type=int, help="select window width in precomputed timeseries; default first available")
    p.add_argument("--allow-traffic-mismatch", action="store_true", help="exclude mismatched task metadata instead of stopping")
    p.add_argument("--keep-db", action="store_true", help="retain SQLite observations for follow-up queries")
    p.add_argument("--summary-output", type=Path, help="summary text path; default OUTPUT/toe_ai_summary_v3.txt")
    return p


@contextmanager
def timed_stage(timings, name):
    started = time.monotonic()
    logging.info("stage_started=%s", name)
    try:
        yield
    finally:
        timings[name] = time.monotonic() - started
        logging.info("stage_completed=%s elapsed_seconds=%.6f", name, timings[name])


def preflight_inputs(all_cases, args, summary_config=None):
    names = {case.name: case for case in all_cases if not case.is_baseline}
    assigned = set()
    for name, directory in args.toe_log_dir or []:
        if name not in names or name in assigned:
            raise ValueError("--toe-log-dir must name a unique ToE or Ideal:CASE input: " + name)
        path = Path(directory).resolve()
        if not path.is_dir():
            raise ValueError("ToE log directory not found: " + str(path))
        names[name].toe_log_dir = path
        assigned.add(name)
    if summary_config is not None:
        targets = set(names) | {args.output.name + "/" + name for name in names}
        for target,path in summary_config.demand_file or []:
            if target not in targets:
                raise ValueError("--demand-file CASE does not match a ToE input: " + target)
            if not Path(path).is_file():
                raise ValueError("demand matrix file not found: " + path)
    for case in all_cases:
        if not case.path.is_dir():
            raise ValueError("case directory not found: " + str(case.path))
        if find(case, "task_statistics.csv") is None:
            raise ValueError("case=%s missing task_statistics.csv; searched=%s" % (case.name, case.searched_paths["task_statistics.csv"]))
        if case.is_baseline:
            continue
        for name in ("toe_summary.csv", "toe_topology.csv", "toe_reconfiguration.csv", "toe_circuits.csv"):
            find(case, name)
        missing = [name for name in ("toe_summary.csv", "toe_topology.csv") if case.file_cache[name] is None]
        if missing:
            details = "; ".join("%s searched=%s" % (name, case.searched_paths[name]) for name in missing)
            if not args.allow_missing_toe_logs:
                raise ValueError("case=%s missing ToE logs: %s; use --toe-log-dir CASE DIR for --toe-output, or --allow-missing-toe-logs for explicit FCT-only analysis" % (case.name, details))
            warn(case, "explicit FCT-only/partial ToE analysis: " + details)


def paired_baseline_p99(con, baseline, case):
    # This query reads only the existing task table, not serialized task CSVs
    # or trace files. Apply the same validity/metadata rules as the row writer.
    join = " FROM tasks b JOIN tasks t ON b.tid=t.tid WHERE b.case_id=? AND t.case_id=? AND b.start_ns IS NOT NULL AND t.start_ns IS NOT NULL AND b.finish_ns>=b.start_ns AND t.finish_ns>=t.start_ns AND NOT COALESCE(" + mismatch_sql() + ",0)"
    params = baseline.id, case.id
    count = con.execute("SELECT COUNT(*)" + join, params).fetchone()[0]
    if not count:
        return None
    rank = (count - 1) * .99
    lo, hi = math.floor(rank), math.ceil(rank)
    values = [r[0] for r in con.execute("SELECT b.finish_ns-b.start_ns AS fct" + join + " ORDER BY fct LIMIT ? OFFSET ?", params + (hi - lo + 1, lo))]
    return micro(values[0] + (values[-1] - values[0]) * (rank - lo))


def collect_link_samples_db(con, source, cases, baseline, args):
    if not args.include_link_samples:
        return
    for analysis_case in cases:
        summary_case = source.cases[analysis_case.name]
        for tid, sample in summary_case.samples.items():
            for observed in (baseline, analysis_case):
                records = con.execute("SELECT * FROM obs WHERE case_id=? AND tid=? AND kind!='L1_PAIR' ORDER BY epoch LIMIT ?", (observed.id, int(tid), args.links_per_task))
                for c, task, ep, kind, a, ap, b, bp, size, packets, first, last, origin in records:
                    sample["links"].append(dict(case_name=observed.name, task_id=str(task), epoch=str(ep), link_kind=kind,
                        src_node=str(a), src_port=str(ap), dst_node=str(b) if b>=0 else "", dst_port=str(bp) if bp>=0 else "",
                        tx_bytes=str(size), packet_count=str(packets), first_tx_us=micro(first), last_tx_us=micro(last),
                        source=origin, peer_mapping_available=int(b>=0)))


def analysis_main(argv=None):
    args, summary_args = build_parser().parse_known_args(argv)
    con = None
    started = time.monotonic()
    timings = {}
    try:
        if not args.case and not args.toe:
            raise ValueError("provide --case NAME DIR or --toe DIR")
        for name in ("epsilon_us", "event_window_us", "recovery_us", "reversal_horizon_seconds", "circuit_gbps"):
            value = getattr(args, name)
            if not math.isfinite(value) or value < 0 or (name in ("event_window_us", "circuit_gbps") and value == 0):
                raise ValueError("invalid --" + name.replace("_", "-"))
        if args.size_threshold_bytes < 0 or (args.metrics_window_us is not None and args.metrics_window_us <= 0):
            raise ValueError("invalid size threshold or metrics window")
        args.output = args.output.resolve()
        summary_argv = [str(args.output)] + list(summary_args)
        summary_argv += ["--circuit-gbps", str(args.circuit_gbps), "--topology-log-format", args.topology_log_format, "--epsilon-us", str(args.epsilon_us)]
        if args.summary_output:
            summary_argv += ["--output", str(args.summary_output.resolve())]
        summary_config = parser().parse_args(summary_argv)
        validate_summary_args(summary_config)
        if summary_config.input:
            raise ValueError("--input is for --summary-only; combined mode summarizes its own output")
        summary_path = (summary_config.output or args.output / "toe_ai_summary_v3.txt").resolve()
        if summary_path.suffix.lower() not in (".txt", ".md", ".log") or summary_path in {args.output/name for name in FILES}:
            raise ValueError("invalid --summary-output: use a separate .txt/.md/.log summary file")
        source = Source(args.output, args.output.name, epsilon=args.epsilon_us)
        source.counts["task_compare.csv"] = 0
        if args.output.exists() and any(args.output.iterdir()):
            raise ValueError("output directory must be new or empty; existing results are never overwritten")
        args.output.mkdir(parents=True, exist_ok=True)
        logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                            handlers=[logging.FileHandler(args.output / "analysis.log", encoding="utf-8"), logging.StreamHandler(sys.stdout)], force=True)
        logging.info("script_version=%s python=%s", ANALYZER_VERSION, sys.version.split()[0])
        logging.info("improvement_pct=(ECMP-ToE)/ECMP*100; positive=improvement; epsilon_us=%s; paired_completed_only", args.epsilon_us)
        entries = list(args.case or []) + ([["ToE", str(args.toe)]] if args.toe else [])
        if len({e[0] for e in entries}) != len(entries) or args.baseline_name in {e[0] for e in entries}:
            raise ValueError("case names must be unique and different from baseline name")
        baseline = Case(0, args.baseline_name, args.baseline.resolve(), True)
        cases = [Case(i + 1, name, Path(path).resolve()) for i, (name, path) in enumerate(entries)]
        all_cases = [baseline] + cases
        if len({c.path for c in all_cases}) != len(all_cases):
            raise ValueError("baseline and ToE case directories must be distinct")
        ideals = {}
        for name, path in args.ideal_case or []:
            if name not in {c.name for c in cases} or name in ideals:
                raise ValueError("--ideal-case must name one unique --case")
            ideal = Case(len(all_cases), "Ideal:" + name, Path(path).resolve())
            ideals[name] = ideal
            all_cases.append(ideal)
        for case in all_cases:
            if not case.path.is_dir():
                raise ValueError("case directory not found: %s" % case.path)
            if args.output == case.path or args.output == case.path / "output":
                raise ValueError("analysis output must not replace a case directory or its output directory")
        with timed_stage(timings, "input_preflight"):
            preflight_inputs(all_cases, args, summary_config)
        db_path = args.output / "analysis.sqlite3"
        con = setup_database(db_path)
        with timed_stage(timings, "load_inputs"):
            for case in all_cases:
                logging.info("stage=load_inputs case=%s path=%s", case.name, case.path)
                load_tasks(con, case)
                load_network(case)
                load_toe(case, args)
                if not case.is_baseline:
                    demand_path = demand_override(summary_config, source.label, case.name) or find(case, "toe_demand.csv")
                    if demand_path:
                        case.files["toe_demand"] = str(demand_path)
                        case.demand_matrices = read_demand_file(demand_path, case.name)
                    else:
                        warn(case, "toe_demand.csv missing; exact historical solver input matrices cannot be reconstructed")
                    case.capabilities["solver_demand_matrix_epochs"] = len(case.demand_matrices)
                load_direct_pause(con, case)
        verify_matching(con, baseline, cases + list(ideals.values()), args)
        zones = Zones([baseline] + cases, args)
        for case in [baseline] + cases:
            logging.info("stage=read_paths_and_metrics case=%s", case.name)
            with timed_stage(timings, "paths_metrics:" + case.name):
                normalized = import_path_csv(con, case, zones) if args.trace_mode != "raw" else False
                if args.trace_mode == "raw" or (args.trace_mode == "auto" and not normalized):
                    scan_traces(con, case, zones, args)
                elif args.trace_mode == "off" and not normalized:
                    warn(case, "raw trace scanning disabled; actual task paths remain unavailable")
                import_metrics(con, case, zones, args)
            with timed_stage(timings, "features:" + case.name):
                build_features(con, case, args)
        con.executescript("""CREATE INDEX obs_pair ON obs(case_id,kind,a,b,tid);
          CREATE INDEX sample_lookup ON samples(case_id,target_case,epoch,a,b,phase,metric);
          CREATE INDEX task_start ON tasks(case_id,start_ns);""")
        prepare_direct_totals(con)
        with timed_stage(timings, "paired_numeric_p99"):
            for case in cases:
                accumulator = source.case(case.name, summary_config)
                accumulator.focus_threshold = paired_baseline_p99(con, baseline, case)
                accumulator.focus_threshold_source = "paired_sql_numeric"
        def consume_row(row):
            source.counts["task_compare.csv"] += 1
            consume_task(source, summary_config, row, source.counts["task_compare.csv"])
        with timed_stage(timings, "task_comparison_and_accumulation"):
            write_task_comparisons(con, baseline, cases, ideals, args.output, args, consumer=consume_row)
        with timed_stage(timings, "focus_exact_quantiles"):
            finish_task_stream(source, summary_config)
            collect_link_samples_db(con, source, cases, baseline, summary_config)
        con.executescript("""CREATE INDEX comparison_base ON comparisons(case_id,status,base_ns);
          CREATE INDEX comparison_toe ON comparisons(case_id,status,toe_ns);
          CREATE INDEX comparison_start ON comparisons(case_id,status,start_ns);""")
        with timed_stage(timings, "analysis_tables"):
            logging.info("stage=write_summaries")
            write_observations(con, [baseline] + cases, args.output)
            write_events(cases, args.output, args)
            summaries = write_group_summary(con, cases, args.output, args, accumulators=source.cases)
            write_event_cohorts(con, cases, zones, args.output)
            write_hotspots(con, baseline, cases, zones, args.output, args)
            write_run_summary(con, baseline, cases, summaries, args.output, args)
        report = dict(script_version=ANALYZER_VERSION, elapsed_seconds=time.monotonic() - started,
            parameters={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
            stage_seconds=timings,
            pipeline=dict(task_compare_csv_read_passes=0, task_link_usage_csv_read_passes=0,
                          task_rows_consumed=source.counts["task_compare.csv"], summary_from="row_accumulators",
                          paired_p99_source="numeric SQLite task join; no task CSV pre-scan"),
            baseline=baseline.name, cases=[dict(name=c.name, path=str(c.path), files=c.files, searched_paths=c.searched_paths,
                        capabilities=c.capabilities, warnings=c.warnings) for c in all_cases],
            interpretation=["Missing cells mean unknown/unavailable; zero is a measured/derived zero.",
                            "FCT metrics use tasks completed in both cases with matching available metadata.",
                            "Pause overlap is temporal exposure, not measured blocked time or causal FCT cost.",
                            "Logical L1-pair throughput is delivered wire traffic, not offered load or direct-link utilization.",
                            "Per-event cohorts can overlap; their FCT deltas must not be summed as event costs.",
                            "Queue/blocked-time metrics require supplied observations; historical missing traces cannot be reconstructed.",
                            "Ideal execution-cost estimates require verified equal topology snapshots and application times; other run parameters must also match."])
        (args.output / "analysis_summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        source.topology_states = {(case.name, epoch): event for case in cases for epoch, event in case.events.items()}
        source.demand_cases = {case.name: case.demand_matrices for case in cases}
        with timed_stage(timings, "summary_render"):
            scan_small_files(source, summary_config)
            for case in source.cases.values():
                if number(case.run.get("paired_tasks")) != case.totals.n:
                    raise ValueError("streamed paired task count disagrees with run summary for " + case.name)
            write_summary_outputs([source], summary_config, started)
        report["elapsed_seconds"] = time.monotonic() - started
        report["stage_seconds"] = timings
        (args.output / "analysis_summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        logging.info("pipeline task_compare_csv_read_passes=0 task_link_usage_csv_read_passes=0 task_rows_consumed=%d", source.counts["task_compare.csv"])
        con.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        con.close(); con = None
        if not args.keep_db:
            for path in (db_path, Path(str(db_path) + "-wal"), Path(str(db_path) + "-shm")):
                if path.exists():
                    path.unlink()
        logging.info("stage=completed elapsed_seconds=%.3f output=%s", time.monotonic() - started, args.output)
        return 0
    except (ValueError, OSError, sqlite3.Error, InvalidOperation, csv.Error) as exc:
        logging.error("analysis_failed: %s", exc)
        print("[ERROR] %s" % exc, file=sys.stderr)
        return 2
    finally:
        if con is not None:
            con.close()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--demand-only" in argv:
        argv.remove("--demand-only")
        return demand_only_main(argv)
    if "--summary-only" in argv:
        argv.remove("--summary-only")
        return summary_main(argv)
    return analysis_main(argv)


if __name__ == "__main__":
    for limit in (sys.maxsize, 2**31 - 1):
        try:
            csv.field_size_limit(limit)
            break
        except OverflowError:
            pass
    raise SystemExit(main())
