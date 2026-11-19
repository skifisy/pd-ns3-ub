#!/usr/bin/env python3
"""Generate a static D--S expanded Mooncake topology in one command.

Each P--D/P--S pair loses one circuit; each D--S pair gains three circuits.
Each cross-group P--P pair gains one circuit. Every leaf keeps 64 OCS ports.
Generate the standard baseline automatically, or supply --base-case to use
an original four-circuit-per-pair case. --bandwidth-divisor scales every
physical link; 100000 changes 400Gbps to 4Mbps. No simulation is started.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import csv
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

# Keep the reusable Mooncake routing generator in tools/, one level above us.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from generate_routing_table import generate_routing_table
import generate_ocs_topology as original_topology


TOPOLOGY_FIELDS = ("nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay")
LEAVES = range(37, 57)
COPY_INPUTS = ("traffic.csv", "network_attribute.txt", "transport_channel.csv")
DEFAULT_OUTPUT = Path("static_ds_case")
RATE_UNITS = {"bps": 1, "Kbps": 10**3, "Mbps": 10**6,
              "Gbps": 10**9, "Tbps": 10**12}


def rate_bps(text: str) -> int:
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*(bps|Kbps|Mbps|Gbps|Tbps)\s*", text)
    if not match:
        raise ValueError(f"unsupported link bandwidth {text!r}; use bps/Kbps/Mbps/Gbps/Tbps")
    try:
        value = Decimal(match[1]) * RATE_UNITS[match[2]]
    except InvalidOperation as error:
        raise ValueError(f"invalid link bandwidth {text!r}") from error
    if value < 1 or value != value.to_integral_value():
        raise ValueError(f"link bandwidth must be a positive integer number of bit/s: {text!r}")
    return int(value)


def format_rate(bits_per_second: int) -> str:
    for unit, factor in reversed(list(RATE_UNITS.items())):
        if bits_per_second >= factor and bits_per_second % factor == 0:
            return f"{bits_per_second // factor}{unit}"
    return f"{bits_per_second}bps"


def scale_bandwidth(rows: list[dict], divisor: int) -> list[dict]:
    if isinstance(divisor, bool) or not isinstance(divisor, int) or divisor < 1:
        raise ValueError("bandwidth divisor must be a positive integer")
    result = [dict(row) for row in rows]
    for row in result:
        bits = rate_bps(row["bandwidth"])
        if bits % divisor or bits // divisor < 1:
            raise ValueError(f"cannot divide {row['bandwidth']} by {divisor} into whole positive bit/s")
        if divisor != 1:
            row["bandwidth"] = format_rate(bits // divisor)
    return result


def generate_default_baseline(directory: Path) -> None:
    """Use the existing generator's functions without writing its default case."""
    directory.mkdir()
    ids = original_topology.generate_ids()
    links = []
    next_id = original_topology.generate_compute_links(links, 0, ids[0], ids[2])
    next_id = original_topology.generate_storage_links(links, next_id, ids[1], ids[3])
    original_topology.generate_ocs_links(links, next_id, ids[2] + ids[3])
    original_topology.generate_node_csv(directory / "node.csv", *ids)
    original_topology.generate_topology_csv(links, directory / "topology.csv")


def role(node: int) -> str:
    return "P" if node <= 48 else "D" if node <= 52 else "S"


def group(node: int) -> int:
    return (node - 37) // 4


def read_table(path: Path, required: tuple[str, ...]) -> tuple[list[str], list[dict]]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        fields = reader.fieldnames or []
        if len(fields) != len(set(fields)) or not set(required) <= set(fields):
            raise ValueError(f"{path.name}: missing or duplicate columns; require {required}")
        rows = []
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"{path.name}: malformed CSV row {reader.line_num}")
            if not row[fields[0]].strip().startswith("#"):
                rows.append(row)
        return fields, rows


def read_nodes(path: Path) -> dict[int, int]:
    _, rows = read_table(path, ("nodeId", "nodeType", "portNum"))
    nodes = {}
    for row in rows:
        bounds = row["nodeId"].strip().split("..")
        if len(bounds) not in (1, 2):
            raise ValueError("node.csv: invalid nodeId range")
        first, last = int(bounds[0]), int(bounds[-1])
        if not 0 <= first <= last <= 56:
            raise ValueError("node.csv: expected Mooncake node IDs 0..56")
        for node in range(first, last + 1):
            expected_ports = 64 if node < 16 else 12 if node < 37 else 128
            expected_type = "DEVICE" if node < 37 else "SWITCH"
            if (node in nodes or int(row["portNum"]) != expected_ports
                    or row["nodeType"].strip() != expected_type):
                raise ValueError(f"node.csv: duplicate or incompatible node {node}")
            nodes[node] = expected_ports
    if set(nodes) != set(range(57)):
        raise ValueError("node.csv: require all 37 hosts and 20 leaves (IDs 0..56)")
    return nodes


@dataclass(frozen=True)
class Link:
    a: int
    ap: int
    b: int
    bp: int

    @classmethod
    def from_row(cls, row: dict) -> Link:
        a, ap, b, bp = (int(row[key]) for key in TOPOLOGY_FIELDS[:4])
        return cls(a, ap, b, bp) if a < b else cls(b, bp, a, ap)

    def endpoints(self) -> tuple[tuple[int, int], tuple[int, int]]:
        return (self.a, self.ap), (self.b, self.bp)

    def as_row(self, template: dict) -> dict:
        return dict(template, **dict(zip(TOPOLOGY_FIELDS[:4],
                                        map(str, (self.a, self.ap, self.b, self.bp)))))


def validate_topology(rows: list[dict], nodes: dict[int, int]) -> dict:
    endpoints = set()
    bundles = defaultdict(list)
    host_links = Counter()
    ocs_attributes = set()
    for index, row in enumerate(rows):
        link = Link.from_row(row)
        if link.a == link.b:
            raise ValueError("topology.csv: self link")
        for node, port in link.endpoints():
            if node not in nodes or not 0 <= port < nodes[node]:
                raise ValueError(f"topology.csv: invalid endpoint {node}:{port}")
            if (node, port) in endpoints:
                raise ValueError(f"topology.csv: duplicate physical endpoint {node}:{port}")
            endpoints.add((node, port))
        if not row["bandwidth"].strip() or not row["delay"].strip():
            raise ValueError("topology.csv: bandwidth and delay must be set")
        if link.a >= 37:
            if group(link.a) == group(link.b) or min(link.ap, link.bp) < 64:
                raise ValueError("topology.csv: OCS links must use ports 64..127 across groups")
            bundles[link.a, link.b].append((link, index))
            ocs_attributes.add((row["bandwidth"].strip(), row["delay"].strip()))
        else:
            if link.b < 37 or link.bp >= (64 if link.b <= 52 else 63):
                raise ValueError("topology.csv: invalid host attachment")
            host_links[link.a, link.b] += 1
    expected_hosts = {}
    for host in range(37):
        first_leaf = 37 + (host // 4) * 4
        leaves = range(first_leaf, first_leaf + 4) if host < 16 else range(53, 57)
        for leaf in leaves:
            expected_hosts[host, leaf] = 16 if host < 16 else 3
    if host_links != expected_hosts:
        raise ValueError("topology.csv: host attachments do not match the Mooncake layout")
    expected_pairs = {(a, b) for a in LEAVES for b in LEAVES
                      if a < b and group(a) != group(b)}
    if set(bundles) != expected_pairs:
        raise ValueError("topology.csv: require all 160 cross-group leaf pairs")
    for leaf in LEAVES:
        if {(leaf, port) for port in range(64, 128)} - endpoints:
            raise ValueError(f"topology.csv: leaf {leaf} must use all 64 OCS ports")
    if len(ocs_attributes) != 1 or next(iter(ocs_attributes))[0] != "400Gbps":
        raise ValueError("topology.csv: require uniform 400Gbps OCS circuits with equal delays")
    return bundles


def transform(rows: list[dict], nodes: dict[int, int]) -> tuple[list[dict], list[dict]]:
    bundles = validate_topology(rows, nodes)
    if any(len(bundle) != 4 for bundle in bundles.values()):
        raise ValueError("require original topology: exactly four circuits per cross-group pair")
    result = [dict(row) for row in rows]
    changes = []
    for i in range(12):
        g, r = divmod(i, 4)
        p = 37 + i
        for t in range(4):
            # Visit all four D leaves and P peers in the next group. For a
            # fixed q, donor S leaves are distinct. Each (D_t, S_r) gets one
            # circuit from each of the three P groups.
            q = 37 + 4 * ((g + 1) % 3) + (r + t) % 4
            d, s = 49 + t, 53 + r
            pd, pd_index = min(bundles[p, d], key=lambda item: item[0].endpoints())
            ps, ps_index = min(bundles[q, s], key=lambda item: item[0].endpoints())
            ds, pp = Link(d, pd.bp, s, ps.bp), Link(p, pd.ap, q, ps.ap)
            if pp.a > pp.b:
                pp = Link(pp.b, pp.bp, pp.a, pp.ap)
            result[pd_index] = ds.as_row(rows[pd_index])
            result[ps_index] = pp.as_row(rows[ps_index])
            for status, link in (("remove", pd), ("remove", ps), ("add", ds), ("add", pp)):
                changes.append(dict(swap_id=4 * i + t + 1, status=status, src_leaf=link.a,
                                    src_port=link.ap, dst_leaf=link.b, dst_port=link.bp))
    after = validate_topology(result, nodes)
    if sum(len(bundle) for bundle in after.values()) != 640:
        raise ValueError("transformation changed the total OCS circuit count")
    return result, changes


def validate_routes(path: Path, topology: list[dict]) -> int:
    """Check completeness and every equal-cost output port using BFS distances."""
    leaf_ports = defaultdict(dict)
    host_ports = defaultdict(dict)
    host_destinations = {}
    for row in topology:
        link = Link.from_row(row)
        if link.a >= 37:
            leaf_ports[link.a][link.ap] = link.b
            leaf_ports[link.b][link.bp] = link.a
        else:
            host_ports[link.a][link.ap] = link.b
            host_destinations[link.a, link.ap] = (link.b, link.bp)
    distances = {}
    for destination in LEAVES:
        dist, queue = {destination: 0}, deque([destination])
        while queue:
            for neighbour in leaf_ports[queue[0]].values():
                if neighbour not in dist:
                    dist[neighbour] = dist[queue[0]] + 1
                    queue.append(neighbour)
            queue.popleft()
        if len(dist) != 20:
            raise ValueError("routing: disconnected leaf topology")
        distances[destination] = dist
    seen = set()
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            source, dest, dp = (int(row[key]) for key in ("nodeId", "dstNodeId", "dstPortId"))
            key = source, dest, dp
            if key in seen or (dest, dp) not in host_destinations or source == dest:
                raise ValueError(f"routing: duplicate or invalid destination in {key}")
            final_leaf, final_port = host_destinations[dest, dp]
            dist = distances[final_leaf]
            if source in LEAVES:
                expected = ({final_port} if source == final_leaf else
                            {port for port, peer in leaf_ports[source].items()
                             if dist[peer] == dist[source] - 1})
                metric = dist[source] + 1
            elif source in host_ports:
                best = min(dist[leaf] for leaf in host_ports[source].values())
                expected = {port for port, leaf in host_ports[source].items() if dist[leaf] == best}
                metric = best + 2
            else:
                raise ValueError(f"routing: unknown source {source}")
            ports = list(map(int, row["outPorts"].split()))
            if (not expected or len(ports) != len(set(ports)) or set(ports) != expected
                    or list(map(int, row["metrics"].split())) != [metric] * len(ports)):
                raise ValueError(f"routing: incorrect output ports or metrics in {key}")
            seen.add(key)
    expected_count = len(host_destinations) * (20 + len(host_ports) - 1)
    if len(seen) != expected_count:
        raise ValueError(f"routing: incomplete table ({len(seen)}/{expected_count} rows)")
    return len(seen)


def topology_summary(rows: list[dict]) -> dict:
    totals = Counter()
    ds_capacity_bps = 0
    per_leaf = {str(leaf): Counter() for leaf in LEAVES}
    ds = [[0] * 4 for _ in range(4)]
    for row in rows:
        link = Link.from_row(row)
        if link.a < 37:
            continue
        totals[role(link.a) + role(link.b)] += 1
        per_leaf[str(link.a)][role(link.b)] += 1
        per_leaf[str(link.b)][role(link.a)] += 1
        if 49 <= link.a <= 52 and 53 <= link.b <= 56:
            ds[link.a - 49][link.b - 53] += 1
            ds_capacity_bps += rate_bps(row["bandwidth"])
    return dict(circuits_by_role=dict(totals), ocs_neighbours_by_role=per_leaf,
                ds_matrix=ds, ds_capacity_bps_per_direction=ds_capacity_bps,
                ds_capacity_gbps_per_direction=ds_capacity_bps / 10**9)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_case(base_case: Path | None, out_dir: Path, bandwidth_divisor: int = 1) -> dict:
    base_case = base_case.resolve() if base_case is not None else None
    out_dir = out_dir.resolve()
    if base_case is not None and (base_case == out_dir or base_case in out_dir.parents
                                  or out_dir in base_case.parents):
        raise ValueError("base case and output must be separate, non-nested directories")
    if out_dir.exists() and (not out_dir.is_dir() or any(out_dir.iterdir())):
        raise ValueError("output must be a new or empty directory; existing files are never overwritten")
    if (isinstance(bandwidth_divisor, bool) or not isinstance(bandwidth_divisor, int)
            or bandwidth_divisor < 1):
        raise ValueError("bandwidth divisor must be a positive integer")
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{out_dir.name}-", dir=out_dir.parent) as temp:
        source = base_case
        if source is None:
            source = Path(temp) / "baseline"
            generate_default_baseline(source)
        nodes = read_nodes(source / "node.csv")
        fields, before = read_table(source / "topology.csv", TOPOLOGY_FIELDS)
        after, changes = transform(before, nodes)
        before = scale_bandwidth(before, bandwidth_divisor)
        after = scale_bandwidth(after, bandwidth_divisor)
        stage = Path(temp) / "case"
        stage.mkdir()
        shutil.copy2(source / "node.csv", stage / "node.csv")
        with (stage / "topology.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fields)
            writer.writeheader()
            writer.writerows(after)
        generate_routing_table(stage / "topology.csv", stage / "routing_table.csv")
        route_rows = validate_routes(stage / "routing_table.csv", after)
        copied = ["node.csv"] if base_case is not None else []
        for name in COPY_INPUTS:
            input_path = source / name
            if input_path.exists():
                shutil.copy2(input_path, stage / name)
                copied.append(name)
        with (stage / "static_ds_changes.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, list(changes[0]))
            writer.writeheader()
            writer.writerows(changes)
        summary = dict(
            schema_version=3, base_case=str(base_case) if base_case is not None else None,
            baseline_source="base-case" if base_case is not None else "builtin-mooncake",
            output_case=str(out_dir), bandwidth_divisor=bandwidth_divisor,
            ocs_bandwidth_bps=400 * 10**9 // bandwidth_divisor,
            allocation="uniform-pair-expansion", swaps=48,
            removed_circuits=96, added_circuits=96,
            pd_removed_per_prefill=4, ps_removed_per_prefill=4,
            pd_removed_per_pair=1, ps_removed_per_pair=1, ds_added_per_pair=3,
            before=topology_summary(before), after=topology_summary(after),
            copied_inputs=copied,
            generated_inputs=["node.csv"] if base_case is None else [],
            route_rows_checked=route_rows,
            missing_run_inputs=[name for name in ("traffic.csv", "network_attribute.txt")
                                if name not in copied],
            sha256={name: sha256(stage / name) for name in
                    ["node.csv", *[name for name in copied if name != "node.csv"],
                     "topology.csv", "routing_table.csv"]},
            simulation_started=False,
        )
        (stage / "static_ds_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        os.replace(stage, out_dir)
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-case", type=Path,
                        help="optional original case; omitted: generate the standard Mooncake baseline")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUTPUT,
                        help="new or empty output directory (default: ./static_ds_case)")
    parser.add_argument("--bandwidth-divisor", type=int, default=1,
                        help="divide every link rate by this positive integer; 100000: 400Gbps -> 4Mbps")
    args = parser.parse_args()
    try:
        summary = build_case(args.base_case, args.out_dir, args.bandwidth_divisor)
    except (ValueError, OSError, csv.Error) as error:
        parser.exit(2, f"error: {error}\n")
    print(f"Generated: {summary['output_case']}")
    print("Every P-D/P-S pair: 4 -> 3 circuits; every D-S pair: 4 -> 7 circuits.")
    old_rate = format_rate(summary["before"]["ds_capacity_bps_per_direction"])
    new_rate = format_rate(summary["after"]["ds_capacity_bps_per_direction"])
    print(f"D-S circuits: 64 -> 112; per-direction capacity: {old_rate} -> {new_rate}")
    print(f"OCS link bandwidth: {format_rate(summary['ocs_bandwidth_bps'])}; "
          f"all link rates divided by {summary['bandwidth_divisor']}")
    print(f"Validated {summary['route_rows_checked']} routing entries; no simulation started.")
    if summary["missing_run_inputs"]:
        print("Topology-only output; missing run inputs: " + ", ".join(summary["missing_run_inputs"]))
    else:
        print("Copied run inputs. For the static ECMP experiment, use --toe-mode=off --jupiter-te=0.")


if __name__ == "__main__":
    main()
