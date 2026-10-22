#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

from traffic_maker.algorithms.registry import algorithm_names, collective_names
from traffic_maker.algorithms.semantic_validator import (
    SemanticValidationError,
    validate_collective_schedule,
    validate_semantic_schedule,
)
from traffic_maker.simai_flow_to_traffic import convert, read_jsonl, read_rank_map
from traffic_maker.v2 import TrafficRequest, build_alltoallv_schedule, write_traffic
from structured_case import (
    compile_structured_routes,
    export_structured_config,
    prepare_structured_case,
)
from case_inspector import check_case, inspect_case
from topology_source import load_topology_source, plot_topology_graph, topology_summary
from trace_analysis.api import parse_trace_case


INTERFACE_VERSION = "v2"
TOOLS_VERSION = "2.1.0"

STRUCTURED_OUTPUT_FILES = (
    "structured_topology.txt",
    "compressed_routing.bin",
    "structured_summary.json",
)
CSV_OUTPUT_FILES = (
    "node.csv",
    "topology.csv",
    "routing_table.csv",
    "routing_port_groups.csv",
    "routing_rules.csv",
    "routing_rule_ranges.csv",
    "routing_topology_rules.csv",
    "structured_summary.json",
)


def require_available_outputs(output_dir: Path, names: tuple[str, ...]) -> None:
    existing = [str(output_dir / name) for name in names if (output_dir / name).exists()]
    if existing:
        raise ValueError(
            f"output already exists: {', '.join(existing)}; choose a new output directory"
        )


def parse_data_size(value: str) -> int:
    normalized = value.upper()
    match = re.fullmatch(r"(0|[1-9][0-9]*)(B|KB|MB|GB)?", normalized)
    if match is None:
        raise argparse.ArgumentTypeError(
            "data-size must be a positive integer with B, KB, MB, or GB, or 0B; "
            "for example 1024B, 1KB, 1MB, or 1GB; zero bytes use 0B"
        )
    multipliers = {
        "GB": 1024**3,
        "MB": 1024**2,
        "KB": 1024,
        "B": 1,
    }
    return int(match.group(1)) * multipliers.get(match.group(2), 1)


def parse_phase_delay(value: str) -> str:
    if re.fullmatch(r"[0-9]+(?:ns|us|ms|s)", value) is None:
        raise argparse.ArgumentTypeError(
            "phase-delay must include a time unit; for example 0ns, 5us, or 1ms"
        )
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Prepare ns-3-ub input artifacts without starting ns-3.",
        epilog="Example: python3 ub_tools.py traffic allreduce --help",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"ns-3-ub-tools interface {INTERFACE_VERSION}; tools {TOOLS_VERSION}",
    )
    commands = parser.add_subparsers(dest="command")
    traffic_parser = commands.add_parser(
        "traffic",
        description="Generate traffic.csv or validate collective block semantics.",
        epilog="Example: python3 ub_tools.py traffic allreduce --help",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    traffic_parser.set_defaults(_current_parser=traffic_parser)
    collectives = traffic_parser.add_subparsers(dest="collective")

    simai_parser = collectives.add_parser(
        "convert-simai",
        description="Convert SimAI flow JSONL into an ns-3-ub traffic.csv.",
        epilog=(
            "Example: python3 ub_tools.py traffic convert-simai flows.jsonl "
            "--output ./case/traffic.csv --host-count 8"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    simai_parser.add_argument("input", type=Path, help="input SimAI flow JSONL file")
    simai_parser.add_argument(
        "--output", type=Path, required=True, help="output traffic.csv path"
    )
    simai_parser.add_argument(
        "--rank-map", type=Path, help="CSV mapping simai_rank,ub_node"
    )
    simai_parser.add_argument(
        "--host-count", type=int, help="validate mapped host IDs in 0..host-count-1"
    )
    simai_parser.add_argument(
        "--phase-mode", choices=["level", "flow"], default="level",
        help="group DAG levels or use one phase per flow (default: level)",
    )
    simai_parser.add_argument(
        "--allow-overlap-collectives", action="store_true",
        help="do not serialize successive collectives",
    )
    simai_parser.add_argument(
        "--overwrite", action="store_true", help="replace an existing output traffic.csv"
    )
    simai_parser.add_argument("--op-type", default="URMA_WRITE", help="traffic.csv opType")
    simai_parser.add_argument("--priority", type=int, default=7, help="traffic.csv priority")
    simai_parser.add_argument("--delay", default="0ns", help="traffic.csv delay")

    validate_parser = collectives.add_parser(
        "validate",
        description="Validate collective block semantics without writing traffic.csv.",
        epilog="Example: python3 ub_tools.py traffic validate allgather --help",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    validate_parser.set_defaults(_current_parser=validate_parser)
    validation_collectives = validate_parser.add_subparsers(dest="validation_collective")
    for collective in (
        "allgather",
        "reduce-scatter",
        "allreduce",
        "alltoall",
        "alltoallv",
    ):
        display_name = {
            "allgather": "AllGather",
            "reduce-scatter": "ReduceScatter",
            "allreduce": "AllReduce",
            "alltoall": "AllToAll",
            "alltoallv": "AllToAllV",
        }[collective]
        example_algorithm = {
            "allgather": "ring",
            "reduce-scatter": "ring",
            "allreduce": "ring",
            "alltoall": "pairwise",
            "alltoallv": "pairwise",
        }[collective]
        example_input = (
            "--distribution uniform --data-size 1KB"
            if collective == "alltoallv"
            else "--data-size 1KB"
        )
        validation_parser = validation_collectives.add_parser(
            collective,
            description=f"Validate an {display_name} schedule by tracking every block.",
            epilog=(
                f"Example: python3 ub_tools.py traffic validate {collective} "
                f"--rank-size 4 {example_input} --algorithm {example_algorithm}"
            ),
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        validation_parser.set_defaults(_current_parser=validation_parser)
        validation_parser.add_argument(
            "--rank-size", type=int, required=True, help="number of ranks in the collective"
        )
        validation_parser.add_argument(
            "--data-size",
            type=parse_data_size,
            required=collective != "alltoallv",
            help="logical input bytes per rank (B, KB, MB, or GB)",
        )
        validation_parser.add_argument(
            "--algorithm",
            choices=algorithm_names(collective),
            required=True,
            help="schedule algorithm",
        )
        validation_parser.add_argument(
            "--output", type=Path, help="optional JSON trace output path"
        )
        validation_parser.add_argument(
            "--overwrite", action="store_true", help="replace an existing JSON output"
        )
        validation_parser.add_argument(
            "--verbose", action="store_true", help="include contract and per-phase state"
        )
        if collective == "alltoallv":
            validation_parser.add_argument(
                "--distribution",
                choices=("matrix", "uniform", "random"),
                required=True,
            )
            validation_parser.add_argument("--traffic-matrix", type=Path)
            validation_parser.add_argument("--seed", type=int)
            validation_parser.add_argument("--include-local", action="store_true")

    topology_parser = commands.add_parser(
        "topology",
        description="Build, validate, or plot a topology source.",
        epilog="Example: python3 ub_tools.py topology check examples/structured_2devices.yaml",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    topology_parser.set_defaults(_current_parser=topology_parser)
    topology_commands = topology_parser.add_subparsers(dest="topology_command")

    topology_build = topology_commands.add_parser(
        "build",
        description="Compile a topology source into a prepared case.",
        epilog=(
            "Example: python3 ub_tools.py topology build topology.py "
            "--output-dir ./case"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    topology_build.set_defaults(_current_parser=topology_build)
    topology_build.add_argument(
        "source", help="input path.py, path.py:function, JSON/YAML file, or CSV case directory"
    )
    topology_build.add_argument(
        "--output-dir", type=Path, required=True, help="output prepared case directory"
    )
    topology_build.add_argument(
        "--case-format", choices=["structured", "csv"], default="structured",
        help="output case format (default: structured)",
    )
    topology_build.add_argument(
        "--route-generation", choices=["full", "structured", "mixed"],
        help="offline route generation strategy (default: mixed for structured, full for csv)",
    )
    topology_build.add_argument("--cache-dir", type=Path, help="route cache directory")
    topology_build.add_argument("--no-cache", action="store_true", help="disable route cache")
    topology_build.add_argument(
        "--verify-routes", action="store_true", help="compare against complete routes"
    )
    topology_build.add_argument(
        "--export-csv", type=Path, help="optional diagnostic CSV output directory"
    )

    topology_check = topology_commands.add_parser(
        "check",
        description="Load and validate a topology source without preparing a case.",
        epilog="Example: python3 ub_tools.py topology check topology.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    topology_check.set_defaults(_current_parser=topology_check)
    topology_check.add_argument(
        "source", help="input path.py, path.py:function, JSON/YAML file, or CSV case directory"
    )

    topology_plot = topology_commands.add_parser(
        "plot",
        description="Plot the same normalized physical graph used by topology build.",
        epilog=(
            "Example: python3 ub_tools.py topology plot topology.py "
            "--output topology.png"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    topology_plot.set_defaults(_current_parser=topology_plot)
    topology_plot.add_argument(
        "source", help="input path.py, path.py:function, JSON/YAML file, or CSV case directory"
    )
    topology_plot.add_argument(
        "--output", type=Path, required=True, help="output image path (for example topology.png)"
    )

    case_parser = commands.add_parser(
        "case",
        description="Show or validate a prepared case without starting ns-3.",
        epilog="Example: python3 ub_tools.py case check ./case",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    case_parser.set_defaults(_current_parser=case_parser)
    case_commands = case_parser.add_subparsers(dest="case_command")
    for name, description in (
        ("show", "show a case summary"),
        ("check", "validate case files and routing artifacts"),
    ):
        command = case_commands.add_parser(
            name,
            description=description,
            epilog=f"Example: python3 ub_tools.py case {name} ./case",
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        command.set_defaults(_current_parser=command)
        command.add_argument("case_path", type=Path, help="prepared case directory")
        command.add_argument("--json", action="store_true", help="print the summary as JSON")

    trace_parser = commands.add_parser(
        "trace",
        description="Parse simulation traces without starting ns-3.",
        epilog="Example: python3 ub_tools.py trace parse ./case --output-dir ./analysis",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    trace_parser.set_defaults(_current_parser=trace_parser)
    trace_commands = trace_parser.add_subparsers(dest="trace_command")
    trace_parse = trace_commands.add_parser(
        "parse",
        description="Parse runlog traces into task and throughput CSV files.",
        epilog=(
            "Example: python3 ub_tools.py trace parse ./case "
            "--output-dir ./analysis"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    trace_parse.set_defaults(_current_parser=trace_parse)
    trace_parse.add_argument("case_path", type=Path, help="input case containing traffic.csv and runlog")
    trace_parse.add_argument(
        "--output-dir", type=Path, required=True, help="output directory for analysis CSV files"
    )
    trace_parse.add_argument(
        "--overwrite", action="store_true", help="replace existing analysis CSV files"
    )
    trace_parse.add_argument("--json", action="store_true", help="print output paths as JSON")

    for collective in collective_names():
        algorithms = algorithm_names(collective)
        display_names = {
            "allreduce": "AllReduce",
            "reduce-scatter": "ReduceScatter",
            "allgather": "AllGather",
            "alltoall": "AllToAll",
            "alltoallv": "AllToAllV",
        }
        display_name = display_names[collective]
        if collective == "alltoallv":
            example = (
                "python3 ub_tools.py traffic alltoallv --host-count 8 "
                "--data-size 1GB --algorithm pairwise --distribution uniform "
                "--output-dir ./output"
            )
        else:
            example = (
                f"python3 ub_tools.py traffic {collective} --host-count 8 "
                f"--data-size 1GB --algorithm {next(iter(algorithms))} "
                "--output-dir ./output"
            )
        collective_parser = collectives.add_parser(
            collective,
            description=f"Generate an {display_name} traffic schedule.",
            epilog=f"Example: {example}",
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        collective_parser.add_argument(
            "--host-count",
            type=int,
            help="total participating physical nodes (required without --placement-file)",
        )
        collective_parser.add_argument(
            "--group-size", type=int, help="ranks per group (default: host-count)"
        )
        collective_parser.add_argument(
            "--data-size",
            type=parse_data_size,
            required=collective != "alltoallv",
            help=(
                f"complete {display_name} data object per rank (B, KB, MB, or GB); "
                "not used with alltoallv matrix distribution"
            ),
        )
        collective_parser.add_argument(
            "--algorithm", choices=algorithms, required=True, help="schedule algorithm"
        )
        placement = collective_parser.add_mutually_exclusive_group()
        placement.add_argument(
            "--group-layout",
            choices=["contiguous", "interleaved"],
            help="automatic physical-node grouping (default: contiguous)",
        )
        placement.add_argument(
            "--placement-file",
            type=Path,
            help="input CSV mapping groupId,rankId,nodeId",
        )
        if collective == "alltoall":
            collective_parser.add_argument(
                "--peers-per-phase",
                type=int,
                help="scatter peers grouped into each phase (default: 1)",
            )
        if collective == "alltoallv":
            collective_parser.add_argument(
                "--distribution",
                choices=["matrix", "uniform", "random"],
                required=True,
                help="byte distribution source",
            )
            collective_parser.add_argument(
                "--traffic-matrix", type=Path, help="CSV file containing an N x N byte matrix"
            )
            collective_parser.add_argument(
                "--seed", type=int, help="deterministic seed for random distribution"
            )
            collective_parser.add_argument(
                "--include-local",
                action="store_true",
                help="keep diagonal local copies (default: omitted)",
            )
        collective_parser.add_argument(
            "--phase-delay",
            type=parse_phase_delay,
            default="0ns",
            help="delay after a dependent phase (default: 0ns)",
        )
        collective_parser.add_argument(
            "--output-dir", type=Path, required=True, help="output directory for traffic.csv"
        )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.command is None:
        parser.print_help(file=sys.stderr)
        return 2
    if args.command == "traffic" and args.collective == "convert-simai":
        try:
            if args.host_count is not None and args.host_count <= 0:
                raise ValueError("host-count must be positive")
            if args.output.exists() and not args.overwrite:
                raise ValueError(
                    f"output already exists: {args.output}; use --overwrite to replace"
                )
            flows = read_jsonl(args.input)
            stats = convert(
                flows=flows,
                output_path=args.output,
                rank_map=read_rank_map(args.rank_map),
                host_count=args.host_count,
                phase_mode=args.phase_mode,
                sequential_collectives=not args.allow_overlap_collectives,
                op_type=args.op_type,
                priority=args.priority,
                delay=args.delay,
            )
        except (OSError, ValueError, json.JSONDecodeError) as error:
            parser.error(str(error))
        print(
            f"traffic.csv written: {args.output} tasks={stats['tasks']} "
            f"phases={stats['phases']} collectives={stats['collectives']} "
            f"total_bytes={stats['total_bytes']}"
        )
        return 0
    if args.command == "traffic" and args.collective == "validate":
        if args.validation_collective is None:
            args._current_parser.print_help(file=sys.stderr)
            return 2
        try:
            if args.rank_size <= 0:
                raise ValueError("rank-size must be positive")
            if args.overwrite and args.output is None:
                raise ValueError("--overwrite requires --output")
            if args.validation_collective == "alltoallv":
                request = TrafficRequest(
                    collective="alltoallv",
                    data_size=args.data_size,
                    algorithm=args.algorithm,
                    output_dir=Path("."),
                    distribution=args.distribution,
                    traffic_matrix=args.traffic_matrix,
                    seed=args.seed,
                    include_local=args.include_local,
                )
                result = validate_semantic_schedule(
                    build_alltoallv_schedule(request, args.rank_size),
                    verbose=args.verbose,
                )
            else:
                result = validate_collective_schedule(
                    args.validation_collective,
                    args.algorithm,
                    args.rank_size,
                    args.data_size,
                    verbose=args.verbose,
                )
            payload = json.dumps(result, indent=2) + "\n"
            if args.output is not None:
                if args.output.exists() and not args.overwrite:
                    raise ValueError(
                        f"output already exists: {args.output}; use --overwrite to replace"
                    )
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(payload, encoding="utf-8")
        except (OSError, SemanticValidationError, ValueError) as error:
            parser.error(str(error))
        if args.output is None:
            print(payload, end="")
        else:
            print(f"semantic validation trace written to: {args.output}")
        return 0
    if args.command == "topology":
        if args.topology_command is None:
            args._current_parser.print_help(file=sys.stderr)
            return 2
        try:
            graph = load_topology_source(args.source)
            summary = topology_summary(graph)
            if args.topology_command == "check":
                print(json.dumps(summary, sort_keys=True))
                return 0
            if args.topology_command == "plot":
                output = plot_topology_graph(graph, args.output)
                print(f"topology plot written to: {output}")
                return 0
            output_dir = args.output_dir
            effective_route_generation = args.route_generation or (
                "full" if args.case_format == "csv" else "mixed"
            )
            if args.case_format == "csv":
                if args.export_csv is not None:
                    raise ValueError("--export-csv cannot be combined with --case-format csv")
                if effective_route_generation != "full":
                    raise ValueError(
                        "--case-format csv supports only --route-generation full; "
                        "use --case-format structured for structured or mixed routing, "
                        "or use --export-csv for a diagnostic snapshot"
                    )
                require_available_outputs(output_dir, CSV_OUTPUT_FILES)
                compile_result = compile_structured_routes(
                    graph,
                    route_generation=effective_route_generation,
                    use_cache=not args.no_cache,
                    cache_dir=args.cache_dir,
                )
                export_structured_config(graph, compile_result, output_dir)
                effective_route_generation = compile_result.route_generation
            else:
                require_available_outputs(output_dir, STRUCTURED_OUTPUT_FILES)
                if args.export_csv is not None:
                    if args.export_csv.expanduser().resolve() == output_dir.expanduser().resolve():
                        raise ValueError("--export-csv must differ from --output-dir")
                    require_available_outputs(args.export_csv, CSV_OUTPUT_FILES)
                prepared = prepare_structured_case(
                    graph,
                    output_dir,
                    route_generation=effective_route_generation,
                    use_cache=not args.no_cache,
                    cache_dir=args.cache_dir,
                    export_dir=args.export_csv,
                    verify_routes=args.verify_routes,
                )
                effective_route_generation = prepared.compile_result.route_generation
        except (ImportError, OSError, TypeError, ValueError) as error:
            parser.error(str(error))
        print(
            f"{args.case_format} case prepared: {output_dir} "
            f"nodes={summary['nodes']} links={summary['links']} "
            f"route-generation={effective_route_generation}"
        )
        return 0
    if args.command == "case":
        if args.case_command is None:
            args._current_parser.print_help(file=sys.stderr)
            return 2
        try:
            result = check_case(args.case_path) if args.case_command == "check" else inspect_case(args.case_path)
        except (OSError, TypeError, ValueError, struct.error) as error:
            parser.error(str(error))
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(
                f"case={result['casePath']} format={result['format']} "
                f"nodes={result['topology']['nodes']} links={result['topology']['links']}"
            )
            if "trafficTasks" in result:
                print(f"traffic tasks={result['trafficTasks']}")
            routing = result.get("routing", {})
            if "routingTableRows" in routing:
                print(f"routing table rows={routing['routingTableRows']}")
            elif routing:
                compressed = routing.get("compressedRouting") or {}
                print(
                    f"routing generation={routing.get('routeGeneration')} "
                    f"table-lookups={routing.get('tableLookupCount')} "
                    f"rules={routing.get('routingRuleCount')} "
                    f"range-rules={routing.get('routingRangeRuleCount')} "
                    f"topology-rules={routing.get('topologyRuleCount')} "
                    f"compressed-bytes={compressed.get('bytes')}"
                )
            if args.case_command == "check":
                print("case check: valid")
        return 0
    if args.command == "trace":
        if args.trace_command is None:
            args._current_parser.print_help(file=sys.stderr)
            return 2
        try:
            result = parse_trace_case(
                args.case_path, args.output_dir, overwrite=args.overwrite
            )
        except (OSError, TypeError, ValueError) as error:
            parser.error(str(error))
        if args.json:
            print(json.dumps(result, indent=2, sort_keys=True))
        else:
            print(f"trace analysis written to: {result['outputDir']}")
        return 0
    if args.collective is None:
        args._current_parser.print_help(file=sys.stderr)
        return 2
    request = TrafficRequest(
        collective=args.collective,
        host_count=args.host_count,
        group_size=args.group_size,
        data_size=args.data_size,
        algorithm=args.algorithm,
        group_layout=args.group_layout,
        placement_file=args.placement_file,
        peers_per_phase=getattr(args, "peers_per_phase", None),
        distribution=getattr(args, "distribution", None),
        traffic_matrix=getattr(args, "traffic_matrix", None),
        seed=getattr(args, "seed", None),
        include_local=getattr(args, "include_local", False),
        phase_delay=args.phase_delay,
        output_dir=args.output_dir,
    )
    try:
        output_path = write_traffic(request)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(f"traffic.csv written to: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
