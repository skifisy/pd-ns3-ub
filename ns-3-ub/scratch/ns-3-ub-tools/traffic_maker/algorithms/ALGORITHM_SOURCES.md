# Collective Algorithm Sources

This file records the external authorities used for the schedule shapes in this
directory. The generator is a network-only model: it does not claim GPU-kernel,
buffer-placement, protocol, or local-compute fidelity.

## Fixed revisions

- NCCL `5067397c2676d5aed50042fc39e5c8ee96eb0027`
- Open MPI `286a705537d5cf5d38e97bf2b5967d4d13312e4a`
- MPICH `8ab039f16205397f33190d34d7d298bfa65fae77`
- HCCL `32ea0cc8401738f212da7c248a0cdcab051c82c4`

## Schedule authorities

| Tool schedule | Authority and scope |
| --- | --- |
| Ring AllGather / ReduceScatter / AllReduce | NCCL ring collective device paths: [all_gather.h](https://github.com/NVIDIA/nccl/blob/5067397c2676d5aed50042fc39e5c8ee96eb0027/src/device/all_gather.h), [reduce_scatter.h](https://github.com/NVIDIA/nccl/blob/5067397c2676d5aed50042fc39e5c8ee96eb0027/src/device/reduce_scatter.h), and [all_reduce.h](https://github.com/NVIDIA/nccl/blob/5067397c2676d5aed50042fc39e5c8ee96eb0027/src/device/all_reduce.h). |
| NHR AllGather / ReduceScatter / AllReduce | HCCL NHR template formulas: [AllGather](https://gitcode.com/cann/hccl/blob/32ea0cc8401738f212da7c248a0cdcab051c82c4/src/ops/all_gather/template/aicpu/ins_temp_all_gather_nhr.cc), [ReduceScatter](https://gitcode.com/cann/hccl/blob/32ea0cc8401738f212da7c248a0cdcab051c82c4/src/ops/reduce_scatter/template/aicpu/ins_temp_reduce_scatter_nhr.cc), and [AllReduce](https://gitcode.com/cann/hccl/blob/32ea0cc8401738f212da7c248a0cdcab051c82c4/src/ops/all_reduce/template/aicpu/ins_temp_all_reduce_nhr.cc). |
| Arbitrary-rank AllGather `recursive-doubling` | MPICH recursive-exchange neighbor construction, with FoldIn/FoldOut around a power-of-two active group: [recexchalgo.c](https://github.com/pmodels/mpich/blob/8ab039f16205397f33190d34d7d298bfa65fae77/src/mpi/coll/algorithms/recexchalgo/recexchalgo.c) and [allgather_intra_recexch.c](https://github.com/pmodels/mpich/blob/8ab039f16205397f33190d34d7d298bfa65fae77/src/mpi/coll/allgather/allgather_intra_recexch.c). The CLI name is retained for compatibility; the modeled mechanism is recursive exchange, not Open MPI's p2-only selector. |
| Arbitrary-rank ReduceScatter `recursive-halving` | MPICH [reduce_scatter_intra_recursive_halving.c](https://github.com/pmodels/mpich/blob/8ab039f16205397f33190d34d7d298bfa65fae77/src/mpi/coll/reduce_scatter/reduce_scatter_intra_recursive_halving.c). |
| Arbitrary-rank AllReduce `recursive-halving-doubling` | MPICH/Rabenseifner [allreduce_intra_reduce_scatter_allgather.c](https://github.com/pmodels/mpich/blob/8ab039f16205397f33190d34d7d298bfa65fae77/src/mpi/coll/allreduce/allreduce_intra_reduce_scatter_allgather.c). |
| Classic p2 recursive-doubling comparison | Open MPI [coll_base_allgather.c](https://github.com/open-mpi/ompi/blob/286a705537d5cf5d38e97bf2b5967d4d13312e4a/ompi/mca/coll/base/coll_base_allgather.c) documents its p2-only implementation and Bruck fallback. It is a comparison boundary, not the tool's arbitrary-rank schedule authority. |

Pairwise AllToAll and matrix-defined AllToAllV are explicit tool schedules: cyclic
distance order makes every remote `src-dst` transfer appear exactly once. They do
not claim to reproduce a vendor kernel or runtime selector.

## Fidelity boundary

Before CSV lowering, the unified `Chunk`/`Transfer`/`CollectiveContract` model
checks chunk IDs, byte lengths, phase-start ownership, reduction contributor
sets, duplicate delivery, source retention, and final holdings. CSV lowering
preserves remote endpoints, bytes, phase order, and dependencies only.

It omits local reduction computation, GPU/NPU memory offsets, in-place semantics,
kernel/channel topology, runtime algorithm selection, and overlap/pipelining.
Treat comparisons as network-schedule comparisons under the chosen rank placement,
not as full device collective latency predictions.
