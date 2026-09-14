# Spec to Toolchain Mapping

<reference-hint>
<use-when>Use this reference when translating a stable `experiment-spec.md` into concrete tool invocations during the run stage.</use-when>
<focus>How spec fields map into topology scripts, workload generation, and simulator execution.</focus>
<keywords>spec, toolchain, net_sim_builder.py, build_traffic.py, run stage</keywords>
</reference-hint>

## Contents

- Core Principle
- Topology Slot -> Topology Generation
- Workload Slot -> Traffic Generation
- Network Overrides Slot -> network_attribute.txt
- Routing Intent Slot -> Route Generation
- Transport Channel Mode Slot -> Case Expectations
- Observability Slot -> Observability Overrides
- Execution
- Spec Parameter Name Convention

## Core Principle

**Always generate a new Python script** in the case directory that calls `net_sim_builder.py` library. Do NOT copy or modify existing example scripts (`user_topo_*.py`). These are code templates for reference only, not reusable tools.

## Node Delay Fields

- `node.csv` `forwardDelay` maps to `ns3::UbSwitch::InPortProcessingDelay`.
- `node.csv` `allocationDelay` maps to `ns3::UbSwitchAllocator::AllocationTime`.
- `forwardDelay` is fixed non-blocking in-port processing latency after route/output are known and before a forwarded packet becomes visible in VOQ to the allocator.
- `allocationDelay` is allocator arbitration latency. It is the only CSV field that overrides `AllocationTime`.
- Do not use `forwardDelay` to model allocator scheduling delay.

## Topology Slot → Topology Generation

### Process

1. **Read topology family and parameters** from `experiment-spec.md`
2. **Select the matching code template** from `topology-options.md` Generation Patterns section
3. **Generate a new script** `{case_dir}/generate_topology.py`:
   - Copy the complete pattern from `topology-options.md`
   - Substitute spec parameters (host_num, leaf_sw_num, etc.)
   - Set `graph.output_dir = str(Path(case_dir)) + "/"` so CSVs land directly in the case directory
   - Adjust bandwidth/delay if specified in Network Overrides
   - Adjust priority_list if specified in Network Overrides
4. **Run the script:** `python3 {case_dir}/generate_topology.py`
5. **Outputs:** `node.csv`, `topology.csv`, `routing_table.csv`, and `transport_channel.csv` only when `transport_channel_mode = precomputed`

### Important Rules

- **Do NOT** copy or modify `scratch/ns-3-ub-tools/user_topo_*.py` scripts
- **Do NOT** run `net_sim_builder.py` directly (it's a library, not a CLI)
- **Do NOT** reuse scripts across cases (each case gets its own generated script)
- **Always** use the complete pattern from `topology-options.md`, not abbreviated code
- **Always** write generated CSVs into `{case_dir}/`, not an extra timestamped output directory

### Example

**From spec:**
```yaml
Topology:
  family: clos-spine-leaf
  host_num: 64
  leaf_sw_num: 8

Network Overrides:
  bandwidth: 200Gbps
  delay: 10ns
```

**Generate `{case_dir}/generate_topology.py`:**
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
    host_num = 64
    leaf_sw_num = 8
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

    # Connect hosts to leaves (bandwidth/delay from Network Overrides)
    host_per_leaf = host_num // leaf_sw_num
    for host_id in range(host_num):
        leaf_id = host_num + (host_id // host_per_leaf)
        graph.add_netisim_edge(host_id, leaf_id, bandwidth='200Gbps', delay='10ns')

    # Connect leaves to spines (full mesh)
    for leaf_idx in range(leaf_sw_num):
        for spine_idx in range(spine_sw_num):
            leaf_id = host_num + leaf_idx
            spine_id = host_num + leaf_sw_num + spine_idx
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth='200Gbps', delay='10ns')

    # Generate config files
    graph.build_graph_config()
    graph.gen_compressed_route_table(path_finding_algo=all_shortest_paths, multiple_workers=4)
    graph.write_config(include_transport=False)
```

**Then run:**
```bash
cd {case_dir}
python3 generate_topology.py
```

### Heterogeneous link bandwidth

When the spec specifies different bandwidths per link layer, modify the `add_netisim_edge` calls accordingly:

```python
    # Host-leaf links: lower bandwidth
    for host_id in range(host_num):
        leaf_id = host_num + (host_id // host_per_leaf)
        graph.add_netisim_edge(host_id, leaf_id, bandwidth='200Gbps', delay='10ns')

    # Leaf-spine links: higher bandwidth
    for leaf_idx in range(leaf_sw_num):
        for spine_idx in range(spine_sw_num):
            leaf_id = host_num + leaf_idx
            spine_id = host_num + leaf_sw_num + spine_idx
            graph.add_netisim_edge(leaf_id, spine_id, bandwidth='400Gbps', delay='20ns')
```

## Workload Slot → Traffic Generation

### Built-in collective algorithms

If the spec workload is one of `ar_ring`, `ar_nhr`, `ar_rhd`, `a2a_pairwise`, `a2a_scatter`, map spec fields to CLI arguments:

```
python3 scratch/ns-3-ub-tools/traffic_maker/build_traffic.py \
  -n {host_num} \
  -c {comm_domain_size} \
  -b {data_size} \
  -a {algo} \
  --scatter-k {scatter_k} \
  -d {phase_delay} \
  --rank-mapping {rank_mapping} \
  -o {case_dir}
```

Copy the generated `traffic.csv` from the output subdirectory into the case directory.

### Reference traffic.csv

If the workload is a reference `traffic.csv`, copy it into the case directory.

### Custom traffic skeleton

If the spec workload is `custom-traffic-skeleton`, generate `{case_dir}/traffic.csv` directly by following the decomposition steps in `workload-options.md`. The generated CSV must conform to the `traffic.csv Schema` section in that document.

Use the Write tool to create the CSV. Verify:
- Header matches exactly: `taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases`
- `taskId` is 0-indexed and sequential
- `sourceNode` and `destNode` are valid host IDs for the topology (0 to `host_num - 1`)
- `opType` is a valid value from `TaOpcodeMap` (see `workload-options.md` "Valid `opType` values" or grep `TaOpcodeMap` in `src/unified-bus/model/ub-app.h`)
- `dependOnPhases` is empty string for first-phase tasks, space-separated phaseIds otherwise

### Multi-collective composition

If the spec combines multiple collectives:
1. Generate each collective's traffic separately (via `build_traffic.py` or hand-written).
2. Merge into a single `traffic.csv` following the composition rules in `workload-options.md`.
3. Write the merged result to `{case_dir}/traffic.csv`.

## Network Overrides Slot → network_attribute.txt

Use the `write_network_attributes()` function from
`.codex/skills/openusim-run-experiment/scripts/openusim_run_experiment/network_attribute_writer.py`:

```python
from openusim_run_experiment.network_attribute_writer import write_network_attributes

write_network_attributes(
    case_dir=Path("<case_dir>"),
    explicit_overrides={"ns3::UbApp::SomeParam": "value", ...},
    observability_overrides={"ns3::UbApp::TraceParam": "value", ...},
)
```

This writes a full catalog snapshot. Explicit overrides take precedence over observability overrides.
Unknown override keys must fail before execution. Do not write keys that are not in the runtime catalog or the documented UB-global fallback surface.

Runtime-catalog boundary:
- Treat the runtime catalog as the current build + current `scratch/ub-quick-example` introspection surface.
- Treat it as authoritative for `ns3::...` attribute keys.
- For UB globals, prefer `--PrintUbGlobals`; if that surface is unavailable in the current build, fall back only to the documented minimum UB-global set used by the project tooling.

### Resolving natural-language parameter intent

When the user describes a parameter change as a behavioral intent (e.g. "开启流控", "启用可靠传输", "buffer 大一点") rather than a concrete `key = value`, resolve it through the runtime catalog:

1. **Identify the target module.** The user's intent usually implies a component: "流控" → UbSwitch, "可靠传输" → UbTransportChannel, "路由" → UbRoutingProcess, "拥塞控制" → UB_CC globals.

2. **Search catalog descriptions.** Read `.openusim/project/parameter-catalog.json` (or call `load_or_build_parameter_catalog()`). For each entry whose `module` matches the target component, check whether its `description` field matches the user's intent. Present the matching entries to the user:
   - parameter key
   - description
   - current default value
   - value type (Boolean, Enum, Integer, ...)

3. **If the parameter is an enum, present all valid values.** The catalog `description` typically lists them (e.g. "NONE, CBFC, CBFC_SHARED, or PFC"). If the description does not list them, grep the C++ source for `MakeEnumChecker` on that attribute (see "Discovering valid values" below).

4. **If the intent is a relative adjustment** ("大一点", "翻倍"), show the current default and ask the user for a concrete target value.

5. **If no catalog entry matches the intent**, the parameter may not exist in the current build. Tell the user and ask them to be more specific or check whether the feature is supported.

6. **Confirm the resolved `key = value` with the user** before writing to the spec.

### Parameter value validation boundary

The skill-layer toolchain validates **parameter keys** (whether a key exists in the catalog) but does **not** validate **parameter values** (whether the value is within a valid range or matches an expected enum). Invalid values will be written to `network_attribute.txt` and only caught by ns-3 at runtime.

#### Discovering valid values

Current examples from this repo's source of truth:

- `ns3::UbSwitch::FlowControl`: `NONE`, `CBFC`, `CBFC_SHARED`, `PFC_FIXED`, `PFC_DYNAMIC`, `PFC_DYNAMIC_PAPER`
- `ns3::UbQueueManager::DynamicPfcResumeGapBytes`: dynamic PFC xon/xoff gap in bytes
- `ns3::UbQueueManager::PaperDynamicPfcBeta`: paper-style dynamic PFC beta used by `PFC_DYNAMIC_PAPER`, the DCQCN paper reproduction mode for **"Congestion Control for Large-Scale RDMA Deployments"** (SIGCOMM 2015)

#### Legacy network attribute keys

Do not write these older keys into `network_attribute.txt`; current `UbUtils::SetComponentsAttribute()` rejects them before `ConfigStore` loads the file.

| Legacy key | Current key/value |
|------------|-------------------|
| `ns3::UbQueueManager::ResumeOffset` | `ns3::UbQueueManager::DynamicPfcResumeGapBytes` |
| `ns3::UbSwitch::EnableCBFC` | `ns3::UbSwitch::FlowControl "CBFC"` |
| `ns3::UbSwitch::EnablePFC` | `ns3::UbSwitch::FlowControl "PFC_FIXED"` or `"PFC_DYNAMIC"` |
| `ns3::UbApiThread::*` | `ns3::UbLdstThread::*` |

When the discussion turns from "which knob exists" to "what these congestion/PFC knobs mean in queue dynamics", consult:

- `congestion-control-and-pfc-lessons.md`

The runtime parameter catalog (`.openusim/project/parameter-catalog.json`) stores a `description` field for each entry. For enum-type attributes, the description typically lists valid values (e.g. "The flow control mechanism to use (NONE, CBFC, CBFC_SHARED, or PFC)."). Consult the catalog description before accepting a user-provided value.

For values not discoverable from the catalog, check the C++ source:

| What to verify | Source of truth in C++ | How to find |
|----------------|----------------------|-------------|
| Enum attribute values (FlowControl, RoutingAlgorithm, VlScheduler, ...) | `MakeEnumChecker(...)` calls in the corresponding `GetTypeId()` | Grep for `MakeEnumChecker` in `src/unified-bus/model/` |
| GlobalValue enum values (UB_CC_ALGO, ...) | `MakeEnumChecker(...)` in `GlobalValue(...)` definitions | Grep for `GlobalValue.*UB_` in `src/unified-bus/` |
| Boolean attributes | `MakeBooleanChecker()` in `GetTypeId()` | Values are always `true` / `false` |
| Integer attributes with range | `MakeUintegerChecker<T>()` or `MakeIntegerChecker<T>(min, max)` | Grep for the attribute name in `src/unified-bus/` |
| traffic.csv `opType` values | `TaOpcodeMap` in `src/unified-bus/model/ub-app.h` | Grep for `TaOpcodeMap` |

#### Risk mitigation during planning

If the user requests a value for an enum parameter, verify it against the C++ source or catalog description before writing to `network_attribute.txt`. If the user provides a suspicious value (negative number, missing unit, string for a numeric field), warn them that the skill layer has no runtime value check and the error will only surface when ns-3 starts.

## Routing Intent Slot → Route Generation

- `routing_algorithm` maps to `default ns3::UbRoutingProcess::RoutingAlgorithm "..."`
- `use_shortest_paths` maps to the corresponding shortest-path toggles in the case attributes
- `path_source = auto-path-finder` means the topology script should call `graph.gen_compressed_route_table(...)` by default for new cases.
- `graph.gen_route_table(...)` is only for cases that explicitly need exact per-destination rows plus precomputed transport channels.
- `graph.gen_2layer_clos_compressed_route_table(host_num, leaf_sw_num)` is an explicit two-layer Clos optimization, not the default new-case path.
- `path_source = manual-route-table` means the topology script should use `graph.set_route_table(...)` for bounded route entries or directly write `routing_table.csv`

`routing_table.csv` supports compressed ranges in `nodeId` and `dstNodeId` using `a..b`. Runtime exact routes win over range routes. Range routes match port-scoped destination IPs by converting the packet destination IP back to node ID, while still using `dstPortId` as part of the route key. The generic compressed generator compresses each `dstPortId` independently and rejects conflicting range overlaps instead of silently choosing one route.

When the user asks for a non-template topology but provides bounded node/link facts, use the `custom-graph` pattern from `topology-options.md` instead of rejecting the request.

## Transport Channel Mode Slot → Case Expectations

- Default to `on-demand`; do not ask the user to precompute TP mappings unless they explicitly want fixed TP ids / priorities / endpoint pairs.
- `precomputed`: topology generation should call `config_transport_channel(...)` and `graph.write_config(include_transport=True)`; the case checker should require `transport_channel.csv`
- `on-demand`: `transport_channel.csv` may be omitted; topology generation should call `graph.write_config(include_transport=False)` so large cases do not precompute host-pair TP mappings. Compressed route generators require this mode.
- `TransportMode=CTP`: `traffic.csv` may include optional `srcEntityId,dstEntityId`; missing values default to `0`.
- CTP multipath is entity based and should not be represented by multiple TP rows.

## Observability Slot → Observability Overrides

```python
from openusim_run_experiment.network_attribute_writer import observability_preset

# Use a named tier
overrides = observability_preset("balanced")

# Or customize: start from a tier, then tweak
overrides = observability_preset("minimal")
overrides["UB_PORT_TRACE_ENABLE"] = "true"  # add port trace to minimal

# Pass to write_network_attributes
write_network_attributes(
    case_dir=Path("<case_dir>"),
    explicit_overrides={...},
    observability_overrides=overrides,
)
```

Refer to `trace-observability.md` for tier semantics and safe wording constraints.

## Execution

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path={case_dir}'
```

Optional runtime switches (not part of experiment definition):
- `--mtp-threads=N`

## Spec Parameter Name Convention

Use toolchain-native parameter names in `experiment-spec.md`:
- `host_num` (not `host_count` or `num_hosts`)
- `leaf_sw_num` (not `leaf_count`)
- `comm_domain_size` (not `comm_size`)
- `data_size` with unit (e.g. `1GB`)
- `routing_algorithm`, `use_shortest_paths`, `path_source`, `transport_channel_mode`

This avoids translation ambiguity at the plan→run boundary.
