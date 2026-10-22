"""Load all supported topology source forms into one graph boundary."""

from __future__ import annotations

import importlib.util
from pathlib import Path

from case_topology_loader import load_case_topology
from net_sim_builder import NetworkSimulationGraph
from structured_topology import load_structured_topology


def _load_python_builder(reference: str):
    if ":" in reference:
        source, function_name = reference.rsplit(":", 1)
    else:
        source, function_name = reference, "build_topology"
    source_path = Path(source).expanduser().resolve()
    if not source_path.is_file():
        raise ValueError(f"topology builder file does not exist: {source_path}")
    if not function_name.isidentifier():
        raise ValueError(f"topology builder function name is invalid: {function_name}")
    spec = importlib.util.spec_from_file_location(
        f"_ns3_ub_topology_{abs(hash(source_path))}", source_path
    )
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load topology builder file: {source_path}")
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as error:
        raise ValueError(f"cannot import topology builder {source_path}: {error}") from error
    builder = getattr(module, function_name, None)
    if not callable(builder):
        raise ValueError(f"topology builder function is not callable: {reference}")
    return builder


def _require_graph(graph, source: str) -> NetworkSimulationGraph:
    if not isinstance(graph, NetworkSimulationGraph):
        raise TypeError(
            f"topology source {source} must produce NetworkSimulationGraph with explicit ports"
        )
    if not graph.port_list:
        graph.build_graph_config()
    return graph


def load_topology_source(source: str | Path) -> NetworkSimulationGraph:
    """Load Python, JSON/YAML, or an existing node.csv/topology.csv case."""
    source_text = str(source)
    python_path = source_text.rsplit(":", 1)[0] if ":" in source_text else source_text
    if Path(python_path).suffix == ".py":
        try:
            graph = _load_python_builder(source_text)()
        except Exception as error:
            raise ValueError(f"topology builder failed: {error}") from error
        return _require_graph(graph, source_text)

    source_path = Path(source_text).expanduser().resolve()
    if source_path.is_dir():
        if not (source_path / "node.csv").is_file() or not (source_path / "topology.csv").is_file():
            raise ValueError(f"CSV topology source requires node.csv and topology.csv: {source_path}")
        return _require_graph(load_case_topology(source_path), source_text)
    if source_path.suffix.lower() in {".json", ".yaml", ".yml"}:
        return _require_graph(load_structured_topology(source_path), source_text)
    if source_path.name == "node.csv":
        return _require_graph(load_case_topology(source_path.parent), source_text)
    raise ValueError(
        "topology source must be path.py, path.py:function, JSON/YAML, or a CSV case directory"
    )


def topology_summary(graph: NetworkSimulationGraph) -> dict[str, int]:
    topology = graph.get_physical_topology()
    return {
        "nodes": len(topology.nodes),
        "links": len(topology.links),
        "devices": sum(node.node_type == "host" for node in topology.nodes),
        "switches": sum(node.node_type != "host" for node in topology.nodes),
    }


def build_plot_graph(graph: NetworkSimulationGraph):
    """Return a MultiGraph so each physical link remains visible to plot callers."""
    import networkx as nx

    topology = graph.get_physical_topology()
    plotted = nx.MultiGraph()
    for node in topology.nodes:
        plotted.add_node(node.node_id, node_type=node.node_type)
    for link in topology.links:
        plotted.add_edge(
            link.left_node,
            link.right_node,
            left_port=link.left_port,
            right_port=link.right_port,
        )
    return plotted


def plot_topology_graph(graph: NetworkSimulationGraph, output_path: str | Path) -> Path:
    """Render the normalized physical graph without creating CSV intermediates."""
    import matplotlib.pyplot as plt
    import networkx as nx

    output = Path(output_path).expanduser().resolve()
    if output.exists():
        raise ValueError(f"plot output already exists: {output}; choose a new output path")
    plotted = build_plot_graph(graph)
    positions = nx.spring_layout(plotted, seed=7)
    colors = [
        "#d95f02" if plotted.nodes[node]["node_type"] == "host" else "#1b9e77"
        for node in plotted.nodes
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    figure, axis = plt.subplots(figsize=(10, 7))
    nx.draw_networkx(
        plotted,
        positions,
        ax=axis,
        node_color=colors,
        node_size=500,
        font_size=8,
        edge_color="#777777",
        with_labels=True,
    )
    axis.set_axis_off()
    figure.tight_layout()
    figure.savefig(output)
    plt.close(figure)
    return output
