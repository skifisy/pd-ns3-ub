---
name: openusim-run-experiment
description: Use when an approved OpenUSim experiment specification needs case generation, simulation execution, or explicit run-error handling.
---

# OpenUSim Run Experiment

## Overview

Use repo-native tools to turn an approved OpenUSim package into runnable case files and concrete runs.
For `single-case`, this means one stable `experiment-spec.md`. For `experiment-group`, this means executing the planned matrix and command manifest without inventing new cases, predictions, or output directories. Keep generated Python bounded and explicit: topology scripts must write CSVs directly into each case directory, parameter validation must happen before run, and transport-channel expectations must follow the chosen `transport_channel_mode` (default `on-demand` unless the user explicitly wants precomputed TP mappings).

## When to Use

- The experiment package is approved for generation or execution.
- The user wants to build a runnable case from the planned experiment.
- The user wants to run the case and understand explicit execution failures.
- The user wants to execute an approved OpenUSim experiment-group matrix.

Do not use this skill to define experiment intent or to perform final result interpretation.

## Input from Plan Stage

- `{planning_mode}`: `single-case` or `experiment-group`
- `{package_root}`: Full path to package root under `scratch/` (e.g., `scratch/20260322-flowcontrol-ablation/`)
- `single-case` packages contain `experiment-spec.md`
- `experiment-group` packages contain `experiment-plan.md`, `matrix.yaml`, `command-manifest.yaml`, `run-ledger.md`, and `cases/<case-id>/experiment-spec.md`

All generation and execution happen within `{package_root}/` or explicitly listed case directories.

## The Process

Completion criterion: every planned case has generated case-directory CSVs and `network_attribute.txt`, the case-file gate matches each chosen `transport_channel_mode`, and the run result or explicit failure is recorded in `experiment-spec.md` for `single-case` or `run-ledger.md` for `experiment-group`.

1. **Verify prerequisites:**
   a. **Accept `{planning_mode}` and `{package_root}` from plan stage**
   b. For `single-case`, read `{package_root}/experiment-spec.md`
   c. For `experiment-group`, read `{package_root}/experiment-plan.md`, `{package_root}/matrix.yaml`, `{package_root}/command-manifest.yaml`, `{package_root}/run-ledger.md`, and every listed per-case `experiment-spec.md`
   d. **Check completeness:** verify all required sections and package artifacts exist
   e. **If package is incomplete:** return to `openusim-plan-experiment`

2. Parse the package and extract execution parameters. For `experiment-group`, Plan owns the matrix: follow `matrix.yaml` and `command-manifest.yaml`; do not add or remove matrix rows.
3. Confirm overwrite risk, output collisions, checkpoint policy, and declared artifact contract before running any simulation.
4. **Generate topology script in each case directory:**
   a. Read topology family and parameters from spec
   b. Select code template from `../openusim-references/topology-options.md` Generation Patterns
   c. Generate `generate_topology.py` by substituting parameters and set `graph.output_dir` to that case directory
   d. Run the generated script
   e. Verify outputs: `node.csv`, `topology.csv`, `routing_table.csv`, and `transport_channel.csv` only when `transport_channel_mode = precomputed`; otherwise keep the default `on-demand` path
5. Generate `traffic.csv` through `scratch/ns-3-ub-tools/traffic_maker/build_traffic.py` when the workload is not a reference file.
6. Validate explicit overrides against the runtime parameter catalog before writing `network_attribute.txt`; stop early on unsupported keys.
7. Write a full `network_attribute.txt` snapshot through the thin query-based writer.
8. Run the light case-file gate before execution, passing the planned `transport_channel_mode`.
9. Run `./ns3` commands from the approved manifest; execute canary or checkpoint cases before later groups when declared.
10. Lightweight checks only: during run, perform only lightweight checks from `controlled-experiment-method.md`: artifact presence, metric availability, obvious prediction direction, repeated failure patterns, and output collisions.
11. Apply checkpoint policy. Stop for safety stops, `pause_for_user` triggers, or explicit `manual_approval_gate: true`.
12. Record only durable execution facts in `experiment-spec.md` or `run-ledger.md`.

## Stop And Ask

- **The experiment package does not exist or is incomplete** — return to plan stage.
- **Repo startup facts block execution** — return to welcome stage.
- The `experiment-group` package has no matrix, command manifest, artifact contract, checkpoint policy, or initialized run ledger.
- The manifest would overwrite outputs or let parallel cases write the same summary/report artifact.
- The topology family in the spec has no mapped code template in `../openusim-references/topology-options.md`, and the bound facts are still insufficient for a bounded `custom-graph`.
- The explicit parameter overrides fail runtime-catalog validation — return to plan stage with the unsupported keys.
- Existing repo tools cannot express the requested case without a new bounded decision.

## Handoff

Stay in this skill when:

- the case is still being generated
- the simulation has not completed
- the current failure is an explicit execution problem
- an `experiment-group` matrix is partially complete and checkpoint policy says to continue

Hand off to `openusim-analyze-results` when:

- the simulation completed and output artifacts exist
- the simulation failed or stalled, and there are console messages, partial outputs, or error logs to interpret
- the user wants interpretation rather than more execution retries
- an `experiment-group` run has completed, paused, or failed with enough artifacts for subset analysis

Before handoff for `single-case`, record in `experiment-spec.md`:

- the actual case path
- the exact run command or runtime switches that materially affect interpretation
- the presence of key output artifacts
- explicit execution failures that remain unresolved

Before handoff for `experiment-group`, record in `run-ledger.md`:

- per-case status, exact command, return code, timing, artifact inventory, failure category, and retryability
- checkpoint policy confirmation and checkpoint decisions
- interim observations marked as interim
- skipped, failed, paused, and completed cases

Return to `openusim-welcome` when:

- repo startup facts are missing and block execution

Return to `openusim-plan-experiment` when:

- the experiment package is incomplete or contradictory
- the user changes the experiment definition instead of retrying execution

## Integration

- Called by: `openusim-plan-experiment`
- Hands off to: `openusim-analyze-results`
- May return to: `openusim-welcome`, `openusim-plan-experiment`
- Tool entry points:
  - `scratch/ns-3-ub-tools/net_sim_builder.py`
  - `scratch/ns-3-ub-tools/traffic_maker/build_traffic.py`
- Required references:
  - `README.md`
  - `QUICK_START.md`
  - `scratch/README.md`
  - `scratch/ns-3-ub-tools/README.md`
  - `../openusim-references/spec-to-toolchain.md`
  - `../openusim-references/topology-options.md`
  - `../openusim-references/workload-options.md`
  - `../openusim-references/spec-rules.md`
  - `../openusim-references/transport-channel-modes.md`
  - `../openusim-references/controlled-experiment-method.md`

## Common Mistakes

- Copying or modifying `user_topo_*.py` scripts instead of generating new scripts from code templates.
- Forgetting to set `graph.output_dir` to the case directory, which leaves generated CSVs under a timestamped subdirectory instead of `{case_dir}/`.
- Treating `custom-graph` as unsupported just because it is not a named topology family.
- Writing unvalidated override keys into `network_attribute.txt` and discovering the mistake only at run time.
- Writing legacy override keys such as `ns3::UbQueueManager::ResumeOffset`, `ns3::UbSwitch::EnableCBFC`, `ns3::UbSwitch::EnablePFC`, or `ns3::UbApiThread::*`; use the migration table in `spec-to-toolchain.md`.
- Asking the user to precompute `transport_channel.csv` even though the default path should remain `on-demand`.
- Requiring `transport_channel.csv` even when the chosen `transport_channel_mode` is `on-demand`.
- Treating `--mtp-threads` as part of experiment intent instead of runtime execution.
- Turning the case checker into a heavy semantic validator.
- Designing a new matrix during run instead of returning to plan.
- Changing predictions after seeing outputs.
- Treating run-time mismatch notes as final causal analysis.
- Running parallel cases that write the same output artifact.
- Hiding explicit execution errors instead of surfacing them and using them to drive the next decision.
- Stating default parameter values (bandwidth, delay, etc.) from memory instead of querying the runtime catalog via `load_or_build_parameter_catalog()`. Always use the catalog; do not recite static values.
- Proceeding with generation when `experiment-spec.md` does not exist or is incomplete.
