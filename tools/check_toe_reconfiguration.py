#!/usr/bin/env python3
"""Check v5 ToE port reuse and topology audit logs without rerunning simulation."""
import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def endpoints(circuits):
    return [(a, ap) for a, ap, b, bp in circuits] + [(b, bp) for a, ap, b, bp in circuits]


def topology(circuits):
    return Counter((a, b) for a, ap, b, bp in circuits)


def validate(circuits, label):
    ports = endpoints(circuits)
    require(len(circuits) == 640, f"{label}: expected 640 circuits")
    require(len(set(ports)) == 1280, f"{label}: repeated physical port")
    require(Counter(leaf for leaf, port in ports) == Counter({leaf: 64 for leaf in range(37, 57)}),
            f"{label}: leaf degree must be 64")
    require(len(topology(circuits)) == 160, f"{label}: incomplete leaf-pair coverage")
    require(all(37 <= a < b <= 56 and (a - 37) // 4 != (b - 37) // 4
                for a, ap, b, bp in circuits), f"{label}: invalid leaf pair")


def check(case_path, toe_dir=None):
    directory = toe_dir or case_path / "toe"
    circuits = defaultdict(list)
    with (directory / "toe_circuits.csv").open(newline="") as file:
        for row in csv.DictReader(file):
            key = int(row["epoch"]), row["status"]
            circuits[key].append(tuple(int(row[name]) for name in
                                       ("src_leaf", "src_port", "dst_leaf", "dst_port")))
    expected = defaultdict(dict)
    with (directory / "toe_topology.csv").open(newline="") as file:
        for row in csv.DictReader(file):
            key = int(row["epoch"]), row["status"]
            pair = int(row["src_leaf"]), int(row["dst_leaf"])
            require(pair not in expected[key], f"duplicate topology row: {key}, {pair}")
            count = int(row["circuit_count"])
            require(float(row["capacity_gbps_per_direction"]) == count * 400,
                    f"{key}: inconsistent capacity")
            expected[key][pair] = count
    audits = {}
    with (directory / "toe_reconfiguration.csv").open(newline="") as file:
        for row in csv.DictReader(file):
            key = int(row["epoch"]), row["status"]
            require(key not in audits, f"duplicate reconfiguration row: {key}")
            audits[key] = row
    summaries = {}
    with (directory / "toe_summary.csv").open(newline="") as file:
        for row in csv.DictReader(file):
            key = int(row["epoch"]), row["status"]
            require(key not in summaries, f"duplicate summary row: {key}")
            summaries[key] = row

    current = circuits[(0, "initial")]
    validate(current, "initial")
    applied_count = 0
    for epoch in sorted({epoch for epoch, status in summaries}):
        solved = summaries.get((epoch, "solved"))
        if solved is None:
            print(f"WARNING epoch={epoch}: solver failed; no topology applied")
            continue
        changed = int(solved["changed_circuits"])
        if changed == 0:
            require(topology(current) == expected.get((epoch, "applied")),
                    f"epoch={epoch}: no-op topology differs from current ports")
            continue
        retained = circuits[(epoch, "retained")]
        removed = circuits[(epoch, "remove")]
        added = circuits[(epoch, "add")]
        require(Counter(retained) + Counter(removed) == Counter(current),
                f"epoch={epoch}: retained/remove do not partition current connections")
        require(len(removed) == len(added) == changed,
                f"epoch={epoch}: physical rewire count differs from solver")
        require(Counter(endpoints(removed)) == Counter(endpoints(added)),
                f"epoch={epoch}: new connections do not use exactly released ports")
        planned = retained + added
        validate(planned, f"epoch={epoch} plan")
        require(topology(planned) == expected.get((epoch, "target")),
                f"epoch={epoch}: planned ports differ from target counts")
        require(len(retained) == sum(min(count, expected[(epoch, "target")][pair])
                                     for pair, count in topology(current).items()),
                f"epoch={epoch}: reusable physical circuits were unnecessarily replaced")
        if (epoch, "applied") not in summaries:
            require(not circuits[(epoch, "applied")], f"epoch={epoch}: unexpected applied ports")
            print(f"WARNING epoch={epoch}: planned reconfiguration timed out or did not finish")
            continue
        actual = circuits[(epoch, "applied")]
        validate(actual, f"epoch={epoch} applied")
        require(Counter(actual) == Counter(planned), f"epoch={epoch}: actual ports differ from plan")
        require(topology(actual) == expected.get((epoch, "applied")),
                f"epoch={epoch}: applied counts differ from actual ports")
        audit = audits[(epoch, "applied")]
        require(int(audit["retained_circuits"]) == len(retained) and
                int(audit["rewired_circuits"]) == changed,
                f"epoch={epoch}: inconsistent audit counts")
        paused, total = int(audit["paused_tp_channels"]), int(audit["total_tp_channels"])
        require(0 <= paused <= total, f"epoch={epoch}: invalid TP counts")
        if audit["scope"] == "global":
            require(paused == total, f"epoch={epoch}: global scope must pause all TPs")
        else:
            require(audit["scope"] == "selective", f"epoch={epoch}: unknown drain scope")
        current = actual
        applied_count += 1
        print(f"PASS epoch={epoch} scope={audit['scope']} retained={len(retained)} "
              f"rewired={changed} paused_tp={paused}/{total} "
              f"unpaused_payload_bytes={audit['unpaused_new_payload_bytes']}")
    print(f"PASS: {applied_count} applied reconfigurations verified; final circuits=640")
    return applied_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case-path", required=True, type=Path)
    parser.add_argument("--toe-dir", type=Path, help="override --toe-output directory")
    args = parser.parse_args()
    try:
        check(args.case_path, args.toe_dir)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"FAIL: {error}\n")


if __name__ == "__main__":
    main()
