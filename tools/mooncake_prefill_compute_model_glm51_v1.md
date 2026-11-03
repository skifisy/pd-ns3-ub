# Mooncake v15 Prefill Compute Model — GLM-5.1

## Scope

This update changes only Prefill compute-duration generation in
`mooncake_trace_to_dag_v15.py`.

- Decode timing and `BatchBuilder.decode_step()` are unchanged.
- KV Cache read/write task generation and dependencies are unchanged.
- Scheduler, request pinning, BS, TP/EP grouping, and DAG task schema are unchanged.
- `mooncake_dag_to_traffic_v15.py` is unchanged.
- Historical Indexer Key Cache is assumed resident at the Prefill compute node. Its network transfer is not modeled; Indexer projection/scoring FLOPs are modeled.

## Public NPU peak used

The baseline maps W8A8 to the INT8 arithmetic path and uses the public
`Ascend 950PR 32-AI-subsystem` value:

- INT8 peak: **919 TOPS = 9.19e14 operations/s**.
- The 28-AI-subsystem 950PR variant is listed at **804 TOPS**; set
  `compute_model.npu_peak_ops_per_sec` to `804e12` if that is the installed SKU.
- Huawei positions 950PR for Prefill workloads.

This is a theoretical peak, not measured sustained throughput. Since no public
GLM-5.1 kernel utilization factor is specified, the supplied config uses
`efficiency=1.0`. Resulting compute times are idealized peak-throughput lower
bounds. Reduce the efficiency values (or use measured kernel throughput) for
more realistic timing.

## Per-layer FLOPs

Let each request i have `m_i` uncached/MISS tokens and `h_i` cached-prefix tokens.

The Indexer score-pair count is modeled as:

`P_i = m_i * (h_i + m_i)`

The selected causal sparse-attention pair count is:

`A_i = sum(j=0..m_i-1) min(index_topk, h_i + j + 1)`

The model uses the GLM-5.1 architecture defaults:

- hidden size 6144; 78 layers; 64 attention heads
- Q LoRA rank 2048; KV LoRA rank 512
- QK NoPE/RoPE dimensions 192/64; V dimension 256
- Indexer 32 heads × 128 dimension; Top-K 2048
- 256 routed experts; Top-8; expert intermediate size 2048
- one shared expert; first 3 layers use Dense FFN intermediate size 12288

For one new token, MLA projections are counted as:

`F_MLA/token = 2 * (d*rq + rq*H*(d_nope+d_rope) + d*(rkv+d_rope) + rkv*H*(d_nope+d_v) + H*d_v*d)`

Indexer projections (excluding Q-A, which is already counted in MLA) are:

`F_indexer_proj/token = 2 * (rq*Ih*Id + d*Id + d*Ih)`

Indexer scoring and DSA sparse attention are:

`F_indexer_score = P * (2*Ih*Id + 2*Ih)`

`F_sparse_attention = A * (2*H*(d_nope+d_rope) + 2*H*d_v)`

Dense FFN FLOPs/token:

`F_dense/token = 6*d*intermediate_size`

MoE layer FLOPs/token:

- Router: `2*d*num_routed_experts`
- Routed experts: `6*d*moe_intermediate_size*top_k`
- Shared expert: `6*d*moe_intermediate_size*num_shared_experts`

The per-layer compute time is the sum of modeled sequential stages:

`T_layer = T_attention + T_dense_or_router + T_routed_expert + T_shared_expert`

Routed expert time uses the shared EP pool (256 NPU) and a configurable load
imbalance multiplier. Attention, Router, Dense FFN, and Shared Expert use the
maximum local TP-group time across the DP engines in the Prefill wave.

The current model excludes TP/EP communication, layer norm/RMSNorm, RoPE,
softmax, activation/elementwise operations, Top-K sorting/selection overhead,
and memory-bandwidth limits. The Indexer Key Cache residency assumption removes
only its extra network transfer; it does not remove Indexer projection/scoring
compute.

## Configuration

The ready-to-run configuration is `mooncake_pd_config_v15_glm51.json`.

Important fields:

- `compute_model.mode = "glm51_flops"`
- `compute_model.npu_model = "Ascend 950PR 32-AI-subsystem"`
- `compute_model.npu_precision = "INT8"`
- `compute_model.npu_peak_ops_per_sec = 919000000000000`
- `compute_model.efficiency.* = 1.0` (ideal peak assumption)
- `compute_model.indexer_key_cache_resident = true`

If the peak field is omitted, the generator defaults to the public 950PR 32-core
INT8 peak of 919 TOPS. Existing configs without `compute_model` remain on the
legacy Prefill timing model.

## Output and validation

The generator writes `prefill_layer_compute.csv` with one row per Prefill batch
and Transformer layer, including FLOPs by component, local TP-group token
counts, stage times, and the integer microsecond duration passed to the DAG.

The smoke test checked:

- Python compilation succeeds.
- A 78-row layer-compute CSV is generated.
- Layers 0–2 are Dense; layers 3–77 are MoE.
- The generated DAG is accepted by the unchanged DAG-to-traffic converter.
- Decode batch compute duration remains the original 10,000 us in the test config.

The smoke test is a synthetic software validation, not a real-hardware benchmark.
