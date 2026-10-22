[中文](README.md) | **English**

# ns-3-ub-tools

`ns-3-ub-tools` prepares OpenUSim / ns-3 Unified Bus case directories. The unified `ub_tools.py`
entry point handles topology, routing, traffic, case validation, and trace analysis. Simulation is
started separately by a scratch runner in the `ns-3-ub` repository.

## Installation

Python 3.11 or later is required.

```bash
uv venv
source .venv/bin/activate
uv pip install -r requirements.txt
python3 ub_tools.py --help
```

## Recommended workflow

Run these commands from the root of the `ns-3-ub` repository.

Activate the tools environment first:

```bash
source scratch/ns-3-ub-tools/.venv/bin/activate
```

### 1. Prepare topology and routes

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py topology check \
  scratch/structured_topo_example.py
```

Output:

```text
{"devices": 4, "links": 14, "nodes": 6, "switches": 2}
```

Build the case:

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py topology build \
  scratch/structured_topo_example.py \
  --output-dir path/to/case \
  --route-generation mixed
```

Output:

```text
structured case prepared: path/to/case nodes=6 links=14 route-generation=mixed
```

The directory now contains:

```text
path/to/case/compressed_routing.bin
path/to/case/structured_summary.json
path/to/case/structured_topology.txt
```

A topology source may be a Python/NetworkX builder, a JSON/YAML declaration, or a directory containing
`node.csv` and `topology.csv`. The default structured output contains:

- `structured_topology.txt`: physical topology and verified structure rules;
- `compressed_routing.bin`: indexed route candidates loaded directly by C++;
- `structured_summary.json`: format version, structure coverage, rule counts, and route statistics.

Add `network_attribute.txt` and `traffic.csv` to make the directory runnable.
`transport_channel.csv` is optional.

### 2. Generate traffic

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py traffic allreduce \
  --host-count 4 \
  --data-size 1MB \
  --algorithm ring \
  --group-layout contiguous \
  --output-dir path/to/case
```

`--data-size` is the complete logical collective data object processed by each rank, not the bytes in
one transfer or phase. See the [traffic guide](traffic_maker/README_en.md) for algorithms, placement,
AllToAllV distributions, and SimAI conversion.

### 3. Inspect and validate the case

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py case show path/to/case
python3 scratch/ns-3-ub-tools/ub_tools.py case check path/to/case --json
```

`case show` can inspect a partially prepared directory. `case check` validates all files required by
the selected runner and should pass before simulation starts.

### 4. Run the simulation

```bash
python3 scratch/run_structured_topo_case.py \
  --case-path path/to/case \
  --route-runtime-mode compressed-table-lookup
```

The runner does not import NetworkX or compile routes. Python and C++ exchange prepared files through
the case directory; there is no stdin protocol or cross-language runtime call.

### 5. Parse traces

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py trace parse \
  path/to/case \
  --output-dir path/to/analysis
```

The command writes `task_statistics.csv` and `throughput.csv`.

## Python topology interface

A Python topology source should expose a no-argument `build_topology()` function:

```python
from net_sim_builder import NetworkSimulationGraph


def build_topology() -> NetworkSimulationGraph:
    graph = NetworkSimulationGraph()
    graph.add_netisim_host(0, allocation_delay="1ns", forward_delay="0ns")
    graph.add_netisim_host(1, allocation_delay="1ns", forward_delay="0ns")
    graph.add_netisim_edge(0, 1, bandwidth="400Gbps", delay="20ns")
    return graph
```

Accepted source forms are:

```text
path.py             call build_topology()
path.py:function    call the named function
path.json/yaml      load a structured declaration
CSV case directory  load node.csv and topology.csv
```

`user_topo_example.py`, `user_topo_2layer_clos.py`, and `user_topo_4x4_2DFM.py` are ready-to-use
examples. Keep graph construction in `build_topology()` and, when direct CSV generation is also
needed, keep file-writing operations in `main()`.

## Routing choices

Offline route generation:

| `--route-generation` | Behavior |
| --- | --- |
| `mixed` | Default. Use verified structures and derive uncovered candidates from the graph |
| `structured` | Require verified structures to cover every route query |
| `full` | Derive every candidate from the physical graph for compatibility or comparison |

Runtime candidate lookup:

| `--route-runtime-mode` | Behavior |
| --- | --- |
| `compressed-table-lookup` | Default. Query `compressed_routing.bin`; works with all generation modes |
| `structured-rule-compute` | Compute candidates from structure rules; requires usable rules |

These choices change only how candidate ports are obtained. HASH, round-robin, adaptive, packet spray,
and one-detour behavior continue through the existing `GetOutPort()` path.

Add `--verify-routes` when an exhaustive comparison with the physical-graph shortest-path baseline is
required. This significantly increases preparation time on large topologies.

Automatic recognition covers FullMesh, Ring, Mesh, Torus, N-dimensional Dimension FullMesh, direct
switch networks, two-level and multi-plane switch networks, classic Dragonfly, pair-design incidence
networks such as Balanced Sparse Tree, switch port extension, and verified safe combinations. Uncovered
parts remain correct under `mixed` generation through graph-derived fallback candidates.

## Migrating existing cases

### Python topology scripts

Move graph construction into a no-argument `build_topology()` and return the
`NetworkSimulationGraph`. Then run:

```bash
python3 ub_tools.py topology build existing_topology.py \
  --output-dir path/to/case \
  --route-generation mixed
```

Use `existing_topology.py:function_name` for another builder. A script may still generate CSV when run
directly by calling the same builder from `main()` and keeping file writes there.

### CSV topology and cases

An existing CSV case can continue to run unchanged through `ub-quick-example`. Its directory can also
be passed to `topology build` to prepare a structured case. Assigned ports, unused ports, parallel links,
`network_attribute.txt`, and `traffic.csv` remain reusable.

### Traffic and traces

An existing `traffic.csv` can be copied into the prepared case without conversion. Use
`ub_tools.py traffic` when regenerating a collective workload. Existing run logs can be parsed directly
with `ub_tools.py trace parse` without rerunning the simulation.

## Compatibility

To generate a traditional CSV topology and full routing artifacts, use:

```bash
python3 ub_tools.py topology build topology.py \
  --output-dir path/to/case \
  --case-format csv
```

CSV output uses full route generation. Add `network_attribute.txt` and `traffic.csv` before running it.
`--export-csv` is a diagnostic snapshot of a structured case and is not runner input.

The individual topology, traffic, and trace scripts remain executable. Use them when their specific
arguments, timestamped directory layout, or output format is required; otherwise use `ub_tools.py`.

## Related documentation

- [Traffic generation](traffic_maker/README_en.md)
- [中文说明](README.md)
- [Parent scratch guide](https://gitcode.com/open-usim/ns-3-ub/blob/next/scratch/README_en.md)
