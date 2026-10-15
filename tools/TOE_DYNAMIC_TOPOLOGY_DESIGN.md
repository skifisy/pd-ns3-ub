# ToE 动态 OCS 拓扑设计（v5）

本文说明 ToE（Topology Engineering）如何根据已观测到的业务需求，周期性地重分配 Mooncake OCS 的物理电路，并在拓扑变更时安全地迁移在途流量。运行和构建命令见[运行手册](./TOE_DYNAMIC_TOPOLOGY_RUN.md)。

## 1. 目标与适用范围

实现面向仓库提供的 Mooncake OCS case：leaf ID 为 37–56，共 20 个 leaf；按连续编号每 4 个 leaf 划为一组，共 5 组；每个 leaf 有 64 个 OCS 端口；每条电路带宽为每方向 400 Gbps。组内 leaf 不建直连，跨组 leaf pair 至少保留一条电路。每个 leaf 的 64 个 OCS 端口都必须用于跨组连接。

ToE 假设 `topology.csv`、`node.csv`、`routing_table.csv` 的 host/leaf 编号符合该拓扑。它需要进程内 HiGHS C++ 后端，并限制为单进程、单线程运行；MPI 和 MTP 并行均不支持。未启用 `--toe-mode` 时，普通路由和原有 Jupiter TE 行为保持独立。

提供两种转发模式：

| 模式 | 拓扑控制 | 数据选路 |
| --- | --- | --- |
| `--toe-mode=only` | ToE 优化 OCS 电路数量 | 最短路径 ECMP；有直连时走直连，没有直连时在可行的单中转物理出端口间哈希 |
| `--toe-mode=te` | ToE 优化电路数量和路径流量份额 | 复用 Jupiter TE 的逐流 WCMP；保留直达路径，并仅在优化需要时分流到单中转路径 |

## 2. 需求采集与预测

控制器只统计首次发送的业务数据载荷，并用实际 host 端口映射到有向 leaf OD `(src, dst)`。ACK、控制报文、READ 请求和重传不作为业务需求重复计数。仿真每 30 秒结算一个完整观测窗口；每次求解针对最近 `--toe-history-windows` 个完整窗口，逐 OD 取最大字节数，再除以 30 秒得到预测速率：

$$
D_{i,j}=\frac{8}{30}\max_{w\in\mathcal{H}} B_{w,i,j}\quad\text{bit/s}
$$

默认保留最近 20 个完整窗口，默认每 300 秒重算一次。因而需求预测来自仿真期间已经发生的业务，不预读未来的 `traffic.csv` 任务。窗口长度固定为 30 秒；重算周期和历史窗口数可单独配置。

## 3. 整数拓扑与流量优化

对每个跨组 leaf pair `{i,j}`，整数变量 `n_ij` 表示并行 OCS 电路数量。拓扑约束为：

$$
1\le n_{ij}\le 49,\qquad \sum_{j:\,group(j)\ne group(i)}n_{ij}=64
$$

每对 leaf 的链路容量为 `400 n_ij Gbps`，并对两个方向分别施加容量约束。下界确保跨组可达；度数等式确保每个 leaf 的 64 个 OCS 端口恰好全部使用。当前仿真初始拓扑通常是每个跨组 pair 4 条电路，ToE 可在该状态上调整。

求解按以下层次进行：

1. **最小化 MLU。** 对候选 MLU 上界做可行性搜索。`only` 模式使用物理端口 ECMP 的实际期望份额建模：跨组 OD 使用直达电路；同组 OD 经其他组的 leaf 转发，流量按源 leaf 到各中转 leaf 的物理端口数占 64 个端口的比例分配。`te` 模式为有需求的 OD 建立直达路径和单中转路径流量变量，并按有向电路容量约束路径负载。
2. **限制拓扑变更。** 将 MLU 上界放宽默认 1% 相对 slack，在该界内最小化相对当前拓扑的电路变更量，避免轻微收益引发不必要的重配。
3. **`te` 模式减少绕路。** 固定第二步选出的拓扑和 MLU 上界，再最小化承载于单中转路径上的预测业务量。所得路径份额交给现有 Jupiter TE 机制用于新流逐流选路。

容量和流量在求解器内部以 Gbps 表示以保持数值尺度；控制器采集的需求 API 单位为 bit/s。HiGHS 每次调用使用单线程，并设有总计约 30 秒的求解时间预算；在时间到达时若存在可行 incumbent，则可继续使用，并在 `solver_optimal` 列标明是否达到最优。

## 4. 拓扑切换和流量迁移

v5 默认 `--toe-reconfiguration=auto`：`only` 模式按受影响 leaf 对局部暂停；`te` 模式保持全局排空。可显式指定 `global` 做重配范围对照；`selective` 只允许与 `only` 搭配。当前暂停入口属于 RTP，运行器拒绝 CTP；局部模式要求 `UbApp::UseShortestPaths=true`，以确保同 leaf 的普通转发不会使用 OCS 绕路。

### 4.1 保留物理连接

端口规划器先逐 pair 保留 `min(当前数量, 目标数量)` 条原有电路，包括其两端物理端口和现有 `UbLink` 对象。只释放数量减少的 bundle 中多余的端口，再将这些端口用于数量增加的 bundle。规划前检查端口唯一性、pair 覆盖和各 leaf 度数守恒。

例如减少两条旧电路、增加两条新电路时，640 条中的 638 条保留，只对剩余两条执行 `Attach()`。`rewired_circuits` 与求解器 `changed_circuits` 一致；每条被重连 circuit 对应两端端口。保留端口的信用和 PFC 状态继续使用；仅重置换了 peer 的端口流控与信用载荷。

### 4.2 局部暂停的范围

这里按 leaf 对暂停 TP 的新包，不按单个 task 暂停：同一 TP 中当前和随后排队的任务遵循相同判定，新建立的 TP 也在发送时判断。

| 流量 | 局部暂停行为 |
| --- | --- |
| 跨组且直连 circuit 数量变化 | 双向 TP 暂停新包；包括增加容量的 pair，避免新旧哈希集合在在途包未排空时切换 |
| 跨组且直连数量不变 | 继续发送，即使其源或目的 leaf 的其它连接发生变化 |
| 同组不同 leaf | 保守暂停；可能经其它组的任意 leaf 中转，不能仅根据端点判断是否使用被移除的链路 |
| 同 leaf | 继续发送，局部模式要求最短路径配置 |
| 端点不能映射 | 保守暂停 |

暂停包括 READ 请求、WRITE、READ_RESPONSE 等 RTP 新数据入口，ACK、CNP 和选择性重传继续运行；GBN 若依赖回退到新数据入口进行重发，可能等到超时恢复才继续。现有 READ_RESPONSE 的源目的方向按实际 TP 端点判定，反向 TP 同样受保护。这里并未实现逐 flow 的具体中转路径跟踪，因此同组流量仍可能被较宽范围地暂停。

### 4.3 排空、传播保护和恢复

1. 保持旧拓扑转发，阻止受影响 TP 产生新包。未受影响的直连业务继续发送。
2. 每 100 微秒检查受影响 TP 的未确认发送边界与控制工作；仅检查换 peer 的端口发送状态、egress、输入处理占用、入端口和出端口 VOQ/控制队列。其它端口持续有流量不会阻塞局部排空。调度器预检会跳过暂停 TP，避免同一 host 端口上的其它 TP 被阻塞。全局模式仍检查所有 TP 和队列。
3. 连续两次排空后，记录换 peer 端口的发送包计数，并等待 `2 × 最大 OCS 传播时延 + 1ns + --toe-switch-delay-seconds`。
4. 保护期结束再次检查排空和端口发送计数。若期间还有 ACK/信用等发送，则重新进入排空，避免旧链路在途报文被遗漏。10 秒超时覆盖排空和传播保护全过程。
5. 只重连规划中新增的电路，重置对应端口流控，读取实际 `UbLink` peer，验证每条连接和目标 circuit 数量。`only` 模式直接更新 ECMP 端口集合；`te` 模式刷新 TE 拓扑、清除缓存并重算权重。
6. 恢复被暂停 TP 的源端口与重连端口。超时则取消待执行切换，记录 `drain-timeout`，保留旧拓扑并唤醒暂停 TP，避免超时后发送器无人触发。

未受影响业务仍可能因共享主机、交换机 buffer 或调度资源受到间接影响。本版保证的是不主动暂停这些 TP，不保证 FCT 一定改善。新电路继续采用初始 OCS 代表性传播时延；保留电路维持原有传播时延。

## 5. 审计输出

默认输出目录为 `<case>/toe/`，可用 `--toe-output` 覆盖：

| 文件 | 内容 |
| --- | --- |
| `toe_summary.csv` | 每次求解的 MLU、允许上界、电路变更数、最优状态；重配置时分别记录 `draining` 和 `applied` 阶段，成功应用时记录 drain 和总暂停时长 |
| `toe_topology.csv` | 每轮每个 leaf pair 的目标 (`target`) 和成功应用 (`applied`) 电路数，以及对应的单向聚合带宽 |
| `toe_reconfiguration.csv` | 每次切换的实际范围、暂停/总 TP 数、保留/重连 circuit 数、未暂停业务发送字节与暂停时长 |
| `toe_circuits.csv` | 原始端口连接、每次计划保留/移除/新增连接和应用后的物理连接 |
| `toe_errors.log` | 求解失败或 10 秒排空超时的时间和原因 |

`toe_topology.csv` 每一行对应一个无向 leaf pair（`src_leaf < dst_leaf`）。`circuit_count` 是两个 leaf 间的 OCS 电路条数；`capacity_gbps_per_direction` 按每条电路 400 Gbps 计算，是该 leaf pair 每个方向可用的聚合容量，不把两个方向相加。`target` 行记录求解器给出的分配；若无需改动拓扑，会立即记录相同的 `applied` 分配；需要切换时，则在物理链路切换完成后记录 `applied`。可按 `epoch`、`status` 对比计划分配和成功应用的分配。

需要切换时，summary 在暂停流量并等待网络排空时先记录 `draining`，随后在链路切换完成时记录 `applied`。排空状态每 100 微秒检查一次，并在 10 秒后超时中止。CSV 时间戳使用 12 位有效数字，便于区分微秒级重配置时延。

ECMP 的负载是基于物理端口哈希的期望份额。有限数量的 flow hash、流量大小差异和包头开销会令实测链路利用率与模型预测不同。

### v5 新增日志口径

`toe_reconfiguration.csv`：

```text
epoch,time_seconds,status,scope,affected_leaf_pairs,paused_tp_channels,total_tp_channels,retained_circuits,rewired_circuits,unpaused_new_payload_bytes,pause_seconds
```

`scope` 为实际执行的 `selective` 或 `global`。TP 数是该时刻已建立的 channel 数，不是活跃 flow/task 数；应用时新增 TP 也计入。`unpaused_new_payload_bytes` 只统计本轮暂停期间仍获准发送的首次业务载荷；值大于零可证明局部暂停期间有其它业务继续发包，零不等同于全网被暂停。`pause_seconds` 表示受影响 TP 的暂停区间长度。

`toe_circuits.csv`：

```text
epoch,time_seconds,status,src_leaf,src_port,dst_leaf,dst_port
```

epoch 0 的 `initial` 为初始连接；后续 `retained/remove/add` 为切换计划，`applied` 为切换后经过实际 peer 验证的全部连接。超时轮的计划没有对应 `applied`，不能当成已生效。
