import csv
import functools
import hashlib
import io
import inspect
import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path

import networkx as nx

from compressed_routing import (
    build_compressed_routing_artifact,
    read_compressed_routing_artifact,
)
from route_split import (
    CompiledPortGroup,
    CompiledRouteConfig,
    CompiledRoutingRangeRule,
    CompiledRoutingRule,
    build_route_lookups,
    build_shortest_route_lookups_for_destination,
    group_route_candidates,
    is_default_shortest_path_finder,
)
from topology_routing import CompiledTopologyNode, CompiledTopologyRule


PROTOCOL_VERSION = 4
COMPILER_VERSION = 8

_ROUTE_GENERATIONS = {"full", "structured", "mixed"}
_LEGACY_ROUTING_MODES = {"table": "full", "rule": "structured", "mixed": "mixed"}


@dataclass(frozen=True)
class StructuredCompileResult:
    config: CompiledRouteConfig
    cache_key: str
    cache_hit: bool
    route_generation: str


@dataclass(frozen=True)
class PreparedStructuredCase:
    case_path: Path
    topology_path: Path
    compressed_routing_path: Path
    summary_path: Path
    compile_result: StructuredCompileResult


def _json_value(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, dict):
        return {
            str(key): _json_value(item)
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    return repr(value)


def _path_finder_fingerprint(path_finding_algo):
    if isinstance(path_finding_algo, functools.partial):
        return {
            "partial": _path_finder_fingerprint(path_finding_algo.func),
            "args": _json_value(path_finding_algo.args),
            "keywords": _json_value(path_finding_algo.keywords or {}),
        }

    implementation = inspect.unwrap(path_finding_algo)
    module = getattr(implementation, "__module__", type(implementation).__module__)
    name = getattr(implementation, "__qualname__", type(implementation).__qualname__)
    fingerprint = {"module": module, "name": name}
    source_path = inspect.getsourcefile(implementation)
    if source_path and Path(source_path).is_file():
        fingerprint["sourceSha256"] = hashlib.sha256(Path(source_path).read_bytes()).hexdigest()
    else:
        try:
            source = inspect.getsource(implementation).encode()
        except (OSError, TypeError):
            state = getattr(implementation, "__dict__", {})
            fingerprint["state"] = _json_value(state)
        else:
            fingerprint["sourceSha256"] = hashlib.sha256(source).hexdigest()
    return fingerprint


def _canonical_topology(network):
    topology = network.get_physical_topology()
    port_counts = {
        node_id: int(network.port_list[node_id][1])
        for node_id in range(len(network.port_list))
    }
    nodes = [
        {
            "nodeId": node.node_id,
            "nodeType": node.node_type,
            "portCount": port_counts[node.node_id],
            "allocationDelay": node.allocation_delay,
            "forwardDelay": node.forward_delay,
            "attributes": _json_value(node.attributes),
        }
        for node in sorted(topology.nodes, key=lambda item: item.node_id)
    ]

    links = []
    for link in topology.links:
        left = (link.left_node, link.left_port)
        right = (link.right_node, link.right_port)
        if right < left:
            left, right = right, left
        links.append({
            "endpoint1": left,
            "endpoint2": right,
            "bandwidth": link.bandwidth,
            "delay": link.delay,
            "attributes": _json_value(link.attributes),
        })
    links.sort(key=lambda item: (
        item["endpoint1"],
        item["endpoint2"],
        item["bandwidth"],
        item["delay"],
        json.dumps(item["attributes"], sort_keys=True),
    ))
    return {"nodes": nodes, "links": links}


def _cache_key(network, route_generation, path_finding_algo):
    value = {
        "compilerVersion": COMPILER_VERSION,
        "networkxVersion": nx.__version__,
        "routeGeneration": route_generation,
        "pathFinder": _path_finder_fingerprint(path_finding_algo),
        "topology": _canonical_topology(network),
    }
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _config_to_data(config):
    return {
        "mode": config.mode,
        "ruleCoverage": config.rule_coverage,
        "portGroups": [
            [group.node_id, group.group_id, list(group.ports)]
            for group in config.port_groups
        ],
        "routingRules": [
            [
                rule.current_node_id,
                rule.destination_node_id,
                rule.destination_port_id,
                list(rule.shortest_port_groups),
                list(rule.non_shortest_port_groups),
            ]
            for rule in config.routing_rules
        ],
        "routingRangeRules": [
            [
                rule.current_node_id,
                rule.destination_start_node_id,
                rule.destination_end_node_id,
                rule.destination_port_id,
                list(rule.shortest_port_groups),
                list(rule.non_shortest_port_groups),
            ]
            for rule in config.routing_range_rules
        ],
        "topologyRules": [
            [
                rule.rule_type,
                list(rule.shape),
                list(rule.connections),
                [
                    [node.node_id, node.node_type, list(node.coordinates)]
                    for node in rule.nodes
                ],
            ]
            for rule in config.topology_rules
        ],
        "tableLookups": [
            [current, destination, destination_port, sorted(port_metrics.items())]
            for (current, destination, destination_port), port_metrics
            in sorted(config.table_lookups.items())
        ],
        "ruleLookupCount": config.rule_lookup_count,
        "baselineLookupCount": config.baseline_lookup_count,
    }


def _config_from_data(data):
    table_lookups = {}
    for current, destination, destination_port, port_metrics in data["tableLookups"]:
        table_lookups[(current, destination, destination_port)] = {
            int(port): int(metric)
            for port, metric in port_metrics
        }
    return CompiledRouteConfig(
        port_groups=tuple(
            CompiledPortGroup(int(node), int(group), tuple(map(int, ports)))
            for node, group, ports in data["portGroups"]
        ),
        routing_rules=tuple(
            CompiledRoutingRule(
                int(current),
                int(destination),
                int(destination_port),
                tuple(map(int, shortest_groups)),
                tuple(map(int, non_shortest_groups)),
            )
            for current, destination, destination_port, shortest_groups, non_shortest_groups
            in data["routingRules"]
        ),
        routing_range_rules=tuple(
            CompiledRoutingRangeRule(
                int(current),
                int(destination_start),
                int(destination_end),
                int(destination_port),
                tuple(map(int, shortest_groups)),
                tuple(map(int, non_shortest_groups)),
            )
            for current, destination_start, destination_end, destination_port,
            shortest_groups, non_shortest_groups
            in data["routingRangeRules"]
        ),
        topology_rules=tuple(
            CompiledTopologyRule(
                str(rule_type),
                tuple(map(int, shape)),
                tuple(map(str, connections)),
                tuple(
                    CompiledTopologyNode(
                        int(node_id),
                        str(node_type),
                        tuple(map(int, coordinates)),
                    )
                    for node_id, node_type, coordinates in nodes
                ),
            )
            for rule_type, shape, connections, nodes in data.get("topologyRules", [])
        ),
        table_lookups=table_lookups,
        mode=str(data["mode"]),
        rule_coverage=float(data["ruleCoverage"]),
        rule_lookup_count=int(data["ruleLookupCount"]),
        baseline_lookup_count=int(data["baselineLookupCount"]),
    )


def default_cache_dir():
    root = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return root / "ns-3-ub-tools" / "structured-routing"


def _read_cache(path, cache_key):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError, KeyError, TypeError):
        return None
    if data.get("formatVersion") != 2 or data.get("cacheKey") != cache_key:
        return None
    try:
        return _config_from_data(data["config"])
    except (KeyError, TypeError, ValueError):
        return None


def _write_cache(path, cache_key, config):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {
        "formatVersion": 2,
        "cacheKey": cache_key,
        "config": _config_to_data(config),
    }
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as file:
        json.dump(data, file, sort_keys=True, separators=(",", ":"))
        temporary_path = Path(file.name)
    os.replace(temporary_path, path)


def _resolve_route_generation(route_generation, routing_mode):
    if routing_mode is not None:
        legacy_generation = _LEGACY_ROUTING_MODES.get(routing_mode)
        if legacy_generation is None:
            raise ValueError("routing_mode must be one of: table, rule, mixed")
        if route_generation != "mixed" and route_generation != legacy_generation:
            raise ValueError("route_generation and routing_mode select different strategies")
        route_generation = legacy_generation
    if route_generation not in _ROUTE_GENERATIONS:
        raise ValueError("route_generation must be one of: full, structured, mixed")
    return route_generation


def compile_structured_routes(network,
                              route_generation="mixed",
                              path_finding_algo=nx.all_shortest_paths,
                              use_cache=True,
                              cache_dir=None,
                              routing_mode=None):
    route_generation = _resolve_route_generation(route_generation, routing_mode)
    internal_mode = {
        "full": "table",
        "structured": "rule",
        "mixed": "mixed",
    }[route_generation]
    if network.next_hop_ports == []:
        network.build_graph_config()

    cache_key = _cache_key(network, route_generation, path_finding_algo)
    cache_path = Path(cache_dir or default_cache_dir()) / f"{cache_key}.json"
    if use_cache:
        cached = _read_cache(cache_path, cache_key)
        if cached is not None:
            return StructuredCompileResult(cached, cache_key, True, route_generation)

    config = network.gen_structured_route_config(
        routing_mode=internal_mode,
        write_file=False,
        path_finding_algo=path_finding_algo,
    )
    if use_cache:
        _write_cache(cache_path, cache_key, config)
    return StructuredCompileResult(config, cache_key, False, route_generation)


def _counted(values):
    values = tuple(values)
    return " ".join([str(len(values)), *(str(value) for value in values)])


def write_structured_config(network,
                            config,
                            output,
                            routing_fingerprint=None,
                            include_table_routes=True):
    if (routing_fingerprint is None or len(routing_fingerprint) != 64 or
            any(character not in "0123456789abcdefABCDEF"
                for character in routing_fingerprint)):
        raise ValueError(
            "routing_fingerprint must be a 64-character hexadecimal SHA-256 digest"
        )
    topology = network.get_physical_topology()
    port_counts = {
        node_id: int(network.port_list[node_id][1])
        for node_id in range(len(network.port_list))
    }
    def write_line(line):
        output.write(line)
        output.write("\n")

    write_line(f"NS3_UB_STRUCTURED {PROTOCOL_VERSION}")
    write_line(f"NODES {len(topology.nodes)}")
    for node in sorted(topology.nodes, key=lambda item: item.node_id):
        node_type = "DEVICE" if node.node_type == "host" else "SWITCH"
        system_id = int(node.attributes.get("system_id", 0))
        write_line(
            f"{node.node_id} {node_type} {port_counts[node.node_id]} "
            f"{node.allocation_delay} {node.forward_delay} {system_id}"
        )

    write_line(f"LINKS {len(topology.links)}")
    for link in topology.links:
        write_line(
            f"{link.left_node} {link.left_port} {link.right_node} {link.right_port} "
            f"{link.bandwidth} {link.delay}"
        )

    if routing_fingerprint is not None:
        write_line(f"ROUTING_FINGERPRINT {routing_fingerprint}")

    table_lookups = config.table_lookups if include_table_routes else {}
    write_line(f"TABLE {len(table_lookups)}")
    for (current, destination, destination_port), port_metrics in sorted(table_lookups.items()):
        candidates = group_route_candidates(port_metrics)
        if candidates is None:
            raise ValueError("compiled table lookup must contain at least one output port")
        write_line(
            f"{current} {destination} {destination_port} "
            f"{_counted(candidates.shortest_ports)} "
            f"{_counted(candidates.non_shortest_ports)}"
        )

    write_line(f"PORT_GROUPS {len(config.port_groups)}")
    for group in config.port_groups:
        write_line(f"{group.node_id} {group.group_id} {_counted(group.ports)}")

    write_line(f"RULES {len(config.routing_rules)}")
    for rule in config.routing_rules:
        write_line(
            f"{rule.current_node_id} {rule.destination_node_id} "
            f"{rule.destination_port_id} {_counted(rule.shortest_port_groups)} "
            f"{_counted(rule.non_shortest_port_groups)}"
        )
    write_line(f"RULE_RANGES {len(config.routing_range_rules)}")
    for rule in config.routing_range_rules:
        write_line(
            f"{rule.current_node_id} {rule.destination_start_node_id} "
            f"{rule.destination_end_node_id} {rule.destination_port_id} "
            f"{_counted(rule.shortest_port_groups)} "
            f"{_counted(rule.non_shortest_port_groups)}"
        )
    write_line(f"TOPOLOGY_RULES {len(config.topology_rules)}")
    for rule in config.topology_rules:
        write_line(
            f"{rule.rule_type} {_counted(rule.shape)} "
            f"{_counted(rule.connections)} {len(rule.nodes)}"
        )
        for node in rule.nodes:
            node_type = "HOST" if node.node_type == "host" else "SWITCH"
            write_line(
                f"{node.node_id} {node_type} {_counted(node.coordinates)}"
            )
    write_line("END")


def serialize_structured_config(network,
                                config,
                                routing_fingerprint=None,
                                include_table_routes=True):
    output = io.StringIO()
    write_structured_config(
        network,
        config,
        output,
        routing_fingerprint=routing_fingerprint,
        include_table_routes=include_table_routes,
    )
    return output.getvalue()


def _summary_data(compile_result, artifact=None):
    config = compile_result.config
    data = {
        "cacheKey": compile_result.cache_key,
        "cacheHit": compile_result.cache_hit,
        "compilerVersion": COMPILER_VERSION,
        "protocolVersion": PROTOCOL_VERSION,
        "routeGeneration": compile_result.route_generation,
        "effectiveRoutingMode": config.mode,
        "ruleCoverage": config.rule_coverage,
        "tableLookupCount": len(config.table_lookups),
        "routingRuleCount": len(config.routing_rules),
        "routingRangeRuleCount": len(config.routing_range_rules),
        "topologyRuleCount": len(config.topology_rules),
        "ruleLookupCount": config.rule_lookup_count,
        "baselineLookupCount": config.baseline_lookup_count,
        "portGroupCount": len(config.port_groups),
    }
    if artifact is not None:
        data["compressedRouting"] = {
            "file": "compressed_routing.bin",
            "formatMajor": 1,
            "formatMinor": 0,
            "bytes": len(artifact.data),
            "routeLookupCount": artifact.route_lookup_count,
            "routeClassCount": artifact.route_class_count,
            "rowCount": artifact.row_count,
            "destinationCount": artifact.destination_count,
            "endpointCount": artifact.endpoint_count,
            "portClassIdWidthBits": artifact.port_id_width_bits,
            "nodeClassIdWidthBits": artifact.node_id_width_bits,
        }
    return data


def _write_json_atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=path.parent,
        prefix=f".{path.name}.",
        delete=False,
    ) as file:
        json.dump(data, file, indent=2, sort_keys=True)
        file.write("\n")
        temporary_path = Path(file.name)
    os.replace(temporary_path, path)


def verify_compressed_routing(network, artifact, fingerprint, path_finding_algo):
    index = read_compressed_routing_artifact(artifact.data, fingerprint)
    topology = network.get_physical_topology()
    destination_ports = {int(node_id): set() for node_id in network.host_ids}
    for link in topology.links:
        if link.left_node in destination_ports:
            destination_ports[link.left_node].add(int(link.left_port))
        if link.right_node in destination_ports:
            destination_ports[link.right_node].add(int(link.right_port))

    complete_baseline = None
    if not is_default_shortest_path_finder(path_finding_algo):
        complete_baseline = build_route_lookups(
            network,
            path_finding_algo=path_finding_algo,
        )

    lookup_count = 0
    node_lookup_count = 0
    mismatches = []
    for destination, ports in sorted(destination_ports.items()):
        if complete_baseline is None:
            baseline = build_shortest_route_lookups_for_destination(network, destination)
        else:
            baseline = {
                lookup: metrics
                for lookup, metrics in complete_baseline.items()
                if lookup[1] == destination
            }
        for current in sorted(network.nodes):
            node_shortest = set()
            node_non_shortest = set()
            for destination_port in sorted(ports):
                lookup = (current, destination, destination_port)
                grouped = group_route_candidates(baseline.get(lookup, {}))
                expected = None if grouped is None else (
                    tuple(grouped.shortest_ports),
                    tuple(grouped.non_shortest_ports),
                )
                actual = index.lookup(*lookup)
                if actual != expected and len(mismatches) < 10:
                    mismatches.append((lookup, expected, actual))
                if expected is not None:
                    lookup_count += 1
                    node_shortest.update(expected[0])
                    node_non_shortest.update(expected[1])
            expected_node = None
            if node_shortest or node_non_shortest:
                expected_node = (
                    tuple(sorted(node_shortest)),
                    tuple(sorted(node_non_shortest)),
                )
                node_lookup_count += 1
            actual_node = index.lookup(current, destination)
            if actual_node != expected_node and len(mismatches) < 10:
                mismatches.append(((current, destination, None), expected_node, actual_node))
    if mismatches:
        raise ValueError(f"compressed routing verification failed: {mismatches}")
    if lookup_count != artifact.route_lookup_count:
        raise ValueError(
            "compressed routing verification count differs from the artifact: "
            f"{lookup_count} != {artifact.route_lookup_count}"
        )
    return {
        "routeLookupCount": lookup_count,
        "nodeLookupCount": node_lookup_count,
        "mismatchCount": 0,
    }


def prepare_structured_case(network,
                            case_path,
                            route_generation="mixed",
                            path_finding_algo=nx.all_shortest_paths,
                            use_cache=True,
                            cache_dir=None,
                            export_dir=None,
                            routing_mode=None,
                            verify_routes=False):
    compile_result = compile_structured_routes(
        network,
        route_generation=route_generation,
        path_finding_algo=path_finding_algo,
        use_cache=use_cache,
        cache_dir=cache_dir,
        routing_mode=routing_mode,
    )
    artifact = build_compressed_routing_artifact(
        network,
        compile_result.config,
        compile_result.cache_key,
        use_shortest_dag_fast_path=is_default_shortest_path_finder(path_finding_algo),
    )
    verification = None
    if verify_routes:
        verification = verify_compressed_routing(
            network,
            artifact,
            compile_result.cache_key,
            path_finding_algo,
        )
    case = Path(case_path).expanduser().resolve()
    case.mkdir(parents=True, exist_ok=True)
    topology_path = case / "structured_topology.txt"
    compressed_routing_path = case / "compressed_routing.bin"
    summary_path = case / "structured_summary.json"

    with tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=case,
        prefix=f".{topology_path.name}.",
        delete=False,
    ) as file:
        write_structured_config(
            network,
            compile_result.config,
            file,
            routing_fingerprint=compile_result.cache_key,
            include_table_routes=compile_result.route_generation != "full",
        )
        temporary_path = Path(file.name)
    os.replace(temporary_path, topology_path)
    with tempfile.NamedTemporaryFile(
        mode="wb",
        dir=case,
        prefix=f".{compressed_routing_path.name}.",
        delete=False,
    ) as file:
        file.write(artifact.data)
        temporary_path = Path(file.name)
    os.replace(temporary_path, compressed_routing_path)
    summary = _summary_data(compile_result, artifact)
    if verification is not None:
        summary["verification"] = verification
    _write_json_atomic(summary_path, summary)

    if export_dir is not None:
        export_structured_config(network, compile_result, export_dir)

    return PreparedStructuredCase(
        case,
        topology_path,
        compressed_routing_path,
        summary_path,
        compile_result,
    )


def export_structured_config(network, compile_result, output_dir):
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    topology = network.get_physical_topology()
    config = compile_result.config

    node_mapping = network.graph.get("node_mapping")
    if node_mapping is not None:
        with (output_path / "node_mapping.csv").open(
            "w", newline="", encoding="utf-8"
        ) as file:
            writer = csv.writer(file)
            writer.writerow(["group", "index", "nodeId"])
            for entry in node_mapping:
                writer.writerow([entry["group"], entry["index"], entry["nodeId"]])

    with (output_path / "node.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow([
            "nodeId", "nodeType", "portNum", "allocationDelay", "forwardDelay", "systemId"
        ])
        for node in sorted(topology.nodes, key=lambda item: item.node_id):
            writer.writerow([
                node.node_id,
                "DEVICE" if node.node_type == "host" else "SWITCH",
                network.port_list[node.node_id][1],
                node.allocation_delay,
                node.forward_delay,
                node.attributes.get("system_id", 0),
            ])

    with (output_path / "topology.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["nodeId1", "portId1", "nodeId2", "portId2", "bandwidth", "delay"])
        for link in topology.links:
            writer.writerow([
                link.left_node,
                link.left_port,
                link.right_node,
                link.right_port,
                link.bandwidth,
                link.delay,
            ])

    with (output_path / "routing_table.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["nodeId", "dstNodeId", "dstPortId", "outPorts", "metrics"])
        for (current, destination, destination_port), port_metrics in sorted(
            config.table_lookups.items()
        ):
            values = sorted(port_metrics.items())
            writer.writerow([
                current,
                destination,
                destination_port,
                " ".join(str(port) for port, _metric in values),
                " ".join(str(metric) for _port, metric in values),
            ])

    with (output_path / "routing_port_groups.csv").open(
        "w", newline="", encoding="utf-8"
    ) as file:
        writer = csv.writer(file)
        writer.writerow(["nodeId", "groupId", "ports"])
        for group in config.port_groups:
            writer.writerow([group.node_id, group.group_id, " ".join(map(str, group.ports))])

    with (output_path / "routing_rules.csv").open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow([
            "nodeId",
            "dstNodeId",
            "dstPortId",
            "shortestPortGroups",
            "nonShortestPortGroups",
        ])
        for rule in config.routing_rules:
            writer.writerow([
                rule.current_node_id,
                rule.destination_node_id,
                rule.destination_port_id,
                " ".join(map(str, rule.shortest_port_groups)),
                " ".join(map(str, rule.non_shortest_port_groups)),
            ])

    with (output_path / "routing_rule_ranges.csv").open(
        "w", newline="", encoding="utf-8"
    ) as file:
        writer = csv.writer(file)
        writer.writerow([
            "nodeId",
            "dstStartNodeId",
            "dstEndNodeId",
            "dstPortId",
            "shortestPortGroups",
            "nonShortestPortGroups",
        ])
        for rule in config.routing_range_rules:
            writer.writerow([
                rule.current_node_id,
                rule.destination_start_node_id,
                rule.destination_end_node_id,
                rule.destination_port_id,
                " ".join(map(str, rule.shortest_port_groups)),
                " ".join(map(str, rule.non_shortest_port_groups)),
            ])

    with (output_path / "routing_topology_rules.csv").open(
        "w", newline="", encoding="utf-8"
    ) as file:
        writer = csv.writer(file)
        writer.writerow([
            "ruleId",
            "ruleType",
            "shape",
            "connections",
            "nodeId",
            "nodeType",
            "coordinates",
        ])
        for rule_id, rule in enumerate(config.topology_rules):
            for node in rule.nodes:
                writer.writerow([
                    rule_id,
                    rule.rule_type,
                    " ".join(map(str, rule.shape)),
                    " ".join(rule.connections),
                    node.node_id,
                    node.node_type,
                    " ".join(map(str, node.coordinates)),
                ])

    _write_json_atomic(output_path / "structured_summary.json", _summary_data(compile_result))
    return output_path
