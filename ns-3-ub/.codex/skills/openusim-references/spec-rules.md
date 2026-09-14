# Spec Rules

<reference-hint>
<use-when>Use this reference when writing or updating OpenUSim experiment package artifacts.</use-when>
<focus>Shared package artifacts, required slots, and handoff-safe spec structure.</focus>
<keywords>experiment-spec.md, experiment-plan.md, matrix.yaml, command-manifest.yaml, run-ledger.md, planning</keywords>
</reference-hint>

## Contents

- Core rule
- Natural-language intent resolution
- Write-back timing
- Planning modes
- Single-case minimal template
- Experiment-group minimal template
- Planning inputs
- Old case rule
- Parameter naming rule
- Parameter value validation rule
- Readiness rule

## Core rule

- OpenUSim stage skills share package artifacts, not hidden chat memory.
- `experiment-spec.md` remains the per-case source of truth.
- `experiment-group` packages add `experiment-plan.md`, `matrix.yaml`, `command-manifest.yaml`, and `run-ledger.md` at the package root.
- These files describe the current experiment package, not a chat log.
- They must contain enough facts for the next stage skill to continue without hidden session memory.

## Natural-language intent resolution — cross-slot principle

Users may describe any planning slot in natural language rather than concrete values. This applies equally to topology, workload, network parameters, observability, and routing. The same resolution principle governs all slots:

1. **Decompose to the slot's primitive elements.** Every slot has a small set of irreducible building blocks:

| Slot | Primitive elements |
|------|--------------------|
| Topology | node groups + connectivity rules + bandwidth constraints |
| Workload | communication primitives (AllReduce, P2P, ...) + phase ordering + data sizes → traffic.csv rows |
| Network overrides | ns3 attribute key + value |
| Observability | named tier or individual trace-switch settings |
| Routing intent | algorithm choice + path-source choice |

2. **Search the matching reference for concrete options.** Each slot has a reference doc (`topology-options.md`, `workload-options.md`, `spec-to-toolchain.md`, `trace-observability.md`). If the user's intent matches an option in the reference, present it for confirmation.

3. **If multiple mappings are possible, present the choices.** Do not pick one silently.

4. **If the intent cannot be mapped or is ambiguous, ask the user.** Never silently assume a concrete value from vague natural language.

5. **Every mapping from natural language to a concrete value requires user confirmation** before writing into `experiment-spec.md`.

## Write-back timing

Write back only when:

- a planning slot gained stable new information
- the user confirmed that slot
- a stage handoff needs durable facts

Do not rewrite the whole spec on every turn.

## Planning modes

Use `single-case` for one runnable OpenUSim case.

Use `experiment-group` when one claim spans a control/treatment matrix, parameter sweep, A/B comparison, sensitivity study, or checkpointed batch run.

Do not encode a multi-case comparison only as prose in one `experiment-spec.md`; preserve the matrix and run ledger at package level.

## Single-case minimal template

Use a small stable per-case structure so every stage skill can find the same facts quickly:

```md
# Experiment Spec

## Planning Mode
- `single-case`

## Goal
- what the user wants to learn

## Topology
- chosen topology family
- concrete sizing facts
- old case reference, if any

## Topology Realization
- `supported-family` or `custom-graph`
- bounded node/link facts if the topology is custom
- case artifact generation notes needed by run stage

## Workload
- chosen workload family or reference traffic file
- concrete scale facts
- rank_mapping (optional, default: linear)
- phase_delay (optional, default: 0)

## Routing Intent
- routing_algorithm (`HASH` or `ADAPTIVE`)
- whether only shortest-path candidates are allowed
- whether route generation is auto-path-finder or manual-route-table
- any bounded routing constraints that must survive handoff

## Network Overrides
- resolved parameter overrides that matter for the run

## Transport Channel Mode
- `precomputed` or `on-demand`
- default: `on-demand`

## Observability
- chosen trace/debug posture

## Startup Readiness
- startup facts that constrain generation or execution

## Execution Record
- actual case path
- actual run command
- return code and status
- produced output artifacts
- unresolved explicit run errors

## Validation Notes
- unsupported configuration asks that were rejected before run
- assumptions or fallback rules used when runtime discovery was incomplete

## Analysis Notes
- result findings that matter for the next iteration
- hypotheses to test next
```

Use empty sections only when the next stage clearly needs that slot.
Do not turn the spec into a transcript or a turn-by-turn checklist.

## Experiment-group minimal template

Use this package shape:

```text
scratch/<experiment-slug>/
  experiment-plan.md
  matrix.yaml
  command-manifest.yaml
  run-ledger.md
  cases/
    <case-id>/
      experiment-spec.md
```

`experiment-plan.md` must include:

```md
# Experiment Plan

## Planning Mode
- `experiment-group`

## Claim
- one-sentence question or hypothesis

## Simulator Boundary
- what OpenUSim can and cannot prove for this claim

## Control And Treatments
- control case id and why it is fair
- treatment case ids
- changed_variable for each block
- fixed_controls that must remain unchanged

## Prediction And Falsification
- prediction
- reason
- falsification_signal
- evidence_plan

## Checkpoint Policy
- checkpoint_policy
- `pause_for_user` or `continue_full_matrix`
- any `manual_approval_gate: true` checkpoint

## Artifact Contract
- required case inputs
- required run outputs
- metric names, source labels, and proxy boundaries

## Analysis Notes
- prediction-vs-actual summary
- mismatch investigations
- limitations and next bounded planning decision
```

`matrix.yaml` must include one row per case:

```yaml
- case_id:
  block_id:
  role: control | treatment
  case_dir:
  changed_variable:
  fixed_controls:
  prediction:
  reason:
  falsification_signal:
  metric_checks:
  expected_artifacts:
  parallel_group:
  checkpoint_ids:
```

`command-manifest.yaml` must include:

```yaml
commands:
  - case_id:
    phase: generate | run | summarize
    command:
    cwd:
    output_dir:
    expected_artifacts:
```

`run-ledger.md` must preserve:

```md
# Run Ledger

## Environment
- repo root
- branch and dirty status
- runtime and runner entrypoint

## Checkpoint Decisions
- checkpoint policy confirmation
- user decisions

## Case Status
- case_id
- status: pending | running | success | failed | skipped | paused_for_user
- command
- return code
- timing
- artifact inventory
- failure category
- retryability
- interim observations
```

## Planning inputs

The planning surface must leave these durable facts before run handoff:

- planning mode: `single-case` or `experiment-group`
- experiment goal
- topology choice
- topology realization mode
- routing intent
- workload choice
- network parameter overrides
- transport channel mode (default to `on-demand` unless the user explicitly requests precomputed TP mappings)
- observability choice
- for `experiment-group`: claim, control, treatments, changed variable, fixed controls, prediction, reason, falsification signal, evidence plan, artifact contract, and checkpoint policy
- explicit approval to generate or run

## Old case rule

If an old case is used as reference:

- summarize it in the conversation first
- do not write it into the new spec until the user says what to keep and what to change

## Parameter naming rule

Use toolchain-native parameter names in the spec to avoid translation ambiguity:
- `host_num`, `leaf_sw_num`, `comm_domain_size`, `data_size`
- See `spec-to-toolchain.md` for the full mapping

## Parameter value validation rule

The skill-layer toolchain validates parameter **keys** against the runtime catalog but does not validate parameter **values**. To reduce the risk of invalid values reaching ns-3:

- For enum parameters (e.g. `FlowControl`, `RoutingAlgorithm`, `VlScheduler`), verify the value against the catalog entry's `description` field or the C++ source `MakeEnumChecker(...)`. See `spec-to-toolchain.md` "Parameter value validation boundary" for the source-of-truth table.
- For `traffic.csv` `opType` values, verify against `TaOpcodeMap` in `src/unified-bus/model/ub-app.h`.
- Do not hardcode a fixed list of valid values in the spec or in agent logic — always consult the code or catalog as the authoritative source.

## Readiness rule

`ready for run` means:

- planning mode is explicit
- topology is concrete enough to generate with repo-native tools
- topology realization mode is explicit enough to produce case-directory CSVs
- routing intent is explicit enough to choose auto or manual route generation
- workload is concrete enough to generate with repo-native tools
- main parameter choices are concrete enough for `network_attribute.txt`
- transport channel mode is chosen explicitly or defaults to `on-demand`
- observability mode is chosen
- for `experiment-group`, `matrix.yaml`, `command-manifest.yaml`, and `run-ledger.md` exist and the matrix rows contain predictions and expected artifacts
- explicit run approval has been given
