"""Read-only summaries and validation for prepared ns-3-ub cases."""

from __future__ import annotations

import csv
import json
import struct
from pathlib import Path

from case_topology_loader import load_case_topology
from compressed_routing import read_compressed_routing_artifact
from net_sim_builder import parse_id_range
from structured_case import PROTOCOL_VERSION
from topology_source import topology_summary

SUPPORTED_STRUCTURED_VERSIONS = {1, 2, 3, PROTOCOL_VERSION}


def _count_csv_rows(path: Path) -> int:
    with path.open(newline="", encoding="utf-8") as file:
        reader = csv.reader(file)
        next(reader, None)
        return sum(1 for _ in reader)


def _read_required_csv(path: Path, required_fields: set[str]):
    try:
        file = path.open(newline="", encoding="utf-8")
    except OSError:
        raise
    with file:
        reader = csv.DictReader(file)
        fields = set(reader.fieldnames or [])
        missing = sorted(required_fields - fields)
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(missing)}")
        try:
            yield from enumerate(reader, start=2)
        except csv.Error as error:
            raise ValueError(f"{path}: cannot parse CSV: {error}") from error


def _validate_traffic(path: Path, node_ids: set[int]) -> int:
    required = {
        "taskId", "sourceNodeId", "destNodeId", "dataSize(Byte)", "opType",
        "priority", "delay", "phaseId", "dependOnPhases",
    }
    count = 0
    for row_num, row in _read_required_csv(path, required):
        try:
            source = int(row["sourceNodeId"])
            destination = int(row["destNodeId"])
            data_size = int(row["dataSize(Byte)"])
            int(row["taskId"])
            int(row["phaseId"])
            if row["dependOnPhases"].strip():
                int(row["dependOnPhases"])
        except (TypeError, ValueError) as error:
            raise ValueError(f"{path}:{row_num}: invalid numeric field") from error
        if source not in node_ids:
            raise ValueError(f"{path}:{row_num}: unknown sourceNodeId {source}")
        if destination not in node_ids:
            raise ValueError(f"{path}:{row_num}: unknown destNodeId {destination}")
        if data_size < 0:
            raise ValueError(f"{path}:{row_num}: dataSize(Byte) must be non-negative")
        count += 1
    return count


def _validate_route_table(path: Path, port_counts: dict[int, int]) -> int:
    required = {"nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"}
    node_ids = set(port_counts)
    count = 0
    for row_num, row in _read_required_csv(path, required):
        try:
            node_start, node_end = parse_id_range(row["nodeId"])
            destination_start, destination_end = parse_id_range(row["dstNodeId"])
            destination_port_start, destination_port_end = parse_id_range(row["dstPortId"])
            out_ports = [int(value) for value in row["outPorts"].split()]
            metrics = [int(value) for value in row["metrics"].split()]
        except (TypeError, ValueError) as error:
            raise ValueError(f"{path}:{row_num}: invalid route row") from error
        sources = range(node_start, node_end + 1)
        destinations = range(destination_start, destination_end + 1)
        if any(node not in node_ids for node in sources):
            raise ValueError(f"{path}:{row_num}: nodeId references an unknown node")
        if any(node not in node_ids for node in destinations):
            raise ValueError(f"{path}:{row_num}: dstNodeId references an unknown node")
        if not out_ports or len(out_ports) != len(metrics):
            raise ValueError(
                f"{path}:{row_num}: outPorts and metrics must be non-empty and equal length"
            )
        if any(metric < 0 for metric in metrics):
            raise ValueError(f"{path}:{row_num}: metrics must be non-negative")
        for source in sources:
            for out_port in out_ports:
                if not 0 <= out_port < port_counts[source]:
                    raise ValueError(
                        f"{path}:{row_num}: out port {out_port} is invalid for node {source}"
                    )
        for destination in destinations:
            if destination_port_start < 0 or destination_port_end >= port_counts[destination]:
                raise ValueError(
                    f"{path}:{row_num}: destination port range is invalid for node {destination}"
                )
        count += 1
    return count


def _structured_topology_version(path: Path) -> int:
    with path.open(encoding="utf-8") as file:
        first = file.readline().strip().split()
    if len(first) != 2 or first[0] != "NS3_UB_STRUCTURED":
        raise ValueError(
            "structured_topology.txt must start with NS3_UB_STRUCTURED <version>"
        )
    try:
        version = int(first[1])
    except ValueError as error:
        raise ValueError("structured_topology.txt version must be an integer") from error
    if version not in SUPPORTED_STRUCTURED_VERSIONS:
        raise ValueError(f"unsupported structured topology version: {version}")
    return version


def _structured_topology_counts(path: Path) -> dict[str, int]:
    node_count = None
    link_count = None
    with path.open(encoding="utf-8") as file:
        _structured_topology_version(path)
        for line in file:
            fields = line.split()
            if len(fields) == 2 and fields[0] == "NODES":
                node_count = int(fields[1])
            elif len(fields) == 2 and fields[0] == "LINKS":
                link_count = int(fields[1])
                break
    if node_count is None or link_count is None:
        raise ValueError("structured_topology.txt is missing NODES or LINKS counts")
    return {"nodes": node_count, "links": link_count}


def inspect_case(case_path: str | Path) -> dict:
    case = Path(case_path).expanduser().resolve()
    if not case.is_dir():
        raise ValueError(f"case directory does not exist: {case}")
    csv_case = (case / "node.csv").is_file() or (case / "topology.csv").is_file()
    structured_case = any(
        (case / name).is_file()
        for name in ("structured_topology.txt", "compressed_routing.bin")
    )
    structured_required = [
        "structured_topology.txt", "compressed_routing.bin", "structured_summary.json"
    ]
    if structured_case:
        if not (case / "structured_topology.txt").is_file():
            raise ValueError(
                "case is missing required file: structured_topology.txt"
            )
        version = _structured_topology_version(case / "structured_topology.txt")
        required = (
            structured_required
            if version == PROTOCOL_VERSION
            else ["structured_topology.txt"]
        )
        missing = [name for name in required if not (case / name).is_file()]
        if missing:
            raise ValueError(f"case is missing required files: {', '.join(missing)}")
        format_name = "structured"
    elif csv_case:
        required = ["node.csv", "topology.csv"]
        format_name = "csv"
    else:
        raise ValueError("case must contain CSV or structured topology artifacts")

    missing = [name for name in required if not (case / name).is_file()]
    if missing:
        raise ValueError(f"case is missing required files: {', '.join(missing)}")

    graph = load_case_topology(case) if format_name == "csv" else None
    result = {"casePath": str(case), "format": format_name, "files": required}
    if graph is not None:
        result["topology"] = topology_summary(graph)
    else:
        summary_path = case / "structured_summary.json"
        if summary_path.is_file():
            try:
                summary = json.loads(summary_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise ValueError(f"cannot read structured_summary.json: {error}") from error
        else:
            summary = {}
        result["structuredVersion"] = _structured_topology_version(
            case / "structured_topology.txt"
        )
        result["summary"] = summary
        result["topology"] = _structured_topology_counts(case / "structured_topology.txt")
    traffic = case / "traffic.csv"
    if traffic.is_file():
        result["trafficTasks"] = _count_csv_rows(traffic)
    route_table = case / "routing_table.csv"
    if route_table.is_file():
        result["routing"] = {"routingTableRows": _count_csv_rows(route_table)}
        topology_rules = case / "routing_topology_rules.csv"
        if topology_rules.is_file():
            result["routing"]["topologyRuleRows"] = _count_csv_rows(topology_rules)
    elif format_name == "structured":
        result["routing"] = {
            "routeGeneration": summary.get("routeGeneration"),
            "compressedRouting": summary.get("compressedRouting"),
            "tableLookupCount": summary.get("tableLookupCount"),
            "routingRuleCount": summary.get("routingRuleCount"),
            "routingRangeRuleCount": summary.get("routingRangeRuleCount"),
            "topologyRuleCount": summary.get("topologyRuleCount"),
        }
    return result


def check_case(case_path: str | Path) -> dict:
    result = inspect_case(case_path)
    case = Path(result["casePath"])
    errors: list[str] = []
    required_runtime_files = ["network_attribute.txt", "traffic.csv"]
    if result["format"] == "csv":
        required_runtime_files.append("routing_table.csv")
    missing_runtime_files = [
        name for name in required_runtime_files if not (case / name).is_file()
    ]
    if missing_runtime_files:
        errors.append(
            "case is missing required runtime files: " + ", ".join(missing_runtime_files)
        )
    if result["format"] == "csv":
        has_port_groups = (case / "routing_port_groups.csv").is_file()
        has_rules = (case / "routing_rules.csv").is_file()
        has_range_rules = (case / "routing_rule_ranges.csv").is_file()
        topology_rules = case / "routing_topology_rules.csv"
        if has_port_groups != has_rules:
            errors.append(
                "routing_port_groups.csv and routing_rules.csv must be provided together"
            )
        if has_range_rules and not has_port_groups:
            errors.append(
                "routing_rule_ranges.csv requires routing_port_groups.csv and routing_rules.csv"
            )
        if topology_rules.is_file() and _count_csv_rows(topology_rules) > 0:
            errors.append(
                "CSV runtime does not load routing_topology_rules.csv; use a structured case "
                "for structured or mixed routing, or regenerate the CSV case with full routing"
            )
    if result["format"] == "csv":
        try:
            graph = load_case_topology(case)
            port_counts = {
                node_id: graph.port_list[node_id][1] for node_id in graph.nodes
            }
            if (case / "traffic.csv").is_file():
                _validate_traffic(case / "traffic.csv", set(port_counts))
            if (case / "routing_table.csv").is_file():
                _validate_route_table(case / "routing_table.csv", port_counts)
        except (OSError, TypeError, ValueError) as error:
            errors.append(str(error))
    elif (case / "traffic.csv").is_file():
        try:
            _validate_traffic(
                case / "traffic.csv", set(range(result["topology"]["nodes"]))
            )
        except (OSError, TypeError, ValueError) as error:
            errors.append(str(error))
    if result["format"] == "structured" and result["structuredVersion"] == PROTOCOL_VERSION:
        summary = result["summary"]
        summary_version = summary.get("protocolVersion")
        if summary_version != PROTOCOL_VERSION:
            errors.append(
                f"structured_summary.json protocolVersion must be {PROTOCOL_VERSION}"
            )
        fingerprint = summary.get("cacheKey")
        if not isinstance(fingerprint, str):
            errors.append("structured_summary.json cacheKey is missing")
        else:
            try:
                read_compressed_routing_artifact(
                    (case / "compressed_routing.bin").read_bytes(), fingerprint
                )
            except (OSError, ValueError, struct.error) as error:
                errors.append(f"compressed_routing.bin is invalid: {error}")
    route_table = case / "routing_table.csv"
    if route_table.is_file():
        try:
            _count_csv_rows(route_table)
        except (OSError, csv.Error) as error:
            errors.append(f"routing_table.csv cannot be read: {error}")
    if errors:
        raise ValueError("; ".join(errors))
    result["valid"] = True
    return result
