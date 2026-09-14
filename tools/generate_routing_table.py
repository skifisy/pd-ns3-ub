#!/usr/bin/env python3
"""Generate routing_table.csv for the Jupiter/Mooncake OCS case.

The historical-TE implementation chooses the first logical OCS hop at the
source leaf.  The ordinary routing table is still required for host egress,
leaf forwarding after the TE-selected hop, exact destination-port delivery,
and runs with --jupiter-te disabled.

By default this script regenerates routing_table.csv in the case produced by
``tools/generate_ocs_topology.py``.  It can also be pointed at another copy of
the same topology layout with ``--case-path`` or explicit input/output paths.
"""

import argparse
import csv
from collections import defaultdict
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CASE_DIR = PROJECT_ROOT / "ns-3-ub" / "scratch" / "mooncake_pd_storage_ocs_topology"
FIRST_LEAF_ID = 37


def generate_routing_table(topology_path: Path, output_path: Path) -> None:
    """Generate exact destination-port shortest routes for the static OCS topology."""
    topology_path = Path(topology_path)
    output_path = Path(output_path)

    host_ports = defaultdict(list)
    leaf_host_ports = {}
    leaf_links = defaultdict(list)
    leaves = set()

    with topology_path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            a, ap = int(row["nodeId1"]), int(row["portId1"])
            b, bp = int(row["nodeId2"]), int(row["portId2"])

            if a >= FIRST_LEAF_ID and b >= FIRST_LEAF_ID:
                leaf_links[a, b].append(ap)
                leaf_links[b, a].append(bp)
                leaves.update((a, b))
            else:
                host, hp, leaf, lp = (
                    (a, ap, b, bp) if a < FIRST_LEAF_ID else (b, bp, a, ap)
                )
                host_ports[host].append((hp, leaf))
                leaf_host_ports[host, hp] = lp
                leaves.add(leaf)

    if not leaves or not host_ports:
        raise ValueError("topology lacks hosts or leaves")

    leaves = sorted(leaves)

    def distance(a: int, b: int) -> int:
        # In this Jupiter topology every pair is either directly connected or
        # reachable through exactly one transit leaf.
        return 0 if a == b else (1 if (a, b) in leaf_links else 2)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"))

        for dest, destination_ports in sorted(host_ports.items()):
            for dp, final_leaf in sorted(destination_ports):
                # Leaf forwarding rules.  Keep every parallel physical port on
                # each equal-cost next logical hop so the existing flow hash can
                # perform ECMP beneath TE/WCMP.
                for leaf in leaves:
                    if leaf == final_leaf:
                        outports = [leaf_host_ports[dest, dp]]
                        metric = 1
                    else:
                        next_leaves = [
                            neighbour
                            for neighbour in leaves
                            if (leaf, neighbour) in leaf_links
                            and distance(neighbour, final_leaf)
                            == distance(leaf, final_leaf) - 1
                        ]
                        outports = [
                            port
                            for neighbour in next_leaves
                            for port in leaf_links[leaf, neighbour]
                        ]
                        metric = 1 + distance(leaf, final_leaf)

                    if not outports:
                        raise ValueError(f"no leaf route {leaf} to {dest}:{dp}")

                    writer.writerow(
                        (
                            leaf,
                            dest,
                            dp,
                            " ".join(map(str, sorted(outports))),
                            " ".join([str(metric)] * len(outports)),
                        )
                    )

                # Host egress rules.  A multi-port host may reach the same
                # destination through several equally short attached leaves.
                for source, source_ports in sorted(host_ports.items()):
                    if source == dest:
                        continue
                    best = min(distance(leaf, final_leaf) for _, leaf in source_ports)
                    outports = sorted(
                        port
                        for port, leaf in source_ports
                        if distance(leaf, final_leaf) == best
                    )
                    writer.writerow(
                        (
                            source,
                            dest,
                            dp,
                            " ".join(map(str, outports)),
                            " ".join([str(best + 2)] * len(outports)),
                        )
                    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--case-path",
        type=Path,
        default=DEFAULT_CASE_DIR,
        help=(
            "case directory containing topology.csv; default: "
            "ns-3-ub/scratch/mooncake_pd_storage_ocs_topology"
        ),
    )
    parser.add_argument(
        "--topology",
        type=Path,
        default=None,
        help="explicit topology.csv path (overrides --case-path/topology.csv)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="explicit output path (default: --case-path/routing_table.csv)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    case_path = args.case_path.resolve()
    topology_path = (args.topology or (case_path / "topology.csv")).resolve()
    output_path = (args.output or (case_path / "routing_table.csv")).resolve()

    if not topology_path.is_file():
        raise FileNotFoundError(
            f"topology.csv not found: {topology_path}. "
            "Run tools/generate_ocs_topology.py first or pass --topology."
        )

    generate_routing_table(topology_path, output_path)
    print(f"Generated routing table: {output_path}")


if __name__ == "__main__":
    main()
