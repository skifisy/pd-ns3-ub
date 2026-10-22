from collections import defaultdict
from dataclasses import dataclass
from topology_structure import StructureVerification, TopologyStructure


@dataclass(frozen=True)
class CompiledTopologyNode:
    node_id: int
    node_type: str
    coordinates: tuple[int, ...]


@dataclass(frozen=True)
class CompiledTopologyRule:
    rule_type: str
    shape: tuple[int, ...]
    connections: tuple[str, ...]
    nodes: tuple[CompiledTopologyNode, ...]


_SUPPORTED_STRUCTURE_TYPES = {
    "fullmesh",
    "dimension-network",
    "direct-switch-network",
    "two-layer-clos",
    "layered-switch-network",
    "dragonfly",
}


def _node_ids(structure: TopologyStructure) -> set[int]:
    return {node.physical_node_id for node in structure.nodes}


def _host_ids(structure: TopologyStructure) -> set[int]:
    return {
        node.physical_node_id
        for node in structure.nodes
        if node.node_type == "host"
    }


def _structure_components(structures: tuple[TopologyStructure, ...]):
    pending = set(range(len(structures)))
    node_sets = [_node_ids(structure) for structure in structures]
    while pending:
        component = {pending.pop()}
        changed = True
        while changed:
            changed = False
            component_nodes = set().union(*(node_sets[index] for index in component))
            joined = {
                index for index in pending if component_nodes & node_sets[index]
            }
            if joined:
                component.update(joined)
                pending.difference_update(joined)
                changed = True
        yield tuple(structures[index] for index in sorted(component))


def _backbone_attachments(structure: TopologyStructure):
    attachments = {}
    for node in structure.nodes:
        if node.node_type != "host":
            continue
        if structure.structure_type == "direct-switch-network":
            attachment = ("direct",)
        elif structure.structure_type in {"two-layer-clos", "layered-switch-network"}:
            attachment = ("leaf", node.coordinates[1])
        elif structure.structure_type == "dragonfly":
            attachment = ("router", node.coordinates[1], node.coordinates[2])
        else:
            raise ValueError(f"unsupported backbone structure {structure.structure_type}")
        attachments[node.physical_node_id] = attachment
    return attachments


def _same_host_partition(left: TopologyStructure,
                         right: TopologyStructure,
                         left_attachments,
                         right_attachments) -> bool:
    hosts = _host_ids(left)
    if hosts != _host_ids(right):
        return False
    if left.structure_type != right.structure_type:
        return False
    if left.structure_type == "dragonfly":
        # Different Dragonfly planes can have different global gateway wiring.
        return False

    def partition(structure):
        groups = defaultdict(list)
        attachments = left_attachments if structure is left else right_attachments
        for host in hosts:
            groups[attachments[host]].append(host)
        return tuple(sorted(tuple(sorted(group)) for group in groups.values()))

    return partition(left) == partition(right)


def _component_supports_direct_rules(component: tuple[TopologyStructure, ...]) -> bool:
    if len(component) == 1:
        return True

    node_sets = {structure.name: _node_ids(structure) for structure in component}
    for index, left in enumerate(component):
        for right in component[index + 1:]:
            shared = node_sets[left.name] & node_sets[right.name]
            if shared and not shared <= (_host_ids(left) & _host_ids(right)):
                return False

    local_networks = [
        structure for structure in component if structure.structure_type == "fullmesh"
    ]
    backbones = [
        structure
        for structure in component
        if structure.structure_type in {
            "direct-switch-network",
            "two-layer-clos",
            "layered-switch-network",
            "dragonfly",
        }
    ]
    if len(local_networks) + len(backbones) != len(component) or not backbones:
        return False

    attachments_by_backbone = {
        backbone.name: _backbone_attachments(backbone) for backbone in backbones
    }

    backbone_hosts = _host_ids(backbones[0])
    if any(_host_ids(backbone) != backbone_hosts for backbone in backbones[1:]):
        return False
    if any(
        not _same_host_partition(
            backbones[0],
            backbone,
            attachments_by_backbone[backbones[0].name],
            attachments_by_backbone[backbone.name],
        )
        for backbone in backbones[1:]
    ):
        return False

    local_host_sets = [_host_ids(network) for network in local_networks]
    if local_host_sets:
        if set().union(*local_host_sets) != backbone_hosts:
            return False
        if sum(map(len, local_host_sets)) != len(backbone_hosts):
            return False
        for hosts in local_host_sets:
            for backbone in backbones:
                group_attachments = {
                    attachments_by_backbone[backbone.name][host] for host in hosts
                }
                if (len(group_attachments) != 1 and
                        backbone.structure_type != "direct-switch-network"):
                    return False
    return True


def _compile_structure(structure: TopologyStructure) -> CompiledTopologyRule:
    if structure.structure_type == "fullmesh":
        rule_type = "dimension"
        shape = (int(structure.parameters["node_count"]),)
        connections = ("fullMesh",)
    elif structure.structure_type == "dimension-network":
        rule_type = "dimension"
        shape = tuple(map(int, structure.parameters["shape"]))
        connections = tuple(map(str, structure.parameters["connections"]))
    elif structure.structure_type == "direct-switch-network":
        rule_type = "direct-switch"
        shape = ()
        connections = ()
    elif structure.structure_type in {"two-layer-clos", "layered-switch-network"}:
        rule_type = "two-level-switch"
        shape = ()
        connections = ()
    elif structure.structure_type == "dragonfly":
        rule_type = "dragonfly"
        shape = ()
        connections = ()
    else:
        raise ValueError(f"unsupported topology rule structure {structure.structure_type}")

    return CompiledTopologyRule(
        rule_type=rule_type,
        shape=shape,
        connections=connections,
        nodes=tuple(
            CompiledTopologyNode(
                node_id=node.physical_node_id,
                node_type=node.node_type,
                coordinates=tuple(map(int, node.coordinates)),
            )
            for node in structure.nodes
        ),
    )


def compile_topology_rules(
        verification: StructureVerification) -> tuple[CompiledTopologyRule, ...] | None:
    structures = verification.verified_structures
    if not verification.is_fully_covered or not structures:
        return None
    if any(
        not structure.rule_capable
        or structure.structure_type not in _SUPPORTED_STRUCTURE_TYPES
        for structure in structures
    ):
        return None
    if any(
        not _component_supports_direct_rules(component)
        for component in _structure_components(structures)
    ):
        return None
    return tuple(_compile_structure(structure) for structure in structures)


class TopologyRuleIndex:
    def __init__(self, network, rules: tuple[CompiledTopologyRule, ...]):
        self.rules = tuple(_IndexedRule(rule) for rule in rules)
        self.rules_by_node = defaultdict(list)
        for index, rule in enumerate(self.rules):
            for node_id in rule.coordinates:
                self.rules_by_node[node_id].append(index)

        self.link_by_endpoint = {}
        self.adjacency = defaultdict(list)
        for link in network.get_physical_topology().links:
            left = (link.left_node, link.left_port)
            right = (link.right_node, link.right_port)
            if left in self.link_by_endpoint or right in self.link_by_endpoint:
                raise ValueError("a physical port must belong to exactly one link")
            self.link_by_endpoint[left] = right
            self.link_by_endpoint[right] = left
            self.adjacency[link.left_node].append(
                (link.right_node, link.left_port, link.right_port)
            )
            self.adjacency[link.right_node].append(
                (link.left_node, link.right_port, link.left_port)
            )

        for rule in self.rules:
            rule.bind_adjacency(self.adjacency)

    def common_rules(self, left: int, right: int):
        right_rules = set(self.rules_by_node.get(right, ()))
        return (
            self.rules[index]
            for index in self.rules_by_node.get(left, ())
            if index in right_rules
        )

    def global_distance(self, left: int, right: int) -> int | None:
        distances = [rule.distance(left, right) for rule in self.common_rules(left, right)]
        return min(distances) if distances else None

    def lookup(self,
               current: int,
               destination: int,
               destination_port: int) -> dict[int, int]:
        if current == destination:
            return {}
        final_link = self.link_by_endpoint.get((destination, destination_port))
        if final_link is None:
            return {}
        predecessor, predecessor_port = final_link
        global_distance = self.global_distance(current, destination)
        if global_distance is None:
            return {}

        candidates = set()
        candidate_metric = None
        for rule in self.common_rules(destination, predecessor):
            if not rule.are_neighbors(destination, predecessor) or not rule.contains(current):
                continue
            distance_to_predecessor = rule.distance(current, predecessor)
            distance_via_predecessor = distance_to_predecessor + 1
            if distance_via_predecessor != global_distance:
                continue
            if not self._has_upstream_source(
                rule,
                current,
                destination,
                distance_via_predecessor,
            ):
                continue
            if current == predecessor:
                candidates.add(predecessor_port)
            else:
                for peer, local_port, _peer_port in self.adjacency[current]:
                    if (
                        rule.contains(peer)
                        and rule.are_neighbors(current, peer)
                        and rule.distance(peer, predecessor) + 1 == distance_to_predecessor
                    ):
                        candidates.add(local_port)
            candidate_metric = distance_via_predecessor

        if candidate_metric is None:
            return {}
        return {port: candidate_metric for port in sorted(candidates)}

    def _has_upstream_source(self,
                             rule,
                             current: int,
                             destination: int,
                             remaining_distance: int) -> bool:
        if rule.node_types[current] == "host":
            return current != destination
        if rule.rule.rule_type == "two-level-switch":
            destination_leaf = rule._attached_leaf(destination)
            role = rule.coordinates[current][0]
            if role == 1:
                if current != destination_leaf:
                    return True
                return any(
                    coordinates[0] == 1 and node_id != destination_leaf
                    for node_id, coordinates in rule.coordinates.items()
                )
            return (
                destination_leaf in rule.adjacency[current]
                and any(
                    rule.coordinates[peer][0] == 1 and peer != destination_leaf
                    for peer in rule.adjacency[current]
                )
            )
        if rule.rule.rule_type == "dragonfly":
            destination_router, _attachment = rule._dragonfly_router(destination)
            if current != destination_router:
                return any(
                    rule.node_types[peer] == "host" and peer != destination
                    for peer in rule.adjacency[current]
                )
            return any(
                source != destination
                and rule._dragonfly_router(source)[0] != destination_router
                for source in rule.hosts
            )

        # Direct-switch structures can overlap local host-only structures. Keep the
        # global-distance check here so a switch is not used when every source has a
        # shorter direct route to the destination.
        for source in rule.hosts:
            if source == destination:
                continue
            source_to_current = rule.distance(source, current)
            source_to_destination = rule.distance(source, destination)
            if source_to_current + remaining_distance != source_to_destination:
                continue
            if self.global_distance(source, destination) == source_to_destination:
                return True
        return False


class _IndexedRule:
    def __init__(self, rule: CompiledTopologyRule):
        self.rule = rule
        self.coordinates = {node.node_id: node.coordinates for node in rule.nodes}
        self.node_types = {node.node_id: node.node_type for node in rule.nodes}
        self.hosts = tuple(
            node.node_id for node in rule.nodes if node.node_type == "host"
        )
        self.adjacency = {}

    def bind_adjacency(self, physical_adjacency):
        self.adjacency = {
            node_id: frozenset(
                peer
                for peer, _local_port, _peer_port in physical_adjacency[node_id]
                if peer in self.coordinates and self.are_neighbors(node_id, peer)
            )
            for node_id in self.coordinates
        }

    def contains(self, node_id: int) -> bool:
        return node_id in self.coordinates

    def are_neighbors(self, left: int, right: int) -> bool:
        left_coordinates = self.coordinates[left]
        right_coordinates = self.coordinates[right]
        if self.rule.rule_type == "dimension":
            differences = [
                dimension
                for dimension, values in enumerate(zip(left_coordinates, right_coordinates))
                if values[0] != values[1]
            ]
            if len(differences) != 1:
                return False
            dimension = differences[0]
            delta = abs(left_coordinates[dimension] - right_coordinates[dimension])
            connection = self.rule.connections[dimension]
            return (
                connection == "fullMesh"
                or delta == 1
                or (connection == "ring" and delta == self.rule.shape[dimension] - 1)
            )
        if self.rule.rule_type == "direct-switch":
            return left_coordinates[0] != right_coordinates[0]
        if self.rule.rule_type == "two-level-switch":
            left_role = left_coordinates[0]
            right_role = right_coordinates[0]
            if {left_role, right_role} == {0, 1}:
                host = left_coordinates if left_role == 0 else right_coordinates
                leaf = left_coordinates if left_role == 1 else right_coordinates
                return host[1] == leaf[1]
            return {left_role, right_role} == {1, 2}
        if self.rule.rule_type == "dragonfly":
            left_role = left_coordinates[0]
            right_role = right_coordinates[0]
            if {left_role, right_role} == {0, 1}:
                host = left_coordinates if left_role == 0 else right_coordinates
                router = left_coordinates if left_role == 1 else right_coordinates
                return host[1:3] == router[1:3]
            return left_role == right_role == 1
        raise ValueError(f"unknown topology rule type {self.rule.rule_type}")

    def distance(self, left: int, right: int) -> int:
        if left == right:
            return 0
        left_coordinates = self.coordinates[left]
        right_coordinates = self.coordinates[right]
        if self.rule.rule_type == "dimension":
            distance = 0
            for dimension, connection in enumerate(self.rule.connections):
                delta = abs(left_coordinates[dimension] - right_coordinates[dimension])
                if connection == "line":
                    distance += delta
                elif connection == "ring":
                    distance += min(delta, self.rule.shape[dimension] - delta)
                elif connection == "fullMesh":
                    distance += int(delta != 0)
                else:
                    raise ValueError(f"unknown dimension connection {connection}")
            return distance
        if self.rule.rule_type == "direct-switch":
            return 1 if left_coordinates[0] != right_coordinates[0] else 2
        if self.rule.rule_type == "two-level-switch":
            return self._two_level_distance(
                left,
                right,
                left_coordinates,
                right_coordinates,
            )
        if self.rule.rule_type == "dragonfly":
            left_router, left_attachment = self._dragonfly_router(left)
            right_router, right_attachment = self._dragonfly_router(right)
            return (
                left_attachment
                + self._router_distance(left_router, right_router)
                + right_attachment
            )
        raise ValueError(f"unknown topology rule type {self.rule.rule_type}")

    def _two_level_distance(self, left_id, right_id, left, right) -> int:
        left_role = left[0]
        right_role = right[0]
        if left_role == 0 and right_role == 0:
            return 2 if self._attached_leaf(left_id) == self._attached_leaf(right_id) else 4
        if left_role == 2 and right_role == 2:
            shared_leaves = {
                node
                for node in self.adjacency[left_id] & self.adjacency[right_id]
                if self.coordinates[node][0] == 1
            }
            return 2 if shared_leaves else 4
        if {left_role, right_role} == {0, 2}:
            host_id = left_id if left_role == 0 else right_id
            upper_id = left_id if left_role == 2 else right_id
            return 2 if upper_id in self.adjacency[self._attached_leaf(host_id)] else 4
        if {left_role, right_role} == {1, 2}:
            return 1 if right_id in self.adjacency[left_id] else 3
        if left_role == 1 and right_role == 1:
            return 2
        host_id = left_id if left_role == 0 else right_id
        leaf_id = left_id if left_role == 1 else right_id
        return 1 if self._attached_leaf(host_id) == leaf_id else 3

    def _attached_leaf(self, host_id: int) -> int:
        return next(
            peer
            for peer in self.adjacency[host_id]
            if self.coordinates[peer][0] == 1
        )

    def _dragonfly_router(self, node_id: int):
        coordinates = self.coordinates[node_id]
        if coordinates[0] == 1:
            return node_id, 0
        router_coordinates = (1, coordinates[1], coordinates[2], 0)
        router = next(
            candidate
            for candidate, values in self.coordinates.items()
            if values == router_coordinates
        )
        return router, 1

    def _router_distance(self, left: int, right: int) -> int:
        if left == right:
            return 0
        if right in self.adjacency[left]:
            return 1
        if self.adjacency[left] & self.adjacency[right]:
            return 2
        # A verified classic Dragonfly has a direct inter-group link and full-mesh groups,
        # so every router pair is at most three hops apart.
        return 3


def build_topology_rule_lookups_for_destination(network,
                                                index: TopologyRuleIndex,
                                                destination: int):
    topology = network.get_physical_topology()
    destination_ports = set()
    for link in topology.links:
        if link.left_node == destination:
            destination_ports.add(link.left_port)
        elif link.right_node == destination:
            destination_ports.add(link.right_port)

    lookups = {}
    for destination_port in sorted(destination_ports):
        for current in sorted(network.nodes):
            candidates = index.lookup(current, destination, destination_port)
            if candidates:
                lookups[(current, destination, destination_port)] = candidates
    return lookups


def build_topology_rule_lookups(network, rules: tuple[CompiledTopologyRule, ...]):
    index = TopologyRuleIndex(network, rules)
    lookups = {}
    for destination in sorted(network.host_ids):
        lookups.update(
            build_topology_rule_lookups_for_destination(
                network,
                index,
                destination,
            )
        )
    return lookups
