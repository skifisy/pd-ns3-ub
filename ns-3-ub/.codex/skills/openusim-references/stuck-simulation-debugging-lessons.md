# Lessons for Debugging "Stuck" Simulations

<reference-hint>
<use-when>Use this reference when the user reports `跑不完`, `卡住`, `ACK 不回来`, or `completion 不增长` and you need to separate stuck, slow, and dependency-blocked cases.</use-when>
<focus>Distinguishing event progress, transport progress, completion progress, and observability overhead.</focus>
<keywords>卡住, 跑不完, ACK 不回来, completion, trace overhead, slow</keywords>
</reference-hint>

## Contents

- Core Experience
- Key Questions
- Diagnostic Ladder
- Practical Rules
- General Lessons from the Two UB128 Cases
- Safe Wording
- Unsafe Wording
- Related References

## Core Experience

不要先问“它是不是死锁了”，先问“哪一层已经不再前进，哪一层还在前进”。

把一个 OpenUSim/UB 用例拆成四层独立的进度面：

1. `event progress`：仿真时间是否还在推进
2. `transport progress`：packet / segment / ACK / TA response 是否还在流动
3. `completion progress`：task-level completion 是否还在增长
4. `observability progress`：trace 写盘、trace 解析、csv 生成是否还在推进

很多“卡住”并不是四层都停了，而是只有其中一层停了。

## Key Questions

排障时先把这四个问题分开：

1. `是完全无进度，还是局部无进度`
2. `是协议没前进，还是只是 completion 条件没满足`
3. `是仿真本体慢，还是 trace / parser 慢`
4. `是业务 bug，还是观测/日志代码自己把进程搞坏了`

只有把这四个问题拆开，后面的推理才不会混乱。

## Diagnostic Ladder

### 1. 先做症状分型，不要直接命名根因

至少先分成四类：

- `crash`：进程异常退出
- `slow but progressing`：进度慢，但还能看到新的仿真时间或新结果
- `transport-progress without completion-progress`：segment/packet 还在动，但 task completion 不再增长
- `true no-progress`：仿真时间、transport、completion 都不再推进

这一步的目标不是解释原因，而是避免把不同问题混成一个词，比如都叫“死锁”。

### 2. 先找最低仍在前进的层

经验上最有效的问题不是“哪里坏了”，而是：

- 还在出现新的 `segment send/complete` 吗
- 还在出现新的 `packet recv/ack` 吗
- 仿真时间还在增长吗
- 只有 `task-level completion` 停了吗

如果低层仍在前进，而高层 completion 停了，优先怀疑：

- completion 依赖的某个条件永远未满足
- 某类 ACK / response / replay 没有闭环
- out-of-order bookkeeping 错了

不要先跳到“调度器停了”或“整个仿真死锁了”。

### 3. 把“完成”画成最小闭环

对每一类 workload，先写出最小完成链：

`request emitted -> data reaches receiver -> receiver state accepts it -> response/ACK generated -> sender consumes ACK/response -> task marked complete`

然后逐段问：

- 哪一段最后一次被观测到
- 下一段为什么没出现
- 这一段没出现，是因为上一段没发生，还是发生了但状态机没接住

这比直接追“大系统为什么不结束”更稳。

### 4. 区分“包没到接收端”与“包到了但没有被计入完成”

后序包到了，不代表前序包也一定被正确接纳。

需要分开检查：

- `delivery`：包有没有被送到 receiver
- `acceptance`：receiver 的有序/失序窗口有没有接住它
- `promotion`：窗口 base 有没有向前推进
- `completion coupling`：推进后是否真正触发 ACK / TAACK / task complete

如果 `delivery = true` 但 `promotion = false`，问题可能在窗口推进逻辑，不在网络路径。

### 5. 把 instrumentation 当成一个独立组件看待

trace 代码不是透明空气，它也是会出 bug 的软件。

出现以下信号时，要优先怀疑 instrumentation：

- 改完 trace batching / logging 后 fresh rerun 才开始崩
- crash stack 落在 `UbUtils` / `PrintTraceInfo` / `std::string append`
- 同一 workload 有时能跑完，有时在 `0 us` 附近崩
- 多线程下 crash，单线程下不 crash

这类问题先从“日志路径是否线程安全”下手，不要先怀疑协议逻辑。

### 6. 不要把 simulated time 当成 wall-clock

`400 us` 的仿真时间可以对应十几分钟真实时间。

如果：

- 进度打印粒度粗
- trace 极重
- 事件密度极高

那么用户看到的“好几分钟没结束”，可能只是 `100 us -> 200 us` 这一段真实开销很大，而不是完全不动。

## Practical Rules

### Rule 1: 先证明“慢”还是“停”

最小证据通常只要这几项：

- 最近一批 `Simulation time progress`
- 最近一批 `task completion`
- 最近一批 `segment completion`
- 进程是否仍占用明显 CPU

如果仿真时间还在增长，就先不要说“卡死”。

### Rule 2: completion plateau 不等于 transport plateau

当 `task-level completion` 停止，但 `segment-level send/complete` 仍继续时，优先排查：

- completion gating condition
- last ACK / TA response
- receiver-side out-of-order replay
- sender-side ack accounting

这比先查 fabric deadlock 更接近问题本体。

### Rule 3: fresh rerun 比复用旧 runlog 更重要

旧 runlog 只能说明“曾经发生过什么”，不能证明“当前代码和当前环境下还会发生什么”。

特别是多线程和日志代码相关问题，必须 fresh rerun。

### Rule 4: A/B 一次只改一个维度

最值得优先切开的几个维度：

- `FlowControl`
- `EnableRetrans`
- `TpOooThreshold` / `JettyOooAckThreshold`
- trace 开关
- `mtp-threads`

一次改一个，才知道结论属于哪一层。

### Rule 5: 先修“让你看不到真相”的 bug

如果 instrumentation 自己会 crash、写坏内存、或者把 rerun 变成不稳定事件，那么先修它。

否则你后面对协议问题的所有观察都可能不可靠。

## General Lessons from the Two UB128 Cases

### Lesson A: “ACK 不回来”首先是 completion-chain 问题，不是礼貌地等 ACK

在第一个 UB128 case 里，真正该问的不是“为什么没有 ACK”，而是：

- completion 依赖的是哪一种 ACK / response
- 它之前的哪一跳最后一次出现在哪里
- 前序片段是没到，还是到了但没被 receiver 的有序/失序状态推进

这类问题最终常常不是“网络里神秘消失了一包”，而是：

- 某个 out-of-order state 没正确保留
- base 没推进
- 后续 replay / ACK 触发条件永远不成立

在这个仓库里，相关修复最终落到了更合适的 sliding window / bitmap bookkeeping 上，而不是盲目增加更多日志。

### Lesson B: “task completion 停在 110us”不等于整个仿真不再前进

在第二个 UB128 case 里，旧结论是：

- `task-level completion` 在大约 `110.081us` 后停止增长
- 但 `segment-level` 事件还继续出现

这条证据本身已经说明：

- event loop 还活着
- transport 不是完全停摆
- 问题更像 completion 依赖没有闭环，而不是整个引擎冻结

先把问题收窄到“哪一层不前进”，才能避免在错误方向上浪费时间。

### Lesson C: “看起来卡了十几分钟”可能只是 observability 非常重

对 `scratch/ub128-nht-only-standard-sim-team-package-20260327/inputs/current-to-simulator/` 的 fresh rerun 里：

- 仿真时间最终推进到 `400 us`
- 进程自然结束
- 仿真本体 wall-clock 约 `898 s`
- trace 解析 wall-clock 约 `147 s`
- `runlog/` 大约 `12G`

所以这个 case 的主要现实问题是“很慢”，不是“跑不完”。

更具体地说，这说明：

- `simulated time` 很短，不代表 `wall-clock` 一定短
- 当 trace 体量巨大时，用户感知会把“慢”误判成“卡住”
- debug 时必须把仿真本体和 trace / parser 开销分开

### Lesson D: instrumentation 也会成为根因

同一个 full-size case 在 fresh rerun 中一度出现：

- `0 us` 附近崩溃
- `BUG IN CLIENT OF LIBMALLOC`
- crash stack 落在 `UbUtils::PrintTraceInfo` / `TpRecvNotify`

这不是协议语义错误，而是多线程 trace batching 本身不线程安全。

通用经验是：

- 一旦 crash stack 落在 logging / trace 层
- 一旦只在 MTP 多线程下出现
- 一旦表现为非稳定复现

就先把“观测代码的数据竞争”当成一等嫌疑人。

## Safe Wording

- `task completion 停了，但 transport 还在推进，所以先看 completion 闭环，不先叫死锁`
- `这个 case 当前证据更像 wall-clock 很慢，不是自然结束条件失效`
- `fresh rerun 先暴露的是 trace 并发安全问题，因此旧的成功日志不能直接当结论`
- `先确认是哪一层停止前进，再决定查 flow-control、retrans 还是 OOO bookkeeping`

## Unsafe Wording

- `CPU 很高，所以肯定是死锁`
- `ACK 没回来，所以肯定是网络丢包`
- `completion 不增长，所以整个仿真已经不前进`
- `以前跑通过一次，所以当前代码也一定已经修好`
- `trace 只是观测，不可能是根因`

## Related References

- `queue-backpressure-vs-topology.md`
- `trace-observability.md`
- `throughput-evidence.md`
