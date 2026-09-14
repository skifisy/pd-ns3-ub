# Topology Options

<reference-hint>
<use-when>Use this reference when the user needs a bounded topology choice for one planned OpenUSim experiment.</use-when>
<focus>Planning-time topology options and generation-pattern selection.</focus>
<keywords>topology, ring, clos, fat-tree, custom-graph, planning</keywords>
</reference-hint>

## Contents

- Core Principle
- Recommended choice format
- Supported Topology Families
- Natural-language and image topology decomposition
- Scale guidance
- Parameter constraint validation
- Mapping rule
- `net_sim_builder.py` Library Interface
- Generation Patterns

## Core Principle

**Always generate a new Python script** that calls `net_sim_builder.py` library. Do NOT copy or modify existing example scripts. Example scripts (`user_topo_*.py`) are code templates for reference only, not reusable tools.

## Recommended choice format

- `1:` recommended topology A
- `2:` recommended topology B
- `3:` recommended topology C
- `4:` user free input

Only offer the three most relevant template choices.

## Supported Topology Families

### `ring`

**When to use:**
- Simple contention and directional throughput questions
- Usually the easiest starting point for a small first experiment

**Planning parameters:**
- `host_num`: total number of hosts (integer)

**Code template reference:** See generation pattern below

### `full-mesh`

**When to use:**
- Direct pair connectivity without intermediate switches
- Small-scale baseline comparisons

**Planning parameters:**
- `host_num`: total number of hosts (integer)

**Code template reference:** See generation pattern below

### `nd-full-mesh`

**When to use:**
- User thinks in dimensions and wants structured direct connectivity
- 2D/3D mesh topologies

**Planning parameters:**
- `row_num`: number of rows (integer)
- `col_num`: number of columns (integer)
- Derived: `host_num = row_num * col_num`

**Code template reference:** `scratch/ns-3-ub-tools/user_topo_4x4_2DFM.py` (for pattern only, do not copy)

### `clos-spine-leaf`

**When to use:**
- Common leaf-spine fabrics
- Pod-level or host-to-host comparisons
- If user says `2-layer Clos`, `leaf-spine`, or `spine-leaf`, prefer this family by default

**Planning parameters:**
- `host_num`: total number of hosts (integer)
- `leaf_sw_num`: number of leaf switches (integer)
- Derived: `spine_sw_num = host_num // leaf_sw_num`
- Constraint: `host_num % leaf_sw_num == 0` — **validate during planning; reject immediately if violated**

**Code template reference:** `scratch/ns-3-ub-tools/user_topo_2layer_clos.py` (for pattern only, do not copy)

### `clos-fat-tree`

**When to use:**
- User explicitly wants canonical Clos family with single `k` parameter
- Prefer this only when user explicitly asks for `fat-tree` or gives a `k`-style input

**Planning parameters:**
- `k`: fat-tree parameter — **must be even integer; validate during planning**
- Derived: `pod_num = k`, `hosts_per_pod = (k/2)^2`, `host_num = k * (k/2)^2`
- Derived: `leaf_sw_num = k * (k/2)`, `spine_sw_num = (k/2)^2`

**Code template reference:** Use `clos-spine-leaf` generation pattern with derived parameters

### old case reference

**When to use:**
- User already has a previous case and wants the new experiment to start from it conceptually
- This is a bounded reference path, not an in-place edit mode

### `custom-graph`

**When to use:**
- The user gives a topology sketch, screenshot, or a structured text description that does not cleanly fit an existing family
- The user wants an innovative topology but can still provide bounded node/link facts

**Planning parameters:**
- `host_count` and switch groups
- bounded link inventory or link construction rule
- per-link `bandwidth` and `delay`
- optional `edge_count` for parallel links
- whether routing should be `auto-path-finder` or `manual-route-table`

**Planning note:**
- First restate the topology as node groups + links in text.
- If those facts are stable enough, the run stage may generate a new `generate_topology.py` and then use `topo_plot.py` for a visual confirmation pass.

### Decomposing any natural-language topology description

When the user describes a topology that does not fit a named family — whether in natural language, pseudocode, a diagram, or any mix — use the following general principle. Do not try to match against specific "path types"; instead, always apply the same decomposition steps.

**Core principle: every topology description, no matter how complex, is reducible to node groups + connectivity rules between groups. Decompose first, then ask about anything unclear.**

#### Step 1 — Identify node groups

Parse the user's description to find all distinct types of nodes mentioned. Each type becomes a group.

> Example: "16 个计算节点分 4 组，组内 ring，组间通过汇聚交换机互联"
> → two groups: **计算节点 (host)**, **汇聚交换机 (switch)**

For each group, determine:
- **Count**: how many nodes? (If not stated, ask.)
- **Role**: host (`add_netisim_host`) or switch (`add_netisim_node`)? If ambiguous, ask.

#### Step 2 — Identify connectivity rules

For each pair of groups (including within the same group), determine whether and how they connect.

Connectivity rules come in a few forms — recognize whichever the user is using:

| Form | Example | Translation |
|------|---------|-------------|
| Named pattern | "fullmesh" / "ring" / "每个连每个" | Well-defined; enumerate links from the pattern |
| Per-node link count | "每个节点出 3 条链路连交换机" | `link_count_per_node` links from each node in group A to nodes in group B; ask how they distribute (e.g. 3 links to 1 switch, or 1 link each to 3 switches) if ambiguous |
| Full bipartite | "每个 leaf 连所有 spine" | Every node in group A connects to every node in group B |
| Algorithmic | "节点 i 连 i XOR 2^k" | Express as Python loop in `generate_topology.py` |
| Unspecified | User didn't say how leaves connect to spines | **Ask.** |

**If any connectivity rule is ambiguous or missing, ask the user before assuming.**

#### Step 3 — Apply bandwidth constraints to derive missing sizing

The user may specify bandwidth constraints (like convergence ratio) instead of concrete node counts. Use these constraints to derive the missing parameters.

**Convergence ratio** (收敛比): the ratio of total downlink bandwidth to total uplink bandwidth at a tier boundary.

- "无收敛" / "non-oversubscribed" / "1:1" → downlink bandwidth = uplink bandwidth at every tier boundary
- "2:1 收敛" → downlink bandwidth is 2× the uplink bandwidth
- "3:1" → downlink is 3× uplink

**Derivation method**: at each tier boundary, compute:

```
total_downlink_bw = (nodes_below) × (links_per_node_upward) × (per_link_bandwidth)
total_uplink_bw   = (nodes_above) × (links_per_node_downward) × (per_link_bandwidth)

convergence_ratio = total_downlink_bw / total_uplink_bw
```

If the user specifies convergence ratio + some sizing, solve for the missing variable. For example:

> "每个 leaf 下挂 8 个 host，每条链路 400Gbps，2:1 收敛" →
> total downlink per leaf = `8 × 400Gbps = 3200Gbps`
> 2:1 → total uplink per leaf = `3200 / 2 = 1600Gbps`
> → each leaf 需要 `1600 / 400 = 4` 条上联到 spine

If the constraint is not sufficient to uniquely determine all parameters, ask the user for the remaining ones.

#### Step 4 — Restate and confirm

Present the decomposed result to the user as a concrete node/link plan:
- List each node group with its count and role
- List each connectivity rule with link count, bandwidth, and delay
- State any derived parameters and the constraint used

**Wait for user confirmation before generating.**

#### Step 5 — Generate

Once confirmed, produce `generate_topology.py` from the `custom-graph` code pattern, expressing each connectivity rule as Python code.

- Algorithmic rules (e.g. "i XOR 2^k") translate directly to Python loops.
- Random topologies must use a fixed seed for reproducibility.
- Use `topo_plot.py` for visual confirmation if the topology is complex.

#### Worked example

User says: "32 个 host 分成 4 组，组内 fullmesh，每组出一条链路连到一个 agg 交换机，agg 之间 fullmesh，无收敛"

**Step 1 — Node groups** (partially specified):

| Group | Role | Count |
|-------|------|-------|
| host | host | 32（每组 8） |
| agg | switch | 4 |

**Step 2 — Connectivity rules**:

| From → To | Rule | Ambiguity? |
|-----------|------|------------|
| host ↔ host (组内) | fullmesh within each 8-node group | Clear |
| host → agg | "每组出一条链路" | **"每组"还是"每个 host"？1 条总共 or 8 条？** |
| agg ↔ agg | fullmesh | Clear |

**Step 3 — Bandwidth constraint**: "无收敛" → agg 下行总带宽 = agg 间互联总带宽。但 host-agg 的链路数还有歧义，无法推导。

**What to ask the user**: "每组出一条链路连 agg"是指整组共用 1 条链路，还是组内每个 host 各出 1 条？链路带宽是多少？

### Processing topology from screenshots or images

When the user provides a topology screenshot, diagram image, or hand-drawn sketch:

1. **Extract structure**: identify all nodes (hosts vs switches) and links from the image. State the extracted facts in text: "I see N hosts, M switches, and these links: ..."
2. **Confirm with user**: present the extracted node groups and link inventory for confirmation before proceeding. Images can be ambiguous — always verify.
3. **Assign node IDs**: hosts numbered from 0 consecutively, then switches.
4. **Determine link properties**: ask the user for bandwidth, delay, and edge_count if not visible in the image.
5. **Proceed as `custom-graph`**: once confirmed, generate `generate_topology.py` from the `custom-graph` pattern.

### Heterogeneous link bandwidth

Named family patterns use uniform bandwidth for all links. If the spec calls for different bandwidths per link layer (e.g. `host-leaf: 200Gbps`, `leaf-spine: 400Gbps`):

- Use the named family's generation pattern as a starting point.
- Replace the single `bandwidth` variable with per-layer variables.
- In `experiment-spec.md`, record the bandwidth map explicitly:

```yaml
Network Overrides:
  host_leaf_bandwidth: 200Gbps
  leaf_spine_bandwidth: 400Gbps
  host_leaf_delay: 10ns
  leaf_spine_delay: 20ns
```

The generated `generate_topology.py` should use different bandwidth/delay values in the respective `add_netisim_edge()` calls.

### Scale guidance

| Topology family | Recommended max hosts | Notes |
|-----------------|----------------------|-------|
| `ring` | ~1000 | Prefer generic compressed route generation; route search remains O(N²) |
| `full-mesh` | ~100 | Link count is O(N²); route table and transport_channel.csv grow quickly |
| `nd-full-mesh` | ~256 (e.g. 16×16) | Same concern as full-mesh |
| `clos-spine-leaf` | ~10000 | Default to generic compressed route generation; the specialized Clos generator is available as an explicit optimization |
| `clos-fat-tree` | ~10000 | Same as clos-spine-leaf with derived parameters |
| `custom-graph` | Depends on density | Sparse graphs should use generic compressed route generation; dense graphs still hit O(N²) routing |

For topologies beyond ~1000 hosts, warn the user that route search may take minutes to tens of minutes. For new cases, use `gen_compressed_route_table(...)` and on-demand TP creation by default; it avoids storing all paths and avoids dense route matrices, but it cannot remove the inherent host-pair path-search cost. For very large regular two-layer Clos, `gen_2layer_clos_compressed_route_table(...)` can be selected explicitly when smaller generation cost matters more than using the generic path.

### Parameter constraint validation

During planning, validate these constraints before proceeding to the run stage. If a constraint is violated, tell the user immediately and ask for corrected values.

| Family | Constraint | Consequence if violated |
|--------|-----------|------------------------|
| `clos-spine-leaf` | `host_num % leaf_sw_num == 0` | Asymmetric host distribution; some leaves get fewer hosts |
| `clos-fat-tree` | `k` is even | Derived counts become fractional |
| `nd-full-mesh` | `row_num > 0 and col_num > 0` | Empty graph |
| `ring` | `host_num >= 2` | Degenerate ring |
| `full-mesh` | `host_num >= 2` | No links to create |
| all families | `host_num` matches workload `host_count` | Mismatch causes invalid sourceNode/destNode in traffic |

## Mapping rule

- Do not invent a custom topology generator path when the user request can be mapped to a supported family above
- If the request cannot be bounded to a supported family, try to restate it as a bounded `custom-graph`
- Ask the user for more topology facts only when the graph is still under-specified after that


## `net_sim_builder.py` Library Interface

`net_sim_builder.py` is a Python library (not a CLI). It provides the `NetworkSimulationGraph` class.

### Core API

```python
import net_sim_builder as netsim
import networkx as nx

def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []

# Create graph
graph = netsim.NetworkSimulationGraph()

# Add hosts (must be numbered 0, 1, 2, ... consecutively)
graph.add_netisim_host(node_id, forward_delay='1ns')

# Add switches (must be added after all hosts)
graph.add_netisim_node(node_id, forward_delay='1ns')

# Add links
graph.add_netisim_edge(u, v, bandwidth='400Gbps', delay='20ns', edge_count=1)

# Generate config files
graph.build_graph_config()
graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
graph.write_config(include_transport=False)
```

### Common parameters

- `bandwidth`: link bandwidth with unit, e.g. `400Gbps` (valid: bps, Kbps, Mbps, Gbps, Tbps)
- `delay`: link delay with unit, e.g. `20ns` (valid: ns, us, ms, s)
- `forward_delay`: switch forward delay, e.g. `1ns`
- `priority_list`: TP priority list, e.g. `[7, 8]`
- `path_finding_algo`: routing algorithm function, default `nx.all_shortest_paths`
- `multiple_workers`: parallel workers for routing, e.g. `4`
- `gen_compressed_route_table(path_finding_algo=...)`: default new-case route generator; emits compressed `a..b` ranges where identical route rules occur and avoids storing all paths or a dense route matrix.
- `gen_2layer_clos_compressed_route_table(host_num, leaf_sw_num)`: explicit two-layer Clos optimization; emits compressed `a..b` destination ranges and avoids all host-pair path enumeration.
- `write_config(include_transport=False)`: write node/topology without `transport_channel.csv`; required with compressed routes and on-demand TP generation.

### Node numbering constraint

- Hosts must be numbered from 0 consecutively
- Hosts must be added before switches via `add_netisim_host()` then `add_netisim_node()`

### Path finding algorithm template

```python
def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []
```

## Generation Patterns

### Pattern: `clos-spine-leaf`

```python
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scratch/ns-3-ub-tools"))

import net_sim_builder as netsim
import networkx as nx

def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []

if __name__ == '__main__':
    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = str(Path(__file__).parent) + "/"

    # Parameters from experiment-spec.md
    host_num = 64  # substitute from spec
    leaf_sw_num = 8  # substitute from spec
    spine_sw_num = host_num // leaf_sw_num

    # Add hosts (0 to host_num-1)
    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')

    # Add leaf switches
    for leaf_idx in range(leaf_sw_num):
        graph.add_netisim_node(host_num + leaf_idx, forward_delay='1ns')

    # Add spine switches
    for spine_idx in range(spine_sw_num):
        graph.add_netisim_node(host_num + leaf_sw_num + spine_idx, forward_delay='1ns')

    # Connect hosts to leaves
    host_per_leaf = host_num // leaf_sw_num
    for host_id in range(host_num):
        leaf_id = host_num + (host_id // host_per_leaf)
        graph.add_netisim_edge(host_id, leaf_id, bandwidth='400Gbps', delay='20ns')

    # Connect leaves to spines (full mesh)
    for leaf_idx in range(leaf_sw_num):
        for spine_idx in range(spine_sw_num):
            leaf_id = host_num + leaf_idx
            spine_id = host_num + leaf_sw_num + spine_idx
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth='400Gbps', delay='20ns')

    # Generate config files
    graph.build_graph_config()
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    graph.write_config(include_transport=False)
```

If the user explicitly requests the two-layer Clos specialized route generator, replace the route line with `graph.gen_2layer_clos_compressed_route_table(host_num, leaf_sw_num)`.

### Pattern: `nd-full-mesh`

```python
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scratch/ns-3-ub-tools"))

import net_sim_builder as netsim
import networkx as nx

def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []

if __name__ == '__main__':
    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = str(Path(__file__).parent) + "/"

    # Parameters from experiment-spec.md
    row_num = 4  # substitute from spec
    col_num = 4  # substitute from spec
    host_num = row_num * col_num

    # Add hosts
    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')

    # Connect in 2D mesh
    for x in range(col_num):
        # Connect hosts in same row
        host_in_row = [x * row_num + y for y in range(row_num)]
        for i in range(row_num):
            for j in range(i + 1, row_num):
                graph.add_netisim_edge(host_in_row[i], host_in_row[j],
                                      bandwidth='400Gbps', delay='10ns')

        # Connect hosts in same column
        host_in_col = [y * col_num + x for y in range(row_num)]
        for i in range(col_num):
            for j in range(i + 1, col_num):
                graph.add_netisim_edge(host_in_col[i], host_in_col[j],
                                      bandwidth='400Gbps', delay='10ns')

    # Generate config files
    graph.build_graph_config()
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    graph.write_config(include_transport=False)
```

### Pattern: `ring`

```python
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scratch/ns-3-ub-tools"))

import net_sim_builder as netsim
import networkx as nx

def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []

if __name__ == '__main__':
    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = str(Path(__file__).parent) + "/"

    # Parameters from experiment-spec.md
    host_num = 8  # substitute from spec

    # Add hosts
    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')

    # Connect in ring
    for host_id in range(host_num):
        next_host = (host_id + 1) % host_num
        graph.add_netisim_edge(host_id, next_host, bandwidth='400Gbps', delay='20ns')

    # Generate config files
    graph.build_graph_config()
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    graph.write_config(include_transport=False)
```

### Pattern: `full-mesh`

```python
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scratch/ns-3-ub-tools"))

import net_sim_builder as netsim
import networkx as nx

def all_shortest_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []

if __name__ == '__main__':
    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = str(Path(__file__).parent) + "/"

    # Parameters from experiment-spec.md
    host_num = 8  # substitute from spec

    # Add hosts
    for host_id in range(host_num):
        graph.add_netisim_host(host_id, forward_delay='1ns')

    # Connect all pairs (full mesh)
    for i in range(host_num):
        for j in range(i + 1, host_num):
            graph.add_netisim_edge(i, j, bandwidth='400Gbps', delay='20ns')

    # Generate config files
    graph.build_graph_config()
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    graph.write_config(include_transport=False)
```

### Pattern: `clos-fat-tree`

Use `clos-spine-leaf` pattern with derived parameters:

```python
# Derive parameters from k
k = 4  # substitute from spec (must be even)
pod_num = k
hosts_per_pod = (k // 2) ** 2
host_num = pod_num * hosts_per_pod
leaf_sw_num = pod_num * (k // 2)
spine_sw_num = (k // 2) ** 2

# Then use clos-spine-leaf pattern above with these derived values
```

### Pattern: `custom-graph`

```python
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / "scratch/ns-3-ub-tools"))

import net_sim_builder as netsim
import networkx as nx


def bounded_paths(G, source, target):
    try:
        return nx.all_shortest_paths(G, source, target)
    except nx.NetworkXNoPath:
        return []


if __name__ == '__main__':
    graph = netsim.NetworkSimulationGraph()
    graph.output_dir = str(Path(__file__).parent) + "/"

    # Replace these groups with the user-confirmed custom graph facts.
    for host_id in range(8):
        graph.add_netisim_host(host_id, forward_delay='1ns')

    for switch_id in range(8, 11):
        graph.add_netisim_node(switch_id, forward_delay='1ns')

    custom_links = [
        (0, 8, '400Gbps', '20ns', 1),
        (1, 8, '400Gbps', '20ns', 1),
        (8, 9, '400Gbps', '20ns', 2),
        (9, 10, '400Gbps', '20ns', 1),
        (10, 2, '400Gbps', '20ns', 1),
    ]
    for u, v, bandwidth, delay, edge_count in custom_links:
        graph.add_netisim_edge(u, v, bandwidth=bandwidth, delay=delay, edge_count=edge_count)

    graph.build_graph_config()
    graph.gen_compressed_route_table(path_finding_algo=bounded_paths, multiple_workers=1)
    graph.write_config(include_transport=False)
```
