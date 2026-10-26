# Trace Observability

<reference-hint>
<use-when>Use this reference when discussing `trace/debug` choices, especially `minimal`, `balanced`, and `detailed`.</use-when>
<focus>Observability tiers, produced evidence, and runtime-overhead tradeoffs.</focus>
<keywords>trace, debug, minimal, balanced, detailed, observability</keywords>
</reference-hint>

## Tier Definitions

| Tier | Intent | Post-processing output |
|------|--------|----------------------|
| minimal | 只要任务级统计，最快跑完 | `task_statistics.csv`（仅任务起止，无首包/末ACK） |
| balanced | 任务 + 包 + 端口，够用的排查能力 | `task_statistics.csv`（含首包/末ACK）+ `throughput.csv` |
| detailed | 全开，含逐包逐跳路径证据 | 同 balanced + `AllPacketTrace_*.tr` |
| custom | 用户自选开关组合 | 取决于所选开关 |

Use `observability_preset(tier)` from `network_attribute_writer` to get the concrete overrides dict.
Do not hardcode switch names in experiment specs or agent logic.

## Hard Facts

- `trace/debug` mode changes observability and logging overhead.
- `detailed` trace can increase wall-clock runtime, trace volume, and post-processing cost.
- `minimal` trace reduces observability compared with `balanced` or `detailed`.
- `trace/debug` mode is an attribute overlay on `network_attribute.txt`, not a separate simulation result.
- `UB_QUEUE_TRACE_ENABLE` gates queue trace files such as `QueueTrace_*` from queue events and periodic sampling.
- `UB_PACKET_TRACE_ENABLE` gates packet timing files such as `PacketTrace_node_*.tr`,
  including CTP first-send and last-ACK rows.
- `QueueTrace_*` can include ingress queue occupancy rows with the `ingress` marker when `UB_QUEUE_TRACE_ENABLE` is enabled.
- `UB_FLOW_CONTROL_TRACE_ENABLE` gates algorithm-emitted flow-control trace files such as `PfcTrace_*`, `PfcDynamicTrace_*`, and `CbfcTrace_*`.
- `UB_CONGESTION_CONTROL_TRACE_ENABLE` gates algorithm-emitted congestion-control trace files such as `DcqcnMarkTrace_*`, `DcqcnCnpTrace_*`, `DcqcnSenderTrace_*`, `CaqmAckTrace_*`, and `CaqmSenderTrace_*`.
- These category gates default to `false`, so experiments must opt in explicitly when these files are needed.

## Optional Replay Telemetry

- Add `--replay` to existing ub-quick-example commands. Mode and case_id are
  inferred from validated Jupiter TE / ToE inputs. Default output is
  `<case>/replay/`: 500ms, all links aggregated by directed node pair, full-run
  reliable URMA WRITE / READ_RESPONSE endpoint/path aggregates, queues off.
- Local MTP (including `--mtp-threads=8`) is supported for ECMP and TE replay.
  Path counters and task buffers belong to logical-process IDs, not OS threads.
  The public LP samples/writes after the worker barrier; worker hooks cannot
  advance the shared sampling window. Shared topology/epoch state is only
  changed by public callbacks. No per-packet global logging lock is used.
- Native TaskTrace is reused when enabled; otherwise replay task CSV is buffered
  per LP and chunk writes are mutex-protected (about 64KiB). CSV order can be
  interleaved; use exact time_ns/task_id, not row order. Final flush follows
  worker join and covers the latest executed LP time.
- MPI output is not implemented and still rejected. ToE controller itself still
  requires single-thread execution; telemetry support does not remove that
  independent base-repository guard. No thread count is silently reduced.
- Reliable business payload is counted once at successful source-Leaf fabric
  transmission, including retransmission. Intermediate Leaf identifies the
  actual first hop; subsequent shortest routing is constrained by existing
  logic, not a destination delivery confirmation. Current te_epoch need not be
  the original route epoch of queued packets.
- Full-run path aggregation has per-packet update and output costs. Boundaries
  split 500ms base windows; physical capacity remains separate from business
  pause. See `../../../../tools/REPLAY_TELEMETRY_V4.md`.

## Non-Implications

- Do not claim that `detailed` trace itself lowers simulated throughput.
- Do not claim that `detailed` trace makes a topology less able to approach line rate.
- Do not treat stronger observability as a network-semantic performance regression.
- Do not treat weaker observability as evidence that performance is better.

## Safe Wording

- `detailed trace` is better for diagnosis, but it can make the run and post-processing slower.
- `balanced` is a good default when the user wants some observability without paying the highest logging cost.
- `minimal` is suitable when the user mainly wants a faster run and accepts weaker debugging evidence.

## Unsafe Wording

- `detailed trace` may itself reduce throughput.
- `detailed trace` is not suitable for judging whether the topology approaches line rate.
- turning on more trace means the simulated network performs worse.

## Authority

- `code/doc fact`: trace/debug is modeled as attribute overrides on `network_attribute.txt`
- `maintainer fact`: detailed trace affects runtime cost and observability, not simulated throughput semantics by itself
