#!/usr/bin/env python3
import csv
from pathlib import Path
from collections import defaultdict

# ============================================================
# Global parameters
# ============================================================
NUM_COMPUTE_SERVERS = 16
COMPUTE_SERVER_PORTS = 64
NUM_STORAGE_SERVERS = 21
STORAGE_SERVER_PORTS = 12
NUM_COMPUTE_LEAF = 16
NUM_STORAGE_LEAF = 4
LEAF_TOTAL_PORTS = 128

NUM_LEAF_GROUPS = 5
LEAVES_PER_GROUP = 4
OCS_PORT_START = 64
PARALLEL_OCS_LINKS_PER_PAIR = 4

LINK_RATE_GBPS = 400
LINK_DELAY_US = 1.0

PROJECT_ROOT = Path(__file__).resolve().parents[1]
CASE_DIR = PROJECT_ROOT / "ns-3-ub" / "scratch" / "mooncake_pd_storage_ocs_topology"


def generate_ids():
    compute_server_ids = list(range(0, 16))
    storage_server_ids = list(range(16, 37))
    compute_leaf_ids = list(range(37, 53))
    storage_leaf_ids = list(range(53, 57))
    return compute_server_ids, storage_server_ids, compute_leaf_ids, storage_leaf_ids


# ============================================================
# Compute Server <-> Compute Leaf
# Same mapping as the previous topology
# ============================================================
def generate_compute_links(links, link_id, compute_server_ids, compute_leaf_ids):
    SERVERS_PER_GROUP = 4
    LEAFS_PER_GROUP = 4
    LINKS_PER_VISIT = 2
    NUM_ROUNDS = 8

    for group_index in range(4):
        server_group = compute_server_ids[group_index * 4:(group_index + 1) * 4]
        leaf_group = compute_leaf_ids[group_index * 4:(group_index + 1) * 4]

        for local_server_index, server_id in enumerate(server_group):
            for round_index in range(NUM_ROUNDS):
                for leaf_offset, leaf_id in enumerate(leaf_group):
                    for lane in range(LINKS_PER_VISIT):
                        server_port = (
                            round_index * LEAFS_PER_GROUP * LINKS_PER_VISIT
                            + leaf_offset * LINKS_PER_VISIT
                            + lane
                        )
                        leaf_port = (
                            local_server_index * NUM_ROUNDS * LINKS_PER_VISIT
                            + round_index * LINKS_PER_VISIT
                            + lane
                        )
                        links.append({
                            "link_id": link_id,
                            "src": server_id,
                            "src_port": server_port,
                            "dst": leaf_id,
                            "dst_port": leaf_port,
                            "rate_gbps": LINK_RATE_GBPS,
                            "delay_us": LINK_DELAY_US,
                            "link_type": "compute_leaf",
                        })
                        link_id += 1
    return link_id


# ============================================================
# Storage Server <-> Storage Leaf
# Same mapping as the previous topology
# ============================================================
def generate_storage_links(links, link_id, storage_server_ids, storage_leaf_ids):
    LINKS_PER_LEAF = 3
    next_leaf_port = {leaf: 0 for leaf in storage_leaf_ids}

    for server_id in storage_server_ids:
        for leaf_index, leaf_id in enumerate(storage_leaf_ids):
            for offset in range(LINKS_PER_LEAF):
                server_port = leaf_index * LINKS_PER_LEAF + offset
                leaf_port = next_leaf_port[leaf_id]

                if leaf_port >= 63:
                    raise RuntimeError(f"Storage Leaf {leaf_id} exceeds ports 0~62")

                links.append({
                    "link_id": link_id,
                    "src": server_id,
                    "src_port": server_port,
                    "dst": leaf_id,
                    "dst_port": leaf_port,
                    "rate_gbps": LINK_RATE_GBPS,
                    "delay_us": LINK_DELAY_US,
                    "link_type": "storage_leaf",
                })
                next_leaf_port[leaf_id] += 1
                link_id += 1

    for leaf_id in storage_leaf_ids:
        if next_leaf_port[leaf_id] != 63:
            raise RuntimeError(f"Storage Leaf {leaf_id}: expected 63 downlinks")

    return link_id


# ============================================================
# OCS Leaf <-> Leaf
# 5 groups x 4 Leafs, no intra-group links
# Every allowed inter-group Leaf pair has 4 parallel circuits
# ============================================================
def generate_ocs_links(links, link_id, all_leaf_ids):
    if len(all_leaf_ids) != 20:
        raise RuntimeError(f"Expected 20 Leafs, got {len(all_leaf_ids)}")

    groups = [
        all_leaf_ids[g * LEAVES_PER_GROUP:(g + 1) * LEAVES_PER_GROUP]
        for g in range(NUM_LEAF_GROUPS)
    ]
    leaf_to_group = {
        leaf: g
        for g, group in enumerate(groups)
        for leaf in group
    }

    print("OCS groups:")
    for g, group in enumerate(groups):
        print(f"  Group {g}: {group}")

    next_ocs_port = {leaf: OCS_PORT_START for leaf in all_leaf_ids}
    pair_count = 0
    ocs_link_count = 0

    for i, leaf_a in enumerate(all_leaf_ids):
        for leaf_b in all_leaf_ids[i + 1:]:
            if leaf_to_group[leaf_a] == leaf_to_group[leaf_b]:
                continue

            pair_count += 1

            for parallel_index in range(PARALLEL_OCS_LINKS_PER_PAIR):
                port_a = next_ocs_port[leaf_a]
                port_b = next_ocs_port[leaf_b]

                if port_a > 127 or port_b > 127:
                    raise RuntimeError("OCS Leaf port overflow")

                links.append({
                    "link_id": link_id,
                    "src": leaf_a,
                    "src_port": port_a,
                    "dst": leaf_b,
                    "dst_port": port_b,
                    "rate_gbps": LINK_RATE_GBPS,
                    "delay_us": LINK_DELAY_US,
                    "link_type": "ocs_leaf_leaf",
                    "parallel_index": parallel_index,
                })

                next_ocs_port[leaf_a] += 1
                next_ocs_port[leaf_b] += 1
                link_id += 1
                ocs_link_count += 1

    if pair_count != 160:
        raise RuntimeError(f"Expected 160 inter-group Leaf pairs, got {pair_count}")
    if ocs_link_count != 640:
        raise RuntimeError(f"Expected 640 OCS links, got {ocs_link_count}")

    for leaf in all_leaf_ids:
        if next_ocs_port[leaf] != 128:
            raise RuntimeError(f"Leaf {leaf}: OCS ports 64~127 not fully used")

    return link_id


# ============================================================
# Validation
# ============================================================
def validate_topology(links, compute_server_ids, storage_server_ids,
                      compute_leaf_ids, storage_leaf_ids):
    all_leaf_ids = compute_leaf_ids + storage_leaf_ids

    expected_compute = 16 * 64       # 1024
    expected_storage = 21 * 12       # 252
    expected_ocs = 160 * 4           # 640
    expected_total = 1916

    counts = defaultdict(int)
    for link in links:
        counts[link["link_type"]] += 1

    if counts["compute_leaf"] != expected_compute:
        raise RuntimeError("Wrong compute-link count")
    if counts["storage_leaf"] != expected_storage:
        raise RuntimeError("Wrong storage-link count")
    if counts["ocs_leaf_leaf"] != expected_ocs:
        raise RuntimeError("Wrong OCS-link count")
    if len(links) != expected_total:
        raise RuntimeError(f"Expected {expected_total} links, got {len(links)}")

    port_limits = {}
    for n in compute_server_ids:
        port_limits[n] = 64
    for n in storage_server_ids:
        port_limits[n] = 12
    for n in all_leaf_ids:
        port_limits[n] = 128

    used = defaultdict(set)

    for link in links:
        for node, port in [
            (link["src"], link["src_port"]),
            (link["dst"], link["dst_port"]),
        ]:
            if not (0 <= port < port_limits[node]):
                raise RuntimeError(f"Port out of range: node={node}, port={port}")
            if port in used[node]:
                raise RuntimeError(f"Port reused: node={node}, port={port}")
            used[node].add(port)

    for n in compute_server_ids:
        if len(used[n]) != 64:
            raise RuntimeError(f"Compute Server {n}: {len(used[n])}/64 ports used")

    for n in storage_server_ids:
        if len(used[n]) != 12:
            raise RuntimeError(f"Storage Server {n}: {len(used[n])}/12 ports used")

    for n in compute_leaf_ids:
        if used[n] != set(range(128)):
            raise RuntimeError(f"Compute Leaf {n}: expected ports 0~127 fully used")

    expected_storage_leaf_ports = set(range(0, 63)) | set(range(64, 128))
    for n in storage_leaf_ids:
        if used[n] != expected_storage_leaf_ports:
            raise RuntimeError(
                f"Storage Leaf {n}: expected 0~62 and 64~127 used; port 63 unused"
            )

    print("Topology validation PASSED")
    print(f"  Compute links : {expected_compute}")
    print(f"  Storage links : {expected_storage}")
    print(f"  OCS links     : {expected_ocs}")
    print(f"  Total links   : {expected_total}")


# ============================================================
# Write node.csv
# ============================================================
def generate_node_csv(path, compute_server_ids, storage_server_ids,
                      compute_leaf_ids, storage_leaf_ids):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["nodeId", "nodeType", "portNum", "allocationDelay", "forwardDelay"])
        w.writerow([f"{compute_server_ids[0]}..{compute_server_ids[-1]}",
                    "DEVICE", 64, "1ns", ""])
        w.writerow([f"{storage_server_ids[0]}..{storage_server_ids[-1]}",
                    "DEVICE", 12, "1ns", ""])
        w.writerow([f"{compute_leaf_ids[0]}..{compute_leaf_ids[-1]}",
                    "SWITCH", 128, "1ns", ""])
        w.writerow([f"{storage_leaf_ids[0]}..{storage_leaf_ids[-1]}",
                    "SWITCH", 128, "1ns", ""])


# ============================================================
# Write topology.csv
# ============================================================
def generate_topology_csv(links, path):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay"])
        for link in links:
            w.writerow([
                link["src"],
                link["src_port"],
                link["dst"],
                link["dst_port"],
                f'{link["rate_gbps"]}Gbps',
                f'{link["delay_us"]:g}us',
            ])


# ============================================================
# Main
# ============================================================
def main():
    CASE_DIR.mkdir(parents=True, exist_ok=True)

    compute_server_ids, storage_server_ids, compute_leaf_ids, storage_leaf_ids = generate_ids()
    all_leaf_ids = compute_leaf_ids + storage_leaf_ids

    print("Node allocation:")
    print("  Compute Servers : 0..15")
    print("  Storage Servers : 16..36")
    print("  Compute Leafs   : 37..52")
    print("  Storage Leafs   : 53..56")
    print("  Spines          : none")
    print()

    links = []
    link_id = 0

    link_id = generate_compute_links(
        links, link_id, compute_server_ids, compute_leaf_ids
    )
    link_id = generate_storage_links(
        links, link_id, storage_server_ids, storage_leaf_ids
    )
    link_id = generate_ocs_links(
        links, link_id, all_leaf_ids
    )

    validate_topology(
        links,
        compute_server_ids,
        storage_server_ids,
        compute_leaf_ids,
        storage_leaf_ids,
    )

    node_file = CASE_DIR / "node.csv"
    topology_file = CASE_DIR / "topology.csv"

    generate_node_csv(
        node_file,
        compute_server_ids,
        storage_server_ids,
        compute_leaf_ids,
        storage_leaf_ids,
    )
    generate_topology_csv(links, topology_file)

    print()
    print("Generated:")
    print(" ", node_file)
    print(" ", topology_file)
    print(f"Total physical links: {len(links)}")


if __name__ == "__main__":
    main()
