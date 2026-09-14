# Count-Based Round Robin Balances Assignments, Not Work

<reference-hint>
<use-when>Use this reference when round robin balances assignment counts but queues, bytes, service time, latency, or completion remain unequal.</use-when>
<focus>How heterogeneous work cost, periodic arrivals, modulo phase, and scheduler state scope can turn count fairness into load imbalance, plus the evidence needed to diagnose it.</focus>
<keywords>round robin, count fairness, work balance, service demand, heterogeneous cost, phase coupling, modulo aliasing, queue imbalance, packet spray, trace semantics</keywords>
</reference-hint>

## Contents

- Core Judgment
- General Model
- Conditions For Phase Coupling
- State Scope Matters
- Pipeline Boundaries Matter
- Observation Can Be Biased
- General Diagnostic Method
- Design And Mitigation Space
- Verified OpenUSim Example
- Safe And Unsafe Conclusions
- Authority And Scope

## Core Judgment

Round robin answers one narrow question:

> How many assignment opportunities did each candidate receive?

It does not answer:

- how many bytes each candidate received
- how much service time each assignment requires
- how much downstream contention each assignment encounters
- how much queueing delay or completion delay each candidate contributes

This distinction applies whenever a deterministic cyclic allocator distributes work items whose
costs are not equal. The work items may be packets, flows, requests, tasks, transactions, or queue
entries. Equal assignment count is load balance only when one assignment is a good proxy for equal
work.

The reusable interpretation rule is:

> Judge balance in the unit consumed by the bottleneck, not automatically in the unit counted by
> the selector.

For a fixed-rate network port, bytes or serialization time are usually closer to work than packet
count. For another resource, the relevant cost could instead be processing cycles, memory traffic,
or predicted completion time.

## General Model

Let work item `i` have cost `c_i`, and let a `K`-way round robin assign it to:

```text
candidate(i) = i mod K
```

Round robin keeps the number of assigned items nearly equal:

```text
N_p = count of items assigned to candidate p
```

But the actual work on candidate `p` is:

```text
W_p = sum(c_i for all i assigned to p)
```

Equal `N_p` does not imply equal `W_p` unless costs are equal or sufficiently decorrelated from
`i mod K`.

For a network outport with rate `R`, service demand over an observation window is:

```text
S_p = sum(on-wire bytes assigned to p) * 8 / R
```

If arrivals during a window of duration `T` create `S_p > T`, backlog on that outport must grow,
even when every outport receives exactly the same number of packets.

Long-run averages are not enough. A short interval can overload one candidate and determine tail
latency or phase completion even if full-run bytes later look balanced.

## Conditions For Phase Coupling

Harmful phase coupling becomes possible when these conditions overlap:

1. `Heterogeneous cost`: work items have materially different service demand.
2. `Structured arrival order`: cost classes appear periodically or in correlated bursts.
3. `Deterministic modulo assignment`: the selector advances through a fixed candidate cycle.
4. `Relevant state scope`: multiple producers contribute to the same cursor sequence.

For example, with two candidates and an arrival pattern:

```text
large, small, large, small, ...
```

one cursor phase produces:

```text
candidate 0: large, large, large, ...
candidate 1: small, small, small, ...
```

Both candidates receive the same number of items, but not the same work. This is modulo aliasing
between the workload period and the scheduler period.

The pattern need not be a strict two-item alternation. Any periodic or correlated sequence can
alias with `K`, including batched requests, control/data mixtures, alternating operation types, or
multiple synchronized producers whose merged order is regular.

Phase coupling is not inevitable:

- equal-cost work items make count fairness a reasonable load proxy
- independent random costs tend to decorrelate from cursor parity over long runs
- randomized or changing arrival order can break a stable phase
- a different cursor scope can either remove or create the correlation

Therefore one counterexample proves the mechanism is possible, not that RR is always imbalanced.

## State Scope Matters

Before interpreting round robin, identify what owns the cursor. Common choices include:

- one cursor per device
- one cursor per candidate set
- one cursor per source, flow, class, priority, or destination
- one cursor per output scheduler

These choices create different merged sequences. A candidate-set-global cursor may combine work
from many inports, flows, and destinations. A per-flow cursor isolates flows but can align every
flow to the same initial phase. Neither scope is universally better.

Initial phase is a separate property from state scope. It may be zero, deterministic from topology
identity, hash-derived, random, or persisted from previous work. Never infer initialization from the
name `round robin`.

To claim phase coupling, capture the real cursor owner, initial state, and complete advance sequence.
Do not infer it only from final path counts.

## Pipeline Boundaries Matter

Many systems contain more than one round-robin stage. Their names do not make them one control loop.
For a switch pipeline, separate:

1. arrival order at the switch
2. routing or assignment to an outport
3. enqueue into a queue already associated with that outport
4. arbitration among queues feeding that outport
5. physical serialization and downstream service

A downstream arbiter usually cannot repair an upstream partition decision. If routing has already
assigned excess work to outport 3, an allocator for outport 3 may choose fairly among its ingress
queues, but it cannot move that work to outport 2.

This is a general rule:

> Fairness inside a partition does not imply balance across partitions, and a later local scheduler
> cannot repair an earlier immutable assignment without an explicit migration mechanism.

Claims that two RR stages are "synchronized" require both decision sequences. Similar frequency or
similar names are not evidence that their phases or state dimensions match.

## Observation Can Be Biased

The trace used for diagnosis must observe the event being claimed.

Completion-oriented traces often record only work that advances state, such as:

- an ACK that advances a cumulative acknowledgement point
- a request that completes a task
- a packet that reaches the final endpoint
- a queue event that survives filtering or sampling

Such traces can omit real wire traffic, redundant control messages, delayed work, dropped work, or
work that no longer changes completion state. This creates survivorship bias: the least delayed
events are more likely to appear as the events that made progress.

Use a decision-time trace for assignment claims and a completion-time trace for progress claims.
Do not substitute one for the other.

The minimum distinction is:

```text
assignment evidence: what was selected, when, and with what cost
service evidence: when service began and ended
progress evidence: whether the event advanced protocol or task state
```

## General Diagnostic Method

When assignment counts are balanced but performance is not, collect decision-time records with:

```text
time, scheduler identity, cursor scope, cursor before selection,
candidate set, selected candidate, work-item identity,
work class, estimated cost, source, destination or owner
```

For packet routing, add:

```text
packet UID, opcode/type, on-wire bytes, inport, outport, priority
```

Then check in this order:

1. `Count balance`: are assignment counts actually equal?
2. `Cost balance`: are bytes, service demand, or predicted work equal?
3. `Window overload`: does one short window inject more work than can be served?
4. `Modulo correlation`: is work class correlated with cursor residue `i mod K`?
5. `State scope`: which producers share the cursor, and are any calls missing?
6. `Pipeline boundary`: can the downstream scheduler change the earlier assignment?
7. `Trace coverage`: are missing records filtered because they did not advance progress?

Useful metrics include:

- count, bytes, and service demand per candidate
- cost distribution conditioned on cursor residue
- maximum imbalance over sliding windows, not only full-run totals
- queue peak and dwell-time distribution
- task or collective tail completion
- complete wire events separately from progress events

A strong controlled comparison keeps workload, topology, candidate sets, timing, flow control, and
random seed fixed, then changes only the selection policy or cursor scope.

## Design And Mitigation Space

The right alternative depends on what cost is stable and observable.

### Cost-aware cyclic policies

- byte-aware or weighted round robin
- deficit round robin
- estimated-service-time-aware assignment

These retain a cyclic structure while accounting for unequal work. They require a cost estimate and
may still miss downstream congestion.

### Feedback-aware policies

- current queue-byte-aware selection
- local service-time or drain-time estimation
- congestion-feedback-driven routing

These react to realized imbalance, but feedback can be stale or local while the true bottleneck is
downstream.

### Decorrelation policies

- randomized initial phase
- independently seeded cursor scopes
- controlled permutation of candidates
- separating materially different work classes

These can break deterministic aliasing but do not by themselves guarantee work balance. Splitting
state too finely can also synchronize every new scope to the same phase or reduce statistical
mixing.

### Semantic policies

- per-flow or per-collective state
- operation-class-aware routing
- deadline- or tail-aware assignment

These may better match application goals, but they introduce ordering, fairness, state-size, and
implementation-cost tradeoffs.

No policy should be called better solely because it fixes one phase pattern. Evaluate count fairness,
work fairness, responsiveness, ordering behavior, and tail completion together.

## Verified OpenUSim Example

The general mechanism was exposed by a deterministic OpenUSim counterexample on branch `next`,
commit `9402d4ccc98670691c5be4dd7359bcafe4c892b0`. The workload was a SyCCL-derived bidirectional
collective on a dual-spine Clos, but the reusable result does not depend on SyCCL.

At leaf 12, remote destinations shared candidate set `(2,3)`. Direct instrumentation before VOQ
enqueue observed one routing object, 4,044 calls, cursor values `0..4043` without gaps, and strictly
alternating indices. RR was working exactly as implemented.

A host-facing inport carried both large data and small ACKs because local endpoints simultaneously
sent data and acknowledged reverse-direction data. Multiple synchronized producers contributed to
the same candidate-set cursor. Raw adjacent decisions included:

```text
timeNs=286143 uid=17923 tpOpcode=1 bytes=4174 inPort=0 outPort=3
timeNs=286145 uid=18103 tpOpcode=2 bytes=62   inPort=0 outPort=2
timeNs=286145 uid=18010 tpOpcode=1 bytes=4174 inPort=1 outPort=3
timeNs=286148 uid=17932 tpOpcode=2 bytes=62   inPort=0 outPort=2
```

During `286-289 us`, both outports received exactly 53 packets:

| Packet class | Outport 2 | Outport 3 |
| --- | ---: | ---: |
| 62 B TP ACK | 41 packets / 2,542 B | 27 packets / 1,674 B |
| 4,174 B on-wire data | 7 packets / 29,218 B | 24 packets / 100,176 B |
| Small response | 5 packets / 395 B | 2 packets / 158 B |
| **Total** | **53 packets / 32,155 B** | **53 packets / 102,008 B** |

At `180 Gbps`, this corresponds to approximately `1.429 us` versus `4.534 us` of serialization
work in a `3 us` arrival window. Outport 3 therefore had to accumulate backlog despite perfect
packet-count balance.

The example also exposed an observation trap. `AllPacketTrace_ACK` was emitted only when an ACK
advanced `m_psnSndUna`. A delayed ACK routed through the busy outport could arrive after a newer
cumulative ACK and disappear from this progress trace:

```text
UID 18103: routed to outport 2, appears in AllPacketTrace_ACK
UID 18113: routed to outport 3, absent from AllPacketTrace_ACK
```

Thus the original appearance that ACKs used only one path was a trace-selection artifact. Direct
routing records showed ACKs on both paths.

In the verified implementation, adaptive routing scored candidate VOQ bytes plus egress-queue
bytes. It could react to the byte backlog invisible to count-based RR. This explains the observed
case result but does not prove adaptive routing is globally superior.

## Safe And Unsafe Conclusions

Safe:

- Count fairness and work fairness are different invariants.
- Periodic heterogeneous work can alias with a deterministic cyclic selector.
- Cursor scope and initial phase are part of policy semantics.
- A later per-partition scheduler cannot normally repair an earlier cross-partition imbalance.
- Progress traces may omit real work and should not be treated as complete assignment traces.
- Short-window service demand can explain tail latency that full-run averages hide.

Unsafe:

- Round robin always causes imbalance.
- Randomizing the initial phase guarantees balance.
- Equal assignment counts imply equal utilization or latency.
- Two RR stages are coupled because both are round robin.
- A missing completion-trace record proves the work item did not traverse the system.
- One OpenUSim case proves hardware behavior or a universal routing-policy ranking.

## Authority And Scope

Verified OpenUSim code paths on `next`:

```text
src/unified-bus/model/protocol/ub-routing-process.cc
src/unified-bus/model/protocol/ub-routing-policy.cc
src/unified-bus/model/ub-switch.cc
src/unified-bus/model/ub-switch-allocator.cc
src/unified-bus/model/protocol/ub-transport.cc
```

Related references:

```text
.codex/skills/openusim-references/queue-backpressure-vs-topology.md
.codex/skills/openusim-references/trace-observability.md
```

Evidence level: the general scheduling argument follows from count-versus-cost accounting; the
specific OpenUSim mechanism and trace interpretation were verified by a deterministic packet-level
counterexample. The example is not a hardware measurement and does not imply that every workload
exhibits persistent phase coupling.
