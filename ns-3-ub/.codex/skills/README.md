# OpenUSim Skills

This directory contains repo-local Claude Code skills for the ns-3-ub project.

## Skill Overview

| Skill | Purpose | Entry Point |
|-------|---------|-------------|
| `openusim-welcome` | Mandatory first gate for repo initialization and readiness check | Always invoke first |
| `openusim-plan-experiment` | Single-case or experiment-group planning, custom topology clarification, routing-intent capture, and controlled-variable design | After welcome |
| `openusim-run-experiment` | Case artifact generation, validation, command-manifest execution, run-ledger updates, and explicit run errors | After plan |
| `openusim-analyze-results` | Result interpretation, prediction-vs-actual comparison, and likely-cause analysis | After run |
| `openusim-capture-insights` | User-approved knowledge-card capture for verified root causes and reusable insights | Optional after analyze |

## Usage Flow

```
openusim-welcome (mandatory gate)
    ↓
openusim-plan-experiment (define experiment)
    ↓
openusim-run-experiment (generate & run)
    ↓
openusim-analyze-results (interpret results)
    ↓
openusim-capture-insights (optional, preserve reusable insight)
```

## Planning Modes

`openusim-plan-experiment` chooses the planning mode before generation:

- `single-case` for smoke runs, old-case reproduction, failed-case debugging, or one concrete topology/workload/parameter combination
- `experiment-group` for A/B comparisons, parameter sweeps, controlled-variable studies, and "which configuration is better" questions

`single-case` uses one case directory with `experiment-spec.md`.

`experiment-group` uses a package root with:

- `experiment-plan.md`
- `matrix.yaml`
- `command-manifest.yaml`
- `run-ledger.md`
- `cases/<case-id>/experiment-spec.md`

In experiment-group mode, Plan owns the matrix. Run executes only the planned matrix and command manifest; analysis uses prediction-vs-actual and keeps failed, skipped, paused, negative, and inconclusive rows visible.

## References

Shared knowledge cards in `openusim-references/`:

- `controlled-experiment-method.md` - Controlled-variable design, experiment-group package contract, checkpoint policy, run ledger, and prediction-vs-actual analysis
- `trace-observability.md` - Trace/debug semantics
- `transport-channel-modes.md` - Transport channel semantics
- `throughput-evidence.md` - Throughput and line-rate interpretation
- `spec-to-toolchain.md` - Spec-to-toolchain mapping
- `topology-options.md` - Supported topology families and bounded `custom-graph` flow
- `workload-options.md` - Workload modes
- `spec-rules.md` - Experiment spec format rules
- `queue-backpressure-vs-topology.md` - Queue backpressure concepts

Each knowledge card exposes a structured `<reference-hint>` block near the top so skills can route by `<use-when>`, `<focus>`, and `<keywords>` before reading the full card.

## Skill Discovery

These skills can be discovered via:
- `.codex/skills/` (canonical location)
- `.claude/skills/` (symlinks)

Each skill's `SKILL.md` defines its purpose and usage conditions.
