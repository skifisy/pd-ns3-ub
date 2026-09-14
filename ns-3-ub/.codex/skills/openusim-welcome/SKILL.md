---
name: openusim-welcome
description: >
  Use when the user might use this ns-3-ub repository, start OpenUSim, run a smoke case, or ask
  whether the local repo is ready.
---

# OpenUSim Welcome

## Overview

Two-phase gate: Phase 1 is a zero-cost greeting. Phase 2 checks repo readiness only after the user confirms intent.

## When to Use

**This skill is the mandatory gate for this project.** Invoke it when:

- **There is even a 1% chance the user intends to use this repository**

You MUST invoke this skill before any other openusim skill (`openusim-plan-experiment`, `openusim-run-experiment`, `openusim-analyze-results`). Only proceed to later stages after this skill completes or the user explicitly skips it.

Do not use this skill to define experiment details or interpret simulation results.

## The Process

Completion criterion: either the user is let through for a non-simulation task, or repo readiness is reported from the documented startup facts and the next stage is explicit.

### Phase 1 — Lightweight Greeting (zero side effects)

Output one line (using user's language), then wait for the user's reply. Do NOT check files, read docs, or introduce workflows.

> 你好！这里是 ns-3-ub 仿真平台。我将辅助你完成项目的设置和仿真实验设计。请问你想做什么？
> 如果你想要运行仿真，我将帮你检查仓库准备情况，确保一切就绪。
> 如果你只是想看代码或问函数，直接告诉我就好！

- If the user's intent is unrelated to simulation (e.g. asking about a function, reading code), **let them through** — do not enter Phase 2.
- If the user wants to run a simulation or experiment, proceed to Phase 2.

### Phase 2 — Repo Readiness Check (only after user confirms simulation intent)

Verify all startup facts OUTLOUD (do not narrate each check individually to the user):

Output one line (using user's language) telling the user you will check the repository readiness and it will take a few moments, then check the following facts silently:

- `./ns3` exists
- `scratch/ns-3-ub-tools/` exists
- `scratch/ns-3-ub-tools/requirements.txt` exists
- `scratch/ns-3-ub-tools/net_sim_builder.py` exists
- `scratch/ns-3-ub-tools/traffic_maker/build_traffic.py` exists
- `scratch/ns-3-ub-tools/trace_analysis/parse_trace.py` exists
- `build/` exists
- `cmake-cache/` exists
- `scratch/2nodes_single-tp` exists

Report result concisely:

- **If all facts pass:** "仓库就绪，你想做什么样的仿真？" — then hand off to `openusim-plan-experiment`.
- **If some facts are missing:** list only the missing items, then explain what actions are needed and their impact. **Wait for user approval before executing any heavy operation** (submodule pull, pip install, configure, build).

Available startup commands (only list the ones actually needed):
- `git submodule update --init --recursive` — pulls external dependencies, may download significant data
- `python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt` — installs Python packages
- `python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja` — configures only the Unified Bus dependency closure and resets previous full ns-3, examples, tests, MPI, or MTP configure choices when needed
- `BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}` — sets parallelism for one active build task
- `python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example` — compiles the UB quick-example target and its dependency closure
- `python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'` — runs the smoke test case without triggering another build

**Do NOT:**
- Check files or read docs during Phase 1
- Introduce the 4-stage workflow during Phase 1
- Enter Phase 2 unless the user has expressed simulation intent
- Create `experiment-spec.md` during welcome — that is the plan stage's job
- Narrate each file check as a separate step
- Read `README.md` or `QUICK_START.md` aloud to the user

## Stop And Ask

- Repo files contradict the documented startup flow.
- Quick Start commands fail and the error is not self-explanatory.
- The user asks for experiment design before the repo readiness question is settled.

## Handoff

Stay in this skill when:

- repo readiness is still unknown
- the user is still setting up the project

Hand off to `openusim-plan-experiment` when:

- the user wants to define a concrete experiment
- startup facts are sufficient for planning

Do not create `experiment-spec.md` during welcome. That is the plan stage's responsibility.

## Integration

- Called by: repo startup or smoke-run requests
- Hands off to: `openusim-plan-experiment`
- Required references: `README.md`, `QUICK_START.md`

## Common Mistakes

- Claiming the repo is ready without checking the documented startup facts.
- Inventing a custom bootstrap helper instead of using the repo docs directly.
- Executing heavy operations (submodule pull, pip install, build) without first explaining what will happen and getting user approval.
- **Checking files or introducing workflows during Phase 1** — Phase 1 is greeting only.
- **Entering Phase 2 when the user hasn't expressed simulation intent** — let non-simulation questions through.
- **Creating `experiment-spec.md` during welcome** — that belongs to the plan stage.
- **Narrating each file check individually** — check silently, report the summary.
