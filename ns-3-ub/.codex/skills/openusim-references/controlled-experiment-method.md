# Controlled Experiment Method

<reference-hint>
<use-when>Use this reference when an OpenUSim task compares cases, controls variables, builds a matrix, or analyzes prediction vs actual.</use-when>
<focus>Experiment-group design, case artifact generation contracts, run ledgers, and evidence-backed analysis.</focus>
<keywords>experiment-group, control, treatment, matrix, prediction, falsification, checkpoint, run ledger</keywords>
</reference-hint>

## Contents

- Purpose
- Mode selection
- Controlled-variable design
- OpenUSim matrix shape
- Case artifact generation contract
- Checkpoint policy
- Artifact contract
- Run ledger
- Lightweight run checks
- Prediction-vs-actual analysis
- Evidence and report discipline
- Mismatch investigation
- Minimal package templates

## Purpose

Use controlled experiments when the user wants to compare alternatives, validate an effect, sweep a parameter, or decide which configuration is better.

The purpose is not to add bureaucracy to a simple smoke run. It is to prevent post-hoc reasoning when several OpenUSim cases are meant to support one claim.

## Mode selection

Use `single-case` when:

- the user wants a smoke run, startup check, or old-case reproduction
- the user is debugging one failed case
- one topology/workload/parameter combination is enough
- the answer only needs one runnable OpenUSim case

Use `experiment-group` when:

- the user says compare, A/B, baseline, treatment, sweep, sensitivity, impact, or which is better
- the user wants to test whether one variable changes throughput, latency, stalls, drops, or trace evidence
- the next decision depends on a control case and one or more treatment cases
- multiple cases must be run under one shared claim

If the user asks for a comparison but only gives one case, ask for the intended changed variable or propose the smallest defensible baseline.

## Controlled-variable design

Every experiment block must state:

- `claim`: the one-sentence question or hypothesis
- `control`: the baseline case
- `treatments`: one or more cases compared against the control
- `changed_variable`: the main variable changed in the treatment
- `fixed_controls`: topology, workload, routing, transport-channel mode, observability, runtime path, and metric definitions that must stay fixed
- `prediction`: expected direction and metric, written before running
- `reason`: why that direction is expected
- `falsification_signal`: what result would weaken or reject the prediction
- `evidence_plan`: artifacts or fields needed to decide

Change one main variable per block. If two variables must change together, label the block as an `interaction-study` and avoid single-variable conclusions.

Do not add or rewrite predictions after seeing results. Negative results remain evidence.

## OpenUSim matrix shape

Each matrix row should include:

```yaml
case_id:
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

For a `single-case` package, the matrix may contain one row whose `role` is `single`.

## Case artifact generation contract

Plan owns the matrix. Run must not invent cases, commands, predictions, or output directories.

Before handoff to run, an `experiment-group` package should contain:

```text
experiment-plan.md
matrix.yaml
command-manifest.yaml
run-ledger.md
cases/<case-id>/experiment-spec.md
```

The package root remains stable through plan, run, analysis, and any later report-like summary.

Generation scripts, if used, need a small contract:

```yaml
script_contract:
  purpose:
  entrypoint:
  inputs:
  outputs:
  idempotency:
  overwrite_policy:
  failure_behavior:
  forbidden_actions:
```

Generation scripts may create case files and summaries, but must not run simulations, rewrite predictions after results exist, or write outside the package unless the package documents that path.

## Checkpoint policy

Choose checkpoint behavior before running an experiment group:

- record the selected value as `checkpoint_policy` in `experiment-plan.md`
- `pause_for_user`: pause if early evidence contradicts the prediction, invalidates a block, or needs a matrix revision
- `continue_full_matrix`: keep running despite prediction mismatch, but record the trigger and continue decision
- `manual_approval_gate: true`: stop after a named checkpoint even if it succeeds, only when the user explicitly asked for this gate

Checkpoint success is not automatically a pause condition. Safety stops override `continue_full_matrix`.

## Artifact contract

Each case must state the artifacts expected after execution. Typical OpenUSim evidence includes:

- root case files: `node.csv`, `topology.csv`, `routing_table.csv`, `traffic.csv`, `network_attribute.txt`
- optional case files: `transport_channel.csv` when `transport_channel_mode = precomputed`
- run outputs: console log, `runlog/`, trace outputs, parser summaries, generated CSV/JSON summaries
- execution records: exact `./ns3` command, return code, runtime switches, explicit failure text

Metric and evidence labels should distinguish:

- `measured`: directly emitted by the simulator or parser
- `trace-derived`: computed from trace files
- `log-derived`: extracted from console or run logs
- `case-derived`: inferred from input CSVs or `network_attribute.txt`
- `proxy`: a substitute metric that cannot prove the full claim alone

Do not accept a metric name until its source artifact and interpretation are clear.

## Run ledger

The run ledger is the durable execution record for a package. Record:

- package root, repo root, branch, dirty status, runtime, and runner entrypoint
- per-case status: `pending`, `running`, `success`, `failed`, `skipped`, `paused_for_user`
- exact command, return code, start/end time, artifact inventory, failure category, retryability
- checkpoint policy confirmation and every user decision that changes execution
- interim observations, clearly marked as interim

For single-case runs, the same fields can live in `experiment-spec.md` under `Execution Record`.

## Lightweight run checks

During run, check only:

- artifact presence
- metric availability
- obvious direction against the pre-registered prediction
- repeated failure patterns
- output collisions or overwrite risk

Do not write final causal conclusions during run. If a mismatch matters, apply checkpoint policy and leave final interpretation to analysis.

## Prediction-vs-actual analysis

Use prediction-vs-actual as the analysis spine:

1. Recover the planned comparison: control, treatment, changed variable, fixed controls, prediction, reason, metric, and falsification signal.
2. Inventory evidence from artifacts before reading code.
3. Validate metric meaning and source labels.
4. Compare each treatment against its planned control with absolute and percent deltas when numeric.
5. Mark status as `matched`, `partially_matched`, `mismatched`, or `inconclusive`.
6. Explain with evidence and name missing evidence explicitly.

Keep failed, skipped, paused, negative, flat, and inconclusive cases visible in the result table.

## Evidence and report discipline

Even without a separate report stage, analysis should be report-ready:

- every conclusion must link to artifacts, summary tables, logs, traces, or code paths
- do not hide failed, skipped, paused, negative, or inconclusive cases
- charts, if requested, must support a conclusion and cite their data source
- do not imply real hardware measurement when evidence is simulation-derived or proxy-derived
- distinguish resource saturation from latency or completion-path attribution

## Mismatch investigation

When prediction and result disagree, test these before concluding:

- unfair or wrong control
- hidden changed variable
- stale, missing, or overwritten artifact
- failed or skipped case
- proxy metric used outside its valid scope
- simulator boundary differs from the claim
- routing, flow-control, transport-channel, or runtime semantics were misunderstood
- implementation or reporting bug

Evidence gap is a valid outcome. Mark it `inconclusive` rather than filling the gap with theory.

## Minimal package templates

Single case:

```text
scratch/<case-slug>/
  experiment-spec.md
  node.csv
  topology.csv
  routing_table.csv
  traffic.csv
  network_attribute.txt
```

Experiment group:

```text
scratch/<experiment-slug>/
  experiment-plan.md
  matrix.yaml
  command-manifest.yaml
  run-ledger.md
  cases/
    control/
      experiment-spec.md
    treatment-<changed-variable>/
      experiment-spec.md
```
