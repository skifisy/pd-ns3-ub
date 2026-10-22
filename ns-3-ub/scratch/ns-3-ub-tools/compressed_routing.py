"""Portable compressed next-hop artifact shared with the Unified Bus runtime."""

from array import array
from dataclasses import dataclass
import itertools
import struct
import sys
import zlib

from route_split import (
    build_shortest_route_lookups_for_destination,
    group_route_candidates,
)
from topology_routing import TopologyRuleIndex


MAGIC = b"UBCRIDX\0"
FORMAT_MAJOR = 1
FORMAT_MINOR = 0
ENDIAN_MARKER = 0x01020304
PORTS_PER_ADDRESS_BLOCK = 256
INVALID_INDEX = 0xFFFFFFFF
HEADER_FORMAT = "<8sHH7IBBH32sQI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


@dataclass(frozen=True)
class CompressedRoutingArtifact:
    data: bytes
    node_count: int
    row_count: int
    destination_count: int
    endpoint_count: int
    route_class_count: int
    port_id_width_bits: int
    node_id_width_bits: int
    route_lookup_count: int


@dataclass(frozen=True)
class CompressedRoutingIndex:
    node_count: int
    row_count: int
    destination_count: int
    endpoint_count: int
    classes: tuple
    row_by_node: tuple
    destination_by_node: tuple
    endpoint_by_address: tuple
    port_routes: tuple
    node_routes: tuple
    fingerprint: str

    def lookup(self, current, destination, destination_port=None):
        if not 0 <= current < self.node_count or not 0 <= destination < self.node_count:
            return None
        row = self.row_by_node[current]
        destination_index = self.destination_by_node[destination]
        if row == INVALID_INDEX or destination_index == INVALID_INDEX:
            return None
        if destination_port is None:
            class_id = self.node_routes[row * self.destination_count + destination_index]
        else:
            if not 0 <= destination_port < PORTS_PER_ADDRESS_BLOCK - 1:
                return None
            endpoint = self.endpoint_by_address[
                destination * PORTS_PER_ADDRESS_BLOCK + destination_port
            ]
            if endpoint == INVALID_INDEX:
                return None
            class_id = self.port_routes[row * self.endpoint_count + endpoint]
        return None if class_id == 0 else self.classes[class_id]


def _pack_uints(values, width):
    if isinstance(values, array) and values.itemsize == width:
        if sys.byteorder == "little" or width == 1:
            return values.tobytes()
        packed = array(values.typecode, values)
        packed.byteswap()
        return packed.tobytes()
    if width == 1:
        return array("B", values).tobytes()
    code = "H" if width == 2 else "I"
    packed = array(code, values)
    if sys.byteorder != "little":
        packed.byteswap()
    return packed.tobytes()


def _id_width(largest_value):
    if largest_value <= 0xFF:
        return 1
    if largest_value <= 0xFFFF:
        return 2
    return 4


class _MutablePackedIds:
    def __init__(self, size):
        self.values = array("B", [0]) * size

    @property
    def width(self):
        return self.values.itemsize

    def _ensure_width(self, value):
        if value <= 0xFF or self.values.itemsize > 1:
            if value <= 0xFFFF or self.values.itemsize > 2:
                return
        typecode = "H" if value <= 0xFFFF else "I"
        self.values = array(typecode, self.values)

    def set(self, index, value):
        self._ensure_width(value)
        self.values[index] = value

    def get(self, index):
        return self.values[index]


def _unpack_uints(data, offset, count, width):
    if width not in {1, 2, 4}:
        raise ValueError("compressed routing class ID width must be 1, 2, or 4 bytes")
    byte_count = count * width
    end = offset + byte_count
    if end > len(data):
        raise ValueError("truncated compressed routing payload")
    if width == 1:
        return tuple(data[offset:end]), end
    code = "H" if width == 2 else "I"
    return struct.unpack_from(f"<{count}{code}", data, offset), end


def read_compressed_routing_artifact(data, expected_fingerprint=None):
    if len(data) < HEADER_SIZE:
        raise ValueError("truncated compressed routing header")
    header = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
    (
        magic,
        major,
        minor,
        endian_marker,
        header_size,
        node_count,
        row_count,
        destination_count,
        endpoint_count,
        class_count,
        port_width,
        node_width,
        reserved,
        fingerprint,
        payload_size,
        checksum,
    ) = header
    if magic != MAGIC:
        raise ValueError("invalid compressed routing magic")
    if (major, minor) != (FORMAT_MAJOR, FORMAT_MINOR):
        raise ValueError(f"unsupported compressed routing version: {major}.{minor}")
    if endian_marker != ENDIAN_MARKER or header_size != HEADER_SIZE or reserved != 0:
        raise ValueError("invalid compressed routing header")
    fingerprint_hex = fingerprint.hex()
    if expected_fingerprint is not None and fingerprint_hex != expected_fingerprint:
        raise ValueError("compressed routing artifact fingerprint mismatch")
    payload = data[HEADER_SIZE:]
    if len(payload) != payload_size:
        raise ValueError("compressed routing payload length mismatch")
    if zlib.crc32(payload) & 0xFFFFFFFF != checksum:
        raise ValueError("compressed routing payload checksum mismatch")

    offset = 0
    row_by_node, offset = _unpack_uints(payload, offset, node_count, 4)
    destination_by_node, offset = _unpack_uints(payload, offset, node_count, 4)
    endpoint_by_address, offset = _unpack_uints(
        payload,
        offset,
        node_count * PORTS_PER_ADDRESS_BLOCK,
        4,
    )
    classes = []
    for _class_id in range(class_count):
        if offset + 8 > len(payload):
            raise ValueError("truncated compressed routing class")
        shortest_count, non_shortest_count = struct.unpack_from("<II", payload, offset)
        offset += 8
        shortest, offset = _unpack_uints(payload, offset, shortest_count, 2)
        non_shortest, offset = _unpack_uints(payload, offset, non_shortest_count, 2)
        if (tuple(sorted(set(shortest))) != shortest or
                tuple(sorted(set(non_shortest))) != non_shortest):
            raise ValueError("compressed routing candidate ports must be sorted and unique")
        if set(shortest) & set(non_shortest):
            raise ValueError(
                "compressed routing shortest and non-shortest candidates overlap"
            )
        classes.append((shortest, non_shortest))
    if not classes or classes[0] != ((), ()):
        raise ValueError("compressed routing class zero must be empty")
    port_routes, offset = _unpack_uints(
        payload,
        offset,
        row_count * endpoint_count,
        port_width,
    )
    node_routes, offset = _unpack_uints(
        payload,
        offset,
        row_count * destination_count,
        node_width,
    )
    if offset != len(payload):
        raise ValueError("compressed routing payload has unused bytes")
    if any(class_id >= class_count for class_id in itertools.chain(port_routes, node_routes)):
        raise ValueError("compressed routing class ID is out of range")
    return CompressedRoutingIndex(
        node_count,
        row_count,
        destination_count,
        endpoint_count,
        tuple(classes),
        row_by_node,
        destination_by_node,
        endpoint_by_address,
        port_routes,
        node_routes,
        fingerprint_hex,
    )


def _destination_ports(network):
    ports = {int(node_id): set() for node_id in network.host_ids}
    for link in network.get_physical_topology().links:
        if link.left_node in ports:
            ports[link.left_node].add(int(link.left_port))
        if link.right_node in ports:
            ports[link.right_node].add(int(link.right_port))
    return {
        node_id: tuple(sorted(node_ports))
        for node_id, node_ports in sorted(ports.items())
        if node_ports
    }


class _CompiledRuleLookup:
    def __init__(self, config):
        groups = {
            (group.node_id, group.group_id): tuple(sorted(set(group.ports)))
            for group in config.port_groups
        }

        def resolve(current, shortest_group_ids, non_shortest_group_ids):
            shortest = tuple(sorted({
                port
                for group_id in shortest_group_ids
                for port in groups[(current, group_id)]
            }))
            non_shortest = tuple(sorted({
                port
                for group_id in non_shortest_group_ids
                for port in groups[(current, group_id)]
            }))
            return shortest, non_shortest

        self.exact = {}
        for rule in config.routing_rules:
            key = (rule.current_node_id, rule.destination_node_id, rule.destination_port_id)
            if key in self.exact:
                raise ValueError(f"duplicate compiled routing rule: {key}")
            self.exact[key] = resolve(
                rule.current_node_id,
                rule.shortest_port_groups,
                rule.non_shortest_port_groups,
            )

        self.ranges = {}
        for rule in config.routing_range_rules:
            key = (rule.current_node_id, rule.destination_port_id)
            entries = self.ranges.setdefault(key, [])
            if entries and entries[-1][1] >= rule.destination_start_node_id:
                raise ValueError(f"overlapping compiled routing ranges: {key}")
            entries.append((
                rule.destination_start_node_id,
                rule.destination_end_node_id,
                resolve(
                    rule.current_node_id,
                    rule.shortest_port_groups,
                    rule.non_shortest_port_groups,
                ),
            ))

    def lookup(self, current, destination, destination_port):
        candidates = self.exact.get((current, destination, destination_port))
        if candidates is not None:
            return candidates
        for start, end, range_candidates in self.ranges.get(
            (current, destination_port), ()
        ):
            if start <= destination <= end:
                return range_candidates
            if start > destination:
                break
        return None


def _table_candidates(config):
    candidates = {}
    for lookup, metrics in config.table_lookups.items():
        grouped = group_route_candidates(metrics)
        if grouped is None:
            raise ValueError(f"empty table route: {lookup}")
        candidates[tuple(map(int, lookup))] = (
            tuple(map(int, grouped.shortest_ports)),
            tuple(map(int, grouped.non_shortest_ports)),
        )
    return candidates


def build_compressed_routing_artifact(network,
                                      config,
                                      fingerprint,
                                      use_shortest_dag_fast_path=True):
    """Compile final runtime lookup matrices without an intermediate text route table."""
    if len(fingerprint) != 64:
        raise ValueError("compressed routing fingerprint must be a SHA-256 hex digest")
    try:
        fingerprint_bytes = bytes.fromhex(fingerprint)
    except ValueError as error:
        raise ValueError("compressed routing fingerprint must be hexadecimal") from error

    node_count = len(network.port_list)
    destination_ports = _destination_ports(network)
    destinations = tuple(destination_ports)
    endpoints = tuple(
        (destination, port)
        for destination in destinations
        for port in destination_ports[destination]
    )
    if not endpoints:
        raise ValueError("compressed routing requires at least one destination endpoint")

    row_count = node_count
    destination_index = {node_id: index for index, node_id in enumerate(destinations)}
    endpoint_index = {endpoint: index for index, endpoint in enumerate(endpoints)}
    port_routes = _MutablePackedIds(row_count * len(endpoints))
    classes = [((), ())]
    class_ids = {classes[0]: 0}

    def intern(candidates, context):
        # Every compiler source already returns sorted, unique physical ports.
        normalized = (tuple(candidates[0]), tuple(candidates[1]))
        overlap = set(normalized[0]) & set(normalized[1])
        if overlap:
            raise ValueError(
                "compressed routing cannot assign physical ports to both shortest "
                f"and non-shortest candidates for {context}: {sorted(overlap)}"
            )
        if not normalized[0] and not normalized[1]:
            return 0
        class_id = class_ids.get(normalized)
        if class_id is None:
            class_id = len(classes)
            class_ids[normalized] = class_id
            classes.append(normalized)
        return class_id

    table = _table_candidates(config)
    used_table_lookups = set()
    direct_rules = _CompiledRuleLookup(config)
    topology = TopologyRuleIndex(network, config.topology_rules) if config.topology_rules else None

    route_lookup_count = 0

    def add_lookup(lookup, candidates):
        nonlocal route_lookup_count
        current, destination, destination_port = lookup
        port_routes.set(
            current * len(endpoints) + endpoint_index[(destination, destination_port)],
            intern(candidates, f"route lookup {lookup}"),
        )
        route_lookup_count += 1

    complete_topology_rules = (
        use_shortest_dag_fast_path
        and topology is not None
        and not direct_rules.exact
        and not direct_rules.ranges
        and not table
    )
    if complete_topology_rules:
        # One reverse shortest-path DAG per destination emits only valid lookups.
        # This materializes the speed-first matrix without interpreting one rule
        # for every empty current-node/destination-port cell.
        for destination in destinations:
            lookups = build_shortest_route_lookups_for_destination(network, destination)
            for lookup, metrics in lookups.items():
                grouped = group_route_candidates(metrics)
                if grouped is None:
                    raise ValueError(f"empty shortest-path lookup: {lookup}")
                add_lookup(lookup, (grouped.shortest_ports, grouped.non_shortest_ports))
    else:
        for current in range(node_count):
            for destination, destination_port in endpoints:
                lookup = (current, destination, destination_port)
                candidates = direct_rules.lookup(*lookup)
                if candidates is None and topology is not None:
                    metrics = topology.lookup(*lookup)
                    grouped = group_route_candidates(metrics)
                    if grouped is not None:
                        candidates = (grouped.shortest_ports, grouped.non_shortest_ports)
                if candidates is None:
                    candidates = table.get(lookup)
                    if candidates is not None:
                        used_table_lookups.add(lookup)
                if candidates is not None:
                    add_lookup(lookup, candidates)

    unused_table = set(table) - used_table_lookups
    if unused_table:
        example = min(unused_table)
        raise ValueError(f"table route is outside the compiled endpoint space: {example}")
    if route_lookup_count == 0:
        raise ValueError("compressed routing contains no route lookups")

    node_routes = _MutablePackedIds(row_count * len(destinations))
    for current in range(row_count):
        row_offset = current * len(endpoints)
        for destination in destinations:
            shortest = set()
            non_shortest = set()
            for destination_port in destination_ports[destination]:
                class_id = port_routes.get(
                    row_offset + endpoint_index[(destination, destination_port)]
                )
                if class_id == 0:
                    continue
                shortest.update(classes[class_id][0])
                non_shortest.update(classes[class_id][1])
            if shortest or non_shortest:
                node_routes.set(
                    current * len(destinations) + destination_index[destination],
                    intern(
                        (tuple(sorted(shortest)), tuple(sorted(non_shortest))),
                        f"node-primary lookup ({current}, {destination})",
                    ),
                )

    row_by_node = tuple(range(node_count))
    destination_by_node = [INVALID_INDEX] * node_count
    for node_id, index in destination_index.items():
        destination_by_node[node_id] = index
    endpoint_by_address = [INVALID_INDEX] * (node_count * PORTS_PER_ADDRESS_BLOCK)
    for (node_id, port_id), index in endpoint_index.items():
        if not 0 <= port_id < PORTS_PER_ADDRESS_BLOCK - 1:
            raise ValueError(f"destination port cannot be encoded in a UB address: {port_id}")
        endpoint_by_address[node_id * PORTS_PER_ADDRESS_BLOCK + port_id] = index

    port_width = port_routes.width
    node_width = node_routes.width
    payload = bytearray()
    payload.extend(_pack_uints(row_by_node, 4))
    payload.extend(_pack_uints(destination_by_node, 4))
    payload.extend(_pack_uints(endpoint_by_address, 4))
    for shortest, non_shortest in classes:
        payload.extend(struct.pack("<II", len(shortest), len(non_shortest)))
        payload.extend(_pack_uints(shortest, 2))
        payload.extend(_pack_uints(non_shortest, 2))
    payload.extend(_pack_uints(port_routes.values, port_width))
    payload.extend(_pack_uints(node_routes.values, node_width))

    checksum = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_MAJOR,
        FORMAT_MINOR,
        ENDIAN_MARKER,
        HEADER_SIZE,
        node_count,
        row_count,
        len(destinations),
        len(endpoints),
        len(classes),
        port_width,
        node_width,
        0,
        fingerprint_bytes,
        len(payload),
        checksum,
    )
    return CompressedRoutingArtifact(
        data=header + payload,
        node_count=node_count,
        row_count=row_count,
        destination_count=len(destinations),
        endpoint_count=len(endpoints),
        route_class_count=len(classes) - 1,
        port_id_width_bits=port_width * 8,
        node_id_width_bits=node_width * 8,
        route_lookup_count=route_lookup_count,
    )
