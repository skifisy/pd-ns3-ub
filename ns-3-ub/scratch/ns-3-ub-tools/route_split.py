import inspect
from collections import defaultdict
from dataclasses import dataclass
from typing import Callable, Mapping

import networkx as nx

from topology_structure import StructureVerification
from topology_routing import (
    CompiledTopologyRule,
    TopologyRuleIndex,
    build_topology_rule_lookups_for_destination,
    compile_topology_rules,
)


RouteLookup = tuple[int, int, int]
PortMetrics = Mapping[int, int]


@dataclass(frozen=True)
class RouteCandidates:
    shortest_ports: tuple[int, ...]
    non_shortest_ports: tuple[int, ...]


@dataclass(frozen=True)
class RouteSplit:
    rule_lookups: Mapping[RouteLookup, RouteCandidates]
    table_lookups: Mapping[RouteLookup, Mapping[int, int]]
    extra_rule_lookups: tuple[RouteLookup, ...]
    baseline_lookup_count: int

    @property
    def mode(self) -> str:
        if not self.rule_lookups:
            return "table"
        if not self.table_lookups:
            return "rule"
        return "mixed"

    @property
    def rule_coverage(self) -> float:
        if self.baseline_lookup_count == 0:
            return 1.0
        return len(self.rule_lookups) / self.baseline_lookup_count


@dataclass(frozen=True)
class CompiledPortGroup:
    node_id: int
    group_id: int
    ports: tuple[int, ...]


@dataclass(frozen=True)
class CompiledRoutingRule:
    current_node_id: int
    destination_node_id: int
    destination_port_id: int
    shortest_port_groups: tuple[int, ...]
    non_shortest_port_groups: tuple[int, ...]


@dataclass(frozen=True)
class CompiledRoutingRangeRule:
    current_node_id: int
    destination_start_node_id: int
    destination_end_node_id: int
    destination_port_id: int
    shortest_port_groups: tuple[int, ...]
    non_shortest_port_groups: tuple[int, ...]


@dataclass(frozen=True)
class CompiledRouteConfig:
    port_groups: tuple[CompiledPortGroup, ...]
    routing_rules: tuple[CompiledRoutingRule, ...]
    routing_range_rules: tuple[CompiledRoutingRangeRule, ...]
    topology_rules: tuple[CompiledTopologyRule, ...]
    table_lookups: Mapping[RouteLookup, Mapping[int, int]]
    mode: str
    rule_coverage: float
    rule_lookup_count: int
    baseline_lookup_count: int


@dataclass(frozen=True)
class TopologyRuleComparison:
    brute_force_lookups: int
    rule_lookups: int
    missing_lookups: int
    extra_lookups: int
    mismatched_lookups: int
    mismatch_examples: tuple[RouteLookup, ...]

    @property
    def is_equivalent(self) -> bool:
        return not (
            self.missing_lookups
            or self.extra_lookups
            or self.mismatched_lookups
        )


@dataclass(frozen=True)
class _DestinationRun:
    start: int
    end: int
    candidates: RouteCandidates

    @property
    def length(self) -> int:
        return self.end - self.start + 1


@dataclass(frozen=True)
class _PlannedRule:
    destination_start: int
    destination_end: int
    candidates: RouteCandidates


def group_route_candidates(port_metrics: PortMetrics) -> RouteCandidates | None:
    if not port_metrics:
        return None
    shortest_metric = min(port_metrics.values())
    shortest_ports = tuple(sorted(
        port for port, metric in port_metrics.items() if metric == shortest_metric
    ))
    # TODO: The current runtime has one shortest group and one merged non-shortest group.
    # Preserve separate non-shortest metric levels if the runtime starts distinguishing them.
    non_shortest_ports = tuple(sorted(
        port for port, metric in port_metrics.items() if metric != shortest_metric
    ))
    return RouteCandidates(shortest_ports, non_shortest_ports)


def split_route_lookups(baseline: Mapping[RouteLookup, PortMetrics],
                        rule_candidates: Mapping[RouteLookup, PortMetrics]) -> RouteSplit:
    rule_lookups = {}
    table_lookups = {}
    for lookup, baseline_metrics in baseline.items():
        baseline_groups = group_route_candidates(baseline_metrics)
        rule_groups = group_route_candidates(rule_candidates.get(lookup, {}))
        if baseline_groups is not None and baseline_groups == rule_groups:
            rule_lookups[lookup] = rule_groups
        else:
            table_lookups[lookup] = dict(baseline_metrics)

    # Primary-address fallback aggregates every physical port on a destination node. Keep all
    # sibling ports in one representation so a rule lookup never needs candidates owned by the
    # compressed table, and a primary rule cannot shadow a more specific table lookup.
    table_owned_pairs = {
        (current_node, destination_node)
        for current_node, destination_node, _ in table_lookups
    }
    for lookup in tuple(rule_lookups):
        if lookup[:2] in table_owned_pairs:
            del rule_lookups[lookup]
            table_lookups[lookup] = dict(baseline[lookup])

    return RouteSplit(
        rule_lookups=rule_lookups,
        table_lookups=table_lookups,
        extra_rule_lookups=tuple(sorted(set(rule_candidates) - set(baseline))),
        baseline_lookup_count=len(baseline),
    )


class _RuleAccumulator:
    def __init__(self):
        self._runs = {}
        self.lookup_count = 0

    def add(self,
            current_node: int,
            destination_node: int,
            destination_port: int,
            candidates: RouteCandidates):
        key = (current_node, destination_port)
        runs = self._runs.setdefault(key, [])
        if runs and destination_node <= runs[-1].end:
            raise ValueError("rule lookups must be added in ascending destination order")
        if (runs and destination_node == runs[-1].end + 1 and
                candidates == runs[-1].candidates):
            previous = runs[-1]
            runs[-1] = _DestinationRun(previous.start, destination_node, candidates)
        else:
            runs.append(_DestinationRun(destination_node, destination_node, candidates))
        self.lookup_count += 1

    def items(self):
        for key in sorted(self._runs):
            contiguous = []
            previous_end = None
            for run in self._runs[key]:
                if previous_end is not None and run.start != previous_end + 1:
                    yield key, _plan_contiguous_runs(contiguous)
                    contiguous = []
                contiguous.append(run)
                previous_end = run.end
            if contiguous:
                yield key, _plan_contiguous_runs(contiguous)


def _plan_contiguous_runs(runs) -> tuple[_PlannedRule, ...]:
    # Cost order keeps the record count minimal, then favors exact hash lookups.
    costs = [(0, 0, 0)]
    steps = [None]
    open_ranges = {}
    total_destinations = 0
    destinations_by_candidate = defaultdict(int)

    for run_index, run in enumerate(runs):
        run_is_range = run.length > 1
        previous_cost = costs[run_index]
        best_cost = (
            previous_cost[0] + 1,
            previous_cost[1] + (run.length if run_is_range else 0),
            previous_cost[2] + (1 if run_is_range else 0),
        )
        best_step = ("single", run_index)

        candidate_destinations = destinations_by_candidate[run.candidates]
        non_candidate_destinations = total_destinations - candidate_destinations
        open_range = open_ranges.get(run.candidates)
        if open_range is not None:
            adjusted_cost, range_start = open_range
            merged_cost = (
                adjusted_cost[0] + 1 + non_candidate_destinations,
                adjusted_cost[1] + candidate_destinations + run.length,
                adjusted_cost[2] + 1,
            )
            if merged_cost < best_cost:
                best_cost = merged_cost
                best_step = ("merged", range_start)

        costs.append(best_cost)
        steps.append(best_step)

        start_cost = (
            previous_cost[0] - non_candidate_destinations,
            previous_cost[1] - candidate_destinations,
            previous_cost[2],
        )
        current_open_range = open_ranges.get(run.candidates)
        if current_open_range is None or start_cost <= current_open_range[0]:
            open_ranges[run.candidates] = (start_cost, run_index)

        total_destinations += run.length
        destinations_by_candidate[run.candidates] += run.length

    planned = []
    run_count = len(runs)
    while run_count:
        step, range_start = steps[run_count]
        range_end = run_count - 1
        if step == "single":
            run = runs[range_end]
            planned.append(_PlannedRule(run.start, run.end, run.candidates))
            run_count -= 1
            continue

        default_candidates = runs[range_end].candidates
        planned.append(_PlannedRule(
            runs[range_start].start,
            runs[range_end].end,
            default_candidates,
        ))
        for run in runs[range_start:run_count]:
            if run.candidates == default_candidates:
                continue
            planned.extend(
                _PlannedRule(destination, destination, run.candidates)
                for destination in range(run.start, run.end + 1)
            )
        run_count = range_start

    return tuple(sorted(
        planned,
        key=lambda rule: (
            rule.destination_start,
            rule.destination_end,
            rule.candidates.shortest_ports,
            rule.candidates.non_shortest_ports,
        ),
    ))


def _compile_accumulated_rules(accumulator: _RuleAccumulator,
                               table_lookups,
                               baseline_lookup_count: int) -> CompiledRouteConfig:
    group_ids_by_node = {}
    port_groups = []
    routing_rules = []
    routing_range_rules = []

    def get_group_id(node_id: int, ports: tuple[int, ...]) -> int:
        groups = group_ids_by_node.setdefault(node_id, {})
        group_id = groups.get(ports)
        if group_id is not None:
            return group_id
        group_id = len(groups)
        groups[ports] = group_id
        port_groups.append(CompiledPortGroup(node_id, group_id, ports))
        return group_id

    for (current_node, destination_port), planned_rules in accumulator.items():
        for planned_rule in planned_rules:
            candidates = planned_rule.candidates
            destination_start = planned_rule.destination_start
            destination_end = planned_rule.destination_end
            shortest_groups = ()
            non_shortest_groups = ()
            if candidates.shortest_ports:
                shortest_groups = (get_group_id(current_node, candidates.shortest_ports),)
            if candidates.non_shortest_ports:
                non_shortest_groups = (
                    get_group_id(current_node, candidates.non_shortest_ports),
                )
            if destination_start == destination_end:
                routing_rules.append(CompiledRoutingRule(
                    current_node_id=current_node,
                    destination_node_id=destination_start,
                    destination_port_id=destination_port,
                    shortest_port_groups=shortest_groups,
                    non_shortest_port_groups=non_shortest_groups,
                ))
            else:
                routing_range_rules.append(CompiledRoutingRangeRule(
                    current_node_id=current_node,
                    destination_start_node_id=destination_start,
                    destination_end_node_id=destination_end,
                    destination_port_id=destination_port,
                    shortest_port_groups=shortest_groups,
                    non_shortest_port_groups=non_shortest_groups,
                ))

    rule_lookup_count = accumulator.lookup_count
    if rule_lookup_count == 0:
        mode = "table"
    elif not table_lookups:
        mode = "rule"
    else:
        mode = "mixed"
    rule_coverage = (
        1.0 if baseline_lookup_count == 0 else rule_lookup_count / baseline_lookup_count
    )

    return CompiledRouteConfig(
        port_groups=tuple(port_groups),
        routing_rules=tuple(routing_rules),
        routing_range_rules=tuple(routing_range_rules),
        topology_rules=(),
        table_lookups=dict(table_lookups),
        mode=mode,
        rule_coverage=rule_coverage,
        rule_lookup_count=rule_lookup_count,
        baseline_lookup_count=baseline_lookup_count,
    )


def compile_route_config(split: RouteSplit) -> CompiledRouteConfig:
    accumulator = _RuleAccumulator()
    for (current_node, destination_node, destination_port), candidates in sorted(
        split.rule_lookups.items()
    ):
        accumulator.add(current_node, destination_node, destination_port, candidates)
    return _compile_accumulated_rules(
        accumulator,
        split.table_lookups,
        split.baseline_lookup_count,
    )


def build_route_lookups(network,
                        path_graph: nx.Graph | None = None,
                        path_finding_algo: Callable = nx.all_shortest_paths,
                        host_pairs=None):
    network.get_physical_topology()
    path_graph = network if path_graph is None else path_graph
    route_table = network.init_sparse_routing_table()
    hosts = list(network.host_ids)
    host_set = set(hosts)
    if host_pairs is None:
        host_pairs = (
            (source, destination)
            for source_index, source in enumerate(hosts)
            for destination in hosts[source_index + 1:]
        )
    for source, destination in host_pairs:
        if source not in host_set or destination not in host_set:
            raise ValueError("route lookup endpoints must both be hosts")
        if source == destination:
            continue
        try:
            for path in path_finding_algo(path_graph, source, destination):
                network.add_path_to_sparse_route_table(route_table, path)
        except nx.NetworkXNoPath:
            continue
    return {lookup: dict(port_metrics) for lookup, port_metrics in route_table.items()}


def build_rule_route_lookups(network,
                             verification: StructureVerification,
                             path_finding_algo: Callable = nx.all_shortest_paths,
                             host_pairs=None):
    rule_graph = build_verified_rule_graph(network, verification)
    return build_route_lookups(
        network,
        rule_graph,
        path_finding_algo,
        host_pairs=host_pairs,
    )


def build_verified_rule_graph(network, verification: StructureVerification) -> nx.Graph:
    topology = network.get_physical_topology()
    physical_links = {link.link_id: link for link in topology.links}
    rule_link_ids = {
        link_id
        for structure in verification.verified_structures
        if structure.rule_capable
        for link in structure.links
        for link_id in link.physical_link_ids
    }
    rule_edges = {
        frozenset((physical_links[link_id].left_node, physical_links[link_id].right_node))
        for link_id in rule_link_ids
    }

    rule_graph = nx.Graph()
    rule_graph.add_nodes_from(network.nodes(data=True))
    rule_graph.add_edges_from(
        (left, right, data)
        for left, right, data in network.edges(data=True)
        if frozenset((left, right)) in rule_edges
    )
    return rule_graph


def is_default_shortest_path_finder(path_finding_algo: Callable) -> bool:
    if isinstance(path_finding_algo, type(nx.all_shortest_paths)):
        return path_finding_algo is nx.all_shortest_paths
    return inspect.unwrap(path_finding_algo) is inspect.unwrap(nx.all_shortest_paths)


def build_shortest_route_lookups_for_destination(network,
                                                 destination: int,
                                                 path_graph: nx.Graph | None = None):
    """Build the default shortest-path lookups without enumerating individual paths."""
    network.get_physical_topology()
    path_graph = network if path_graph is None else path_graph
    if destination not in path_graph:
        return {}

    distances = nx.single_source_shortest_path_length(path_graph, destination)
    source_hosts = [
        host
        for host in network.host_ids
        if host != destination and host in distances
    ]
    if not source_hosts:
        return {}

    # Mark the union of all shortest-path DAGs from a host to this destination.
    used_nodes = set()
    pending = list(source_hosts)
    while pending:
        current = pending.pop()
        if current in used_nodes:
            continue
        used_nodes.add(current)
        current_distance = distances[current]
        if current_distance == 0:
            continue
        pending.extend(
            peer
            for peer in path_graph.neighbors(current)
            if distances.get(peer) == current_distance - 1
        )

    destination_neighbors = [
        peer
        for peer in sorted(path_graph.neighbors(destination))
        if distances.get(peer) == 1 and peer in used_nodes
    ]
    branch_index = {peer: index for index, peer in enumerate(destination_neighbors)}
    branch_masks = {destination: 0}
    for peer, index in branch_index.items():
        branch_masks[peer] = 1 << index
    for current in sorted(used_nodes, key=lambda node: (distances[node], node)):
        current_distance = distances[current]
        if current_distance <= 1:
            continue
        branch_masks[current] = 0
        for peer in path_graph.neighbors(current):
            if distances.get(peer) == current_distance - 1:
                branch_masks[current] |= branch_masks.get(peer, 0)

    destination_ports_by_branch = {
        index: tuple(network.get_link_ports(destination, peer))
        for peer, index in branch_index.items()
    }
    lookups = {}
    for current in sorted(used_nodes):
        current_distance = distances[current]
        if current_distance == 0 or not branch_masks.get(current, 0):
            continue

        ports_by_branch = defaultdict(set)
        if current_distance == 1:
            branch = branch_index[current]
            for destination_port in destination_ports_by_branch[branch]:
                output_ports = network.link_port[(destination, current, destination_port)]
                lookups[(current, destination, destination_port)] = {
                    output_port: current_distance for output_port in sorted(output_ports)
                }
            continue
        else:
            for next_hop in path_graph.neighbors(current):
                if distances.get(next_hop) != current_distance - 1:
                    continue
                mask = branch_masks.get(next_hop, 0)
                output_ports = network.get_link_ports(current, next_hop)
                while mask:
                    branch_bit = mask & -mask
                    branch = branch_bit.bit_length() - 1
                    ports_by_branch[branch].update(output_ports)
                    mask ^= branch_bit

        for branch, output_ports in ports_by_branch.items():
            for destination_port in destination_ports_by_branch[branch]:
                lookups[(current, destination, destination_port)] = {
                    output_port: current_distance for output_port in sorted(output_ports)
                }
    return lookups


def compare_topology_rules_to_brute_force(network,
                                          rules: tuple[CompiledTopologyRule, ...],
                                          mismatch_limit: int = 10,
                                          progress_callback=None):
    """Compare direct topology rules to shortest-path DAGs one destination at a time."""
    index = TopologyRuleIndex(network, rules)
    brute_force_count = 0
    rule_count = 0
    missing_count = 0
    extra_count = 0
    mismatched_count = 0
    examples = []

    def remember(lookups):
        for lookup in sorted(lookups):
            if len(examples) >= mismatch_limit:
                break
            examples.append(lookup)

    destinations = sorted(network.host_ids)
    for destination_index, destination in enumerate(destinations, start=1):
        brute_force = build_shortest_route_lookups_for_destination(
            network,
            destination,
        )
        direct = build_topology_rule_lookups_for_destination(
            network,
            index,
            destination,
        )
        brute_force_count += len(brute_force)
        rule_count += len(direct)
        missing = set(brute_force) - set(direct)
        extra = set(direct) - set(brute_force)
        mismatched = {
            lookup
            for lookup in set(brute_force) & set(direct)
            if brute_force[lookup] != direct[lookup]
        }
        missing_count += len(missing)
        extra_count += len(extra)
        mismatched_count += len(mismatched)
        remember(missing)
        remember(extra)
        remember(mismatched)
        if progress_callback is not None:
            progress_callback(destination_index, len(destinations))

    return TopologyRuleComparison(
        brute_force_lookups=brute_force_count,
        rule_lookups=rule_count,
        missing_lookups=missing_count,
        extra_lookups=extra_count,
        mismatched_lookups=mismatched_count,
        mismatch_examples=tuple(examples),
    )


def build_direct_shortest_route_config(network,
                                       verification: StructureVerification,
                                       routing_mode: str) -> CompiledRouteConfig:
    if routing_mode not in {"rule", "mixed"}:
        raise ValueError("direct shortest-route compilation requires rule or mixed mode")

    rule_graph = build_verified_rule_graph(network, verification)
    rule_graph_covers_all_edges = {
        frozenset(edge) for edge in rule_graph.edges
    } == {
        frozenset(edge) for edge in network.edges
    }
    accumulator = _RuleAccumulator()
    table_lookups = {}
    baseline_lookup_count = 0

    for destination in sorted(network.host_ids):
        baseline = build_shortest_route_lookups_for_destination(network, destination)
        rule_candidates = (
            baseline
            if rule_graph_covers_all_edges
            else build_shortest_route_lookups_for_destination(
                network,
                destination,
                path_graph=rule_graph,
            )
        )
        baseline_lookup_count += len(baseline)
        split = split_route_lookups(baseline, rule_candidates)
        for lookup, candidates in sorted(split.rule_lookups.items()):
            accumulator.add(*lookup, candidates)
        table_lookups.update(split.table_lookups)

    if routing_mode == "rule" and table_lookups:
        raise ValueError(
            "rule routing requires every route lookup to pass equivalence verification"
        )
    return _compile_accumulated_rules(
        accumulator,
        table_lookups,
        baseline_lookup_count,
    )


def build_topology_route_config(network,
                                verification: StructureVerification,
                                routing_mode: str) -> CompiledRouteConfig | None:
    if routing_mode not in {"rule", "mixed"}:
        return None
    topology_rules = compile_topology_rules(verification)
    if topology_rules is None:
        return None
    return CompiledRouteConfig(
        port_groups=(),
        routing_rules=(),
        routing_range_rules=(),
        topology_rules=topology_rules,
        table_lookups={},
        mode="rule",
        rule_coverage=1.0,
        # The topology rules prove full ownership without materializing every lookup.
        rule_lookup_count=0,
        baseline_lookup_count=0,
    )
