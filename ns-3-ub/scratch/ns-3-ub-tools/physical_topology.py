from dataclasses import dataclass
from typing import Mapping


@dataclass(frozen=True)
class PhysicalNode:
    node_id: int
    node_type: str
    allocation_delay: str
    forward_delay: str
    attributes: Mapping[str, object]


@dataclass(frozen=True)
class PhysicalLink:
    link_id: int
    left_node: int
    left_port: int
    right_node: int
    right_port: int
    bandwidth: str
    delay: str
    attributes: Mapping[str, object]


@dataclass(frozen=True)
class PhysicalTopology:
    nodes: tuple[PhysicalNode, ...]
    links: tuple[PhysicalLink, ...]


def build_physical_topology(graph, link_infos) -> PhysicalTopology:
    nodes = []
    for node_id, data in graph.nodes(data=True):
        attributes = {
            key: value
            for key, value in data.items()
            if key not in {"type", "allocation_delay", "forward_delay"}
        }
        nodes.append(PhysicalNode(
            node_id=node_id,
            node_type=data["type"],
            allocation_delay=data["allocation_delay"],
            forward_delay=data["forward_delay"],
            attributes=attributes,
        ))

    links = []
    for link_id, link_info in enumerate(link_infos):
        left_node, left_port, right_node, right_port, bandwidth, delay = link_info
        data = graph.edges[left_node, right_node]
        attributes = {
            key: value
            for key, value in data.items()
            if key not in {"bandwidth", "delay", "edge_count"}
        }
        links.append(PhysicalLink(
            link_id=link_id,
            left_node=left_node,
            left_port=left_port,
            right_node=right_node,
            right_port=right_port,
            bandwidth=bandwidth,
            delay=delay,
            attributes=attributes,
        ))

    return PhysicalTopology(nodes=tuple(nodes), links=tuple(links))
