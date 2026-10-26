# ToE lossy cutover v2

Base: `skifisy/pd-ns3-ub` main, commit `2f964944db9fc4e303210bb819687e4d62f0c720`.
Apply the patch from the repository root (the directory containing `ns-3-ub/`):

```bash
git apply --check toe-lossy-cutover-main-v2.patch
git apply toe-lossy-cutover-main-v2.patch
```

## Configuration

In the case's `network_attribute.txt`, replace the existing EnableRetrans entry with:

```text
default ns3::UbTransportChannel::EnableRetrans "true"
```

Append `--toe-cutover=lossy` to your existing ToE command. For example, from
`ns-3-ub/`, after building locally:

```bash
./ns3 run --no-build 'scratch/ub-quick-example --case-path=YOUR_CASE --toe-mode=only --toe-cutover=lossy --toe-switch-delay-seconds=0.001'
```

Keep the existing thread count, seeds, retransmission mode and RTO configuration.
The new parameter does not alter thread selection or automatically enable retransmission.
The existing RTP and Mooncake 20-leaf/64-OCS-port restrictions still apply.
`--toe-mode=te` is also supported. `--toe-cutover=drain` is the default and keeps
old drain behavior. `--toe-reconfiguration` continues to select drain scope;
lossy always changes only the physical endpoints selected by the port planner.

## Model

1. Withdraw changed output ports and gate their schedulers. End hosts continue
   sending. Retained circuits remain connected with their existing flow control.
2. Invalidate removed channels immediately. Drop queued packets on changed
   outputs, with normal queue and buffer release accounting. Cancel the tracked
   serialization-complete event. Ignore delayed input-processing results whose
   output connection generation is stale. Do not migrate packets between queues.
3. After exactly `toe-switch-delay-seconds`, attach new links, initialize changed
   ports' flow control, rebuild the ToE port map, refresh/recompute TE where enabled,
   then enable transmission. No drain polling or propagation guard is used.

While the optical paths are disconnected, packets use ECMP over retained
circuits (direct paths preferred; otherwise one transit leaf). This also applies
to the interruption interval in TE mode; TE resumes on the new topology. No path
means an explicit drop. Non-TE control/ACK traffic in lossy mode also uses the
current ToE map rather than the original topology's static routes.

A removed link's scheduled deliveries are suppressed, including a packet that
was already fully serialized but not yet delivered. This is a deliberate model
assumption, not a statement that real OCS hardware loses every in-flight packet.
Packets already admitted to a switch can continue if their output connection
remains valid. Hop-local generation tags prevent their later buffer release from
returning old CBFC credit to a new neighbor. PFC release decisions continue to
use current physical buffer occupancy so that a new peer can resume when it falls.

TP state, PSNs, congestion state, retry budgets and RTOs are not reset. Loss
recovery is entirely through the configured transport retransmission mechanism.
Defaults are checked before traffic creation, and live channels are checked at
cutover. No changes are made to task-completion or FCT accounting.

## Output

`toe_reconfiguration.csv` adds lossy scope and these stage values:
`withdrawn`, `disconnected`, `connected`, `routes-installed`, `applied`.
The first two share one simulator timestamp; the last three share the timestamp
after optical switching. No controller/RPC distribution latency is added.
In lossy mode `paused_tp_channels` is zero, `drain_seconds` is zero, and existing
`pause_seconds` columns represent the cutover interval, not an endpoint pause.

`toe_cutover_drops.csv` contains aggregate `epoch,reason,packets,bytes` rows.
Reasons: `inflight`, `egress`, `voq`, `processing`, `no-route`, `admission`.
Epoch is the controller epoch when a drop is observed. `no-route` and `admission`
count all such losses while lossy mode is enabled, including outside cutover;
they are not proof that each loss was caused by reconfiguration. Bytes include
simulator packet headers, not just application payload. In-flight loss is counted
at the suppressed delivery callback, so its observation time can follow apply.
Snapshots are written at apply, before each solver decision, and at normal
simulator destruction. A forced process termination may leave the last snapshot
incomplete. Intentional cutover losses also enter common runtime drop diagnostics.
No per-packet disk log is added.

## Validation status and suggested local checks

Per request, this change has NOT been compiled or run, and no simulation or test
has been executed. Only source review, whitespace validation and patch
applicability checks were performed. Compilation and behavioral correctness
remain to be verified locally.

Suggested checks: busy-port cutover with nonzero input/allocation latency;
zero and nonzero optical delay; lost tail packet/ACK and timeout recovery;
multiple consecutive cutovers; CBFC, shared CBFC and PFC accounting; retained
circuit progress; both only and TE mode; drain-mode regression; full task
completion and retransmission counts. Fault-injection callbacks and distributed
MPI operation have not been validated. Use identical retransmission settings
for all comparison groups.

This revision is rebased onto current main and preserves its 10 ms replay default,
`--replay-interval` option and successful-transmission/path observation hooks.
Replay events include the lossy stage names; the physical topology snapshot is
published at reconnect. During disconnection its last topology snapshot describes
the previous wiring, while the `disconnected` phase identifies the outage.
