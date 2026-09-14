# ns-3-UB Release Notes

**Language**: [English](RELEASE_NOTES_UB_en.md) | [中文](RELEASE_NOTES_UB.md)

## Release 1.3.0

**Release Date**: July 2026

### Simulation Features

- **Enhanced RTP reliable transport modeling**: Added selective retransmission support, including static / dynamic RTO modes, fast selective retransmission, selective MarkPSN, and SACK / NAK feedback handling. Retransmission remains an explicit opt-in capability, so existing no-retransmission cases are not silently changed.
- **RTP loss and fault-injection validation**: Added a representative `retrans_fault.csv` case and deterministic fault-injection path for reproducing DATA / ACK / SACK loss, delay, and recovery behavior.
- **MPI / MTP Traffic DAG semantics**: TrafficGen DAGs now extend from single-process cases to local MTP, MPI, and MPI+MTP hybrid modes. Task completion messages can propagate across ranks, and dependency visibility is bound to MPI lookahead, keeping dependent traffic workloads semantically consistent across threaded, multi-process, and hybrid runs.
- **Large-scale routing model expression**: Simulation cases can use range-based compressed route tables for large rule sets, and newly generated cases default to the generic compressed route path. This targets 1K-host and larger topologies where repeated routing rules would otherwise expand into million-line CSV files.
- **Parallel semantic consistency validation**: Added a canonical output path for comparing task completion results across local, MTP, MPI, and hybrid modes, making it easier to catch semantic drift in parallel execution.

### Simulation Engine Efficiency

- **TrafficGen initial load optimization**: Reworked traffic record parsing, opcode / delay parse caching, source app caching, and runtime task storage to reduce CPU and memory overhead during large `traffic.csv` load and activation.
- **Traffic DAG state storage optimization**: Replaced high-overhead presence bitmap / dense helper structures with compact task state and vector-based dependency storage, reducing resident memory for large DAG workloads.
- **Parallel runtime scheduling optimization**: Tuned ready-task collection, phase-id storage, cross-rank completion visibility, and MTP event ordering to reduce run-phase time for no-trace large workloads.
- **Compressed route loading optimization**: Reduced route file size, route-load memory pressure, and parse time so large Clos cases are no longer dominated by expanded routing CSV cost.

### Agent Skills

The in-repo OpenUSim Skills are a staged workflow of 5 agent flows covering the full UB simulation lifecycle:

- **welcome**: checks repo, toolchain, and build artifacts.
- **plan-experiment**: turns a natural-language goal into an executable experiment description.
- **run-experiment**: generates case files, configuration, execution, and explicit failure handling.
- **analyze-results**: interprets simulation outputs against the experiment goal.
- **capture-insights**: preserves verified root causes and reusable conclusions as knowledge cards.

This release adds a **comparison group mode** for A/B comparisons, parameter sweeps, and controlled-variable studies. Predictions and success criteria are registered before case generation; results are classified as `matched` / `mismatched` / `inconclusive` against those predictions, preventing post-hoc reasoning. Run and analysis stages are separated.

### Key Validation Metrics

The following data was collected with trace and parse disabled, using medians from repeated runs. Efficiency comparisons use `749a09f` as the baseline and `a5a519e` as this release.

<table>
  <thead>
    <tr>
      <th>Category</th>
      <th>Validation Item</th>
      <th>Test Scenario</th>
      <th>Result</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Simulation feature</td>
      <td>Four-mode output consistency</td>
      <td>32-host Clos, 16-task fan-in / fan-out DAG; canonical output compared across local, MTP, MPI, and hybrid modes.</td>
      <td>
        <ul>
          <li>All four modes produced identical output.</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td>Engine efficiency</td>
      <td>1K-host Clos compressed route table</td>
      <td><code>clos_1024h_32l_32s</code>; expanded route compared with compressed route.</td>
      <td>
        <ul>
          <li>Route file reduced 177.61x: 21.56 MB -> 121.36 KB.</li>
          <li>RSS dropped 44.81%: 1.021 GB -> 563.4 MB.</li>
          <li>Simulation run phase sped up 5.70x: 1959.6 ms -> 344.1 ms.</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td>Engine efficiency</td>
      <td>Multi-threaded performance (MTP, 4 threads, vs baseline <code>749a09f</code>)</td>
      <td>32-host Clos with 20,000 independent <code>URMA_WRITE</code> tasks.</td>
      <td>
        <ul>
          <li>Wall time sped up 1.26x: 7.980 s -> 6.350 s.</li>
          <li>Simulation run phase sped up 1.26x: 7805 ms -> 6214 ms.</li>
          <li>RSS dropped 3.86%: 466.0 MB -> 448.0 MB.</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td>Engine efficiency</td>
      <td>Single-thread performance (local, vs baseline <code>749a09f</code>)</td>
      <td>Same workload, local mode.</td>
      <td>
        <ul>
          <li>Wall time sped up 1.10x: 14.776 s -> 13.452 s.</li>
          <li>Simulation run phase sped up 1.09x: 14502 ms -> 13313 ms.</li>
          <li>RSS was effectively unchanged: 428.0 MB -> 427.6 MB.</li>
        </ul>
      </td>
    </tr>
  </tbody>
</table>

### Compatibility and Migration

- `traffic.csv` files with dependencies need a dependency visibility delay in MPI / hybrid mode, ensuring cross-rank completion visibility does not violate lookahead constraints.
- When `EnableRetrans=false`, `ub-quick-example` now fails fast on real packet drops. Experiments that intentionally validate loss recovery should explicitly enable `EnableRetrans` and choose an appropriate `RetransmissionMode`.
- `SelectiveAckBitmapBits=0` means AUTO; the runtime selects the feedback width needed to cover the receiver-side out-of-order evidence window. Use `EnableFastSelectiveRetrans` carefully when packet spray or other multipath routing may reorder packets.

### Fixes and Documentation

- Fixed logical-sequence wrap handling in comparisons and window checks.
- Fixed Traffic DAG dependency maintenance, task activation, and priority-field validation in the parallel runtime.
- Fixed DCQCN boundary marking so packets above `kmax` are marked deterministically.
- Fixed route hash salting so packet-spray path selection is isolated per node.
- Updated Quick Start, scratch case documentation, and the `ns-3-ub-tools` submodule to clarify UB-only focused builds, `--no-build` runs, route ranges, and traffic numeric field semantics.

---

## Release 1.2.1

**Release Date**: April 2026

### Features and Behavior Changes

- Completed the unified hook architecture for congestion control and flow control. Congestion-control algorithms plug into sender, switch, and receiver events through hooks such as `OnSender*`, `OnSwitch*`, `OnReceiver*`, and `OnTpAttached`; flow-control algorithms plug into ingress, egress, control-frame, and data-credit events through hooks such as `OnIngress*`, `OnEgress*`, `OnControlFrameReceived`, and `OnDataPacketReceived`. Users can add custom algorithms while reusing the existing topology, queue, trace, and case configuration paths, keeping most algorithm logic inside the corresponding algorithm classes and required enum/config entries instead of making invasive changes to switch, transport, or case-template code. The current implementation supports DCQCN and C-AQM congestion control, plus CBFC and PFC flow control.
- Added RTP-side DCQCN support, plus `PFC_DYNAMIC_PAPER` as the paper-style dynamic PFC threshold reproduction mode for the DCQCN paper **"Congestion Control for Large-Scale RDMA Deployments"** (SIGCOMM 2015).
- `ub-quick-example` now stops early when `EnableRetrans=false` and a packet is dropped, with guidance to check routing, buffer, and flow-control settings instead of continuing a run that has no recovery path.

### Compatibility and Migration

- This release keeps copied `scratch` case migration in the release notes, with runtime diagnostics for common legacy configuration keys.
- `network_attribute.txt` is now scanned for known legacy keys before `ConfigStore` loads it, so copied cases get a migration hint. Known migrations include `ns3::UbQueueManager::ResumeOffset` -> `ns3::UbQueueManager::DynamicPfcResumeGapBytes`, `ns3::UbSwitch::EnableCBFC/EnablePFC` -> `ns3::UbSwitch::FlowControl`, and `ns3::UbApiThread::*` -> `ns3::UbLdstThread::*`.
- If an older case depends on the previous `CbfcRetCellGrainControlPacket=1` behavior, set that value explicitly in `network_attribute.txt`; the current repo default is `32`.
- Fine-grained trace files are controlled by new switches: `UB_QUEUE_TRACE_ENABLE`, `UB_FLOW_CONTROL_TRACE_ENABLE`, and `UB_CONGESTION_CONTROL_TRACE_ENABLE`. Older cases that omit them still run, but they do not automatically produce the corresponding `QueueTrace_*`, `PfcTrace_*`, `CbfcTrace_*`, `Dcqcn*`, or `Caqm*` files.

---

## Release 1.2.0

**Release Date**: March 2026

### New Features

- **OpenUSim Agent Skill System**: Introduced four-stage repository-bundled AI Agent Skills, enabling AI coding assistants (Codex / Claude Code / Cursor, etc.) to drive UB simulation experiments end-to-end. The four stages are: environment readiness check (welcome), experiment planning and parameter convergence (plan-experiment), case generation and simulation execution (run-experiment), and result interpretation with root-cause analysis (analyze-results). Includes an AGENTS.md routing policy, a shared knowledge base (topology options, workload patterns, trace observability, etc. — 7 reference documents), and automated simulation configuration scripts

- **Full URMA Read Data Path**: Implemented the complete URMA Read request/response data path. Read requests are sent with zero payload carrying logical byte counts; the remote side automatically generates a Read Response with the actual data. The transport layer supports multi-packet Read response reassembly and completion detection, while the transaction layer distinguishes Request/Response directions and correctly handles the different completion semantics of Read vs. Write

- **Flow Control and Buffer Management Overhaul**:
  - **Shared Buffer Dynamic Admission Control**: Redesigned ingress buffer management with a Reserve → Shared → Headroom three-tier admission model. Each ingress queue has a dedicated reserve quota; excess traffic competes for allocation from a global shared pool via dynamic thresholds (Alpha); under PFC, per-port headroom absorbs in-flight packets. Supports XOFF/XON watermark queries and anti-oscillation resume offset
  - **CBFC / PFC Flow Control Modes**: Flow control expanded to five modes — NONE, CBFC (exclusive credit), CBFC_SHARED (shared credit pool), PFC_FIXED (fixed-threshold backpressure), and PFC_DYNAMIC (buffer-occupancy-based dynamic-threshold backpressure). CBFC and PFC operate as peer flow control strategies sharing the same ingress admission model, selectable per scenario

- **MPI Multi-Process Data Path**: Added a remote link abstraction to support cross-process UB packet transmission via MPI, enabling distributed multi-process simulation. Combined with the unified quick-example entry point, supports MPI config-driven multi-host topology simulation

### Improvements

- **Simulation Stall Warning**: The case-runner monitors task completion progress in real time and emits a potential deadlock warning when no task completes for an extended period, helping quickly identify flow-control deadlocks or routing loops
- **Fine-Grained Tracing**: Module-level trace switches allow per-layer trace output to be enabled or disabled on demand, reducing I/O overhead in large-scale simulations
- **Observability Tier Presets**: Multiple observability presets enable one-click switching of log verbosity between quick validation and deep analysis scenarios
- **TrafficGen Thread Safety**: The traffic generator supports safe invocation under UNISON multi-threaded concurrent scheduling
- **TrafficGen URMA Read Support**: Traffic description files now support specifying the URMA_READ operation type
- **Unified Simulation Entry**: ub-quick-example restructured as a config-driven unified entry point supporting both MPI multi-process and MTP multi-threaded execution modes

### Bug Fixes

- Fixed fairness issue in TA-layer WQE Segment scheduling that caused some segments to starve
- Fixed multi-packet URMA Read request slicing reassembly logic to ensure data integrity
- Fixed incorrect port information in routing traces and VOQ index bounds checking
- Fixed unified-bus library link failures under certain build configurations
- Fixed race conditions during initialization, improving startup stability
- MPI-related tests are now conditionally compiled by build flags; non-MPI builds no longer fail

### Build & CI

- Simplified CI pipeline to Ubuntu single-platform
- Added uv.lock dependency lock file, pinned Python 3.11
- Updated ns-3-ub-tools submodule

### Tests

- Added regression tests for URMA Read, shared buffer admission, and MPI CBFC hybrid mode
- Added TrafficGen and quick-example entry boundary tests
- Added Agent Skill documentation and helper script tests
- ~2800 net new lines of test code

---

## Release 1.1.0

**Release Date**: January 2026

### New Features

- **UNISON Multi-threaded Parallel Simulation**: Integrated UNISON framework for multi-threaded parallel simulation
- **DWRR Scheduling Algorithm**: Added Deficit Weighted Round Robin (DWRR) based inter-VL scheduling support on both network and data link layers
- **Adaptive Routing**: Implemented port-load-aware adaptive routing with configurable routing attributes
- **Deadlock Detection**: Added potential deadlock detection in UB switch and transport layer with enhanced packet arrival time tracking
- **CBFC Credit-Shared Mode**: Introduced CBFC credit-shared mode for more flexible flow control configuration

### Optimizations & Bug Fixes

- Optimized DWRR user configuration method
- Refactored buffer management architecture with unified VOQ management (dual-view with egress statistics)
- Enhanced routing table lookup process
- Improved queue management with byte-limit based egress queue management
- Fixed LDST CBFC compatibility issues
- Optimized flow control configuration interface
- Fixed TP removal and credit resumption at switch allocator
- Support for automatic TP generation without configuration files
- Support for useless TP removal optimization

---

## Release 1.0.0

Initial release of ns-3-UB simulator implementing the UnifiedBus Base Specification with comprehensive protocol stack support across function, transaction, transport, network, and data link layers.

**Key Features:**
- Complete UB protocol stack implementation
- Support for Load/Store and URMA programming interfaces
- Congestion control and flow control mechanisms
- Multi-path routing and load balancing
- QoS support with SP scheduling
- Credit-based flow control with CBFC support
