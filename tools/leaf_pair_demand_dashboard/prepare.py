#!/usr/bin/env python3
"""Stream Replay CSVs into an atomic, indexed Leaf-pair analysis cache."""
import argparse
from collections import defaultdict
import csv
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import tempfile

try:
    from .metrics import finite_number, picoseconds, rate, weighted_quantiles, PS_PER_SECOND
except ImportError:
    from metrics import finite_number, picoseconds, rate, weighted_quantiles, PS_PER_SECOND

SCHEMA = 1
FANOUT = 4
HERE = Path(__file__).resolve().parent


def source(path, required=True):
    path = Path(path)
    for candidate in (path, Path(str(path) + ".gz")):
        if candidate.is_file():
            return candidate
    if required:
        raise ValueError(f"Missing input: {path} (or .gz)")
    return None


def rows(path, required_columns):
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        missing = set(required_columns.split()) - set(reader.fieldnames or [])
        if missing:
            raise ValueError(f"{path}: missing columns {', '.join(sorted(missing))}")
        yield from reader


def ids(spec):
    if spec is None:
        return None
    result = set()
    for item in spec.split(","):
        if not item.strip():
            continue
        ends = item.strip().replace("..", "-").split("-")
        lo, hi = (int(ends[0]), int(ends[-1]))
        if len(ends) > 2 or lo < 0 or hi < lo:
            raise ValueError(f"Invalid node range: {item}")
        result.update(range(lo, hi + 1))
    return result


def check_case(row, case_id):
    if row.get("case_id", case_id) != case_id:
        raise ValueError("Input contains a different case_id; do not mix runs")


def read_snapshots(path, case_id):
    snapshots, times, ports = {}, {}, set()
    access_edges = set()
    for row in rows(path, "time_ns topology_epoch src_node src_port dst_node dst_port data_rate_bps"):
        check_case(row, case_id)
        epoch, t = int(row["topology_epoch"]), picoseconds(row["time_ns"])
        a, ap, b, bp = (int(row[k]) for k in ("src_node", "src_port", "dst_node", "dst_port"))
        if epoch in times and times[epoch] != t:
            raise ValueError("One topology epoch has multiple effective times")
        key = (epoch, a, ap)
        if key in ports:
            raise ValueError(f"Duplicate topology port: {key}")
        ports.add(key)
        times[epoch] = t
        snapshots.setdefault(epoch, defaultdict(float))[(a, b)] += finite_number(
            row["data_rate_bps"], "data_rate_bps", positive=True)
        access_edges.add((a, b))
    if not times or len(set(times.values())) != len(times):
        raise ValueError("Replay topology is empty or has duplicate effective times")
    return snapshots, times, access_edges


def infer_roles(node_file, edges, args):
    explicit = [ids(getattr(args, name)) for name in
                ("prefill_leaves", "decode_leaves", "storage_leaves")]
    if all(group is not None for group in explicit):
        groups = explicit
    else:
        if node_file is None:
            raise ValueError("Provide --topology containing node.csv, or all three --*-leaves options")
        nodes = {}
        for row in rows(node_file, "nodeId nodeType portNum"):
            for n in ids(row["nodeId"]):
                if n in nodes:
                    raise ValueError(f"Duplicate node: {n}")
                nodes[n] = (row["nodeType"].upper(), int(row["portNum"]))
        hosts = [ids(args.prefill_nodes), ids(args.decode_nodes)]
        if hosts[0] & hosts[1]:
            raise ValueError("Prefill and Decode host sets overlap")
        groups = [set(), set(), set()]
        for a, b in edges:
            if a not in nodes or b not in nodes:
                raise ValueError(f"Topology node {a} or {b} is absent from node.csv")
            if nodes[a][0] == "SWITCH" or nodes[b][0] != "SWITCH":
                continue
            if a in hosts[0]:
                role = 0
            elif a in hosts[1]:
                role = 1
            elif nodes[a][1] != 64:
                role = 2
            else:
                raise ValueError(f"Unknown compute host {a}: provide --prefill-nodes / --decode-nodes")
            groups[role].add(b)
        groups = [given if given is not None else inferred for given, inferred in zip(explicit, groups)]
    p, d, s = groups
    if not s or not (p or d):
        raise ValueError("Need Storage leaves and at least one Prefill or Decode leaf")
    if p & d or p & s or d & s:
        raise ValueError("Leaf roles overlap; provide explicit disjoint --*-leaves sets")
    known = {n for edge in edges for n in edge}
    if not (p | d | s) <= known:
        raise ValueError("Some configured leaves are absent from replay_topology.csv")
    return {"P": sorted(p), "D": sorted(d), "S": sorted(s)}


def make_pairs(roles):
    pairs, directions = [], {}
    for role in ("P", "D"):
        for compute in roles[role]:
            for storage in roles["S"]:
                pair = {"id": f"{compute}-{storage}", "group": role, "compute": compute,
                        "storage": storage, "label": f"{role}{compute} ↔ S{storage}", "directions": []}
                for a, b in ((compute, storage), (storage, compute)):
                    direction = len(directions)
                    directions[a, b] = direction
                    pair["directions"].append(direction)
                pairs.append(pair)
    return pairs, directions


def create_tables(db):
    db.executescript("""
        PRAGMA journal_mode=OFF;
        PRAGMA synchronous=OFF;
        PRAGMA cache_size=-65536;
        PRAGMA temp_store=FILE;
        CREATE TABLE metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);
        CREATE TABLE windows(id INTEGER PRIMARY KEY, t0 INTEGER UNIQUE, t1 INTEGER, epoch INTEGER);
        CREATE INDEX windows_end ON windows(t1);
        CREATE TABLE capacities(direction INTEGER, time INTEGER, bps REAL,
                                PRIMARY KEY(direction,time)) WITHOUT ROWID;
        CREATE TABLE raw(direction INTEGER, window INTEGER, payload INTEGER, wire INTEGER,
                         capacity REAL, bps REAL, PRIMARY KEY(direction,window)) WITHOUT ROWID;
        CREATE TABLE tiles(direction INTEGER, level INTEGER, idx INTEGER,
                           first INTEGER, last INTEGER, t0 INTEGER, t1 INTEGER,
                           payload REAL, wire REAL, capacity REAL, cap_min REAL, cap_max REAL,
                           payload_peak REAL, wire_peak REAL,
                           PRIMARY KEY(direction,level,idx)) WITHOUT ROWID;
    """)


def import_links(db, path, snapshots, times, directions, metadata):
    """Only one sample's port aggregates live in Python at a time."""
    key, previous_end, current, seen = None, None, {}, set()
    windows = {}
    aggregation_kind = None
    ordered_epochs = sorted(times, key=times.get)
    next_times = {epoch: times[ordered_epochs[i + 1]] if i + 1 < len(ordered_epochs) else math.inf
                  for i, epoch in enumerate(ordered_epochs)}

    def flush():
        nonlocal previous_end
        t0, t1, epoch = key
        if previous_end is not None and t0 != previous_end:
            raise ValueError("Link windows must be contiguous, ordered and nonoverlapping")
        if epoch not in times or t0 < times[epoch] or t1 > next_times[epoch]:
            raise ValueError("Link window crosses or disagrees with a topology snapshot")
        if "sample_interval_ns" in metadata and t1 - t0 > picoseconds(metadata["sample_interval_ns"]):
            raise ValueError("Link window exceeds sample_interval_ns")
        window = len(windows)
        windows[t0, t1] = (window, epoch)
        db.execute("INSERT INTO windows VALUES(?,?,?,?)", (window, t0, t1, epoch))
        values = []
        for pair, direction in directions.items():
            expected = snapshots[epoch].get(pair, 0)
            wire, capacity, bps = current.get(direction, (0, 0, 0))
            if not math.isclose(bps, expected, rel_tol=1e-9, abs_tol=1e-6):
                raise ValueError(f"Incomplete link samples for {pair} at {t0 / PS_PER_SECOND:g}s")
            values.append((direction, window, 0, wire, capacity, bps))
        db.executemany("INSERT INTO raw VALUES(?,?,?,?,?,?)", values)
        previous_end = t1
        if window % 10000 == 0:
            db.commit()
            print(f"  link windows: {window + 1:,}", flush=True)

    for row in rows(path, "start_ns end_ns topology_epoch src_node dst_node data_rate_bps capacity_bits tx_wire_bytes aggregation"):
        check_case(row, metadata["case_id"])
        t0, t1 = picoseconds(row["start_ns"]), picoseconds(row["end_ns"])
        epoch = int(row["topology_epoch"])
        if t1 <= t0:
            raise ValueError("Nonpositive link window")
        new_key = (t0, t1, epoch)
        if key is not None and new_key != key:
            flush()
            current, seen = {}, set()
        key = new_key
        kind = row["aggregation"]
        if kind not in ("node_pair", "physical_port") or (aggregation_kind and kind != aggregation_kind):
            raise ValueError("Link aggregation must consistently be node_pair or physical_port")
        aggregation_kind = kind
        pair = int(row["src_node"]), int(row["dst_node"])
        direction = directions.get(pair)
        if direction is None:
            continue
        unique = pair if kind == "node_pair" else (pair, int(row["src_port"]))
        if unique in seen:
            raise ValueError("Duplicate selected link row in a sample")
        seen.add(unique)
        bps = finite_number(row["data_rate_bps"], "data_rate_bps")
        capacity = finite_number(row["capacity_bits"], "capacity_bits")
        if not math.isclose(capacity, bps * (t1 - t0) / PS_PER_SECOND, rel_tol=1e-8, abs_tol=1e-6):
            raise ValueError("capacity_bits disagrees with rate × actual window duration")
        wire = int(row["tx_wire_bytes"])
        if wire < 0:
            raise ValueError("Negative wire bytes")
        old = current.get(direction, (0, 0, 0))
        current[direction] = (old[0] + wire, old[1] + capacity, old[2] + bps)
    if key is None:
        raise ValueError("No link samples")
    flush()
    first = next(iter(windows))[0]
    for field, actual in (("start_ns", first), ("end_ns", previous_end)):
        if field in metadata and picoseconds(metadata[field]) != actual:
            raise ValueError(f"Replay {field} disagrees with sampled recording; logs may be truncated")
    db.commit()
    return windows


def import_payload(db, path, windows, directions, metadata):
    count = 0
    batch = []
    for row in rows(path, "start_ns end_ns topology_epoch src_leaf dst_leaf tx_payload_bytes"):
        check_case(row, metadata["case_id"])
        key = picoseconds(row["start_ns"]), picoseconds(row["end_ns"])
        if key not in windows or windows[key][1] != int(row["topology_epoch"]):
            raise ValueError("Payload window does not match link samples")
        direction = directions.get((int(row["src_leaf"]), int(row["dst_leaf"])))
        value = int(row["tx_payload_bytes"])
        if value < 0:
            raise ValueError("Negative payload bytes")
        if direction is not None:
            batch.append((value, direction, windows[key][0]))
        count += 1
        if count % 20000 == 0:
            db.executemany("UPDATE raw SET payload=payload+? WHERE direction=? AND window=?", batch)
            db.commit()
            batch.clear()
        if count % 1000000 == 0:
            print(f"  payload rows: {count:,}", flush=True)
    db.executemany("UPDATE raw SET payload=payload+? WHERE direction=? AND window=?", batch)
    db.commit()


def build_tiles(db, direction_count, window_count, has_payload):
    summaries = {}
    for direction in range(direction_count):
        db.execute("""INSERT INTO tiles
            SELECT r.direction,0,w.id,w.id,w.id,w.t0,w.t1,r.payload,r.wire,r.capacity,r.bps,r.bps,
                   r.payload*8.0*1e12/(w.t1-w.t0),r.wire*8.0*1e12/(w.t1-w.t0)
            FROM raw r JOIN windows w ON r.window=w.id WHERE r.direction=?""", (direction,))
        level, count = 0, window_count
        while count > 1:
            db.execute("""INSERT INTO tiles
                SELECT direction,level+1,idx/?,MIN(first),MAX(last),MIN(t0),MAX(t1),
                       SUM(payload),SUM(wire),SUM(capacity),MIN(cap_min),MAX(cap_max),
                       MAX(payload_peak),MAX(wire_peak)
                FROM tiles WHERE direction=? AND level=? GROUP BY idx/?""",
                       (FANOUT, direction, level, FANOUT))
            count = (count + FANOUT - 1) // FANOUT
            level += 1
        samples = list(db.execute("""SELECT payload,wire,capacity,w.t1-w.t0
            FROM raw r JOIN windows w ON w.id=r.window WHERE direction=?""", (direction,)))
        duration = sum(row[3] for row in samples)
        capacity = sum(row[2] for row in samples)
        summary = {}
        for metric, column in (("payload", 0), ("wire", 1)):
            if metric == "payload" and not has_payload:
                continue
            rates = [(rate(row[column], row[3]), row[3]) for row in samples]
            ratios = [(row[column] * 8 / row[2], row[3]) for row in samples if row[2] > 0]
            total_bits = sum(row[column] * 8 for row in samples)
            summary[metric] = {"mean_bps": total_bits * PS_PER_SECOND / duration,
                               "peak_bps": max(value for value, _ in rates),
                               **{f"{k}_bps": v for k, v in weighted_quantiles(rates, duration).items()},
                               "mean_ratio": total_bits / capacity if capacity else None,
                               "peak_ratio": max((v for v, _ in ratios), default=None),
                               "hot80_seconds": sum(dt for v, dt in ratios if v >= .8) / PS_PER_SECOND,
                               "hot90_seconds": sum(dt for v, dt in ratios if v >= .9) / PS_PER_SECOND}
        summaries[str(direction)] = summary
        db.commit()
        print(f"  indexed directions: {direction + 1}/{direction_count}", flush=True)
    # raw is needed for import only; the level-0 tiles retain the original samples.
    db.execute("DROP TABLE raw")
    db.commit()
    return summaries


def prepare(args):
    case = Path(args.case).resolve()
    log = case / "replay" if (case / "replay").is_dir() else case
    meta_file = source(log / "replay_run.csv")
    metadata = {row["key"]: row["value"] for row in rows(meta_file, "key value")}
    if not metadata.get("case_id"):
        raise ValueError("Missing case_id in replay_run.csv")
    topology_file = source(log / "replay_topology.csv")
    link_file = source(log / "replay_link_samples.csv")
    payload_file = source(log / "replay_node_path_samples.csv", required=False)
    topology_dir = Path(args.topology).resolve() if args.topology else case
    node_file = source(topology_dir / "node.csv", required=False)
    physical_file = source(topology_dir / "topology.csv", required=False)
    snapshots, times, edges = read_snapshots(topology_file, metadata["case_id"])
    if physical_file:
        for row in rows(physical_file, "nodeId1 nodeId2"):
            a, b = int(row["nodeId1"]), int(row["nodeId2"])
            edges.update(((a, b), (b, a)))
    roles = infer_roles(node_file, edges, args)
    pairs, directions = make_pairs(roles)
    scale = finite_number(args.throughput_scale, "throughput-scale", positive=True)
    has_payload = bool(payload_file and metadata.get("node_path_enabled", "true") == "true"
                       and metadata.get("detail_windows", "all") == "all")
    warnings = []
    if not has_payload:
        warnings.append("业务载荷不可用：需要启用全程 node_path 采集；当前仅展示物理链路吞吐。")
    if int(metadata.get("unattributed_path_packets", "0")):
        warnings.append("日志中存在未归属路径的数据包，业务载荷统计可能偏低。")
    inputs = [p for p in (meta_file, topology_file, link_file, payload_file, node_file, physical_file) if p]
    identity = [[str(p), p.stat().st_size, p.stat().st_mtime_ns] for p in inputs]
    processing = [hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                  for name in ("prepare.py", "metrics.py")]
    fingerprint = hashlib.sha256(json.dumps([SCHEMA, roles, scale, identity, processing]).encode()).hexdigest()
    output_dir = Path(args.out_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    cache = output_dir / "leaf-pairs.sqlite"
    if cache.is_file() and not args.force:
        try:
            with sqlite3.connect(f"{cache.as_uri()}?mode=ro", uri=True) as db:
                if db.execute("SELECT value FROM metadata WHERE key='fingerprint'").fetchone() == (fingerprint,):
                    print(f"Reusing {cache}")
                    return cache
        except sqlite3.DatabaseError:
            pass
    handle, temp_name = tempfile.mkstemp(prefix=".leaf-pairs-", suffix=".sqlite", dir=output_dir)
    os.close(handle)
    try:
        with sqlite3.connect(temp_name) as db:
            create_tables(db)
            for epoch in sorted(times, key=times.get):
                db.executemany("INSERT INTO capacities VALUES(?,?,?)",
                               ((direction, times[epoch], snapshots[epoch].get(pair, 0))
                                for pair, direction in directions.items()))
            print(f"Importing {metadata['case_id']}: {len(pairs)} pairs, {len(directions)} directions", flush=True)
            windows = import_links(db, link_file, snapshots, times, directions, metadata)
            if has_payload:
                import_payload(db, payload_file, windows, directions, metadata)
            summaries = build_tiles(db, len(directions), len(windows), has_payload)
            t0, t1 = db.execute("SELECT MIN(t0),MAX(t1) FROM windows").fetchone()
            max_level = db.execute("SELECT MAX(level) FROM tiles").fetchone()[0]
            info = {"schema": SCHEMA, "case_id": metadata["case_id"], "mode": metadata.get("mode", "unknown"),
                    "synthetic": metadata.get("synthetic") == "true", "roles": roles, "pairs": pairs,
                    "start": t0 / PS_PER_SECOND, "end": t1 / PS_PER_SECOND, "window_count": len(windows),
                    "sample_interval_seconds": float(metadata.get("sample_interval_ns", 0)) / 1e9,
                    "throughput_scale": scale, "metrics": ["payload", "wire"] if has_payload else ["wire"],
                    "warnings": warnings, "summaries": summaries, "max_level": max_level}
            for key, value in (("bootstrap", json.dumps(info, ensure_ascii=False, allow_nan=False)),
                               ("fingerprint", fingerprint), ("schema", str(SCHEMA))):
                db.execute("INSERT INTO metadata VALUES(?,?)", (key, value))
            db.commit()
            db.execute("VACUUM")
        os.replace(temp_name, cache)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)
    print(f"Ready: {cache}", flush=True)
    return cache


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--case", required=True, help="ECMP case directory or replay directory")
    result.add_argument("--topology", help="Directory containing node.csv / topology.csv (default: case)")
    result.add_argument("--out-dir", required=True, help="Prepared SQLite output directory")
    result.add_argument("--throughput-scale", type=float, default=1, help="Display rate and capacity multiplier")
    result.add_argument("--prefill-nodes", default="0-11", help="Prefill host IDs for role inference")
    result.add_argument("--decode-nodes", default="12-15", help="Decode host IDs for role inference")
    for role in ("prefill", "decode", "storage"):
        result.add_argument(f"--{role}-leaves", help="Explicit Leaf IDs, e.g. 37-48 or 37,39; empty disables a group")
    result.add_argument("--force", action="store_true", help="Rebuild even when inputs are unchanged")
    return result


if __name__ == "__main__":
    arguments = parser().parse_args()
    try:
        prepare(arguments)
    except (ValueError, OSError, sqlite3.Error) as error:
        raise SystemExit(f"Preparation failed: {error}")
