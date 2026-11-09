# Decode fixed timing and folded network delays (v10)

Regenerate both `task_dag.csv` and traffic outputs with the matching scripts in
`tools/generate_trace/`. This revision changes Python generation/conversion only;
it does not change the native nine-column traffic format or require a C++ change.
The experiment launcher resolves its configuration and Python scripts beside
itself in `tools/generate_trace/`; set its trace and output paths for your run.

## Timing policy

Prefill and Decode timing are selected independently:

```json
"compute_model": {
  "mode": "glm51_flops",
  "decode_mode": "legacy"
},
"timing": {
  "prefill_us_per_token": 50,
  "decode_us_per_token": 10000,
  "first_decode_step_us": 10000,
  "storage_latency_us": 150
}
```

The supplied configuration retains Prefill FLOPs and switches Decode to fixed
timing. `decode_mode` defaults to `legacy` when omitted. Prefill's linear value
is only used when its own mode is `legacy`; setting it to 50 does not disable
Prefill FLOPs.

For each scheduled Decode step, take the maximum member duration: a new member
uses `first_decode_step_us`, a resident member uses `decode_us_per_token`.
With the supplied values, every nonempty step has 10,000 us of pure computation,
independent of batch size and context length. These are simulation assumptions,
not hardware measurements. Storage latency and network time are additional.

`--decode-ms-per-token 7` overrides both fixed Decode values to 7,000 us. It is
allowed with Prefill FLOPs. For arithmetic comparisons, explicitly select
`compute_model.decode_mode=glm51_flops`; that option requires Prefill FLOPs
configuration and rejects the fixed-duration CLI override. Arithmetic still uses
one new query per member and H=input_length+completed_steps cached keys.

## Logical DAG versus traffic execution

The logical DAG retains `DECODE_STEP_COMPUTE`, `D_COMPUTE_DONE`, and step ordering
for scheduling forecasts and diagnostics. The converter folds Decode computation
and control tasks into weighted network dependencies. No Decode `TIMER` rows
are written to `traffic.csv`, `traffic_debug.csv`, or `mapping.csv`. Prefill still
has its existing explicit batch-start and layer-computation TIMERs.

Every first-use `STORAGE_TO_D` still waits for its request's final Prefill phase.
Read delay is storage latency only (150 us by default). All input layers, blocks,
TP members, and uint32 chunks of a Decode batch share an ALL-completion phase.
A new member's Compute waits for all required input Reads and the previous
Decode computation. Resident steps do not reread input KV.

For one isolated request with three output tokens:

- Input Read phase R finishes at the time measured by ns-3.
- The logical DAG has three steps, each taking 10 ms.
- The final output Write depends on R, with `delay=30000us`.
- A slow Read pushes the Write start later by the same amount; Compute is not
  replaced by a fixed absolute timestamp.

A simplified native Write row is:

```csv
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
100,5,20,1024,URMA_WRITE,7,30000us,31,22
```

Here 22 is the input Read completion phase. All TP members and uint32 chunks
of that final Write receive the same computation dependency and delay.
No intermediate token Writes are added. Final output bytes remain
`output_length * kv_bytes_per_token`, excluding input KV. Writes are TP sharded
and use the existing `decode:<request_id>:output` storage object.

The next step depends on computation, not completion of a previous output Write.
When computation is folded, its dependencies can include earlier batches' Read
phases with accumulated delays. Never replace that weighted dependency set with
just the numerically largest phase: a larger phase ID does not prove that an
earlier independent Read has completed.

## One-delay format limitation

A weighted gate can require `max(finish(R1)+20ms, finish(R2)+10ms)` when new
requests join between steps. The CSV has only one delay after an AND barrier,
so it cannot in general represent this expression exactly without extra
execution semantics or TIMERs.

This converter keeps both phases and conservatively uses
`max(finish(R1), finish(R2))+20ms`. It never schedules the Write earlier than the
logical computation, but it may schedule it later. Uniform lags, including a
single isolated request and fully resident steps without new joins, are exact.
`--timing-policy error` rejects unequal-lag Decode network gates. Both
`conservative` and `max-phase` retain all Decode dependencies and apply padding;
`max-phase` is not permission to drop a Decode dependency. Prefill policy is
unchanged.

Summary fields expose the representation and approximation:

- `decode_compute_phases`, `decode_step_compute_phases`,
  `decode_layer_compute_phases`: zero.
- `folded_decode_compute_tasks`, `folded_decode_compute_ns`: logical computation
  folded into network dependencies (not elapsed simulation time).
- `decode_padded_network_tasks`: network tasks requiring unequal-lag padding.
- `conservative_examples`: weighted predecessor lags for sampled padded gates.

`decode_compute.csv` still reports one logical step per batch with `compute_us`.
FLOPs columns are blank under fixed timing. `--decode-compute-detail layer`
provides arithmetic detail only in FLOPs mode; it does not add execution TIMERs.
`batches_debug.csv` and `requests_debug.csv` remain generator forecasts. Mapping
consumers must not expect a physical `DECODE_STEP_COMPUTE` traffic row.

## Scope of this revision

Input Read release conditions, all-layer prefetch, batch membership, TP placement,
and final-write aggregation retain the current behavior. This change does not
restore the old layer-by-layer Read/Compute pipeline, add runtime Admission or
KV memory reservation, impose a prefetch bandwidth limit, or dynamically skip
unready requests. HBM capacity is still not modeled. Removing Decode TIMERs alone
does not fix excessive prefetch concurrency.

Run Python regressions without building or running ns-3:

```bash
python3 -m unittest discover -s tools/tests -p 'test_mooncake*.py'
```
