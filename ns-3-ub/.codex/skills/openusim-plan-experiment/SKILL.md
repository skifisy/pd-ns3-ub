---
name: openusim-plan-experiment
description: Use when the user wants to design, refine, generate, or run one OpenUSim experiment whose definition is still incomplete.
---

# OpenUSim Plan Experiment

## Overview

Turn a natural-language OpenUSim goal into a runnable single-case package or an experiment-group package.
Only solve one blocking planning decision per turn, but choose the planning mode early: use `single-case` for a smoke, reproduction, or one concrete simulation; use `experiment-group` for comparisons, parameter sweeps, controlled-variable studies, or "which is better" questions. Unless the user explicitly asks to precompute TP mappings, default `on-demand` `transport_channel_mode` behavior is acceptable.

## When to Use

- The user wants to design a new experiment or experiment group.
- The user wants to refine an old case into a new experiment.
- The user asks to generate or run, but the experiment definition is still incomplete.

Do not use this skill to verify repo startup or to interpret completed run results.

## The Process

Output one line (using user's language) telling the user about the planning process, then bind the user-provided facts and ask for the next input. Follow the steps below:

Completion criterion: the chosen package root exists, all required artifacts for its mode exist (`experiment-spec.md` for `single-case`; `experiment-plan.md`, `matrix.yaml`, `command-manifest.yaml`, `run-ledger.md`, and per-case specs for `experiment-group`), and the user has explicitly approved generation or execution.

1. Bind the user-provided facts before asking for more input.
2. Classify the first meaningful request as `broad`, `semi-specified`, or `reference-based`.
3. Choose `planning_mode`:
   - `single-case` for a smoke, reproduction, debug run, or one concrete topology/workload/parameter combination
   - `experiment-group` when the user asks to compare, A/B, validate impact, sweep parameters, use a baseline, or manage control/treatment cases
4. For `experiment-group`, apply `controlled-experiment-method.md` before case artifact generation:
   - define `claim`
   - choose a fair `control`
   - define treatments with one main `changed_variable` per block
   - list `fixed_controls`
   - pre-register `prediction`, `reason`, `falsification_signal`, and `evidence_plan`
   - choose `checkpoint_policy`
5. Resolve the first unstable OpenUSim case slot in this order:
   - `goal`
   - `topology_shape` — validate parameter constraints from `topology-options.md` immediately (e.g. `host_num % leaf_sw_num == 0`); reject invalid values before proceeding
   - `routing_intent`
   - `workload` — for non-builtin workloads, follow the `custom-traffic-skeleton` decomposition steps in `workload-options.md`
   - `network_overrides` — verify parameter keys exist in the runtime catalog; for enum parameters, verify values against the catalog description or C++ source (see `spec-to-toolchain.md` "Parameter value validation boundary"); warn the user if a value cannot be confirmed
   - `observability`
   - `approval_ready`
6. Ask only one smallest blocking question per turn.
7. Use bounded `1/2/3/4` choices only when they help the user decide.
8. **When all slots are resolved and user approves generation/execution:**
   a. **Determine package root:** Propose semantic package name with timestamp: `YYYYMMDD-<semantic-name>` (e.g., `20260322-clos-32hosts-bw-test`), form path as `scratch/{package_name}/`
   b. **Create package root if not exists**
   c. For `single-case`, create `{case_dir}/experiment-spec.md` from `../openusim-references/spec-rules.md`
   d. For `experiment-group`, create `{package_root}/experiment-plan.md`, `{package_root}/matrix.yaml`, `{package_root}/command-manifest.yaml`, `{package_root}/run-ledger.md`, and `cases/<case-id>/experiment-spec.md` files
   e. Verify package completeness by mode
   f. Announce handoff readiness with the package root and planning mode

## Stop And Ask

- The user wants comparison or parameter impact but the control, changed variable, or success metric is unclear.
- The experiment group would change multiple variables but the user has not accepted an `interaction-study` interpretation.
- The checkpoint policy for an experiment group is not chosen.
- The requested topology facts are still too incomplete to express as a bounded `custom-graph` or a supported family.
- The requested routing intent cannot be expressed through repo-native route generation or a bounded manual route table.
- The user asks for a parameter that is not in the current project parameter surface and cannot be confirmed from the runtime catalog or documented UB globals.
- The user asks to generate or run before the planning gate is satisfied.
- Startup facts are missing and block execution decisions.
- Repo startup facts have not been verified for this session and the user is approaching generation approval.

## Case Directory Naming

All experiment packages are created under `scratch/` with the following naming convention:

```
scratch/YYYYMMDD-<semantic-name>/
```

- `YYYYMMDD`: Date stamp (e.g., `20260322`)
- `<semantic-name>`: Short semantic description (e.g., `clos-32hosts-bw-test`, `ring-8nodes-a2a`, `flowcontrol-ablation`)

**Examples:**
- `scratch/20260322-clos-32hosts-bw-test/`
- `scratch/20260322-ring-8nodes-a2a/`
- `scratch/20260322-flowcontrol-ablation/`

For `single-case`, `experiment-spec.md` is written inside this directory: `scratch/YYYYMMDD-<name>/experiment-spec.md`.
For `experiment-group`, shared artifacts are written at the package root and per-case specs are written under `scratch/YYYYMMDD-<name>/cases/<case-id>/experiment-spec.md`.

This `{package_root}` is passed to `openusim-run-experiment` for generation and execution.

## Handoff

Stay in this skill when:

- the experiment goal is still ambiguous
- topology shape, routing intent, or workload is still unresolved
- `experiment-group` claim, control, treatment, prediction, falsification signal, evidence plan, or checkpoint policy is unresolved
- the user has not explicitly approved generation or execution

Hand off to `openusim-run-experiment` when:

- Step 8 of The Process is complete (package written and verified, `{package_root}` determined)
- The experiment goal is stable
- Topology, workload, network parameters, and observability are concrete enough to run
- For `experiment-group`, `matrix.yaml`, `command-manifest.yaml`, artifact contract, and checkpoint policy are concrete enough that run-stage does not invent cases
- The user explicitly approved generation or execution
- **Handoff data:** `{planning_mode}`, `{package_root}`, and case directories under `scratch/`

Before handoff for `single-case`, ensure `{case_dir}/experiment-spec.md` exists on disk with all required sections from `../openusim-references/spec-rules.md`. The file must contain:

- the confirmed experiment goal
- the chosen topology path or `custom-graph` realization facts
- the chosen routing intent
- the chosen workload path
- the chosen network parameter overrides
- the chosen transport channel mode (default `on-demand`, unless the user explicitly asked for precomputed TP mappings)
- the chosen observability mode
- any assumptions needed to make execution concrete

Before handoff for `experiment-group`, ensure the package root contains:

- `experiment-plan.md` with claim, simulator boundary, control, treatments, fixed controls, prediction, falsification signal, evidence plan, and checkpoint policy
- `matrix.yaml` with one row per case
- `command-manifest.yaml` with commands or command templates for each case
- `run-ledger.md` initialized with pending cases
- `cases/<case-id>/experiment-spec.md` for every matrix row

Return to `openusim-welcome` when:

- repo startup facts are missing and block execution

## Integration

- Called by: user requests for experiment design, `openusim-welcome`, `openusim-analyze-results`
- Hands off to: `openusim-run-experiment`
- May return to: `openusim-welcome`
- Required references:
  - `../openusim-references/topology-options.md`
  - `../openusim-references/workload-options.md`
  - `../openusim-references/spec-rules.md`
  - `../openusim-references/spec-to-toolchain.md`
  - `../openusim-references/controlled-experiment-method.md`

## Common Mistakes

- Ignoring already-provided facts and falling back to a generic intake.
- Asking a full questionnaire when one missing decision is enough.
- Forcing an innovative topology back into a template family when the user already gave enough facts for a bounded `custom-graph`.
- Leaving routing intent implicit and hoping run-stage defaults match the user's goal.
- Prompting the user to configure `transport_channel.csv` even though the default path should stay `on-demand` unless they explicitly want precomputed TP mappings.
- Treating runtime switches such as `--mtp-threads` as core experiment-definition slots.
- Treating a comparison as a single case and losing the control/treatment relationship.
- Letting run-stage invent `matrix.yaml`, predictions, commands, or output directories.
- Adding predictions after seeing results.
- Starting generation or backend inspection before the planning gate is satisfied.
- Accepting topology parameters without checking constraints (e.g. `host_num % leaf_sw_num == 0` for clos-spine-leaf).
- Hardcoding parameter enum values from memory instead of verifying against the runtime catalog description or C++ source. Always check the actual source before telling the user a value is valid or invalid.
- Rejecting a non-builtin workload pattern when `custom-traffic-skeleton` in `workload-options.md` provides a concrete decomposition path.
