# Queue Backpressure vs Topology Capacity

<reference-hint>
<use-when>Use this reference when the user asks why a fabric with enough graph capacity still shows drop, hang, or `Potential Deadlock`.</use-when>
<focus>Queue buildup, local buffer pressure, path realization, and transport recovery semantics.</focus>
<keywords>Clos, oversubscription, queue, buffer full, Potential Deadlock, drop</keywords>
</reference-hint>

## First-Principles Model

Separate these questions before reasoning:

1. `Topology capacity`: does the graph have enough cut bandwidth in principle?
2. `Path realization`: which equal-cost path does each flow actually take?
3. `Queue dynamics`: do packets arrive at one local ingress/egress faster than they can be served?
4. `Progress semantics`: if one packet is lost, does the transport recover or stall forever?

A fabric can satisfy `1` and still fail at `2` to `4`.

## Hard Facts In This Repo

- `non-oversubscribed` or `full-bisection` only constrains long-term graph capacity. It does not guarantee zero queue buildup at every instant.
- Switch forwarding drops at ingress admission time when local buffer space is insufficient:
  - `src/unified-bus/model/ub-switch.cc`: `CheckInPortSpace(...)` failure logs `buffer full. Packet Dropped!`
- `ns3::UbSwitch::FlowControl` default is `NONE`:
  - `src/unified-bus/model/ub-switch.cc`
- With `FlowControl = NONE`, there is no upstream credit-based throttling before enqueue.
- The switch allocator consults `IsFcLimited(...)` before selecting a queue to send:
  - `src/unified-bus/model/ub-switch-allocator.cc`
- `CBFC` implements credit consume/restore and can block sending before downstream space is overrun:
  - `src/unified-bus/model/protocol/ub-flow-control.cc`
- Routing default is `HASH`, not queue-aware adaptive balancing:
  - `src/unified-bus/model/protocol/ub-routing-process.cc`
- Transport defaults include:
  - `EnableRetrans = false`
  - `UsePacketSpray = false`
  - `UseShortestPaths = true`
  - `src/unified-bus/model/protocol/ub-transport.cc`

## What This Means

- Equal-cost multipath only gives a candidate set. It does not mean all flows will be evenly spread.
- A synchronized collective can create a burst where many large flows hash onto the same uplink, VOQ, or ingress port at the same time.
- If there is no backpressure, local buffers absorb the burst until they run out.
- In this implementation, the overflow behavior is drop, not infinite buffering.
- If retransmission is disabled, one dropped packet can prevent a task or collective phase from ever reaching completion.
- The later `Potential Deadlock` warning means the head packet has been stuck too long. It is a progress symptom, not proof that the topology graph itself is cyclically deadlocked.

## Safe Wording

- A non-oversubscribed Clos can still drop packets if synchronized bursts overrun a local queue before service catches up.
- In this repo, `FlowControl = NONE` means there is no credit-based protection against downstream buffer overrun.
- `CBFC` changes the failure mode from `overrun then drop` to `credit-limited before send`.
- If `EnableRetrans = false`, loss can become a completion failure rather than a transient throughput hit.

## Unsafe Wording

- non-oversubscribed means no packet loss
- eight ECMP paths means traffic is automatically balanced
- `Potential Deadlock` proves the Clos topology is deadlocked
- if `throughput.csv` exists then the transport was lossless

## Minimal Diagnostic Path

When a user reports `理论上无收敛却丢包/挂住`, check in this order:

1. `network_attribute.txt`:
   - `ns3::UbSwitch::FlowControl`
   - `ns3::UbRoutingProcess::RoutingAlgorithm`
   - `ns3::UbTransportChannel::UsePacketSpray`
   - `ns3::UbTransportChannel::EnableRetrans`
2. run log symptoms:
   - `buffer full`
   - `No task completed`
   - `Potential Deadlock`
3. `traffic.csv`:
   - whether one phase starts many large tasks at the same timestamp
4. `routing_table.csv`:
   - whether there are many ECMP candidates but no evidence of adaptive selection
5. minimal A/B:
   - keep topology and workload fixed
   - change only `FlowControl` from `NONE` to `CBFC`

If the A/B flips the outcome, the first suspect is queue/backpressure semantics rather than fabric cut capacity.
