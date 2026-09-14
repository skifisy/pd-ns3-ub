# Workload Options

<reference-hint>
<use-when>Use this reference when the user needs a bounded workload choice for one planned OpenUSim experiment.</use-when>
<focus>Planning-time workload options and `traffic.csv` schema expectations.</focus>
<keywords>workload, traffic.csv, allreduce, p2p, schema, planning</keywords>
</reference-hint>

## Contents

- Recommended choice format
- traffic.csv Schema
- Supported planning modes
- `custom-traffic-skeleton`
- `all2allv_maker.py`
- Mapping rule
- CLI Interface

## Recommended choice format

- `1:` recommended workload A
- `2:` recommended workload B
- `3:` recommended workload C
- `4:` user free input

Only offer the three most relevant template choices.

## traffic.csv Schema

All workload paths — built-in generator, reference file, or custom skeleton — must produce a CSV conforming to this schema.

### Header

```
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
```

### Column definitions

| Column | Type | Description | Constraints |
|--------|------|-------------|-------------|
| `taskId` | int | Unique task identifier | 0-indexed, monotonically increasing |
| `sourceNode` | int | Source host node ID | Must be a valid host ID in the topology (`0 .. host_num-1`) |
| `destNode` | int | Destination host node ID | Must be a valid host ID; `sourceNode != destNode` |
| `dataSize(Byte)` | int | Data size in bytes | > 0 |
| `opType` | string | Operation type | See valid values below |
| `priority` | int | Task priority | 0 is highest (not recommended); typical value: `7` |
| `delay` | string | Task start delay with time unit | e.g. `10ns`, `0ns`; valid units: `ns`, `us`, `ms`, `s` |
| `phaseId` | int | Phase identifier | Tasks in the same phase can execute in parallel |
| `dependOnPhases` | string | Space-separated phaseId list | Empty string for first phase; tasks wait for all listed phases to complete |

### Valid `opType` values

**Source of truth:** `TaOpcodeMap` in `src/unified-bus/model/ub-app.h` and `src/unified-bus/model/ub-traffic-gen.h`. Grep for `TaOpcodeMap` to verify the current set.

Snapshot (verify against code before relying on this list):

| opType | Semantics |
|--------|-----------|
| `URMA_WRITE` | Remote write (most common, default for collective generators) |
| `URMA_READ` | Remote read (data returned from destination) |
| `URMA_WRITE_NOTIFY` | Remote write with completion notification |
| `MEM_STORE` | Memory-semantic store (maps to TA_OPCODE_WRITE via LdstInstance path) |
| `MEM_LOAD` | Memory-semantic load (maps to TA_OPCODE_READ via LdstInstance path) |

**`build_traffic.py` only generates `URMA_WRITE`.** For other opTypes, use `reference traffic.csv` or `custom-traffic-skeleton`.

### Phase dependency rules

- Tasks within the same `phaseId` may execute concurrently once their `dependOnPhases` are all satisfied.
- `dependOnPhases` contains phaseIds, not taskIds. A phase is considered complete when all tasks in that phase have finished.
- First-phase tasks should have an empty `dependOnPhases` field (empty string, not `0`).
- Cross-phase dependency example: task in phase 20 depends on phase 10 → `dependOnPhases` = `10`.
- Multiple dependencies: `dependOnPhases` = `10 20` (space-separated).

## Supported planning modes

### `ar_ring`

- Ring AllReduce.
- Good for simple collective baselines and phase-ordered bandwidth questions.

### `ar_nhr`

- NHR (Nonuniform Hierarchical Ring) AllReduce.
- Good when the user wants an AllReduce variant with a different phase structure.

### `ar_rhd`

- Recursive halving/doubling AllReduce.
- Good for users explicitly comparing collective schedules.

### `a2a_pairwise`

- Pairwise all-to-all.
- Good for direct all-to-all traffic pressure and pattern comparisons.

### `a2a_scatter`

- Scatter-style all-to-all.
- Good when the user wants a bounded all-to-all variant with `scatter_k`.

### reference `traffic.csv`

- Use when the user already has a concrete workload file and wants to anchor on that instead of a built-in template.

### `custom-traffic-skeleton`

- Use when the user can describe a workload clearly but it does not map defensibly to the built-in generators.
- Typical triggers:
  - Point-to-point or subset traffic patterns (e.g. "first half sends to second half")
  - Incast, hotspot, or permutation traffic
  - User-described collective algorithm not in the built-in set (e.g. butterfly, recursive-doubling with custom twist)
  - User-provided pseudocode or algorithmic description of communication steps
  - Mixed opType workloads (e.g. some WRITE + some READ)
  - Multi-collective composition (e.g. AllReduce then All-to-All)

#### Pre-step: application-level intent → communication primitives

The user may describe workload at different levels of abstraction. Before producing traffic.csv rows, first determine which level the description is at and decompose downward:

| User's abstraction level | Example | What to do |
|--------------------------|---------|------------|
| **Application / workflow** | "模拟一个训练 step 的通信" or "MoE 的 dispatch + combine" | Ask: this workflow contains which communication steps? In what order? With what data sizes? Decompose into a sequence of communication primitives. |
| **Communication primitive** | "AllReduce on 8 nodes, 1GB" or "all-to-all across 16 hosts" | Check if it matches a built-in algo; if yes, use `build_traffic.py`; if not, proceed to decomposition steps below. |
| **Transfer-level / pattern** | "前一半节点打后一半" or "每个节点发给右邻居" | Already at the right level for decomposition steps below. |

**The principle:** `traffic.csv` is a DAG of point-to-point transfers grouped into phases. Any workload description, no matter how high-level, must be decomposed down to this level. Decompose layer by layer and confirm each layer with the user before going deeper.

For application-level descriptions, the typical decomposition is:
1. Ask the user to list the communication steps (e.g. "先 AllReduce 梯度，再 AllToAll 做 dispatch")
2. For each step, determine: which hosts participate, what primitive (AllReduce / AllToAll / P2P / Broadcast / ...), and the data size
3. Determine the ordering: which steps depend on which (sequential? overlapping?)
4. For each step, either use a built-in algo or proceed to the decomposition steps below
5. Merge all steps into a single traffic.csv (see multi-collective composition below)

#### Decomposition steps

When the user provides a natural-language or algorithmic workload description at the transfer level, follow these steps to produce a bounded `traffic.csv`:

1. **Identify participants**: which host IDs send and receive? Map any abstract naming (e.g. "first half", "rank i", "node group A") to concrete host ID ranges.
2. **Identify phases**: does the traffic happen in one shot, or in sequential steps? Each step where the next set of tasks depends on the previous set becoming a distinct `phaseId`.
3. **For each phase, enumerate tasks**: for each (source, dest) pair, determine `dataSize`, `opType`, and `priority`. One row per task.
4. **Wire dependencies**: first phase has empty `dependOnPhases`; subsequent phases list the phaseIds they depend on.
5. **Assign taskIds**: sequential from 0.
6. **Present the skeleton to the user for confirmation** before writing to disk.

#### Example: "first half of nodes send to second half"

Given 8 hosts (IDs 0–7), "first half sends to second half" with 1MB per pair:

```csv
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
0,0,4,1048576,URMA_WRITE,7,0ns,0,
1,0,5,1048576,URMA_WRITE,7,0ns,0,
2,0,6,1048576,URMA_WRITE,7,0ns,0,
3,0,7,1048576,URMA_WRITE,7,0ns,0,
4,1,4,1048576,URMA_WRITE,7,0ns,0,
5,1,5,1048576,URMA_WRITE,7,0ns,0,
6,1,6,1048576,URMA_WRITE,7,0ns,0,
7,1,7,1048576,URMA_WRITE,7,0ns,0,
8,2,4,1048576,URMA_WRITE,7,0ns,0,
9,2,5,1048576,URMA_WRITE,7,0ns,0,
10,2,6,1048576,URMA_WRITE,7,0ns,0,
11,2,7,1048576,URMA_WRITE,7,0ns,0,
12,3,4,1048576,URMA_WRITE,7,0ns,0,
13,3,5,1048576,URMA_WRITE,7,0ns,0,
14,3,6,1048576,URMA_WRITE,7,0ns,0,
15,3,7,1048576,URMA_WRITE,7,0ns,0,
```

All tasks are in the same phase (no inter-phase dependency), so this is a single burst.

#### Example: translating a collective algorithm to traffic.csv

Given pseudocode for a recursive-doubling exchange on 4 hosts:
```
for step s = 0, 1:
    each host i exchanges data_size bytes with host i XOR 2^s
```

Translate to phased traffic (each step is a phase, bidirectional exchange = two tasks per pair):

```csv
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases
0,0,1,1048576,URMA_WRITE,7,0ns,0,
1,1,0,1048576,URMA_WRITE,7,0ns,0,
2,2,3,1048576,URMA_WRITE,7,0ns,0,
3,3,2,1048576,URMA_WRITE,7,0ns,0,
4,0,2,1048576,URMA_WRITE,7,0ns,1,0
5,2,0,1048576,URMA_WRITE,7,0ns,1,0
6,1,3,1048576,URMA_WRITE,7,0ns,1,0
7,3,1,1048576,URMA_WRITE,7,0ns,1,0
```

Phase 1 depends on phase 0 completing first.

#### Multi-collective composition

To compose multiple collectives (e.g. AllReduce followed by All-to-All):

1. Generate each collective's traffic.csv separately using `build_traffic.py` (or hand-write).
2. Merge into a single file:
   - **taskId**: renumber sequentially across the combined file (second collective continues from where first left off).
   - **phaseId**: renumber the second collective's phaseIds to not collide with the first (e.g. if first uses phases 0–5, second starts at 6).
   - **dependOnPhases**: the first task(s) of the second collective should depend on the last phaseId of the first collective, creating the sequential ordering.
3. All other columns remain unchanged.

### `all2allv_maker.py` (experimental)

`scratch/ns-3-ub-tools/traffic_maker/all2allv_maker.py` is a separate experimental tool for MoE-style non-uniform all-to-all traffic. It is **not integrated with `build_traffic.py`** and has different CLI arguments.

- Algorithms: `all2allv` (all hosts send to all), `all2allv_random` (token-routing to random experts)
- Intended for MoE dispatch patterns with shared/route experts
- Has hardcoded constraints (e.g. `assert sender < 320`) and is not production-ready
- CLI: `-bs` (batch_size), `-ts` (token_size), `-ks` (topK_shared), `-kr` (topK_route), etc.

**Guidance**: if the user needs MoE-style all-to-all-v traffic, point them to this script as a starting reference but warn that it may need adaptation. For general non-uniform all-to-all, prefer the `custom-traffic-skeleton` path.

## Mapping rule

- Prefer the repo-native `scratch/ns-3-ub-tools/traffic_maker/build_traffic.py` path for supported collectives.
- If the user asks for `incast`, `hotspot`, or another custom pattern, do not invent a generator. Map it clearly to a supported workload if the mapping is defensible, or use `custom-traffic-skeleton` to hand-write a bounded CSV.
- If the user has bounded but non-template workload facts, use `custom-traffic-skeleton` rather than forcing a misleading algorithm name.
- If the user provides pseudocode or an algorithm description, follow the `custom-traffic-skeleton` decomposition steps to translate it into a concrete `traffic.csv`.
- If the user wants to combine multiple collectives, follow the multi-collective composition rules above.

## CLI Interface

Tool: `python3 scratch/ns-3-ub-tools/traffic_maker/build_traffic.py`

### Required arguments

| Argument | Type | Description |
|----------|------|-------------|
| `-n, --host-count` | int | Total number of hosts |
| `-c, --comm-domain-size` | int | Communication domain size (hosts per domain) |
| `-b, --data-size` | str | Per-participant data volume (B/KB/MB/GB) |
| `-a, --algo` | str | Collective algorithm name |

### Optional arguments

| Argument | Type | Default | Description |
|----------|------|---------|-------------|
| `--scatter-k` | int | 1 | For `a2a_scatter`: merge every k phases |
| `-d, --phase-delay` | int | 0 | Inter-phase delay in ns |
| `-o, --output-dir` | str | `./output` | Output root directory |
| `--rank-mapping` | str | `linear` | Rank assignment: `linear` or `round-robin` |

### Hard constraints

- `host_count % comm_domain_size == 0` (raises ValueError)
- `1 <= scatter_k < comm_domain_size`
- `data_size` must parse as number + unit (B/KB/MB/GB, case-insensitive)
- `rank_mapping` must be `linear` or `round-robin`

### Planning note

`rank_mapping` and `phase_delay` affect experiment results.
Collect them in the workload slot during planning, not as runtime afterthoughts.
