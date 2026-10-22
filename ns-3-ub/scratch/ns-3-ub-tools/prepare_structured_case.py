#!/usr/bin/env python3
"""Prepare a reusable structure-aware simulation directory."""

import argparse
import importlib.util
import sys
from pathlib import Path

from net_sim_builder import NetworkSimulationGraph
from structured_topology import load_structured_topology


def _load_function(reference, purpose):
    try:
        source, function_name = reference.rsplit(":", 1)
    except ValueError as error:
        raise ValueError(f"{purpose} must use path.py:function format") from error

    source_path = Path(source).expanduser().resolve()
    if not source_path.is_file():
        raise ValueError(f"{purpose} file does not exist: {source_path}")
    if not function_name.isidentifier():
        raise ValueError(f"{purpose} function name is invalid: {function_name}")

    module_name = f"_ns3_ub_{purpose.replace(' ', '_')}_{abs(hash(source_path))}"
    spec = importlib.util.spec_from_file_location(module_name, source_path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load {purpose} file: {source_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    function = getattr(module, function_name, None)
    if not callable(function):
        raise ValueError(f"{purpose} function is not callable: {reference}")
    return function


def _parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Compile a NetworkX or JSON/YAML topology into a reusable case directory."
    )
    parser.add_argument(
        "topology",
        help="Python builder in path.py:function format, or a .json/.yaml topology file",
    )
    parser.add_argument("--case-path", required=True, help="Simulation case directory to prepare")
    generation = parser.add_mutually_exclusive_group()
    generation.add_argument(
        "--route-generation",
        choices=("full", "structured", "mixed"),
        default="mixed",
        help="Offline route generation strategy (default: mixed)",
    )
    generation.add_argument(
        "--routing-mode",
        choices=("table", "rule", "mixed"),
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--path-finder", help="Optional route function in path.py:function format")
    parser.add_argument("--cache-dir", help="Override the content-addressed route cache")
    parser.add_argument("--no-cache", action="store_true", help="Disable the route cache")
    parser.add_argument(
        "--verify-routes",
        action="store_true",
        help="Exhaustively compare the prepared candidates with the full routing baseline",
    )
    parser.add_argument(
        "--export-csv",
        metavar="DIRECTORY",
        help="Optionally export diagnostic topology and routing CSV files",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        source_path = Path(args.topology.split(":", 1)[0]).expanduser()
        if source_path.suffix.lower() in {".json", ".yaml", ".yml"} and ":" not in args.topology:
            graph = load_structured_topology(source_path)
        else:
            graph = _load_function(args.topology, "topology builder")()
        if not isinstance(graph, NetworkSimulationGraph):
            raise TypeError(
                "topology builder must return NetworkSimulationGraph, "
                "the ns-3-ub NetworkX graph with explicit port metadata"
            )

        options = {
            "route_generation": args.route_generation,
            "use_cache": not args.no_cache,
            "cache_dir": args.cache_dir,
            "export_dir": args.export_csv,
            "verify_routes": args.verify_routes,
        }
        if args.routing_mode is not None:
            options["routing_mode"] = args.routing_mode
        if args.path_finder:
            options["path_finding_algo"] = _load_function(args.path_finder, "path finder")
        prepared = graph.prepare_structured_case(args.case_path, **options)
        config = prepared.compile_result.config
        rule_lookups = "not-expanded" if config.topology_rules else str(config.rule_lookup_count)
        cache_state = "hit" if prepared.compile_result.cache_hit else "miss"
        print(
            f"[structured] prepared={prepared.case_path}, "
            f"generation={prepared.compile_result.route_generation}, routing={config.mode}, "
            f"coverage={config.rule_coverage:.1%}, "
            f"rules={len(config.routing_rules) + len(config.routing_range_rules)}, "
            f"topology-rules={len(config.topology_rules)}, rule-lookups={rule_lookups}, "
            f"table-lookups={len(config.table_lookups)}, cache={cache_state}"
        )
        return 0
    except (ImportError, OSError, TypeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
