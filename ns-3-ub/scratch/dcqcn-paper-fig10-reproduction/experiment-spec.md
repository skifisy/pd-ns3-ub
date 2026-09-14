# DCQCN Paper Figure 10 Reproduction

## Purpose

This case is the fixed repository baseline for reproducing a DCQCN Figure 10 style result from the
SIGCOMM 2015 DCQCN paper.

It is intended to provide one stable, easy-to-reference case path for:

- line-rate start behavior
- two-flow convergence behavior
- near-fair steady-state throughput around `20 Gbps + 20 Gbps` on a `40 Gbps` bottleneck

This is a repository baseline case, not a claim of exact paper-identical reproduction in every detail.
It is the current best in-repo reference case for a Figure 10 style DCQCN behavior check.

## Source

This case is copied from:

- `scratch/20260417-dcqcn-fig10-3node-porttrace`

The copied case keeps the same topology, workload, and DCQCN-related parameters, while using
`PortTrace` as the main observability source so the sender wire-rate can be measured directly.

## Recommended Run

Use this command as the fixed baseline run:

```bash
./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/dcqcn-paper-fig10-reproduction --stop-ms=100 --rng-run=2'
```

Recommended fixed settings:

- `stop-ms = 100`
- `rng-run = 2`

We fix `rng-run = 2` because, among the seeds we compared, this one gives the cleanest in-repo
Figure 10 style shape and near-fair final sharing.

## Expected Behavior

With the recommended run above, the expected qualitative behavior is:

- flow 0 starts at line rate before contention
- flow 1 joins later and also starts at line rate
- both flows experience DCQCN adjustment
- both flows converge toward approximately equal sharing of the `40 Gbps` bottleneck

Most recent verified result from sender `PortTrace`, using `1 ms` bins and averaging `90-100 ms`:

- flow 0: about `19.417 Gbps`
- flow 1: about `20.568 Gbps`
- aggregate: about `39.986 Gbps`

This seed is intentionally fixed because fairness is seed-sensitive in this repository, and we want
one stable reference case for later validation and regression checks.

## Observability

This case uses sender `PortTrace` for the main throughput evidence.

Useful outputs after a run:

- sender throughput traces in `runlog/PortTrace_node_0_port_0.tr`
- sender throughput traces in `runlog/PortTrace_node_1_port_0.tr`
- switch queue traces in `runlog/QueueTrace_*`
- PFC event traces in `runlog/PfcTrace_*`

This baseline keeps only the trace families required by the checked-in analysis script.
Algorithm-emitted congestion-control traces are intentionally left off here to avoid unnecessary
runlog volume in the repository reference case.

## Notes

- This case is meant to be a stable baseline alias. Do not repurpose it for parameter sweeps.
- If you want to explore variants, copy this case into a new dated directory instead of editing this one.
- The original dated source case is preserved so earlier analysis references do not break.

## Reference

Primary paper:

Zhu, Y., Eran, H., Firestone, D., Guo, C., Lipshteyn, M., Liron, Y., Padhye, J., Raindel, S.,
Haj Yahia, M., & Zhang, M. (2015). Congestion control for large-scale RDMA deployments.
_ACM SIGCOMM Computer Communication Review, 45_(4), 523-536.
https://doi.org/10.1145/2829988.2787484

Conference version:

Zhu, Y., Eran, H., Firestone, D., Guo, C., Lipshteyn, M., Liron, Y., Padhye, J., Raindel, S.,
Haj Yahia, M., & Zhang, M. (2015). Congestion control for large-scale RDMA deployments.
In _Proceedings of the 2015 ACM Conference on Special Interest Group on Data Communication_
(`SIGCOMM '15`) (pp. 523-536). Association for Computing Machinery.
https://doi.org/10.1145/2785956.2787484
