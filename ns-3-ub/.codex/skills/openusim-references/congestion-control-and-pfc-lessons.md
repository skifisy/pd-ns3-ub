# Congestion Control and PFC Lessons

<reference-hint>
<use-when>Use this reference when discussing DCQCN queue spikes, PFC with ECN/CNP, or paper-style PFC parameters.</use-when>
<focus>First-principles reasoning for ECN -> CNP -> rate-cut timing, PFC mode semantics, and the practical experiment rules that decide whether a run can complete.</focus>
<keywords>DCQCN, ECN, CNP, PFC, CBFC, headroom, dynamic threshold, overshoot, retransmission</keywords>
</reference-hint>

## Contents

- Core Judgment
- First-Principles Model
- What A Queue Spike Means
- Why Line-Rate Start Makes PFC Hard To Avoid
- PFC Modes In This Repo
- Shared Template For Comparing PFC Algorithms
- Headroom Semantics
- Dynamic PFC Configuration Rules
- When A Run Should Stop Early
- CBFC vs PFC
- Practical Experiment Advice

## Core Judgment

先把这几个问题拆开，不要混成一句“DCQCN 不对”：

1. `marking semantics`：交换机在什么队列量上打 ECN
2. `feedback latency`：`ECN mark -> receiver CNP -> sender rate cut` 要多久
3. `local protection`：这段反馈空窗期靠什么兜底，`CBFC / PFC / headroom / retransmission`
4. `parameter spec`：这些门限和速率参数是否真和目标论文/实验对齐

很多“队列压不低”不是实现错。
而是 `feedback latency` 和 `local protection` 没拆开看。

## First-Principles Model

DCQCN 不是“交换机一 mark，发送端立刻降速”。

中间至少有这条链：

`packet marked at switch -> marked packet reaches receiver -> receiver emits CNP -> CNP returns to sender -> sender applies rate cut`

所以只要 sender 还是 line rate 或接近 line rate：

- 交换机先看到队列上涨
- 交换机先开始 mark
- sender 还会在一个反馈窗口里继续注入数据
- queue 会继续涨
- 如果本地保护门限比这段窗口可注入量更小，就会触发 PFC

这类峰值先涨、后 cut，不自动说明 bug。

## What A Queue Spike Means

在这个 repo 里，先问：

1. `first mark` 什么时候发生
2. `first CNP RX at sender` 什么时候发生
3. `first rate cut` 是否紧跟 CNP

如果：

- `mark -> CNP RX` 已经是几微秒量级
- sender 收到 CNP 立刻 `rate cut`

那这个 spike 更可能是“反馈空窗期里的正常过冲”，不是 sender 没响应。

安全说法：

- `queue spike before first CNP` is compatible with a correct DCQCN implementation
- `sender cuts immediately after first CNP` means the dominant delay is the feedback path, not local control handling

不安全说法：

- `queue stayed high therefore DCQCN implementation is wrong`
- `PFC happened therefore ECN/CNP is broken`

## Why Line-Rate Start Makes PFC Hard To Avoid

如果 flow 从 line rate 开始发：

- 第一批包还没来得及形成完整闭环反馈
- queue 先涨
- ECN 先开始 mark
- sender 还没收到 CNP

所以：

- `PFC hard to avoid` 不是反常
- 它本来就是 `local protection during feedback delay`

这点在单瓶颈、低 hop 数、较大 BDP、较小 headroom 下更明显。

不要把下面两件事混为一谈：

- `DCQCN eventually converges`
- `DCQCN always avoids any PFC`

前者可以成立，后者不一定。

## PFC Modes In This Repo

这几个模式本质上都是：

`if ingress queue occupancy crosses a pause threshold -> send pause`

差别不在“会不会看 backlog”。
差别在“pause threshold 怎么算”和“resume threshold 怎么算”。

### `PFC_FIXED`

直接用端口静态阈值：

- `pause when ingressTotal >= PfcUpThld`
- `resume when ingressTotal < PfcLowThld`

适合：

- 单优先级
- 想做最稳定、最好解释的实验
- 不需要 paper dynamic sharing semantics

### `PFC_DYNAMIC`

基于 shared pool 剩余量做每队列门限：

- `xoff = (SharedPoolBytes - sharedUsedBytes) >> AlphaShift`
- `xon = max(xoff - DynamicPfcResumeGapBytes, 0)`
- queue 超过 reserve 后先吃 shared
- shared 再不够就进 headroom
- `pause when inHeadroom or sharedUsed >= xoff`

经验：

- `AlphaShift` 越大，门限越激进地缩小
- `DynamicPfcResumeGapBytes` 越大，resume 越晚，更不容易抖，但停得更久
- 如果 `resume gap > max xoff`，`xon` 会塌到 0，resume 会很保守

### `PFC_DYNAMIC_PAPER`

`PFC_DYNAMIC_PAPER` 是为 DCQCN 论文 **"Congestion Control for Large-Scale RDMA Deployments"** (SIGCOMM 2015) 做复现/对比时使用的 paper-style dynamic PFC 模式。

paper-style dynamic 也是“共享池剩余量决定门限”，但公式不同：

- `threshold = beta * max(SharedPoolBytes - globalOccupancyBytes, 0) / priorities`
- `resume threshold = max(threshold - 2 * MTU, 0)`
- `pause when ingressTotal >= threshold or inHeadroom`

这里看的是 `global occupancy`，不是单队列 sharedUsed。

本 repo 当前实现里的关键语义：

- `global occupancy` 取整台 switch 上各出端口 VOQ backlog 加 egress queue backlog
- `PFC_DYNAMIC_PAPER` admission 不再受旧 `AlphaShift` 门限主导
- `PaperDynamicPfcBeta` 是 paper-style dynamic 的主配置量
- 不要把它当成泛化默认动态 PFC；普通动态 PFC 实验优先用 `PFC_DYNAMIC`

## Shared Template For Comparing PFC Algorithms

可以把 fixed / dynamic / paper dynamic 写成同一个模板：

`pause if queue_or_port_observation >= pause_threshold`

其中：

- `observation`
  - `PFC_FIXED`: ingress queue total bytes
  - `PFC_DYNAMIC`: ingress shared bytes plus headroom state
  - `PFC_DYNAMIC_PAPER`: ingress total bytes plus headroom state, threshold来自全局占用
- `pause_threshold`
  - `PFC_FIXED`: static hi watermark
  - `PFC_DYNAMIC`: shared-remaining-derived threshold
  - `PFC_DYNAMIC_PAPER`: paper global-remaining-derived threshold
- `resume_threshold`
  - `PFC_FIXED`: static low watermark
  - `PFC_DYNAMIC`: `xoff - DynamicPfcResumeGapBytes`
  - `PFC_DYNAMIC_PAPER`: `xoff - 2 * MTU`

所以算法层面上可以说它们是同一数学模板。
不同的是阈值函数。

## Headroom Semantics

这个 repo 的 ingress accounting 是三层：

`reserve -> shared -> headroom`

可以这么理解：

- `reserve`: 每个 ingress queue 自己的保底
- `shared`: 大家共用的池子
- `headroom`: PFC 已经该生效但飞行中的包还会继续到达时，用来兜最后一段 inflight

当 queue 进入 headroom，通常意味着：

- 从 congestion-control 角度已经太晚了
- 这不是“刚开始拥塞”
- 而是“保护机制正在吸收反馈空窗期残余流量”

所以如果一个实验大量使用 headroom：

- 不能直接说它错
- 但可以说它已经依赖 PFC/backpressure，而不是只靠 ECN 把 queue 压低

## Dynamic PFC Configuration Rules

### `PFC_DYNAMIC`

最关键的配置量：

- `SharedPoolBytes`
- `HeadroomPerPortBytes`
- `AlphaShift`
- `DynamicPfcResumeGapBytes`

经验规则：

- `HeadroomPerPortBytes` 太小：会更早丢包
- `AlphaShift` 太大：pause threshold 太小，过于激进
- `DynamicPfcResumeGapBytes` 太大：pause/resume 抖动少，但恢复慢

### `PFC_DYNAMIC_PAPER`

最关键的配置量：

- `SharedPoolBytes`
- `PaperDynamicPfcBeta`

其余要点：

- 来源语义是 DCQCN 论文 **"Congestion Control for Large-Scale RDMA Deployments"** (SIGCOMM 2015) 的 paper-style dynamic PFC 阈值复现
- `2 * MTU` resume gap 是算法内置语义，不是现在这个模式的主调参数
- 多优先级数会进入 paper threshold 公式
- 当前 repo 默认按 `8` 个 priorities 计算 paper threshold

## When A Run Should Stop Early

如果：

- 发生丢包
- `EnableRetrans = false`

那这个实验很可能不会完成。

原因不是“wall clock 慢”。
而是协议闭环已经断了：

- 某个包丢了
- 没有 end-to-end recovery
- completion 可能永远不增长

默认安全规则：

- `drop + retrans disabled` 可以作为 fail-fast 条件
- 诊断信息里应明确提醒用户：
  - 开启 `EnableRetrans`
  - 或使用 `CBFC`
  - 或在必须使用 PFC 时谨慎调 PFC 参数

## CBFC vs PFC

在这个 repo 里：

- `CBFC` 更接近“发送前就用 credit 限住”
- `PFC` 更接近“接收侧 backlog 到阈值后发 pause”

因此：

- `CBFC` 更适合把问题变成真正的 lossless backpressure
- `PFC` 即使开启，也仍然依赖阈值、headroom、反馈时序是否合适
- 配置不当的 `PFC` 仍可能在 pause 生效前先丢包

安全说法：

- `CBFC` is the more robust lossless protection mode in this repo
- `PFC` is a threshold-based pause mechanism, not a proof of zero loss by itself

## Practical Experiment Advice

如果目标是：

- 先验证 congestion-control 主机制
- 先让实验稳定结束

优先级通常是：

1. 先确认 `EnableRetrans`
2. 再决定要不要 `CBFC`
3. 必须做 PFC 论文复现时，再切 `PFC_FIXED / PFC_DYNAMIC / PFC_DYNAMIC_PAPER`
4. 观察 `mark -> CNP -> rate cut -> queue peak -> pause` 整条时序，不要只盯最终平均队列

一句话版本：

`ECN/CNP controls eventual sender rate; PFC protects the feedback window; whether queue stays low depends on both timing and threshold design.`
