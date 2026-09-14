# USAGE — Running cases under `scratch/`

`scratch/` provides a set of scenario cases. Each case can be executed quickly by preparing the case directory and editing its configuration files (TXT/CSVs).

This document describes:
- The case directory layout and configuration file semantics (schema, constraints, and legal values).
- How `python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=...'` consumes these files to run a prepared ns-3 simulation and schedule traffic.
- In most cases you **do not** need to write these configuration files from scratch; you can use the Python tools in `scratch/ns-3-ub-tools/` to generate them (see https://gitcode.com/open-usim/ns-3-ub-tools). Of course, you can also author the TXT/CSV files manually following the schemas below.

---

## Build once before running cases

Prepare the UB-only simulation entry before running cases.

```bash
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-werror -d release -G Ninja
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

After this, run cases with `--no-build`:

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'
```

For Unison/MTP runs, configure the same UB-only build with MTP enabled:

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --enable-mtp --disable-werror -d release -G Ninja
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

If you are reusing a workspace that previously configured a full ns-3 build, examples, tests, MPI, or MTP, you can explicitly reset those old configure choices:

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja
```

If you need to reset old configure choices, use the explicit reset form above. If you do not need that reset, use the shorter UB-only configure command at the start of this section.

---

## Directory layout of a typical case

Each case directory under `scratch/` usually contains:

- `network_attribute.txt` — Global defaults and feature toggles set via ns-3 Attributes and project-level globals.
- `node.csv` — Node inventory: devices and switches with port counts and optional per-node delay fields.
- `topology.csv` — L2 links between ports, with bandwidth and propagation delay.
- `routing_table.csv` — Per-node forwarding rules for a given destination and destination-port.
- `transport_channel.csv` — Optional explicit Transport Path Numbers (TPNs) and priorities between endpoints and ports.
- `traffic.csv` — Application-level tasks (ops, size, priority, dependency, timing).
- `fault.csv` — Optional, only if faults are enabled (see `UB_FAULT_ENABLE`).

During a run, the entry program also emits:
- `runlog/` — Packet and task traces.
- `output/` (or `test/`) — Post-processed CSVs, e.g. `throughput.csv`, `task_statistics.csv`.

The entry program (`scratch/ub-quick-example`, or `src/unified-bus/examples/ub-quick-example` when examples are enabled) builds a scenario by reading these files in the following order:
1) `network_attribute.txt` → `UbUtils::SetComponentsAttribute`
2) `node.csv` → `UbUtils::CreateNode`
3) `topology.csv` → `UbUtils::CreateTopo`
4) `routing_table.csv` → `UbUtils::AddRoutingTable`
5) optional `transport_channel.csv` → `UbUtils::CreateTp`
6) `traffic.csv` → reserve missing RTP connection records on every process → schedule tasks

---

## How [ns-3-ub-tools](https://gitcode.com/open-usim/ns-3-ub-tools) generates configurations

The submodule contains helpers to synthesize config files:

- Topology builders and visualization:
  - `user_topo_*.py` — declarative topology definitions (e.g., `user_topo_4x4_2DFM.py`, `user_topo_2layer_clos.py`).
  - `net_sim_builder.py` — expands node ranges, renders `node.csv`, `topology.csv`, `routing_table.csv`, and `transport_channel.csv` according to a chosen topology.
  - `topo_plot.py` — draw `network_topology.png` for quick visual checks.
- Traffic makers  [README.md](./ns-3-ub-tools/README.md):
  - `traffic_maker/*.py` — generate `traffic.csv` for workloads (e.g., all-to-all, RDMA write/read patterns, collective-like flows).
- Trace analysis:
  - `trace_analysis/parse_trace.py` — orchestrates post-processing, runs:
    - `task_statistics.py` — merges Task/Packet traces back into `traffic.csv`.
    - `cal_throughput.py` — parses `PortTrace_node_*` to produce `throughput.csv`.

You can study how these scripts interpret/write each column to better understand legal values and intended semantics.

---

## Conventions and units

- Time strings follow ns-3 `Time` parsing (both `20ns` and `+20ns` are accepted), e.g. `20ns`, `5us`, `3ms`, `1s`.
- Data rates use ns-3 `DataRate` parsing, e.g. `400Gbps`, `1200Gbps`, `10Gbps`, `100Mbps`.
- Booleans are `true`/`false` (case-insensitive; tools normalize them).
- Integer lists are space-separated (e.g., `"1 2 3"`).
- Ranges use `a..b` inclusive (e.g., `0..3`) and are expanded by the tools.

---

## `network_attribute.txt`

Lines in two forms:

- Set ns-3 Attributes (defaults):
  - `default ns3::ClassName::AttributeName "value"`
  - Example: `default ns3::UbPort::UbDataRate "400Gbps"`
- Set project-level globals:
  - `global NAME "value"`
  - Example: `global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"`

Common UB attributes you’ll see (all names below come from `GetTypeId().AddAttribute(...)` in the code):

- Link/Port timing and rate:
  - `ns3::UbLink::Delay` (Time)
  - `ns3::UbPort::UbDataRate` (DataRate)
  - `ns3::UbPort::UbInterframeGap` (Time)
- Credit-based/PFC knobs:
  - `ns3::UbSwitch::FlowControl` (`NONE`, `CBFC`, `CBFC_SHARED`, `PFC_FIXED`, `PFC_DYNAMIC`, `PFC_DYNAMIC_PAPER`)
  - `ns3::UbPort::CbfcFlitLenByte`, `CbfcFlitsPerCell`, `CbfcInitCreditCell`, `CbfcRetCellGrainDataPacket`, `CbfcRetCellGrainControlPacket`
  - `ns3::UbPort::PfcUpThld`, `PfcLowThld`
- Congestion control (CAQM) and buffers:
  - `ns3::UbCaqm::*`, `ns3::UbHostCaqm::*`, `ns3::UbSwitchCaqm::*`
  - `ns3::UbQueueManager::ReservePerQueueBytes`
  - `ns3::UbQueueManager::SharedPoolBytes`
  - `ns3::UbQueueManager::HeadroomPerPortBytes`
  - `ns3::UbQueueManager::AlphaShift`
  - `ns3::UbQueueManager::DynamicPfcResumeGapBytes`
  - `ns3::UbQueueManager::PaperDynamicPfcBeta`
- Transport behavior (`ns3::UbTransportChannel`):
  - `UsePacketSpray` (bool)
  - `UseShortestPaths` (bool)
  - `EnableRetrans`, `BaseRTO`, `MaxRetransAttempts`, `RetransExponentFactor`, `DefaultMaxWqeSegNum`, `DefaultMaxInflightPacketSize`
  - `RetransmissionMode`, `TpOooThreshold`, `SelectiveAckBitmapBits`, `EnableFastRetrans`, `EnableSelectiveMarkPsn`
- Allocator:
  - `ns3::UbSwitchAllocator::AllocationTime` (Time)
- App & API LD/ST knobs:
  - `ns3::UbApp::EnableMultiPath` (bool)
  - `ns3::UbApiLdst::*` (ThreadNum, LoadResponseSize, StoreRequestSize, QueuePriority)
  - `ns3::UbApiLdstThread::*` (StoreOutstanding, LoadOutstanding, LoadRequestSize, QueuePriority, UsePacketSpray, UseShortestPaths)

### Upgrading copied cases

If you copied an older `scratch/` case into your own workspace, check `network_attribute.txt` before running it against a newer build. Recognized transport aliases are translated to current names. Unsupported legacy keys are rejected before ns-3 `ConfigStore` loads the file so the error points at the migration instead of a generic attribute failure.

| Old key or behavior | Current form |
|---------------------|--------------|
| `default ns3::UbQueueManager::ResumeOffset "..."` | `default ns3::UbQueueManager::DynamicPfcResumeGapBytes "..."` |
| `default ns3::UbSwitch::EnableCBFC "true"` | `default ns3::UbSwitch::FlowControl "CBFC"` |
| `default ns3::UbSwitch::EnablePFC "true"` | Choose `default ns3::UbSwitch::FlowControl "PFC_FIXED"` or `"PFC_DYNAMIC"` |
| `default ns3::UbApiThread::*` | `default ns3::UbLdstThread::*` |
| `default ns3::UbTransportChannel::InitialRTO "..."` | `default ns3::UbTransportChannel::BaseRTO "..."` |
| `default ns3::UbTransportChannel::EnableFastSelectiveRetrans "..."` | `default ns3::UbTransportChannel::EnableFastRetrans "..."` |
| Depend on old `CbfcRetCellGrainControlPacket` default | Set `default ns3::UbPort::CbfcRetCellGrainControlPacket "1"` explicitly to reproduce older behavior; current repo default is `32` |
| Need `QueueTrace_*` files | Set `global UB_QUEUE_TRACE_ENABLE "true"` |
| Need `PfcTrace_*` or `CbfcTrace_*` files | Set `global UB_FLOW_CONTROL_TRACE_ENABLE "true"` |
| Need `Dcqcn*` or `Caqm*` algorithm traces | Set `global UB_CONGESTION_CONTROL_TRACE_ENABLE "true"` |

Runs with `default ns3::UbTransportChannel::EnableRetrans "false"` now stop early when a packet is dropped. Fix the route/buffer/flow-control cause, or enable retransmission if the experiment intentionally allows loss recovery.

The existing default retransmission algorithm remains GoBackN when
`RetransmissionMode` is omitted. To opt into UB RTP selective retransmission in a
case file, keep the configuration at the protocol level:

```text
default ns3::UbTransportChannel::EnableRetrans "true"
default ns3::UbTransportChannel::RetransmissionMode "SELECTIVE"
```

`SelectiveAckBitmapBits` defaults to `0`, which means AUTO. AUTO chooses the
smallest SAETPH-supported feedback width covering `min(TpOooThreshold, 1024)`.
`TpOooThreshold` is the endpoint out-of-order receive evidence capacity in PSNs;
it is not a private receive-buffer implementation name. `EnableFastRetrans`
defaults to `false` and should generally stay disabled when packet-spray or other
multipath routing may reorder packets. For a controlled non-reordering experiment,
advanced settings may be written explicitly:

```text
default ns3::UbTransportChannel::SelectiveAckBitmapBits "64"
default ns3::UbTransportChannel::EnableFastRetrans "true"
default ns3::UbTransportChannel::EnableSelectiveMarkPsn "true"
```

`SelectiveRetransmitNotify` is an ns-3 `TraceSource` on `UbTransportChannel`
for manual `TraceConnect` observers. It reports node id, TPN, PSN, and payload
bytes when a queued selective retransmission packet is emitted; it is not written
to the default runlog files by the existing global trace switches.

### `PFC_DYNAMIC_PAPER`

`PFC_DYNAMIC_PAPER` is a paper-reproduction mode for the DCQCN paper **"Congestion Control for Large-Scale RDMA Deployments"** (SIGCOMM 2015). It is separate from the repo's default `PFC_DYNAMIC` mode.

The current implementation uses:

- `xoff = PaperDynamicPfcBeta * max(SharedPoolBytes - totalBufferedBytes, 0) / priorities`
- `xon = max(xoff - 2 * UB_MTU_BYTE, 0)`
- `totalBufferedBytes = VOQ backlog + egress queue backlog` across the switch

Use `PFC_DYNAMIC_PAPER` when the experiment is explicitly trying to reproduce or compare against that paper-style dynamic PFC threshold. Use `PFC_DYNAMIC` for the repo's shared-pool dynamic PFC mode.

Project-level `global` keys (defined as `GlobalValue` in code and read by UB):

- `UB_FAULT_ENABLE` (bool) — If `true`, `fault.csv` must exist.
- `UB_PRIORITY_NUM`/`UB_VL_NUM` (int) — QoS/virtual lanes sizing.
- `UB_CC_ALGO` (string) — e.g., `CAQM`.
- `UB_CC_ENABLED` (bool) — enable/disable CC.
- Trace toggles: `UB_TRACE_ENABLE`, `UB_TASK_TRACE_ENABLE`, `UB_PACKET_TRACE_ENABLE`, `UB_PORT_TRACE_ENABLE`, `UB_PARSE_TRACE_ENABLE`, `UB_RECORD_PKT_TRACE` (bool).
- `UB_PYTHON_SCRIPT_PATH` — Path to the Python post-processing entry (`parse_trace.py`).

Legal values and discovery:
- Names and types are defined in each class’s `GetTypeId().AddAttribute(...)`.
- To discover legal attributes quickly:
  - Search: `grep -R "GetTypeId\(|AddAttribute\(" src/unified-bus/model`
  - Check the Attribute value type (Time/DataRate/Boolean/Uinteger) to format literals correctly.
- If parsing fails, ns-3 prints an error; fix the literal (e.g., `20ns`, `400Gbps`, `true`).

---

## `node.csv`

Schema:
```
nodeId,nodeType,portNum[,forwardDelay[,allocationDelay[,systemId]]]
```
- `nodeId` — integer or range `a..b`, inclusive.
- `nodeType` — `DEVICE` (end host) or `SWITCH`.
- `portNum` — number of ports on the node.
- `forwardDelay` — optional per-node in-port processing delay (Time). It maps to `InPortProcessingDelay`.
- `allocationDelay` — optional per-node allocator arbitration delay (Time). It maps to `AllocationTime`.
- `systemId` — optional ns-3 MPI system ID. If absent, it defaults to `0`.

Examples:
```
0..1,DEVICE,1,,1ns
2..3,SWITCH,4,10ns,1ns
```

Notes on `forwardDelay`:
- **Meaning**: `forwardDelay` sets fixed non-blocking in-port processing latency after route/output are known and before the packet becomes visible in VOQ to the allocator.
- **Code mapping**: when present, `UbUtils::CreateNode()` applies this value as `UbSwitch::InPortProcessingDelay`.
- **Mechanism**: this delay is non-serial. Bytes are visible to existing ingress/outPort/switch-total buffer queries during processing, but the allocator cannot schedule the packet until processing completes.
- **Scope**: Applies to both `SWITCH` nodes and `DEVICE` nodes.
- **Format**: use ns-3 Time literals (e.g. `10ns`, `1us`, `1ms`).
- **Example**: `0,SWITCH,4,10ns,` sets `InPortProcessingDelay` to `10ns` for node `0`.
- **Allocator delay**: use `allocationDelay`, e.g. `0,SWITCH,4,,10ns` sets allocator `AllocationTime` to `10ns`.
- **Inspecting at runtime**: run your case with `--ClassName=ns3::UbSwitch --AttributeName=InPortProcessingDelay` or `--ClassName=ns3::UbSwitchAllocator --AttributeName=AllocationTime`.

---

## `topology.csv`

Schema:
```
nodeId1,portId1,nodeId2,portId2,bandwidth,delay
```
- `nodeId1/2` — node indices that are connected.
- `portId1/2` — local port indices at each endpoint.
- `bandwidth` — DataRate string, e.g. `400Gbps`, `1200Gbps`.
- `delay` — propagation Time, e.g. `20ns`.

Example:
```
0,0,2,0,400Gbps,20ns
1,0,3,0,1200Gbps,20ns
```

UB interprets these links via `UbLink` and attaches corresponding `UbPort`s with the specified rate and delay.

---

## `routing_table.csv`

Schema:
```
nodeId,dstNodeId,dstPortId,outPorts,metrics
```
- `nodeId` — the router (switch or device) where this rule applies.
- `dstNodeId` — destination end-host node.
- `dstPortId` — destination port at the end host (usually `0` unless multi-port hosts).
- `outPorts` — space-separated list of egress port IDs to use from `nodeId`.
- `metrics` — space-separated list of integer metrics (same length as `outPorts`), lower is better.

Examples:
```
0,1,0,0,4
2,1,0,1 2 3,3 3 3
```

Compressed range rows:
- `nodeId` and `dstNodeId` may also use an inclusive `a..b` range, for example `10..25`.
- A range row is semantically equivalent to the expanded per-`nodeId`, per-`dstNodeId` rows with the same `dstPortId`, `outPorts`, and `metrics`.
- Overlapping range rows are accepted only when they describe the same outport/metric set. Conflicting overlaps fail during `routing_table.csv` loading.
- For large generated cases, combine compressed routing rows with automatic TP generation when possible. A fully materialized `transport_channel.csv` can still dominate startup memory and time.

UB stores outports per destination grouped by metric. The group with the smallest metric is installed as “shortest”; other groups are installed as “other”. If `UseShortestPaths` is `true` (see below), selection is made from the shortest group; otherwise selection may consider all outports defined for that destination.

Note — destination-port aware lookup with fallback:
- The switch `ns3::UbRoutingProcess::GetOutPort(...)` first tries to route by the exact pair `(dstNodeId, dstPortId)` encoded in the packet headers.
- If no entry exists for that exact pair, `ns3::UbRoutingProcess::GetOutPort(...)` masks the destination-port field in the network address and retries using only `dstNodeId` (i.e., route “to the node”, regardless of its local port).
- The simulator currently assumes all ports on a node are mutually reachable (equivalent) at the destination, so the fallback may deliver to another port on that node.
- Implementation references: `ns3::UbRoutingProcess::GetOutPort(rtKey, inPort)` performs this exact-then-fallback lookup; `UbSwitch::ForwardDataPacket` builds the routing key via `GetURMARoutingKey` / `GetLdstRoutingKey`. Address fields originate from `utils::NodeIdToIp(nodeId[,portId])` and CNA helpers `utils::Cna16ToNodeId/Cna16ToPortId`.

---

## `transport_channel.csv`

Schema:
```
nodeId1,portId1,tpn1,nodeId2,portId2,tpn2,priority,metric
```
- Defines Transport Path Numbers (TPNs) between a local `(nodeId1,portId1)` and a remote `(nodeId2,portId2)`.
- `tpn1/tpn2` — local TP numbers at each side; pairwise mapping.
- `priority` — traffic class / priority (0..15 by default; see `UB_PRIORITY_NUM`).
- `metric` — relative preference when multiple TPs exist (small is better).

Example (multi-TP between two hosts across three links):
```
0,0,0,1,0,0,7,2
0,1,1,1,1,1,7,2
0,2,2,1,2,2,7,2
```

UB reads these into `TpConnectionManager`. The `priority` field allows selecting TPNs by priority, and the `metric` field is used to prefer lower-metric TPNs when multiple candidates are present.

Constraints and tips:
- TPNs are looked up per port in controllers (TPN→`UbTransportChannel` map). Ensure uniqueness per (node, port); duplicates will collide in demux.
- `priority` should be within `UB_PRIORITY_NUM`.
- `metric` is an unsigned integer; lower is preferred when selecting among multiple TPNs.

### Automatic TP Generation (Optional)

If `transport_channel.csv` is missing or has no matching channel for a traffic row, the config-driven runner automatically reserves RTP connection records from the routing table on every process while loading `traffic.csv`. This happens before any task is scheduled, so MPI and hybrid receiver ranks have the same reservation state before the first packet arrives.

- **Mechanism**:
  1. It queries the routing table (`UbRoutingProcess`) to find all reachable paths from source to destination.
  2. It respects the `UseShortestPaths` attribute (default `true`) to filter for shortest paths or allow non-shortest ones.
  3. It assigns TPN pairs and stores the same connection records on both endpoint views without creating either TP object.
  4. When the source task starts, `EnableMultiPath=true` materializes all selected sender endpoints; otherwise the source randomly selects and materializes one sender endpoint.
  5. The receiver validates the incoming channel key against its reservation and materializes only its local endpoint on the first valid packet.

- **Usage**:
  This is useful for simple scenarios where manual TP configuration is tedious. You can simply omit this file. However, for complex scenarios requiring specific TP mappings, fixed path selection, or specific multi-path policies, providing this file is recommended.

- **Performance Note**:
  - **CSV Configuration (Pre-instantiated)**: The simulator reads `transport_channel.csv` at startup and **immediately creates all TP objects** defined in it. In large-scale topologies (e.g., thousands of nodes), this file can be huge, and creating millions of TP objects upfront consumes significant memory and initialization time, even for TPs that may never carry traffic.
  - **Automatic Reservation + Local Materialization**: Connection records are reserved before traffic, but TP objects are created locally only when the sender uses a path or the receiver accepts its first valid packet. This avoids a massive CSV and avoids instantiating unused TP objects, making it **highly recommended** for large-scale simulations.

### TransportMode and CTP

`ns3::UbApp::TransportMode` defaults to `RTP`. In `RTP` mode, `transport_channel.csv`
uses existing TP Channel / TPG behavior.

In `CTP` mode, CTP traffic does not use RTP TP mappings from `transport_channel.csv`.
The file may still be present and loaded during setup, but CTP does not create RTP TP
channels and does not use TPN, PSN, TPACK, TPNAK, TPSACK, or end-to-end RTP retransmission.
Optional `traffic.csv` fields `srcEntityId` and `dstEntityId` select CTP entities; missing
fields default to `0`.

### TPN in the code (what it does and how to set it)

- TPN is an integer identifier written into packet headers and used to index `UbTransportChannel` objects via per-port maps in controllers.
- You set TPNs in `transport_channel.csv` (`tpn1`/`tpn2`). The code does not de-duplicate TPNs at parse time; avoid assigning the same TPN twice on the same (node, port).
- Selection uses `priority` (exact match) and `metric` (prefer minimum) in `TpConnectionManager`.

TP channel and TP group in code:
- A TP channel is the UB transport-layer shared path that the transaction layer uses. In code it is `ns3::UbTransportChannel`, created by `UbController::CreateTp(...)` and stored in a per-port TPN map (`m_numToTp`; see `UbController::GetTp/GetTpnMap`). Endpoints are fixed at creation: source/destination node IDs and ports, priority, and the `(srcTpn,dstTpn)` pair are passed to `UbTransportChannel::SetUbTransport(...)` inside `CreateTp`.
- A jetty (function layer context) can be bound to multiple TP channels to form a TP group for multipath. In code (`UbApp::SendTraffic`), after `UbFunction::CreateJetty(...)`, the app collects candidate TPNs from `TpConnectionManager` and calls `UbFunction::jettyBindTp(src,dest,jettyNum,multiPath,tpns)`. When `multiPath` is true, `jettyBindTp` looks up each TP by TPN (`UbController::GetTp`) and calls `UbTransportChannel::CreateTpJettyRelationship(...)` for each; the vector is recorded in `UbFunction::m_jettyTpGroup[jettyNum]`.
- Control-plane negotiation/bring-up is not modeled. Explicit rows are loaded by `UbUtils::CreateTp(...)`; otherwise the config-driven traffic loader reserves connection records before tasks start and each endpoint materializes locally when needed. For administrative procedures, refer to the [UnifiedBus (UB) Base Specification](https://www.unifiedbus.com/zh); they are currently outside this simulator’s scope.

---

## `traffic.csv`

Schema:
```
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
```

Recommendation: Generate `traffic.csv` (e.g., all-to-all, RDMA-like patterns, collective-like workloads) via `scratch/ns-3-ub-tools/traffic_maker/`. See `traffic_maker/README.md` in the `open-usim/ns-3-ub-tools` submodule repository.
- `taskId` — integer ID (unique per file).
- `sourceNode` / `destNode` — end-host node IDs.
- `dataSize(Byte)` — payload size in bytes.
- `opType` — e.g., `URMA_WRITE`, `URMA_READ`, `MEM_STORE`, … (supported by `UbApp`/API LDST layer).
- `priority` — 0..(UB_PRIORITY_NUM-1).
- `delay` — schedule offset relative to simulation start (Time).
- `phaseId` — integer phase tag; tasks with the same phase can run concurrently.
- `dependOnPhases` — optional list (space-separated) of phase IDs that must complete before this task’s phase starts.

Numeric field rules:
- `taskId`, `sourceNode`, `destNode`, `dataSize(Byte)`, `priority`, `phaseId`, and each `dependOnPhases` token are parsed as unsigned decimal integers.
- Signed spellings such as `-1` and `+1` are not accepted.
- `priority` must be within `0..(UB_PRIORITY_NUM-1)`; with the default build this is `0..15`. Out-of-range values fail fast while loading `traffic.csv`.

Examples:
```
0,0,1,4000000,URMA_WRITE,7,10ns,0,
1,0,1,4096,URMA_READ,7,20ns,1,0
0,0,10,16384009,MEM_STORE,7,10ns,1,
1,0,13,16384000,MEM_STORE,7,10ns,1,
```

Current URMA read/write constraints in `traffic.csv`:
- `URMA_WRITE` and `URMA_READ` do not require extra CSV columns for remote address, token, local address, or read offset in this iteration.
- In `CTP` mode, `URMA_WRITE` and `URMA_READ` may use optional trailing `srcEntityId,dstEntityId` columns. Missing values default to entity `0`.
- `URMA_WRITE` and `URMA_READ` complete on transaction responses (`TAACK` / `READ_RESPONSE`), not when the request's TP ACK arrives.
- Only the ROI success path is modeled for URMA read/write at the transaction layer right now; other service modes are rejected explicitly.
- `URMA_READ` is sliced at the TA layer. Each read request slice sends exactly one TP request packet with zero wire payload; the logical slice length is carried in `MAETAH.Length`.
- Each `URMA_READ` request slice generates exactly one `READ_RESPONSE`. The response is queued through `m_tpRelatedRemoteRequests` and may be split into multiple TP packets, but it is not transaction-sliced again.
- A multi-slice `URMA_READ` WQE still completes only once, after all slice responses arrive back at the initiator.

UB’s runner (`UbTrafficGen`) uses these to enqueue WQEs, connect to the proper `TpConnectionManager`, and drive sending/ACK tracking. The post-processing script `trace_analysis/task_statistics.py` merges Task and Packet traces back into this CSV, adding columns:
- `taskStartTime(us)`, `taskCompletesTime(us)` — task timeline
- `firstPacketSends(us)`, `lastPacketACKs(us)` — packet timeline
- `taskThroughput(Gbps)` — computed throughput per task

---

## Optional: `fault.csv`

Only required if `UB_FAULT_ENABLE` is `true` in `network_attribute.txt`. The exact schema depends on `UbFault` implementation (see `src/unified-bus/model/ub-fault*` and how `UbUtils::InitFaultMoudle` parses it). Typical fields include fault type, affected links/ports/nodes, start time, duration.

---

## Mapping to UB model code

- Attribute application happens in `UbUtils::SetComponentsAttribute` using ns-3 `Config::SetDefault` under the hood; attribute names map 1:1 with `GetTypeId().AddAttribute(...)` in classes like `UbPort`, `UbLink`, `UbSwitch`, `UbTransportChannel`, `UbApp`, etc.
- Node/port/link creation flows via `UbUtils::CreateNode` and `UbUtils::CreateTopo`, assembling `UbLink` between `UbPort`s. `topology.csv` bandwidth maps to `UbPort::UbDataRate`, delay to `UbLink::Delay`.
- Routing installs per-node forwarding tables from `routing_table.csv`.
- Transport channels (`transport_channel.csv`) build TPN mappings used by `UbApp` through `TpConnectionManager`.
- Tasks (`traffic.csv`) are scheduled by `UbTrafficGen`, and `UbApp` sends over the selected TPs, honoring `UsePacketSpray` vs. `EnableMultiPath`.

For advanced users, search in `src/unified-bus/model/`:
- `GetTypeId\(` and `AddAttribute\(` — discover attribute names/types and defaults.
- `UbUtils::Create*` — CSV parsing and object creation details.
- `UbTransportChannel`, `UbEgressQueue`, `UbSwitchAllocator` — behavior under multipath/spray and priority.

---

## Validating configuration values

- Attribute names/values:
  - If a run fails early, check console for attribute parse errors; fix the literal (e.g., `400Gbps`, `+10ns`, `true`).
  - To enumerate attributes of a component, inspect its `GetTypeId()` in the source.
- Structural consistency:
  - Ensure `node.csv` declares enough `portNum` to match `topology.csv` port IDs.
  - `routing_table.csv` `outPorts` must be valid egress ports on that `nodeId`.
  - `metrics` list length must equal `outPorts` length.
  - `transport_channel.csv` TPNs unique per (nodeId, portId).
  - `priority` values must be within `UB_PRIORITY_NUM`.
- Quick smoke test:
  - Run the case; UB will print clear errors for out-of-range indexes or bad formats.
  - Use `topo_plot.py` to visualize and catch wiring mistakes.

---

## Example: minimal 2-node single-TP

```
network_attribute.txt
  default ns3::UbPort::UbDataRate "400Gbps"
  default ns3::UbLink::Delay "+20ns"
  global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"

node.csv
  nodeId,nodeType,portNum,forwardDelay,allocationDelay
  0..1,DEVICE,1,,1ns
  2..3,SWITCH,4,10ns,1ns

topology.csv
  nodeId1,portId1,nodeId2,portId2,bandwidth,delay
  0,0,2,0,400Gbps,20ns
  1,0,3,0,400Gbps,20ns
  2,1,3,1,400Gbps,20ns

routing_table.csv
  nodeId,dstNodeId,dstPortId,outPorts,metrics
  0,1,0,0,1
  1,0,0,0,1
  2,0,0,0,1
  2,1,0,1,1
  3,0,0,1,1
  3,1,0,0,1

transport_channel.csv
  nodeId1,portId1,tpn1,nodeId2,portId2,tpn2,priority,metric
  0,0,0,1,0,0,7,1

traffic.csv
  taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
  0,0,1,4000000,URMA_WRITE,7,10ns,0,
```

In `ub-quick-example`, `UbUtils::ParseTrace()` runs after the simulator. If `UB_PARSE_TRACE_ENABLE` is `true` and `UB_PYTHON_SCRIPT_PATH` points to `parse_trace.py`, the script processes `runlog/` and writes analysis CSVs (e.g., `task_statistics.csv`, `throughput.csv`) under the same case directory.

The shipped case `scratch/2nodes_single-tp` keeps `UB_QUEUE_TRACE_ENABLE`, `UB_FLOW_CONTROL_TRACE_ENABLE`, and `UB_CONGESTION_CONTROL_TRACE_ENABLE` enabled on purpose as a trace example. Treat it as an observability-rich sample, not as the minimal recommended default for every new case.

---

## Network modeling notes (from code)

- Link rate and packet size determine transmission time; `ns3::UbLink::Delay` adds propagation delay; `forwardDelay` maps to `ns3::UbSwitch::InPortProcessingDelay`, while `allocationDelay` drives `ns3::UbSwitchAllocator::AllocationTime`.
- IFG: `ns3::UbPort::UbInterframeGap` (set to `0ns` to disable spacing).
- Queue/buffer: `ns3::UbQueueManager::BufferSize` bounds ingress/egress accounting used by the switch.
- Path choice: `UseShortestPaths` influences which outport sets are considered; `UsePacketSpray` toggles per-packet load-balance usage in headers and routing.
- Congestion control: `UB_CC_ALGO` and `UB_CC_ENABLED` pick and enable the algorithm. `CAQM` and RTP-only `DCQCN` are implemented.

---

## How to configure common behaviors (exact lines)

Place these in `network_attribute.txt` as needed (values shown are examples taken from shipped cases):

- Link/port basics
  - `default ns3::UbLink::Delay "20ns"`
  - `default ns3::UbPort::UbDataRate "400Gbps"`
  - `default ns3::UbPort::UbInterframeGap "0ns"`
- Switch allocator
  - `default ns3::UbSwitchAllocator::AllocationTime "10ns"`
- Transport toggles
  - `default ns3::UbTransportChannel::UsePacketSpray "false"`
  - `default ns3::UbTransportChannel::UseShortestPaths "true"`
  - Retransmission knobs (if used): `EnableRetrans`, `BaseRTO`, `MaxRetransAttempts`, ...
- Application multipath
  - `default ns3::UbApp::EnableMultiPath "false"`
- LD/ST API threads
  - `default ns3::UbApiLdst::ThreadNum "10"`
  - `default ns3::UbApiLdstThread::UsePacketSpray "true"`
  - `default ns3::UbApiLdstThread::UseShortestPaths "true"`
- Flow control and buffers (as needed)
  - `default ns3::UbSwitch::FlowControl "PFC_FIXED"`
  - `default ns3::UbPort::PfcUpThld "1677721"`
  - `default ns3::UbPort::PfcLowThld "1342176"`
  - `default ns3::UbQueueManager::ReservePerQueueBytes "1048576"`
  - `default ns3::UbQueueManager::SharedPoolBytes "12582912"`
  - `default ns3::UbQueueManager::HeadroomPerPortBytes "262144"`
  - `default ns3::UbQueueManager::AlphaShift "1"`
  - `default ns3::UbQueueManager::DynamicPfcResumeGapBytes "4096"`
  - `default ns3::UbQueueManager::PaperDynamicPfcBeta "8"` (only used with `PFC_DYNAMIC_PAPER`, the paper-style mode for **"Congestion Control for Large-Scale RDMA Deployments"**)
- Congestion control
  - `global UB_CC_ALGO "CAQM"`
  - `global UB_CC_ENABLED "false"`
  - RTP-only DCQCN defaults
  - `global UB_CC_ALGO "DCQCN"`
  - `default ns3::UbHostDcqcn::CnpInterval "50us"`
  - `default ns3::UbHostDcqcn::InitialRate "50Gbps"`
  - `default ns3::UbHostDcqcn::RateIncreaseTimer "55us"`
  - `default ns3::UbHostDcqcn::ByteCounterThreshold "10485760"`
  - `default ns3::UbHostDcqcn::RateAi "40Mbps"`
  - `default ns3::UbHostDcqcn::HyperAiRate "100Mbps"`
  - `default ns3::UbSwitchDcqcn::KminBytes "5120"`
  - `default ns3::UbSwitchDcqcn::KmaxBytes "204800"`
  - `default ns3::UbSwitchDcqcn::Pmax "0.01"`
- Trace and parsing
  - `global UB_TRACE_ENABLE "true"`
  - `global UB_PARSE_TRACE_ENABLE "true"`
  - `global UB_RECORD_PKT_TRACE "true"`
  - `global UB_QUEUE_TRACE_ENABLE "false"` (enable when `QueueTrace_*` evidence is needed)
  - `global UB_FLOW_CONTROL_TRACE_ENABLE "false"` (enable when `PfcTrace_*`/`CbfcTrace_*` evidence is needed)
  - `global UB_CONGESTION_CONTROL_TRACE_ENABLE "false"` (enable when `Dcqcn*`/`Caqm*` algorithm traces are needed)
  - `global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"`
- Priority/VL sizing
  - `global UB_PRIORITY_NUM "16"`
  - `global UB_VL_NUM "16"`

Then author the CSVs:
- `node.csv` (declare nodes/ports), `topology.csv` (wire ports with rate/delay),
- `routing_table.csv` (outPorts and metrics; smallest metric group becomes “shortest”),
- `transport_channel.csv` (TPNs and priorities; keep TPNs unique per (node, port)),
- `traffic.csv` (tasks).

---

## Discovering attributes at runtime (no doxygen)

ns-3’s CommandLine lets you introspect available TypeIds and Attributes directly from your program. Run your scenario with these flags:

Examples:

```bash
# List all registered TypeIds (you can pipe through grep Ub)
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintTypeIds'

# Show attributes for a specific component
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintAttributes=ns3::UbPort'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintAttributes=ns3::UbLink'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintAttributes=ns3::UbTransportChannel'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintAttributes=ns3::UbSwitchAllocator'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintAttributes=ns3::UbApp'

# Print global default paths (useful when writing network_attribute.txt)
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintGlobals'

# Print Unified Bus globals with type metadata
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp --PrintUbGlobals'

# General help for supported flags
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --PrintHelp'
```

Note: You can run the same inspection flags against other runnable ns-3 programs/examples. This avoids relying on doxygen and guarantees you see exactly what your build exposes.

---

## FAQ

- “How do I get the latest tools?” — If you use submodules, remember that `git submodule update --init --recursive` checks out the submodule SHA recorded by the parent repo. To follow the tools’ latest `main`, either:
  - In the submodule: `git checkout main && git pull` (local only), or
  - Update the submodule pointer in the parent repo and `git commit` it, so others get it via `submodule update`.
  - Optionally set a tracking branch in `.gitmodules` and use `--remote` with submodule update.

- “How do I find legal values for an Attribute?” — Look for `GetTypeId().AddAttribute(...)` in the component source. The C++ type (Time/DataRate/Uinteger/Boolean/Enum) dictates the literal format.

---

If anything is unclear, consult the corresponding tool script (`scratch/ns-3-ub-tools/`) and the UB model class to see exactly how a field is parsed and applied.

---

## Related Documentation

| Document | Description |
|----------|-------------|
| [../README.md](../README.md) | Project overview (中文) |
| [../README_en.md](../README_en.md) | Project overview (English) |
| [../QUICK_START.md](../QUICK_START.md) | Quick start: build, run, and tooling setup (中文) |
| [../QUICK_START_en.md](../QUICK_START_en.md) | Quick start: build, run, and tooling setup (English) |
| [ns-3-ub-tools/README.md](ns-3-ub-tools/README.md) | Python tools: topology/routing/traffic generation (incl. `traffic_maker/`) and trace analysis |
