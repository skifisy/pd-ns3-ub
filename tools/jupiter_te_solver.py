#!/usr/bin/env python3
"""Solve a static leaf-level Jupiter-style traffic engineering epoch.

Input matrix: src_leaf,dst_leaf,bps. Output: src_leaf,dst_leaf,transit_leaf,weight.
transit_leaf=-1 denotes the direct logical edge. This script is run after each
measurement window by ub-case-runner, not given future traffic.csv values.
"""

import argparse
import csv
from collections import defaultdict


def read_topology(path):
    ports = defaultdict(list)
    capacities = defaultdict(float)
    host_leaf = {}
    with open(path, newline="") as stream:
        for row in csv.DictReader(stream):
            a, ap = int(row["nodeId1"]), int(row["portId1"])
            b, bp = int(row["nodeId2"]), int(row["portId2"])
            rate = row["bandwidth"].strip()
            if rate.endswith("Gbps"):
                bps = float(rate[:-4]) * 1e9
            elif rate.endswith("Mbps"):
                bps = float(rate[:-4]) * 1e6
            else:
                raise ValueError(f"unsupported bandwidth: {rate}")
            if a >= 37 and b >= 37:
                ports[a, b].append(ap)
                ports[b, a].append(bp)
                capacities[a, b] += bps
                capacities[b, a] += bps
            else:
                host, hp, leaf = (a, ap, b) if a < 37 else (b, bp, a)
                if (host, hp) in host_leaf:
                    raise ValueError("host port connected twice")
                host_leaf[host, hp] = leaf
    return ports, capacities, host_leaf


def candidates(src, dst, capacities):
    paths = []
    if (src, dst) in capacities:
        paths.append((src, dst, -1, ((src, dst),)))
    leaves = sorted({edge[0] for edge in capacities})
    for middle in leaves:
        if middle != src and middle != dst and (src, middle) in capacities and (middle, dst) in capacities:
            paths.append((src, dst, middle, ((src, middle), (middle, dst))))
    return paths


def solve(capacities, demands, s=0.0):
    """Minimize max directed edge utilization, then minimize transit traffic."""
    if not 0 <= s <= 1:
        raise ValueError("S must be in [0, 1]")
    from scipy.optimize import linprog
    from scipy.sparse import lil_matrix, vstack

    paths = []
    od_rows = {}
    for (src, dst), demand in sorted(demands.items()):
        if demand <= 0:
            continue
        if src == dst:
            continue
        available = candidates(src, dst, capacities)
        if not available:
            raise ValueError(f"no direct or single-transit route: {src}->{dst}")
        od_rows[src, dst] = len(od_rows)
        paths.extend(available)
    if not paths:
        return {}, 0.0

    # Normalize each commodity to Gbps to improve the LP conditioning.
    edges = sorted(capacities)
    edge_rows = {edge: i for i, edge in enumerate(edges)}
    n = len(paths)
    eq = lil_matrix((len(od_rows), n + 1))
    ub = lil_matrix((len(edges), n + 1))
    limits = []
    for i, (src, dst, transit, used) in enumerate(paths):
        eq[od_rows[src, dst], i] = 1
        for edge in used:
            ub[edge_rows[edge], i] = 1
        path_capacity = min(capacities[e] for e in used)
        total_path_capacity = sum(min(capacities[e] for e in p[3])
                                  for p in candidates(src, dst, capacities))
        limit = (demands[src, dst] / 1e9 * path_capacity / (s * total_path_capacity)) if s else None
        limits.append((0, limit))
    for edge, i in edge_rows.items():
        ub[i, n] = -capacities[edge] / 1e9
    rhs = [0.0] * len(edges)
    target = [0.0] * len(od_rows)
    for od, i in od_rows.items():
        target[i] = demands[od] / 1e9
    bounds = limits + [(0, None)]
    first_objective = [0.0] * n + [1.0]
    first = linprog(first_objective, A_ub=ub.tocsr(), b_ub=rhs,
                    A_eq=eq.tocsr(), b_eq=target, bounds=bounds, method="highs")
    if not first.success:
        raise RuntimeError(f"min-utilization LP failed: {first.message}")
    objective = [1.0 if transit != -1 else 0.0 for _, _, transit, _ in paths] + [0.0]
    util_cap = lil_matrix((1, n + 1))
    util_cap[0, n] = 1
    second = linprog(objective,
                     A_ub=vstack((ub.tocsr(), util_cap.tocsr())),
                     b_ub=rhs + [first.x[n] + max(1e-9, first.x[n] * 1e-6)],
                     A_eq=eq.tocsr(), b_eq=target, bounds=bounds, method="highs")
    if not second.success:
        raise RuntimeError(f"minimum-transit LP failed: {second.message}")
    weights = {}
    for i, (src, dst, transit, _) in enumerate(paths):
        weights[src, dst, transit] = max(0.0, second.x[i] / (demands[src, dst] / 1e9))
    return weights, first.x[n]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology", required=True)
    parser.add_argument("--matrix", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--s", type=float, default=0.0)
    args = parser.parse_args()
    _, capacities, _ = read_topology(args.topology)
    demands = {}
    with open(args.matrix, newline="") as stream:
        for row in csv.DictReader(stream):
            src, dst = int(row["src_leaf"]), int(row["dst_leaf"])
            demands[src, dst] = float(row["bps"])
    weights, max_util = solve(capacities, demands, args.s)
    with open(args.output, "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("src_leaf", "dst_leaf", "transit_leaf", "weight"))
        for (src, dst, transit), weight in sorted(weights.items()):
            writer.writerow((src, dst, transit, f"{weight:.17g}"))
    print(f"TE LP solved: {len(demands)} active ODs; minimum max utilization={max_util:.6g}")


if __name__ == "__main__":
    main()
