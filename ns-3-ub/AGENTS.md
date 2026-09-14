# Repo Instructions / 仓库指南

This repository standardizes on four repo-local OpenUSim stage skills:

- `openusim-welcome`
- `openusim-plan-experiment`
- `openusim-run-experiment`
- `openusim-analyze-results`

All four ship inside this repository under `.codex/skills/` and are maintained with the main repo.

The repository also contains one optional post-analysis companion skill:

- `openusim-capture-insights`

It is not a fifth stage in the normal experiment flow. Use it only after analysis has already produced a stable reusable conclusion and the user agrees to preserve it as a knowledge card.

## Skill Routing

When the user wants help with Unified Bus / OpenUSim work in this repository, route by stage:

- use `openusim-welcome` for repo initialization, readiness, and bounded Quick Start smoke runs
- use `openusim-plan-experiment` for single-case or experiment-group definition and clarification
- use `openusim-run-experiment` for case artifact generation, command-manifest execution, run-ledger updates, and explicit run errors
- use `openusim-analyze-results` for result interpretation, prediction-vs-actual comparison, and likely-cause analysis

Do not route the user into legacy `openusim-*` skills unless the user explicitly asks to inspect or modify that legacy multi-skill system.

## User-Facing Reply Surface

The user-visible reply is not a policy summary.

Keep internal control information internal:

- do not echo skill names, routing decisions, or compliance statements to the user
- do not restate repository policy, prompt policy, or skill policy unless the user explicitly asks how the system works
- do not fall back to meta wording such as "I will follow this skill" or "I will process this with the repo rules"

The user-facing reply should contain only:

- the experiment facts already bound from the user's message
- one short statement of the current experiment step
- one smallest blocking question for the next decision
- bounded `1/2/3/4` options only when they help the user decide

Keep this structure implicit rather than labeled.

- avoid visible control labels such as `已知事实`, `当前步骤`, `下一步`, or `Decision` in normal chat replies
- prefer natural connective phrasing such as "你这个目标可以先按…来收口", "先把…定下来", "接下来只差…"
- only switch to explicit headings when the user asks for a structured summary

When confirming all slots before generation, use implicit wording rather than a labeled checklist. For example:

- Good: "拓扑、负载和参数都定下来了，可以生成了吗？"
- Good: "目标、拓扑、workload 都齐了，确认一下就可以跑。"
- Bad: "**goal**: 验证吞吐量 / **topology**: clos-spine-leaf / **workload**: ar_ring / ..."

## First Reply Rule

For a broad first-turn request such as "我想做一次 openusim 仿真":

- stay in planning mode
- explain that the workflow will first define the experiment goal, topology, workload, and key parameters
- ask one question about what the user wants to learn from the simulation

The first reply must not:

- start running commands
- inspect backend scripts or code paths
- create directories or generate files
- present internal workflow labels such as `existing_case`, `prepared_case`, or `goal-to-experiment`

Use natural user-facing wording instead, for example:

- old case reference
- new experiment case
- current experiment description

## First-Turn Classification

Classify the user's first meaningful OpenUSim request into one of these shapes:

- `broad`: the user only says they want to run or design a simulation
- `semi-specified`: the user already gives some combination of goal, topology, workload, or key parameter intent
- `reference-based`: the user gives an old case path, `traffic.csv`, or another bounded reference artifact
- `experiment-group`: the user asks for A/B comparison, baseline vs treatment, parameter sweep, controlled-variable impact, sensitivity study, or "which configuration is better"

Handling rules:

- for `broad`, ask what the user wants to learn from the simulation
- for `semi-specified`, bind the user-provided facts first, then ask only the most blocking unresolved decision
- for `reference-based`, summarize the known reference facts first, then ask what to keep or change
- for `experiment-group`, bind the comparison intent first, then ask for the smallest missing scientific decision: claim, control, changed variable, metric, or checkpoint policy
- if the user says `2-layer Clos`, `leaf-spine`, or `spine-leaf`, bind that directly as `clos-spine-leaf`
- only ask `clos-spine-leaf` vs `clos-fat-tree` when the user explicitly says `fat-tree`, gives `k`, or the wording is genuinely ambiguous

Do not ignore already-provided facts by falling back to a generic intake template.

## Planning Modes

OpenUSim planning has two planning modes:

- `single-case`: one runnable OpenUSim case, backed by one `experiment-spec.md`
- `experiment-group`: one controlled experiment package, backed by `experiment-plan.md`, `matrix.yaml`, `command-manifest.yaml`, `run-ledger.md`, and `cases/<case-id>/experiment-spec.md`

Use `single-case` for smoke runs, old-case reproduction, failed-case debugging, or one concrete topology/workload/parameter combination.

Use `experiment-group` for A/B comparisons, baseline/treatment studies, parameter sweeps, controlled-variable studies, and "which configuration is better" questions.

For `experiment-group`:

- plan must define claim, control, treatments, changed variable, fixed controls, prediction, falsification signal, evidence plan, artifact contract, and checkpoint policy before run handoff
- run must execute only the planned matrix and command manifest; it must not invent cases, predictions, commands, or output directories
- analysis must use prediction-vs-actual and keep failed, skipped, paused, negative, and inconclusive cases visible

## Question Granularity

Ask only the smallest blocking question.

This means:

- do not ask for a full form when one missing decision is enough to continue
- do not reopen already stable choices
- if topology is already clear, ask about workload or scale instead of restating the whole intake
- if a reference case is already given, ask about the intended delta instead of asking from scratch
- keep the wording conversational rather than sounding like a checklist

## Generation Gate

Only generate or run a case after:

- the goal is stable enough to summarize
- the user has confirmed the main topology/workload/parameter choices
- the user gives explicit approval for generation or execution

For `experiment-group`, only generate or run after:

- the claim and control/treatment relationship are stable
- the main changed variable and fixed controls are explicit
- prediction, falsification signal, evidence plan, artifact contract, and checkpoint policy are recorded

If the user asks to generate or run before this gate is satisfied:

- do not start commands
- do not inspect backend scripts or code paths just because the user said `generate`
- do not create directories
- do not generate files
- reply with the current bound facts and the one smallest remaining blocking question

## Project Startup Gate

Before claiming that the repo can generate cases, use `ns-3-ub-tools`, or run a simulation, establish startup facts from:

- `README.md`
- `QUICK_START.md`

Do this by reading the docs and operating on the repo directly. Do not depend on a separate bootstrap helper script for this gate.

Check at least these repo facts:

- `./ns3` launcher exists
- `scratch/ns-3-ub-tools/` exists
- `scratch/ns-3-ub-tools/requirements.txt` exists
- critical tool scripts exist:
  - `scratch/ns-3-ub-tools/net_sim_builder.py`
  - `scratch/ns-3-ub-tools/traffic_maker/build_traffic.py`
  - `scratch/ns-3-ub-tools/trace_analysis/parse_trace.py`
- build artifacts such as `build/` and `cmake-cache/` exist before claiming the repo is built
- the default smoke case path `scratch/2nodes_single-tp` exists before proposing the Quick Start smoke run

Do not pretend startup is complete if these facts are missing.

When the user clearly wants to start or run the project, help execute the bounded Quick Start flow instead of only describing it:

- `git submodule update --init --recursive`
- `python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt`
- `python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja`
- `BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}`
- `python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example`
- `python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'`

When the user needs current Unified Bus parameter/default guidance, initialize a runtime parameter catalog and reuse it instead of reciting a fixed table:

- this runtime catalog is backed by `python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp --PrintTypeIds'`
- it queries per-component attributes via `--ClassName=...`
- it queries Unified Bus globals via `--PrintUbGlobals`
- treat the generated project parameter catalog as the current baseline until it is refreshed
- generate `network_attribute.txt` as a full resolved snapshot from that catalog, not as a patch inherited from a sample case
- do not rely on a hand-written static Unified Bus parameter table

## Domain Knowledge Routing

When the discussion turns to a specialized OpenUSim domain topic that is easy to misstate from generic intuition, read the matching repo-local knowledge card before answering.

Current required card:

- for `trace/debug` semantics and recommendations: `.codex/skills/openusim-references/trace-observability.md`
- for transport-channel semantics: `.codex/skills/openusim-references/transport-channel-modes.md`
- for throughput evidence and line-rate interpretation: `.codex/skills/openusim-references/throughput-evidence.md`
- for spec-to-toolchain mapping: `.codex/skills/openusim-references/spec-to-toolchain.md`
- for queue backpressure vs topology capacity: `.codex/skills/openusim-references/queue-backpressure-vs-topology.md`
- for controlled experiment groups, package contracts, run ledgers, and prediction-vs-actual analysis: `.codex/skills/openusim-references/controlled-experiment-method.md`

## Skill Reference Maintenance

When tool scripts under `scratch/ns-3-ub-tools/` change their interface (parameters, CLI arguments, constraints, output schema), the corresponding reference files under `.codex/skills/openusim-references/` must be updated in the same commit.

Affected mappings:

- `net_sim_builder.py` API → `topology-options.md`, `spec-to-toolchain.md`
- `build_traffic.py` CLI → `workload-options.md`, `spec-to-toolchain.md`
- `trace_analysis/*.py` → `throughput-evidence.md`, `spec-to-toolchain.md`
- experiment package artifact schema or controlled-variable workflow → `spec-rules.md`, `controlled-experiment-method.md`, `test_skill_docs.py`
- `src/unified-bus/model/ub-utils.h` trace GlobalValue definition → `network_attribute_writer.py` `_OBSERVABILITY_PRESETS`, `trace-observability.md`
- `src/unified-bus/model/ub-utils.h` + `src/unified-bus/model/ub-datatype.cc` + `src/unified-bus/model/protocol/ub-congestion-control.cc` UB GlobalValue definitions → `network_attribute_writer.py` `_FALLBACK_UB_GLOBAL_KEYS`
- `src/unified-bus/model/ub-app.h` + `src/unified-bus/model/ub-traffic-gen.h` `TaOpcodeMap` → `workload-options.md` valid opType table
- `src/unified-bus/model/ub-switch.cc` `MakeEnumChecker` (FlowControl, VlScheduler) → `spec-to-toolchain.md` parameter value source-of-truth table
- `src/unified-bus/model/protocol/ub-routing-process.cc` `MakeEnumChecker` (RoutingAlgorithm) → `spec-to-toolchain.md` parameter value source-of-truth table
- `SKILL.md` Required references changes → `test_skill_docs.py` assertions

---

# 项目结构与模块组织

```
ns-3-ub/
├── src/unified-bus/          # UB 协议栈实现（核心开发区）
│   ├── model/                # 所有 C++ 模型代码
│   │   └── protocol/         # 协议层（function/transaction/transport/routing/flow-control/caqm/header）
│   ├── test/ub-test.cc       # 单测文件
│   ├── examples/             # ub-quick-example 用户后端与相关示例
│   └── doc/                  # switch-buffer-architecture.md
├── scratch/                  # 仿真场景入口与 CSV case 数据（15 个 case 目录）
│   ├── ub-quick-example.cc   # 对 example/ub-quick-example 的薄封装
│   └── <case>/               # 各场景: *_single-tp, clos_*, 2dfm4x4_*, ...（含 CSV 配置）
├── dev-knowledge/            # 知识基线（规范文档、经验沉淀）
│   └── specification/        # UB Base Specification 2.0 PDF + key-index
├── src/mtp/                  # Unison 多线程仿真模块（非标准 ns-3）
├── utils/                    # 辅助脚本（run-examples, create-module.py 等）
├── scratch/ns-3-ub-tools/    # 子模块: 拓扑/流量生成 + trace 解析
└── build/                    # 构建产物（忽略）
```

## 代码风格与命名约定

遵循 `.editorconfig` 与 `.clang-format` (BasedOnStyle=Microsoft, C++20, ColumnLimit=100)。
- 缩进: `*.cc`/`*.h`/`*.py` 使用 4 空格；CMake/YAML/Markdown 使用 2 空格；`Makefile` 使用 tab。
- UB 文件命名: `ub-*.cc/.h`；协议层代码置于 `model/protocol/`。
- 类命名: PascalCase + Ub 前缀（`UbPort`, `UbTransaction`）。
- 场景命名: 延续 `2nodes_*`、`clos_*`、`2dfm4x4_*`。
- 指针对齐: `Type* ptr`（PointerAlignment=Left）。

## 构建与验证

### 首次配置
```bash
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

### 快速验证（推荐）
开发时先按上面命令构建 `ub-quick-example`，后续场景验证直接复用已构建产物：
```bash
# 运行场景（复用已构建产物）
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'
```

### 模块级构建（可选）
如需单独构建模块而不运行场景：
```bash
ninja -C cmake-cache unified-bus unified-bus-test
```

### 单元测试
```bash
# 构建 test-runner（首次或测试代码修改后）
ninja -C cmake-cache test-runner

# 运行指定模块的测试（test-name 为运行时过滤）
./build/utils/ns3.44-test-runner-default --test-name=unified-bus
```

### 全量构建与测试（必要时）

**构建说明**：
- `python3.12 ./ns3 build`（无参数）= 构建所有模块
- `python3.12 ./ns3 run '<program>'` = 按需构建所需目标及其依赖，然后执行
- `python3.12 ./ns3 run --no-build` = 不触发构建，直接运行已编译的程序
- UB 仿真开发默认使用 `python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example`，避免全量构建；单个 build 可以用 `-jN` 加速，但不要同时启动多个 build/test 构建任务。

**仅以下情况需要全量构建**：
- 修改了 `src/core/` 等基础模块
- 修改了 CMake 配置或头文件接口
- 准备提交前最终验证
- CI/CD 流水线

```bash
# 全量构建所有模块
python3.12 ./ns3 build

# 全量测试
./test.py
```

## 提交与 PR 规范

### 提交格式
Conventional Commit: `type(scope): subject`
- `feat(scope): ...` - 新功能
- `fix(scope): ...` - 修复
- `docs(scope): ...` - 文档
- `refactor(scope): ...` - 重构
- `test(scope): ...` - 测试
- `chore: ...` - 杂项

标题要求：祈使句、现在时、≤72 字符。

### PR 内容清单
- [ ] 问题背景与改动范围
- [ ] 关键设计或行为变化
- [ ] 实际执行过的验证命令
- [ ] `Knowledge Sources` 小节（引用 dev-knowledge 路径 + 采用原因）
- [ ] 涉及流程或输出变化时附日志/截图

## ANTI-PATTERNS

- ❌ 开发时进行不必要的全量构建/测试（浪费资源，用模块级代替）
- ❌ <IMPORTANT>电脑性能有限，不要同时启动多个 build/test 构建任务；单个 build 内部可以用 `-jN` 加速。</IMPORTANT>

# Repository Notes

- 对 `UbLink` / `UbRemoteLink` 的测试，默认把它们视为"搬移序列化后的 `Packet` bytes"的承载层；不要在 link 测试里引入高层协议语义假设。
- `UB_CONTROL_FRAME`、`VL`、`TP opcode`、credit 恢复等语义，应以 `UbSwitch`、flow-control、transport 等模块的实际解析与处理路径为准；优先基于这些模块已有逻辑或更高层 trace 做验证。
- 如果测试必须解析 `Packet`，应尽量复用 unified-bus 模块现有的报文解析逻辑与类型边界，避免测试 oracle 自行发明额外语义。
- 当测试 oracle 与模块真实语义冲突时，优先删除或重做该 oracle，不要为了保住测试去扭曲实现。
