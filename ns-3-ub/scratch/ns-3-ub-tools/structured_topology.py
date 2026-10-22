import itertools
import json
from collections import defaultdict
from collections.abc import Mapping
from pathlib import Path

from net_sim_builder import NetworkSimulationGraph


SUPPORTED_VERSION = 1


def _mapping(value, field):
    if not isinstance(value, Mapping):
        raise ValueError(f"{field} must be an object")
    return value


def _list(value, field):
    if not isinstance(value, list):
        raise ValueError(f"{field} must be a list")
    return value


def _integer(value, field, minimum=0):
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"{field} must be an integer >= {minimum}")
    return value


def _name(value, field):
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    return value


def _reject_unknown_fields(value, allowed, field):
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise ValueError(f"{field} contains unknown fields: {', '.join(unknown)}")


class _StructuredTopologyBuilder:
    def __init__(self, data):
        self.data = _mapping(data, "topology")
        _reject_unknown_fields(
            self.data,
            {
                "version",
                "defaults",
                "nodeGroups",
                "groupConnections",
                "layeredConnections",
                "explicitConnections",
            },
            "topology",
        )
        if self.data.get("version") != SUPPORTED_VERSION:
            raise ValueError(f"version must be {SUPPORTED_VERSION}")

        defaults = _mapping(self.data.get("defaults", {}), "defaults")
        _reject_unknown_fields(defaults, {"node", "link"}, "defaults")
        self.node_defaults = _mapping(defaults.get("node", {}), "defaults.node")
        self.link_defaults = _mapping(defaults.get("link", {}), "defaults.link")
        _reject_unknown_fields(
            self.node_defaults,
            {"allocationDelay", "forwardDelay"},
            "defaults.node",
        )
        _reject_unknown_fields(
            self.link_defaults,
            {"bandwidth", "delay"},
            "defaults.link",
        )

        self.graph = NetworkSimulationGraph()
        self.groups = {}
        self.port_counts = []
        self.used_ports = defaultdict(set)
        self.links = []
        self.node_mapping = []

    def build(self):
        self._add_node_groups()
        self._add_explicit_connections()
        self._add_group_connections()
        self._add_layered_connections()
        self.graph.build_graph_config_from_links(self.links, self.port_counts)
        self.graph.graph["node_mapping"] = tuple(self.node_mapping)
        return self.graph

    def _add_node_groups(self):
        groups = _list(self.data.get("nodeGroups", []), "nodeGroups")
        if not groups:
            raise ValueError("nodeGroups must contain at least one group")

        for group_index, raw_group in enumerate(groups):
            field = f"nodeGroups[{group_index}]"
            group = _mapping(raw_group, field)
            _reject_unknown_fields(
                group,
                {
                    "name",
                    "type",
                    "count",
                    "portCount",
                    "allocationDelay",
                    "forwardDelay",
                    "systemId",
                },
                field,
            )
            group_name = _name(group.get("name"), f"{field}.name")
            if group_name in self.groups:
                raise ValueError(f"duplicate node group: {group_name}")
            node_type = group.get("type")
            if node_type not in {"device", "switch"}:
                raise ValueError(f"{field}.type must be device or switch")
            count = _integer(group.get("count"), f"{field}.count", minimum=1)
            port_count = group.get("portCount")
            if port_count is not None:
                port_count = _integer(port_count, f"{field}.portCount")
            allocation_delay = group.get(
                "allocationDelay", self.node_defaults.get("allocationDelay")
            )
            forward_delay = group.get("forwardDelay", self.node_defaults.get("forwardDelay"))
            if not isinstance(allocation_delay, str) or not allocation_delay:
                raise ValueError(f"{field} requires allocationDelay or defaults.node.allocationDelay")
            if not isinstance(forward_delay, str) or not forward_delay:
                raise ValueError(f"{field} requires forwardDelay or defaults.node.forwardDelay")
            system_id = _integer(group.get("systemId", 0), f"{field}.systemId")

            node_ids = []
            for index in range(count):
                node_id = len(self.port_counts)
                add_node = (
                    self.graph.add_netisim_host
                    if node_type == "device"
                    else self.graph.add_netisim_node
                )
                add_node(
                    node_id,
                    allocation_delay=allocation_delay,
                    forward_delay=forward_delay,
                    system_id=system_id,
                )
                node_ids.append(node_id)
                self.port_counts.append(port_count)
                self.node_mapping.append({
                    "group": group_name,
                    "index": index,
                    "nodeId": node_id,
                })
            self.groups[group_name] = tuple(node_ids)

    def _resolve_group_node(self, endpoint, field):
        group_name = _name(endpoint.get("group"), f"{field}.group")
        if group_name not in self.groups:
            raise ValueError(f"{field}.group references unknown group: {group_name}")
        index = _integer(endpoint.get("index"), f"{field}.index")
        if index >= len(self.groups[group_name]):
            raise ValueError(f"{field}.index is outside group {group_name}")
        return self.groups[group_name][index]

    def _link_properties(self, value, field, parent=None):
        parent = parent or {}
        bandwidth = value.get("bandwidth", parent.get("bandwidth", self.link_defaults.get("bandwidth")))
        delay = value.get("delay", parent.get("delay", self.link_defaults.get("delay")))
        if not isinstance(bandwidth, str) or not bandwidth:
            raise ValueError(f"{field} requires bandwidth or defaults.link.bandwidth")
        if not isinstance(delay, str) or not delay:
            raise ValueError(f"{field} requires delay or defaults.link.delay")
        return bandwidth, delay

    def _add_link(self, left, left_port, right, right_port, bandwidth, delay, field):
        if left == right:
            raise ValueError(f"{field} cannot connect a node to itself")
        for node_id, port_id in ((left, left_port), (right, right_port)):
            if port_id in self.used_ports[node_id]:
                raise ValueError(f"{field} reuses node {node_id} port {port_id}")
            declared_count = self.port_counts[node_id]
            if declared_count is not None and port_id >= declared_count:
                raise ValueError(
                    f"{field} uses node {node_id} port {port_id}, "
                    f"but portCount is {declared_count}"
                )
        self.used_ports[left].add(left_port)
        self.used_ports[right].add(right_port)
        self.links.append((left, left_port, right, right_port, bandwidth, delay))

    def _add_explicit_connections(self):
        connections = _list(
            self.data.get("explicitConnections", []),
            "explicitConnections",
        )
        for connection_index, raw_connection in enumerate(connections):
            field = f"explicitConnections[{connection_index}]"
            connection = _mapping(raw_connection, field)
            _reject_unknown_fields(
                connection,
                {"endpoints", "bandwidth", "delay"},
                field,
            )
            endpoints = _list(connection.get("endpoints"), f"{field}.endpoints")
            if len(endpoints) != 2:
                raise ValueError(f"{field}.endpoints must contain exactly two endpoints")
            resolved = []
            for endpoint_index, raw_endpoint in enumerate(endpoints):
                endpoint_field = f"{field}.endpoints[{endpoint_index}]"
                endpoint = _mapping(raw_endpoint, endpoint_field)
                _reject_unknown_fields(endpoint, {"group", "index", "port"}, endpoint_field)
                node_id = self._resolve_group_node(endpoint, endpoint_field)
                port_id = _integer(endpoint.get("port"), f"{endpoint_field}.port")
                resolved.append((node_id, port_id))
            bandwidth, delay = self._link_properties(connection, field)
            self._add_link(*resolved[0], *resolved[1], bandwidth, delay, field)

    def _connection_endpoint(self, raw_endpoint, field, node_ids=None):
        endpoint = _mapping(raw_endpoint, field)
        _reject_unknown_fields(endpoint, {"group", "ports"}, field)
        group_name = _name(endpoint.get("group"), f"{field}.group")
        if group_name not in self.groups:
            raise ValueError(f"{field}.group references unknown group: {group_name}")
        ports = endpoint.get("ports")
        if ports is not None:
            ports = _list(ports, f"{field}.ports")
            if len(ports) != 2:
                raise ValueError(f"{field}.ports must be [first, last]")
            first = _integer(ports[0], f"{field}.ports[0]")
            last = _integer(ports[1], f"{field}.ports[1]")
            if first > last:
                raise ValueError(f"{field}.ports first value must not exceed the last value")
            ports = (first, last)
        return {
            "group": group_name,
            "nodes": tuple(self.groups[group_name] if node_ids is None else node_ids),
            "ports": ports,
        }

    def _allocate_port(self, endpoint, node_id, field):
        declared_count = self.port_counts[node_id]
        port_range = endpoint["ports"]
        first = 0 if port_range is None else port_range[0]
        if port_range is not None:
            last = port_range[1]
        elif declared_count is not None:
            last = declared_count - 1
        else:
            last = None

        port_id = first
        while port_id in self.used_ports[node_id] and (last is None or port_id <= last):
            port_id += 1
        if last is not None and port_id > last:
            raise ValueError(f"{field} has no free port for node {node_id}")
        return port_id

    def _node_pairs(self, pattern, endpoints, field):
        if pattern in {"fullMesh", "line", "ring"}:
            if len(endpoints) != 1:
                raise ValueError(f"{field} pattern {pattern} requires one endpoint group")
            nodes = endpoints[0]["nodes"]
            if pattern == "fullMesh":
                return itertools.combinations(nodes, 2)
            if pattern == "line":
                return zip(nodes, nodes[1:])
            if len(nodes) < 3:
                raise ValueError(f"{field} pattern ring requires at least three nodes")
            return zip(nodes, (*nodes[1:], nodes[0]))

        if pattern not in {"allToAll", "oneToOne"}:
            raise ValueError(
                f"{field}.pattern must be fullMesh, allToAll, oneToOne, line, or ring"
            )
        if len(endpoints) != 2:
            raise ValueError(f"{field} pattern {pattern} requires two endpoint groups")
        left_nodes = endpoints[0]["nodes"]
        right_nodes = endpoints[1]["nodes"]
        if set(left_nodes) & set(right_nodes):
            raise ValueError(f"{field} must use disjoint groups; use fullMesh for one group")
        if pattern == "allToAll":
            return itertools.product(left_nodes, right_nodes)
        if len(left_nodes) != len(right_nodes):
            raise ValueError(f"{field} pattern oneToOne requires equal group sizes")
        return zip(left_nodes, right_nodes)

    def _expand_connection(self, connection, endpoints, field, parent=None):
        pattern = connection.get("pattern")
        if not isinstance(pattern, str):
            raise ValueError(f"{field}.pattern must be a string")
        parallel_links = _integer(
            connection.get("parallelLinks", 1),
            f"{field}.parallelLinks",
            minimum=1,
        )
        bandwidth, delay = self._link_properties(connection, field, parent=parent)
        for left, right in self._node_pairs(pattern, endpoints, field):
            for _ in range(parallel_links):
                left_port = self._allocate_port(endpoints[0], left, field)
                right_endpoint = endpoints[-1]
                right_port = self._allocate_port(right_endpoint, right, field)
                self._add_link(left, left_port, right, right_port, bandwidth, delay, field)

    def _add_group_connections(self):
        connections = _list(
            self.data.get("groupConnections", []),
            "groupConnections",
        )
        for connection_index, raw_connection in enumerate(connections):
            field = f"groupConnections[{connection_index}]"
            connection = _mapping(raw_connection, field)
            _reject_unknown_fields(
                connection,
                {"pattern", "endpoints", "parallelLinks", "bandwidth", "delay"},
                field,
            )
            raw_endpoints = _list(connection.get("endpoints"), f"{field}.endpoints")
            endpoints = [
                self._connection_endpoint(endpoint, f"{field}.endpoints[{index}]")
                for index, endpoint in enumerate(raw_endpoints)
            ]
            self._expand_connection(connection, endpoints, field)

    def _plane_nodes(self, nodes, plane_mode, plane_count, plane, field):
        if plane_mode == "shared":
            return nodes
        if len(nodes) % plane_count != 0:
            raise ValueError(f"{field} group size must be divisible by planeCount")
        count = len(nodes) // plane_count
        return nodes[plane * count:(plane + 1) * count]

    def _add_layered_connections(self):
        fabrics = _list(
            self.data.get("layeredConnections", []),
            "layeredConnections",
        )
        for fabric_index, raw_fabric in enumerate(fabrics):
            field = f"layeredConnections[{fabric_index}]"
            fabric = _mapping(raw_fabric, field)
            _reject_unknown_fields(
                fabric,
                {
                    "planeCount",
                    "planeMode",
                    "layers",
                    "connections",
                    "bandwidth",
                    "delay",
                },
                field,
            )
            plane_count = _integer(fabric.get("planeCount"), f"{field}.planeCount", minimum=1)
            plane_mode = fabric.get("planeMode")
            if plane_mode not in {"shared", "separate"}:
                raise ValueError(f"{field}.planeMode must be shared or separate")
            raw_layers = _list(fabric.get("layers"), f"{field}.layers")
            if len(raw_layers) < 2:
                raise ValueError(f"{field}.layers must contain at least two layers")
            layers = [
                self._connection_endpoint(layer, f"{field}.layers[{index}]")
                for index, layer in enumerate(raw_layers)
            ]
            connections = _list(fabric.get("connections"), f"{field}.connections")
            for connection_index, raw_connection in enumerate(connections):
                connection_field = f"{field}.connections[{connection_index}]"
                connection = _mapping(raw_connection, connection_field)
                _reject_unknown_fields(
                    connection,
                    {
                        "betweenLayers",
                        "pattern",
                        "parallelLinks",
                        "bandwidth",
                        "delay",
                    },
                    connection_field,
                )
                layer_indexes = _list(
                    connection.get("betweenLayers"),
                    f"{connection_field}.betweenLayers",
                )
                if len(layer_indexes) != 2:
                    raise ValueError(
                        f"{connection_field}.betweenLayers must contain two layer indexes"
                    )
                left_layer = _integer(
                    layer_indexes[0], f"{connection_field}.betweenLayers[0]"
                )
                right_layer = _integer(
                    layer_indexes[1], f"{connection_field}.betweenLayers[1]"
                )
                if left_layer == right_layer or max(left_layer, right_layer) >= len(layers):
                    raise ValueError(f"{connection_field}.betweenLayers is invalid")

                for plane in range(plane_count):
                    endpoints = []
                    for layer_index in (left_layer, right_layer):
                        layer = layers[layer_index]
                        endpoints.append({
                            **layer,
                            "nodes": tuple(self._plane_nodes(
                                layer["nodes"],
                                plane_mode,
                                plane_count,
                                plane,
                                f"{field}.layers[{layer_index}]",
                            )),
                        })
                    self._expand_connection(
                        connection,
                        endpoints,
                        f"{connection_field}.plane[{plane}]",
                        parent=fabric,
                    )


def build_structured_topology(data):
    """Expand a declarative topology into the shared NetworkSimulationGraph path."""
    return _StructuredTopologyBuilder(data).build()


def load_structured_topology(path):
    """Load a versioned JSON or YAML topology file."""
    source = Path(path).expanduser().resolve()
    suffix = source.suffix.lower()
    if suffix == ".json":
        data = json.loads(source.read_text(encoding="utf-8"))
    elif suffix in {".yaml", ".yml"}:
        try:
            import yaml
        except ImportError as error:
            raise ImportError("YAML input requires PyYAML from requirements.txt") from error
        data = yaml.safe_load(source.read_text(encoding="utf-8"))
    else:
        raise ValueError("topology file must use .json, .yaml, or .yml")
    return build_structured_topology(data)
