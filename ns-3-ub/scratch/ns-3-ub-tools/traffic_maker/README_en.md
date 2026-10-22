[中文](README.md) | **English** | [Tools home](../README_en.md)

# Collective traffic generation

Use `ub_tools.py traffic` from the tools root to generate `traffic.csv`. Traffic generation does not
start ns-3 or select a topology.

## Quick start

```bash
python3 ub_tools.py traffic allreduce \
  --host-count 8 \
  --group-size 8 \
  --data-size 1GB \
  --algorithm ring \
  --group-layout contiguous \
  --output-dir ./case
```

Available subcommands:

```bash
python3 ub_tools.py traffic allreduce --help
python3 ub_tools.py traffic reduce-scatter --help
python3 ub_tools.py traffic allgather --help
python3 ub_tools.py traffic alltoall --help
python3 ub_tools.py traffic alltoallv --help
python3 ub_tools.py traffic convert-simai --help
python3 ub_tools.py traffic validate --help
```

## Common options

| Option | Meaning |
| --- | --- |
| `--host-count` | Total physical nodes; optional with a placement file |
| `--group-size` | Ranks in each communication group; defaults to host count |
| `--data-size` | Complete collective data object processed by each rank |
| `--algorithm` | Schedule used for the selected collective |
| `--phase-delay` | Delay between dependent phases; defaults to `0ns` |
| `--output-dir` | Case directory that receives `traffic.csv` |

Data sizes accept `B`, `KB`, `MB`, and `GB` with binary multipliers. An existing `traffic.csv` is not
overwritten by default.

## Algorithms

| Collective | `--algorithm` |
| --- | --- |
| AllReduce | `ring`, `nhr`, `recursive-halving-doubling` |
| ReduceScatter | `ring`, `nhr`, `recursive-halving` |
| AllGather | `ring`, `nhr`, `recursive-doubling` |
| AllToAll | `pairwise` |
| AllToAllV | `pairwise` |

Algorithm selection is grounded as follows:

- `ring` is the portable bandwidth-oriented baseline for AllGather and
  ReduceScatter; NVIDIA NCCL implements ring kernels for both collectives;
- the `recursive-doubling` AllGather name models binary Recursive Exchange;
  it, recursive halving, and recursive halving-doubling use complete
  FoldIn/active-exchange/FoldOut handling for arbitrary group sizes;
- `nhr` matches HCCL's standalone AllGather and ReduceScatter NHR templates,
  including step, peer, and slice calculations for non-power-of-two rank counts;
  it is not a standard algorithm name shared by every collective library.

`--data-size` is the complete logical input size at each rank. AllGather produces
a gathered object of rank count times that size; AllToAll splits each rank's
object by destination. Every builder emits a unified `Chunk`/`Transfer`/
`CollectiveContract` schedule and it is validated before CSV lowering. See
[`algorithms/ALGORITHM_SOURCES.md`](algorithms/ALGORITHM_SOURCES.md) for the
implementation evidence.

Every CSV write validates its schedule. The same validator can be run directly
for AllGather, ReduceScatter, AllReduce, AllToAll, and AllToAllV:

```bash
python3 ub_tools.py traffic validate allgather \
  --rank-size 4 \
  --data-size 1KB \
  --algorithm ring
```

Use `--verbose` for the contract, chunks, per-phase transfers, and holdings.
Use `--output trace.json` to save it; existing files require `--overwrite`.

AllToAll `pairwise` accepts `--peers-per-phase K`. It groups consecutive atomic
phases without changing chunks, peers, transfers, or total wire bytes, and
requires `1 <= K < group-size`. `scatter` is not a current algorithm name.

## Rank placement

Use `--group-layout contiguous` or `--group-layout interleaved` for automatic placement. For explicit
placement, provide a CSV file:

```csv
groupId,rankId,nodeId
0,0,17
0,1,9
1,0,3
1,1,12
```

```bash
python3 ub_tools.py traffic allreduce \
  --data-size 1MB \
  --algorithm nhr \
  --placement-file placement.csv \
  --output-dir ./case
```

`--group-layout` and `--placement-file` are mutually exclusive.

## AllToAllV

AllToAllV supports three byte distributions:

```text
matrix   read an N x N byte matrix; --data-size is not used
uniform  distribute --data-size deterministically across destination ranks
random   distribute --data-size with a fixed --seed
```

```bash
python3 ub_tools.py traffic alltoallv \
  --host-count 4 \
  --algorithm pairwise \
  --distribution matrix \
  --traffic-matrix matrix.csv \
  --output-dir ./case
```

Local diagonal data is always satisfied by the contract. Add `--include-local`
only when CSV self tasks are required. Without it, a single-rank operation is
valid and writes no task. Matrix zero cells produce neither chunks nor network transfers.

## SimAI conversion

An existing SimAI MockNCCL flow can be converted without selecting a new collective algorithm:

```bash
python3 ub_tools.py traffic convert-simai flows.jsonl \
  --output ./case/traffic.csv \
  --rank-map rank_map.csv \
  --host-count 8
```

## Migrating existing traffic

An existing `traffic.csv` remains valid for both structured and CSV cases and does not need conversion.
Map a `build_traffic.py` command to the unified entry point as follows:

| Existing setting | Unified CLI |
| --- | --- |
| Collective type | `traffic allreduce`, `reduce-scatter`, `allgather`, `alltoall`, or `alltoallv` |
| Algorithm name | `--algorithm` |
| Rank or node count | `--host-count` and `--group-size` |
| Data amount | `--data-size`, the complete object per rank |
| Rank order | `--group-layout` or `--placement-file` |
| Output location | `--output-dir` |

Existing run logs can be parsed with `ub_tools.py trace parse`; traffic does not need to be regenerated.

## Next steps and compatibility

```bash
python3 ub_tools.py case check ./case
python3 ub_tools.py trace parse ./case --output-dir ./analysis
```

`build_traffic.py`, `all2allv_maker.py`, and `simai_flow_to_traffic.py` remain executable. Use their
original commands when their specific arguments or output directory layout is required.
