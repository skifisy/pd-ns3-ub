from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Mapping

from physical_topology import PhysicalTopology


@dataclass(frozen=True)
class StructureNode:
    physical_node_id: int
    coordinates: tuple[int, ...]
    node_type: str


@dataclass(frozen=True)
class StructureLink:
    left: tuple[int, ...]
    right: tuple[int, ...]
    physical_link_ids: tuple[int, ...]


@dataclass(frozen=True)
class LocalPortGroup:
    name: str
    physical_node_id: int
    port_ids: tuple[int, ...]


@dataclass(frozen=True)
class DestinationCoverage:
    name: str
    node_ids: tuple[int, ...]


@dataclass(frozen=True)
class TopologyStructure:
    name: str
    structure_type: str
    nodes: tuple[StructureNode, ...]
    links: tuple[StructureLink, ...]
    port_groups: tuple[LocalPortGroup, ...] = ()
    destination_coverage: tuple[DestinationCoverage, ...] = ()
    parameters: Mapping[str, object] = field(default_factory=dict)
    rule_capable: bool = True


@dataclass(frozen=True)
class RejectedStructure:
    structure: TopologyStructure
    reasons: tuple[str, ...]


@dataclass(frozen=True)
class StructureVerification:
    verified_structures: tuple[TopologyStructure, ...]
    rejected_structures: tuple[RejectedStructure, ...]
    uncovered_link_ids: tuple[int, ...]
    duplicate_link_ids: tuple[int, ...]

    @property
    def is_fully_covered(self) -> bool:
        return (
            not self.rejected_structures
            and not self.uncovered_link_ids
            and not self.duplicate_link_ids
        )


@dataclass(frozen=True)
class _PhysicalIndex:
    nodes_by_id: Mapping[int, object]
    links_by_id: Mapping[int, object]
    link_ids_by_endpoints: Mapping[frozenset[int], frozenset[int]]
    link_ids_by_node: Mapping[int, frozenset[int]]
    ports_by_node: Mapping[int, frozenset[int]]


def _build_physical_index(topology: PhysicalTopology) -> _PhysicalIndex:
    links_by_endpoints = defaultdict(set)
    links_by_node = defaultdict(set)
    ports_by_node = defaultdict(set)
    for link in topology.links:
        links_by_endpoints[frozenset((link.left_node, link.right_node))].add(link.link_id)
        links_by_node[link.left_node].add(link.link_id)
        links_by_node[link.right_node].add(link.link_id)
        ports_by_node[link.left_node].add(link.left_port)
        ports_by_node[link.right_node].add(link.right_port)
    return _PhysicalIndex(
        nodes_by_id={node.node_id: node for node in topology.nodes},
        links_by_id={link.link_id: link for link in topology.links},
        link_ids_by_endpoints={
            endpoints: frozenset(link_ids)
            for endpoints, link_ids in links_by_endpoints.items()
        },
        link_ids_by_node={
            node_id: frozenset(link_ids) for node_id, link_ids in links_by_node.items()
        },
        ports_by_node={
            node_id: frozenset(port_ids) for node_id, port_ids in ports_by_node.items()
        },
    )


def _verify_candidate(index: _PhysicalIndex,
                      candidate: TopologyStructure) -> tuple[str, ...]:
    reasons = []
    physical_nodes = index.nodes_by_id
    physical_links = index.links_by_id
    nodes_by_coordinates = {node.coordinates: node for node in candidate.nodes}

    if len(nodes_by_coordinates) != len(candidate.nodes):
        reasons.append("structure coordinates must be unique")
    if len({node.physical_node_id for node in candidate.nodes}) != len(candidate.nodes):
        reasons.append("physical nodes must be unique within one structure")

    for node in candidate.nodes:
        physical_node = physical_nodes.get(node.physical_node_id)
        if physical_node is None:
            reasons.append(f"physical node {node.physical_node_id} does not exist")
        elif physical_node.node_type != node.node_type:
            reasons.append(
                f"physical node {node.physical_node_id} has type {physical_node.node_type}"
            )

    claimed_link_ids = []
    for link in candidate.links:
        left = nodes_by_coordinates.get(link.left)
        right = nodes_by_coordinates.get(link.right)
        if left is None or right is None:
            reasons.append(
                f"structure link {link.left}->{link.right} references unknown coordinates"
            )
            continue
        expected_endpoints = {left.physical_node_id, right.physical_node_id}
        if not link.physical_link_ids:
            reasons.append(f"structure link {link.left}->{link.right} has no physical links")
            continue
        for link_id in link.physical_link_ids:
            claimed_link_ids.append(link_id)
            physical_link = physical_links.get(link_id)
            if physical_link is None:
                reasons.append(f"physical link {link_id} does not exist")
                continue
            actual_endpoints = {physical_link.left_node, physical_link.right_node}
            if actual_endpoints != expected_endpoints:
                reasons.append(f"physical link {link_id} has different endpoints")

        all_parallel_links = index.link_ids_by_endpoints.get(
            frozenset(expected_endpoints),
            frozenset(),
        )
        if set(link.physical_link_ids) != all_parallel_links:
            reasons.append(
                f"structure link {link.left}->{link.right} does not cover all parallel links"
            )

    duplicate_claims = [
        link_id for link_id, count in Counter(claimed_link_ids).items() if count > 1
    ]
    if duplicate_claims:
        reasons.append(f"physical links claimed more than once: {duplicate_claims}")

    candidate_node_ids = {node.physical_node_id for node in candidate.nodes}
    if len({group.name for group in candidate.port_groups}) != len(candidate.port_groups):
        reasons.append("local port group names must be unique within one structure")
    for group in candidate.port_groups:
        if group.physical_node_id not in candidate_node_ids:
            reasons.append(f"local port group {group.name} references a node outside the structure")
        if not group.port_ids:
            reasons.append(f"local port group {group.name} must contain at least one port")
        invalid_ports = sorted(
            set(group.port_ids) - index.ports_by_node.get(group.physical_node_id, frozenset())
        )
        if invalid_ports:
            reasons.append(f"local port group {group.name} has invalid ports {invalid_ports}")

    coverage_names = {coverage.name for coverage in candidate.destination_coverage}
    if len(coverage_names) != len(candidate.destination_coverage):
        reasons.append("destination coverage names must be unique within one structure")
    for coverage in candidate.destination_coverage:
        if not coverage.node_ids:
            reasons.append(f"destination coverage {coverage.name} must contain at least one node")
        missing_nodes = sorted(set(coverage.node_ids) - set(physical_nodes))
        if missing_nodes:
            reasons.append(
                f"destination coverage {coverage.name} has unknown nodes {missing_nodes}"
            )
    return tuple(reasons)


def verify_topology_structures(topology: PhysicalTopology,
                               candidates: tuple[TopologyStructure, ...]) -> StructureVerification:
    index = _build_physical_index(topology)
    individually_verified = []
    rejection_by_name = {}
    name_counts = Counter(candidate.name for candidate in candidates)
    duplicate_names = {name for name, count in name_counts.items() if count > 1}
    for candidate in candidates:
        if candidate.name in duplicate_names:
            rejection_by_name[candidate.name] = ("structure names must be unique",)
            continue
        reasons = _verify_candidate(index, candidate)
        if reasons:
            rejection_by_name[candidate.name] = reasons
        else:
            individually_verified.append(candidate)

    preliminary_claims = [
        link_id
        for candidate in individually_verified
        for link in candidate.links
        for link_id in link.physical_link_ids
    ]
    claim_counts = Counter(preliminary_claims)
    duplicate_link_ids = tuple(
        sorted(link_id for link_id, count in claim_counts.items() if count > 1)
    )
    duplicate_set = set(duplicate_link_ids)
    verified = []
    for candidate in individually_verified:
        candidate_links = {
            link_id for link in candidate.links for link_id in link.physical_link_ids
        }
        duplicates = sorted(candidate_links & duplicate_set)
        if duplicates:
            rejection_by_name[candidate.name] = (
                f"physical links have multiple owners: {duplicates}",
            )
        else:
            verified.append(candidate)

    while verified:
        owned_link_ids = {
            link_id
            for candidate in verified
            for link in candidate.links
            for link_id in link.physical_link_ids
        }
        incomplete = []
        for candidate in verified:
            candidate_node_ids = {node.physical_node_id for node in candidate.nodes}
            incident_link_ids = {
                link_id
                for node_id in candidate_node_ids
                for link_id in index.link_ids_by_node.get(node_id, frozenset())
            }
            internal_link_ids = {
                link_id
                for link_id in incident_link_ids
                if index.links_by_id[link_id].left_node in candidate_node_ids
                and index.links_by_id[link_id].right_node in candidate_node_ids
            }
            unowned_internal = sorted(internal_link_ids - owned_link_ids)
            if unowned_internal:
                rejection_by_name[candidate.name] = (
                    f"internal physical links have no structure owner: {unowned_internal}",
                )
                incomplete.append(candidate)
        if not incomplete:
            break
        incomplete_names = {candidate.name for candidate in incomplete}
        verified = [candidate for candidate in verified if candidate.name not in incomplete_names]

    rejected = [
        RejectedStructure(candidate, rejection_by_name[candidate.name])
        for candidate in candidates
        if candidate.name in rejection_by_name
    ]
    accepted_link_ids = {
        link_id
        for candidate in verified
        for link in candidate.links
        for link_id in link.physical_link_ids
    }
    uncovered_link_ids = tuple(sorted(
        link.link_id for link in topology.links if link.link_id not in accepted_link_ids
    ))
    return StructureVerification(
        verified_structures=tuple(verified),
        rejected_structures=tuple(rejected),
        uncovered_link_ids=uncovered_link_ids,
        duplicate_link_ids=duplicate_link_ids,
    )
