#!/usr/bin/env python3
"""Generate exact destination-port routes for the Mooncake OCS topology.

TE chooses the first logical OCS edge at the source leaf; these ordinary
shortest routes remain necessary for host egress, the final leaf-to-host hop,
and for the simulation when --jupiter-te is off.
"""

import argparse
import csv
from collections import defaultdict
from pathlib import Path


def generate_routing_table(topology_path, output_path):
    host_ports = defaultdict(list)
    leaf_host_ports = {}
    leaf_links = defaultdict(list)
    leaves = set()
    with open(topology_path, newline="") as stream:
        for row in csv.DictReader(stream):
            a, ap = int(row["nodeId1"]), int(row["portId1"])
            b, bp = int(row["nodeId2"]), int(row["portId2"])
            if a >= 37 and b >= 37:
                leaf_links[a, b].append(ap)
                leaf_links[b, a].append(bp)
                leaves.update((a, b))
            else:
                host, hp, leaf, lp = (a, ap, b, bp) if a < 37 else (b, bp, a, ap)
                host_ports[host].append((hp, leaf))
                leaf_host_ports[host, hp] = lp
                leaves.add(leaf)
    if not leaves or not host_ports:
        raise ValueError("topology lacks hosts or leaves")
    leaves = sorted(leaves)

    def distance(a, b):
        return 0 if a == b else (1 if (a, b) in leaf_links else 2)

    with open(output_path, "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"))
        for dest, destination_ports in sorted(host_ports.items()):
            for dp, final_leaf in sorted(destination_ports):
                for leaf in leaves:
                    if leaf == final_leaf:
                        outports = [leaf_host_ports[dest, dp]]
                        metric = 1
                    else:
                        next_leaves = [neighbour for neighbour in leaves
                                       if (leaf, neighbour) in leaf_links
                                       and distance(neighbour, final_leaf) == distance(leaf, final_leaf) - 1]
                        outports = [p for neighbour in next_leaves
                                    for p in leaf_links[leaf, neighbour]]
                        metric = 1 + distance(leaf, final_leaf)
                    if not outports:
                        raise ValueError(f"no leaf route {leaf} to {dest}:{dp}")
                    writer.writerow((leaf, dest, dp,
                                     " ".join(map(str, sorted(outports))),
                                     " ".join([str(metric)] * len(outports))))
                for source, source_ports in sorted(host_ports.items()):
                    if source == dest:
                        continue
                    best = min(distance(leaf, final_leaf) for _, leaf in source_ports)
                    outports = sorted(p for p, leaf in source_ports
                                      if distance(leaf, final_leaf) == best)
                    writer.writerow((source, dest, dp,
                                     " ".join(map(str, outports)),
                                     " ".join([str(best + 2)] * len(outports))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate_routing_table(args.topology, args.output)


if __name__ == "__main__":
    main()
