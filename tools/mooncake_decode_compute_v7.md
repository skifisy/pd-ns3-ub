# Decode FLOPs model and whole-step TIMER

The arithmetic model was introduced by v7 on top of v6. The v8 increment is
applied on top of v7 and changes Decode to read all input before computation.
The v9 increment applies on top of v8 and writes output KV once per request.
Regenerate both
`task_dag.csv` and traffic outputs with the matching scripts. The simulator's
existing TIMER support is sufficient; this change does not modify C++.

## Computation model

`compute_model.mode=glm51_flops` now applies to both Prefill and Decode.
Decode reuses the current expanded-KV MLA/DSA/MoE arithmetic coefficients,
architecture, precision-specific NPU operation rate and MAC-as-two-operations
convention. Prefill arithmetic is unchanged.

For each active Decode request, exactly one new query is computed per step.
The cached context is `H = input_length + completed_decode_steps`; the current
query attends to `L = H + 1` keys, including its own key. The first Decode step
uses completed steps zero. The configured output length controls how many
steps are generated; it is not charged as a single compute workload.

Projection, dense FFN, router and expert arithmetic scales with the number of
new queries, not with the cached context size. Indexer score pairs sum `L` per
request; sparse-attention pairs sum `min(L, index_topk)`. History is not projected
or passed through FFN a second time. Prefix hits still participate in attention.

Requests are grouped by their pinned Decode TP group. Attention, dense FFN and
router use `parallel.decode_tp`; each stage takes the slowest local group.
Routed experts share the EP pool and retain the existing load-imbalance model.
Shared experts use `compute_model.decode_shared_expert_tp_degree`, falling back
to the explicit shared-expert setting and then Decode TP. The supplied config
sets Decode shared-expert TP to 2 while keeping Prefill shared-expert TP at 8.

`compute_model.decode_efficiency` optionally overrides the common efficiency,
as either one scalar or a dictionary with attention, dense_ffn, router,
routed_expert and shared_expert entries. Omitted entries inherit the common
values; a default entry can override all omitted components.

All configured model layers contribute. Their time is quantized using
cumulative rounding so layer durations sum to the same integer-microsecond
total as a merged step. Dense/MoE and Indexer-on/off metric classes are reused
within each batch to avoid repeated calculations.

This is the same idealized arithmetic-throughput estimate used by the existing
Prefill model. It does not include HBM weight/KV traffic, TP/EP communication,
softmax, Top-K selection, normalization, or kernel launch overhead. Efficiency
1.0 uses peak arithmetic rate; it is not a measured end-to-end token latency.

Fixed `timing.first_decode_step_us` and `timing.decode_us_per_token` are ignored
in FLOPs mode and listed as ignored settings in the summary. An explicit
`--decode-ms-per-token` conflicts with FLOPs mode and is rejected. With mode
legacy, the historical max-member-step timing remains available.

## Task dependencies

Every Decode batch now uses one DECODE_STEP_COMPUTE TIMER for all model layers.
For requests entering Decode, all block/layer/TP Read records are generated
before computation, each depending only on that request's P_DONE frontier.
This permits all input layers to be prefetched while a previous batch computes.
There is no Decode inter-layer Read/Compute pipeline.

All input Read records in one batch share a single completion phase. Their
individual start dependencies still point at their own Prefill final phase;
sharing a completion phase does not make them wait for other requests to start.
The completion phase is ready only when ALL of its Read records finish.
The whole-step Compute waits for that phase plus the preceding Decode Compute.
This normally gives one dependency per Read and at most two for computation,
including large batches with hundreds of new requests.

Fully resident batches emit no input Reads, so their Compute only waits for
previous computation. The Compute TIMER has layer_index=-1 and empty request_id.
A request keeps generated KV locally until its final output token completes.
It then emits one logical DECODE_WRITE gathering all output tokens and layers.
There are no intermediate token writes or D_WRITES_DONE accumulation gates.
Each request writes as soon as it finishes, even if other batch members continue.
D_DONE waits for all of that request's final-write TP members and uint32 chunks.
The next batch waits for Compute, not outstanding writes.

Unscaled write bytes equal output_length * kv_bytes_per_token, excluding the
input KV already in storage. Each rank's size equals output_length times the
sum of its per-token layer shards, preserving integer rounding across ranks.
All ranks use one storage object keyed decode:<request_id>:output. This replaces
the old per-output-block placement; block_index=-1 and decode_token_index is the
last token's zero-based index. Full DAG output additionally records storage_key
and kv_extent_kind=all_output_tokens_all_layers. Zero-output requests emit no write.

One write means one logical request write, not necessarily one traffic CSV row:
nonempty TP ranks remain separate, and the converter splits each rank at uint32
size limits. With TP=2 and rank payloads below that limit, 100 output tokens now
produce two WRITE rows, rather than 200. Storage traffic shifts to request end;
its total unscaled payload is unchanged. --data-size-divisor still rounds up per
uint32 chunk after aggregation, so scaled byte totals can differ from v8.
The historical write-completion dependency expansion is removed, but other
conversion costs, including input Read expansion and file/SQLite I/O, remain.

The legacy storage_to_d_streaming setting is false in the supplied config.
A true value in older configs is ignored and reported; it cannot re-enable the
old inter-layer pipeline. Input flow sizes and layer/rank decomposition are
retained. Different network contention and the lost Read/Compute overlap can
change predicted batch timing and subsequent offline scheduling decisions.

Batch membership, placement, resource-release forecasts and TIMER durations
use the same FLOPs estimate. This remains an offline scheduled DAG, not a
runtime batch scheduler.

## Outputs

`decode_compute.csv` has one row per Decode batch by default. It includes batch
and request IDs, layer/step granularity, new queries, cached/visible context,
local group loads, full-model component arithmetic, stage times and rounded
compute_us. Sum the rounded Compute TIMER delays for that batch to reproduce
compute_us. FLOPs fields are blank for the legacy model.

Add `--decode-compute-detail layer` to the Trace-to-DAG command to also write
`decode_layer_compute.csv`, including all layer metrics even for merged steps.
This diagnostic option does not change the DAG or compute durations. Without it,
no 78-row-per-batch Decode detail is produced.

`traffic.csv` keeps the native nine-column format. `traffic_debug.csv` adds
request_id as before; batch Compute TIMERs leave it empty. Mapping consumers
must recognize both DECODE_LAYER and DECODE_STEP_COMPUTE. The traffic summary
reports total and merged-step Decode Compute phase counts, plus shared input Read phase count.
