---
name: openusim-analyze-results
description: Use when OpenUSim outputs, partial outputs, logs, stalls, or failures need interpretation against an experiment goal.
---

# OpenUSim Analyze Results

## Overview

Interpret completed run outputs against the original experiment goal or experiment-group claim.
Make claims only to the extent supported by output files, case inputs, run ledgers, and repo code. For experiment groups, use prediction-vs-actual as the spine and preserve failed, skipped, paused, negative, and inconclusive cases.

<HARD-GATE>
When choosing a knowledge card from `../openusim-references/`, do not read full cards first.

The routing phase must stay low-cost:
- first list candidate files
- then extract only each candidate card's `<reference-hint>...</reference-hint>` block
- only after choosing 1-2 matching cards may you open those cards in full

Use structured-block extraction during routing with this preference order:
1. prefer `rg -U -o` when available
2. fallback to `perl -0ne` for multiline extraction
3. fallback to `python3` only when the CLI extractors above are unavailable or awkward

All of these are valid only if they extract `<reference-hint>...</reference-hint>` and nothing else. For example:
- `find ../openusim-references -maxdepth 1 -name '*.md' | sort`
- `python3 - <<'PY'`
- `import pathlib, re; text = pathlib.Path('../openusim-references/throughput-evidence.md').read_text(encoding='utf-8'); print(re.search(r'<reference-hint>.*?</reference-hint>', text, re.S).group(0))`
- `PY`
- `perl -0ne 'print $1 if /(<reference-hint>.*?<\/reference-hint>)/s' ../openusim-references/throughput-evidence.md`
- `rg -U -o '<reference-hint>[\\s\\S]*?</reference-hint>' ../openusim-references/throughput-evidence.md`

Do not use line counts or `cat` whole cards just to decide what to read.
</HARD-GATE>

## When to Use

- The simulation has completed and outputs already exist.
- The simulation failed or stalled, and there are partial outputs, error logs, or console messages to interpret.
- The user wants to know whether the results match the intended experiment goal.
- The user wants help investigating likely causes for an unexpected outcome.
- The user wants to compare control and treatment cases, parameter sweeps, or an experiment-group matrix.

Do not use this skill to initialize the repo or to define a new experiment from scratch.
Do not use this skill for pre-run readiness, case-file gating, or execution-path validation.

## The Process

Completion criterion: the answer separates supported claims, unsupported claims, and remaining uncertainty; for experiment groups, every matrix row is classified as `matched`, `partially_matched`, `mismatched`, or `inconclusive`; any next iteration is reduced to one bounded planning decision.

1. Identify the planning mode:
   - `single-case`: read the current `experiment-spec.md`
   - `experiment-group`: read `experiment-plan.md`, `matrix.yaml`, `run-ledger.md`, and per-case `experiment-spec.md`
2. Inspect the relevant case inputs, output artifacts, and run ledger entries.
2b. If a run failed or stalled, collect failure evidence:
    - Console messages (`buffer full`, `Potential Deadlock`, `No task completed`, etc.)
    - Partial trace files in `runlog/`
    - Compare the failing case's `network_attribute.txt` against a known-good baseline case
    - Identify the minimal parameter difference that could explain the failure
3. For `experiment-group`, recover each planned comparison from `controlled-experiment-method.md`: control, treatment, changed variable, fixed controls, prediction, reason, metric, and falsification signal.
4. Before answering specialized domain questions, list candidate cards under `../openusim-references/`.
5. Extract only each candidate card's `<reference-hint>...</reference-hint>` block with regex.
6. Choose the matching knowledge card(s) from those `<reference-hint>` blocks, then open only the relevant cards in full.
7. If no single card is obvious yet, start from the cards whose `<use-when>` or `<keywords>` mention the current symptom most directly, such as throughput interpretation, trace/debug semantics, queue/backpressure behavior, transport-channel mode, stuck-simulation debugging, or controlled experiment method.
8. Validate metric meaning and evidence source labels before making conclusions. Label proxy, trace-derived, log-derived, measured, and case-derived evidence.
9. Compare the observed evidence against the original experiment goal or each planned prediction.
10. For experiment groups, mark each treatment as `matched`, `partially_matched`, `mismatched`, or `inconclusive` and keep failed/skipped/paused rows visible.
11. State what the evidence supports, what it does not support, and what remains uncertain.
12. If another iteration is needed, hand the next bounded decision back to planning.
13. If the analysis has produced a verified root cause, reusable simulation insight, or stable interpretation rule, ask the user whether they want to preserve it as a knowledge card before handing off to `openusim-capture-insights`.

## Stop And Ask

- The necessary output artifacts do not exist.
- The requested conclusion depends on evidence the repo does not currently produce.
- The experiment-group matrix or run ledger is missing and the control/treatment design cannot be reconstructed.
- The user is actually changing the experiment definition rather than asking for result interpretation.

## Handoff

Stay in this skill when:

- the user is still asking for interpretation of the current run
- the next question is still about result evidence or likely causes

Hand off to `openusim-plan-experiment` when:

- the user wants to change the experiment for another iteration
- the current analysis identifies a new bounded decision for the next run

Hand off to `openusim-capture-insights` when:

- the analysis has already produced a stable reusable conclusion
- the conclusion is broader than the current case
- the user explicitly agrees that it should be captured as a knowledge card

Before handoff, record in `experiment-spec.md`:

- the most important findings that affect the next iteration
- the specific result gaps or hypotheses that motivated the next change

For experiment groups, also record or update the analysis notes at package level with:

- comparison status for each matrix row
- evidence source labels
- mismatch investigations
- limitations and missing artifacts
- the next bounded planning decision

Before handing off to `openusim-capture-insights`, pass along:

- the conclusion summary
- the strongest supporting evidence
- whether this looks like a new card or an update to an existing one
- any candidate existing card discovered through `<reference-hint>` routing

## Integration

- Called by: `openusim-run-experiment`, direct requests about existing outputs
- Hands off to: `openusim-plan-experiment`, `openusim-capture-insights`
- Knowledge card directory: `../openusim-references/`
- Required method reference for experiment groups: `../openusim-references/controlled-experiment-method.md`
- Knowledge card selection rule:
  - first list candidate cards in the directory
  - then extract only the `<reference-hint>...</reference-hint>` block from each candidate card
  - prefer `rg -U -o` first
  - fallback to `perl -0ne` second
  - fallback to `python3` regex third
  - Python regex is the fallback extractor, not the only allowed implementation
  - route on `<use-when>`, `<focus>`, and `<keywords>` only
  - then open only the card(s) whose `<reference-hint>` matches the user's current question
  - do not hardcode a fixed card list as the only valid source, because new cards may be added over time
  - do not rely on line-number heuristics during routing
  - do not `cat` every card in the directory just to choose
  - every card should expose one `<reference-hint>...</reference-hint>` block near the top
  - each block should contain `<use-when>`, `<focus>`, and `<keywords>`
  - keep `<use-when>` as one short routing sentence, ideally within about 160 characters

## Failure Interpretation Checklist

When the run failed or produced unexpected results, check in this order:

1. **Is it a simulation result or a toolchain bug?**
   - `buffer full. Packet Dropped!`, `Potential Deadlock`, `No task completed` → simulation result, not a bug
   - Segfault, Python traceback, missing file → toolchain bug, return to run-experiment

2. **Compare against baseline:**
   - Find the closest known-good case (e.g., `scratch/2nodes_single-tp`)
   - Diff `network_attribute.txt` to identify parameter differences
   - Focus on: `FlowControl`, buffer sizes, `EnableRetrans`, routing mode

3. **Form a single-variable hypothesis:**
   - Change exactly one parameter from the failing case toward the baseline value
   - Re-run and compare

4. **Record durable findings in `experiment-spec.md`:**
   - The failure symptom and its code-level origin
   - The parameter change that resolved it (or didn't)
   - The root cause chain (e.g., "synchronous burst + no backpressure + finite ingress buffer + no retransmission")

## Experiment Group Checklist

When analyzing an experiment group:

1. Confirm the authoritative `experiment-plan.md`, `matrix.yaml`, and `run-ledger.md`.
2. Reconstruct the planned control/treatment comparison before reading final results.
3. Check fixed controls for hidden differences: topology, workload, routing, transport-channel mode, observability, runtime path, and metric definition.
4. Compare actual evidence to the pre-registered prediction and falsification signal.
5. Classify each row as `matched`, `partially_matched`, `mismatched`, or `inconclusive`.
6. Investigate mismatches for unfair control, hidden changed variable, stale artifact, failed case, proxy metric misuse, simulator boundary mismatch, runtime semantics, or reporting bug.
7. Keep negative, flat, failed, skipped, paused, and inconclusive outcomes visible.

## Common Mistakes

- Claiming line-rate conclusions without stating both the metric and the comparison target.
- Confusing runtime overhead from trace settings with simulated network semantics.
- Treating missing evidence as proof.
- Explaining an experiment group without reconstructing the planned control/treatment matrix.
- Hiding negative, failed, skipped, paused, or inconclusive cases.
- Treating resource saturation as latency attribution without artifact evidence.
- Changing the experiment definition inside the analysis phase instead of handing back to planning.
