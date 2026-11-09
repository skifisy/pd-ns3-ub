# Mooncake 流量的历史预测 TE 仿真

整体技术思路、控制闭环和优化模型见 [`JUPITER_TE_DESIGN.md`](./JUPITER_TE_DESIGN.md)。具体的脚本执行顺序、`routing_table.csv` 单独生成方式、ns-3 编译/运行命令和结果分析步骤见 [`JUPITER_TE_RUN.md`](./JUPITER_TE_RUN.md)。本文档保留 TE/WCMP 的实现摘要。

本实现仅对 `tools/generate_ocs_topology.py` 的静态 OCS 拓扑启用：37–56 每个 leaf 是一个 block。每对跨组 leaf 的 4 条 400 Gbps 物理链路在 LP 中是一条 1.6 Tbps **有向**逻辑边；不同组有直达路径和 12 条一跳中转路径，同组没有直达路径、有 16 条一跳中转路径。拓扑生成脚本同时生成 `node.csv`、`topology.csv` 和逐个目的 host 端口精确匹配的 `routing_table.csv`。已有 `topology.csv` 时也可用 `tools/generate_routing_table.py` 单独重建路由表。

`--jupiter-te=1` 在发送端传输层首次生成 WRITE 或 READ-response 数据包时统计业务载荷字节，并按实际 source/destination host port 经 `topology.csv` 映射到 leaf。这是仿真运行中已经发起的业务量，不读取未来的 `traffic.csv`；READ request、transaction/transport ACK、其他控制包和重传不进入矩阵，因此也不存在 24-bit PSN 回绕导致的重复过滤问题。交换机同样只让 WRITE/READ-response 业务流进入 TE 路径；控制流量继续使用普通路由。另在 OCS 物理端口计数所有成功发出的线速字节，汇总到有向逻辑边，以便分析最忙链路（这项数据包含头部和重传）。统计窗口固定 30 秒；在每次重算时，取过去 `--te-history-windows` 个已完成窗口各 OD 的最大值作为预测，默认 120 个窗口（最多 1 小时）。重算间隔 `--te-recompute-seconds` 默认 30 秒。未观测到业务的 OD 走直达，直达不存在时按候选路径的瓶颈容量比例走单中转备份；前 30 秒也是这套策略。前 600 秒仍执行控制，但评估时应排除这个预热阶段。

两阶段 LP 由 `UbTeSolver` 通过 HiGHS C API 在 ns-3 进程内构建并求解：第一级最小化有向链路最大利用率 `U`，第二级在 `U` 基本不变时最小化中转路径流量。不会启动 shell 或 Python 子进程，也不使用 CSV 在进程之间传递矩阵和策略。LP 直接以每个 OD 的路径份额为变量，使流守恒等式恒为 1，避免低速 OD 被求解器数值容差当成零；发布前还会校验每个活跃 OD 的非负权重和为 1。`--te-s` 控制每条路径的流量上限 `D * C_path / (S * sum(C_path))`，范围 `[0,1]`，默认 `0` 表示禁用该上限。`S=1` 会使同容量候选路径的份额近乎平均；不应将其误解为“只走直达”。编译时必须能找到带 CMake package 的 HiGHS C++ 库。

从仓库的 `ns-3-ub` 目录运行（假设已准备好 `network_attribute.txt`、`traffic.csv`，并沿用已有 trace→DAG→traffic 脚本）：

```bash
python3 ../tools/generate_ocs_topology.py
./ns3 run 'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --jupiter-te=1 --te-recompute-seconds=30 --te-history-windows=120 --te-s=0 --mtp-threads=8'
```

普通模式不会输出 TE 日志或 CSV，矩阵和策略完全保存在内存中。排查或采集实验数据时添加 `--debug-te=1`，过程信息只写入 `te_debug.log`，不会刷到控制台；默认调试目录是 `<case>/jupiter_te/`，也可同时用 `--te-output=/path/to/result` 修改。此时 `matrix-current.csv`、`policy-current.csv` 和 `solver-summary-current.csv` 只是内存状态的审计快照，不参与求解。除 epoch 结果外，`live_status.csv` 和 `observed-current.csv` 会记录当前未结算窗口的 task/报文进度与实时矩阵，`demand_events.csv` 会给出业务输入接受或拒绝样本。最后一个不足 30 秒的观测窗口以 `complete=0` 写出，不会用于预测。

Debug 模式还会生成 `WcmpSelectionTrace.csv`：每个已观测 task 通过报文携带的
`UbFlowTag.taskId` 直接关联到源 leaf 的 WCMP 路径，不依赖 ECMP trace 反推。可依次运行
`tools/leaf_pair_active_flow_analysis.py <case>` 和
`tools/analyze_leaf_pair_hotspot_duration_matrix_60s.py <case>`，生成真实 payload 方向的
Leaf-pair task 并发区间与分窗热点矩阵。WRITE 方向为 task source→destination，READ 方向为
task destination→source。

跑完后可用 `python3 ../tools/analyze_jupiter_te.py --topology=<case>/topology.csv --link-bytes=<case>/jupiter_te/link_bytes.csv --output=<case>/jupiter_te/utilization.csv` 输出已完成且处于 600 秒预热期之后的每窗口最忙有向链路利用率。这里用的是 OCS 发送线速字节，包含报文头和重传；LP 用的是源端业务字节，因此两者不会严格相等。

源 leaf 对路径按已有 `CalcHash` 的逐流哈希键做 WCMP，并在固定流的生命周期内保留已选路径。每个 leaf 的 routing process 缓存首次 WCMP 物理端口，也缓存目的 leaf 的“非 TE 跳”判定，后续包不会重复进入全局控制器。选到单中转后，包头的 `RoutingPolicy` 在源 leaf 被置为 shortest；中转 leaf 只允许直达目标 leaf，末端 leaf 按准确的目的 host 端口交付。四条并行物理链路仍按该流哈希做 ECMP。一个 TPN 使用 packet spray 时 TE 会拒绝该流；TE 当前只支持单 MPI rank，MTP 可通过线程锁保护矩阵和策略发布。实验控制只配置一路历史预测 TE，不做 oracle 重放。

脚本回归测试：`python3 -m unittest tools/test_jupiter_te.py -v`；两阶段 LP 的数值测试位于 unified-bus C++ test suite。还需跑含实际 `traffic.csv` 的场景，验证在线策略发布和最终业务完成率。

## 可选中转比例上限

`--te-max-transit-ratio=0.10` 表示：对拓扑上同时具有直达和单中转候选的 OD，分配到中转路径的预测需求合计不超过这些 OD 预测需求合计的 10%。按预测速率加权，不按路径条数或 flow 数计数；这是全局上限，不是每个 OD 的上限，也不是要求达到 10%。

令 O_choice 为上述可分流 OD，D_o 为预测需求，x_o,p 为路径份额，则两阶段 LP 都增加约束：

    sum(o in O_choice, p in transit) D_o * x_o,p <= R * sum(o in O_choice) D_o

默认 `-1` 关闭，省略参数保持原 TE 行为；有效值为 `-1` 或 `[0,1]`，`0` 禁止可选中转，`1` 不额外限制。无直达路径的必经中转 OD 不进入预算，但仍参与链路负载约束。没有可分流 OD 时不增加这一约束。启用 TE 的拓扑变化后重新求解也沿用同一个比例参数。

运行示例（在 `ns-3-ub` 目录）：

```bash
./ns3 run 'scratch/ub-quick-example --case-path=scratch/mooncake_fix_wcmp_opt --jupiter-te=1 --debug-te=1 --te-max-transit-ratio=0.10 --te-recompute-seconds=30 --te-history-windows=120 --te-s=0 --progress-interval=1000ms --mtp-threads=8'
```

限制作用于 LP 预测流量分配，实测字节比例会受到预测误差、flow 大小和已有转发行为影响，不能将该值当作实测流量的硬上限。初始化调试日志记录 `max_transit_ratio`，replay 元数据记录 `te_max_transit_ratio`。参数不会自动开启 TE；需使用现有 TE 启用方式。

建议与 `--te-s=0` 配合：较大的 `te-s` 会限制直达份额，与较小中转预算组合时可能不可行，求解器会报错，不会悄悄取消上限。离线参考求解器使用 `--max-transit-ratio=0.10`，默认也为 `-1`。
