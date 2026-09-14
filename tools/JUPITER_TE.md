# Mooncake 流量的历史预测 TE 仿真

本实现仅对 `tools/generate_ocs_topology.py` 的静态 OCS 拓扑启用：37–56 每个 leaf 是一个 block。每对跨组 leaf 的 4 条 400 Gbps 物理链路在 LP 中是一条 1.6 Tbps **有向**逻辑边；不同组有直达路径和 12 条一跳中转路径，同组没有直达路径、有 16 条一跳中转路径。拓扑生成脚本同时生成 `node.csv`、`topology.csv` 和逐个目的 host 端口精确匹配的 `routing_table.csv`。

`--jupiter-te=1` 从源主机端口**成功发出**的 URMA 数据包统计业务载荷字节；读取实际发出的源端口及报文的目的端口，按 `topology.csv` 映射到 leaf。无载荷请求、ACK、控制包和同一 `(host, TPN, PSN)` 的重传不进入矩阵。另在 OCS 物理端口计数所有成功发出的线速字节，汇总到有向逻辑边，以便分析最忙链路（这项数据包含头部和重传）。统计窗口固定 30 秒；在每次重算时，取过去 `--te-history-windows` 个已完成窗口各 OD 的最大值作为预测，默认 120 个窗口（最多 1 小时）。重算间隔 `--te-recompute-seconds` 默认 30 秒。未观测到业务的 OD 走直达，直达不存在时按候选路径的瓶颈容量比例走单中转备份；前 30 秒也是这套策略。前 600 秒仍执行控制，但评估时应排除这个预热阶段。

`tools/jupiter_te_solver.py` 使用 SciPy 的 HiGHS 求解器：第一级最小化有向链路最大利用率 `U`，第二级在 `U` 基本不变时最小化中转路径流量。`--te-s` 控制每条路径的份额上限 `D * C_path / (S * sum(C_path))`，范围 `[0,1]`，默认 `0` 表示禁用该上限。`S=1` 会使同容量候选路径的份额近乎平均；不应将其误解为“只走直达”。运行环境必须安装 `scipy`。

从仓库的 `ns-3-ub` 目录运行（假设已准备好 `network_attribute.txt`、`traffic.csv`，并沿用已有 trace→DAG→traffic 脚本）：

```bash
python3 ../tools/generate_ocs_topology.py
./ns3 run 'scratch/ub-quick-example --case-path=scratch/mooncake_pd_storage_ocs_topology --jupiter-te=1 --te-recompute-seconds=30 --te-history-windows=120 --te-s=0 --mtp-threads=8'
```

使用任意其他 case 目录时，指定该目录中的 `topology.csv`、`routing_table.csv` 等完整配置，并传 `--te-solver=/absolute/path/to/tools/jupiter_te_solver.py`。也可用 `--te-output=/path/to/result` 修改输出路径。默认写在 `<case>/jupiter_te/`：`observed.csv` 是实际源端业务字节，`link_bytes.csv` 是逻辑边的实际线速字节，`prediction.csv` 是每次控制周期的预测速率，`weights.csv` 是对应 leaf 对和直达/中转路径权重；最后一个不足 30 秒的观测窗口也会写出，`complete=0`，不会用于预测。当前矩阵及权重文件供在线求解器交互。实验中应只对 600 秒之后的有效区间比较网络指标；流量没有运行到 30 秒时不会有历史矩阵。

跑完后可用 `python3 ../tools/analyze_jupiter_te.py --topology=<case>/topology.csv --link-bytes=<case>/jupiter_te/link_bytes.csv --output=<case>/jupiter_te/utilization.csv` 输出已完成且处于 600 秒预热期之后的每窗口最忙有向链路利用率。这里用的是 OCS 发送线速字节，包含报文头和重传；LP 用的是源端业务字节，因此两者不会严格相等。

源 leaf 对路径按已有 `CalcHash` 的逐流哈希键做 WCMP，并在固定流的生命周期内保留已选路径。选到单中转后，包头的 `RoutingPolicy` 在源 leaf 被置为 shortest；中转 leaf 只允许直达目标 leaf，末端 leaf 按准确的目的 host 端口交付。四条并行物理链路仍按该流哈希做 ECMP。一个 TPN 使用 packet spray 时 TE 会拒绝该流；TE 当前只支持单 MPI rank，MTP 可通过线程锁保护矩阵和策略发布。实验控制只配置一路历史预测 TE，不做 oracle 重放。

局部回归测试：`python3 -m unittest tools/test_jupiter_te.py -v`。仅提交源文件时还需在完整的 ns-3 工作树构建并跑含实际 `traffic.csv` 的场景，才能验证 C++ 对接和最终业务完成率。
