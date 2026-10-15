# ToE 动态 OCS 拓扑运行手册（v5）

本文说明如何准备依赖、构建并运行 ToE。拓扑约束、优化模型和链路切换流程见[设计文档](./TOE_DYNAMIC_TOPOLOGY_DESIGN.md)。以下命令默认从仓库根目录开始。

## 1. 准备 case

生成仓库默认的 Mooncake OCS topology：

```bash
python3 tools/generate_ocs_topology.py
```

脚本生成 case：

```text
ns-3-ub/scratch/mooncake_pd_ocs/
```

运行器要求 case 至少包含 `network_attribute.txt`、`node.csv`、`topology.csv`、`routing_table.csv` 和 `traffic.csv`。生成脚本会创建节点、拓扑和路由文件；请将实验需要的 `network_attribute.txt` 与 `traffic.csv` 放到该目录。使用其它 case 时，确认它满足[设计文档所列的固定 ToE 拓扑假设](./TOE_DYNAMIC_TOPOLOGY_DESIGN.md#1-目标与适用范围)。

## 2. 安装 HiGHS 并构建

ToE 需要带 C API 的 HiGHS C++ 开发包，以及可被 CMake `find_package(highs CONFIG)` 找到的 `highs-config.cmake`。如果 HiGHS 没有安装在默认 CMake 搜索路径，设置其 CMake package 目录：

```bash
HIGHS_CMAKE_DIR=/path/to/lib/cmake/highs
```

在 `ns-3-ub` 目录配置和编译。ToE 当前不支持 MPI 或 MTP，因此关闭这两种并行后端：

```bash
cd ns-3-ub
./ns3 configure \
  --enable-modules=unified-bus \
  --disable-mpi \
  --disable-mtp \
  --disable-werror \
  -d release \
  -G Ninja \
  -- -Dhighs_DIR="$HIGHS_CMAKE_DIR"
./ns3 build -j 8 ub-quick-example
```

确认 configure 输出包含 `UnifiedBus Jupiter TE: in-process HiGHS backend enabled`。如果 HiGHS 未找到，普通 unified-bus 可以编译，但启动 ToE 时会因缺少求解器而失败。更换 HiGHS 安装位置或构建选项后，重新运行 configure。

## 3. 链路重配冒烟测试

仓库提供一段约 92 秒仿真时间的流量：[`toe_reconfiguration_smoke_traffic.csv`](./toe_reconfiguration_smoke_traffic.csv)。它分四批发送 1 MiB WRITE task，在两个 host/存储目的集合之间交替。每批开始时间约为 0、31、61、91 秒。将其放入 OCS case：

```bash
CASE_DIR=scratch/mooncake_pd_ocs
cp ../tools/toe_reconfiguration_smoke_traffic.csv "$CASE_DIR/traffic.csv"
```

确保 case 已有 `network_attribute.txt`。然后以 ToE Only、30 秒重算周期和一个历史窗口运行：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --toe-mode=only --toe-recompute-seconds=30 --toe-history-windows=1 --progress-interval=100ms'
```

从 `ns-3-ub` 目录运行以上命令。每轮只用最近一个完整的 30 秒窗口，使两组交替流量能分别影响后续的拓扑求解。仿真结束后检查：

```bash
cat "$CASE_DIR/toe/toe_summary.csv"
cat "$CASE_DIR/toe/toe_topology.csv"
cat "$CASE_DIR/toe/toe_errors.log" 2>/dev/null || true
```

重点确认 summary 中出现 `solved` 行；若该轮需要改变拓扑且排空成功，应随后出现 `applied` 行，并且 `changed_circuits` 大于 0。`applied` 行记录排空耗时和总暂停时间。若只有 `solved` 且 `changed_circuits=0`，说明该轮目标拓扑与当前拓扑一致；若出现 `solver-error` 或 `drain-timeout`，则查阅错误日志。是否发生拓扑变化取决于端口映射后的 OD 负载和求解结果，不能仅凭 trace 的四个波次保证每一轮都重配。

## 4. 运行 ToE Only

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --toe-mode=only'
```

该模式根据历史流量重配 OCS，数据面使用最短路径 ECMP：跨组 OD 有直达电路时走直达；同组 OD 或没有直达电路时通过可行的单中转 leaf 转发。拓扑控制周期和观测历史仍会运行，但不会启动 Jupiter TE 权重控制。

## 5. 运行 ToE + Jupiter TE

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --toe-mode=te --debug-te=1'
```

`--toe-mode=te` 会启用 ToE 拓扑控制和 Jupiter TE/WCMP。不要同时传 `--jupiter-te=1`；`te` 模式本身已经启用 TE。需要时可分别指定 ToE 与 Jupiter TE 的调试输出目录：`--toe-output` 和 `--te-output`。

ToE 仅支持单进程、单线程：不要通过 `mpirun` 启动，也不要传 `--mtp-threads`。`--toe-mode=te` 可与 `--debug-te=1` 搭配，便于分析 Jupiter 的观测和 WCMP 路径选择。

## 6. 参数

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| `--toe-mode` | `off` | `off`、`only` 或 `te` |
| `--toe-reconfiguration` | `auto` | `only` 自动局部暂停，`te` 自动全局排空；可指定 `global`，或仅在 `only` 下指定 `selective` |
| `--toe-recompute-seconds` | `300` | 拓扑优化周期，单位秒 |
| `--toe-history-windows` | `20` | 预测取最近多少个完整的 30 秒窗口，必须大于 0 |
| `--toe-mlu-slack` | `0.01` | 允许 MLU 相对最优值放宽的比例，范围 `[0,1]`；在此范围内优先减少电路改动 |
| `--toe-switch-delay-seconds` | `0` | 排空后额外等待的 OCS 重配置时延，单位秒 |
| `--toe-output` | `<case>/toe` | ToE 汇总和错误日志目录 |

例如，将每 300 秒重算改为每 120 秒，使用最近 10 个窗口并为 OCS 重配预留 2 ms：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --toe-mode=only --toe-recompute-seconds=120 --toe-history-windows=10 --toe-switch-delay-seconds=0.002'
```

注意：窗口长度固定为 30 秒，`--toe-history-windows=10` 表示最多回看 5 分钟。缩短拓扑重算周期不会改变窗口长度。

## 7. 输出和结果检查

默认输出在：

```text
<case>/toe/toe_summary.csv
<case>/toe/toe_topology.csv
<case>/toe/toe_reconfiguration.csv
<case>/toe/toe_circuits.csv
<case>/toe/toe_errors.log
```

`toe_summary.csv` 的列依次为：

```text
epoch,time_seconds,status,min_mlu,applied_mlu_bound,changed_circuits,solver_optimal,drain_seconds,pause_seconds
```

每次求解写入 `solved` 行。需要拓扑切换时，summary 会先写入 `draining`，随后等待网络排空并执行重配置；成功后写入 `applied` 行，记录排空时间及总暂停时间。排空状态每 100 微秒检查一次，10 秒后超时中止本轮切换并恢复旧拓扑。`changed_circuits` 是调整的电路总数，不是每个 leaf 的端口数。求解错误和排空超时写入 `toe_errors.log`。CSV 时间戳使用 12 位有效数字，可显示微秒级的重配置时间。v5 局部模式的 `pause_seconds` 是受影响 TP 的暂停时长，不再代表全网暂停。

`toe_topology.csv` 逐行记录 leaf pair 的分配：

```text
epoch,time_seconds,status,src_leaf,dst_leaf,circuit_count,capacity_gbps_per_direction
```

每轮求解后写 `target` 行。如果目标和当前拓扑相同，会立即写相同的 `applied` 行；如果需要重配置，则在物理链路切换成功后写 `applied` 行。`circuit_count` 是两个 leaf 间电路数，`capacity_gbps_per_direction` 为 `circuit_count × 400`，单位 Gbps，表示**每个方向**的聚合链路容量。比如 `...,1,30,target,37,41,49,19600` 表示 leaf 37 与 41 之间计划分配 49 条电路，即每个方向 19,600 Gbps。可以用下面的命令筛选某一轮的目标与最终应用结果：

```bash
awk -F, '$1 == 1 { print }' "$CASE_DIR/toe/toe_topology.csv"
```

排查顺序：

1. `case-path` 下的五个必需文件是否存在，`topology.csv` 是否为 37–56 号 leaf、每个 leaf 64 个 OCS 端口的目标拓扑。
2. CMake 是否找到 HiGHS，并且配置日志启用了 in-process HiGHS backend。
3. 是否误用了 `--mtp-threads` 或 MPI，或在 `toe-mode=te` 时又加了 `--jupiter-te=1`。
4. 检查 `<case>/toe/toe_errors.log` 中的 solver error 或 `drain-timeout`。

ToE 的预测 MLU 是由 30 秒窗口业务需求和路径模型计算的值。应结合应用完成时间和链路利用率评估结果；有限 flow 的哈希分布与模型期望可能不同。

## 8. 从 v4 升级与局部重配检查

已应用 v4：在仓库根目录（包含 `ns-3-ub/` 和 `tools/`）应用 `toe-dynamic-topology-v5-incremental.patch`。完全干净的基线 `8efd34b`：应用 `toe-dynamic-topology-v5.patch`。两种 patch 二选一；完整 patch 不可叠加到已有 v4 上。

```bash
git apply --check ./toe-dynamic-topology-v5-incremental.patch
git apply ./toe-dynamic-topology-v5-incremental.patch
cd ns-3-ub
./ns3 build -j 8 ub-quick-example
./ns3 run 'scratch/ub-quick-example --case-path=scratch/mooncake_pd_large_toe --toe-mode=only --toe-reconfiguration=selective --toe-recompute-seconds=30 --progress-interval=1000ms --dependency-visibility-delay=1ns'
```

默认 `auto` 在 `only` 下也是局部暂停。ToE 要求 RTP；局部模式要求 `UbApp::UseShortestPaths=true`。`te + selective` 会明确报错，不能把 WCMP 的任意中转流量按直连 pair 简化判断。使用 `--toe-reconfiguration=global` 可以在同一版代码上对照全局与局部暂停；这两组都复用物理连接，因此 global v5 也不同于 v4 的全量端口重建。

检查：

```bash
cat scratch/mooncake_pd_large_toe/toe/toe_reconfiguration.csv
python3 ../tools/check_toe_reconfiguration.py --case-path scratch/mooncake_pd_large_toe
```

关注 `scope=selective`、`rewired_circuits=changed_circuits`、`retained_circuits + rewired_circuits=640`。如果该轮存在未受影响的已建立 TP，应有 `paused_tp_channels < total_tp_channels`。当 `applied` 的 `unpaused_new_payload_bytes > 0`，说明暂停期间其它业务确实继续发包。零值也可能因为该区间没有其它业务，不能独立判定失败。

离线检查器核对端口唯一性、每 leaf 64 端口、640 circuit、实际目标一致、旧连接复用、被释放端口恰好被重新使用，并保留超时/未完成轮的警告。它检查的是日志一致性；运行时另有实际 `UbLink` peer 断言。

### 无需 ns-3 的端口规划回归

在仓库根目录运行：

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=undefined tools/tests/test_toe_reconfiguration.cc -o /tmp/test_toe_reconfiguration
/tmp/test_toe_reconfiguration
```

覆盖不变拓扑、两条 circuit 迁移保留 638 条连接、双向暂停范围、非法输入拒绝和 1000 轮随机重配。ns-3 的 `unified-bus-switch-voq` 测试还覆盖输入处理占用、控制队列和局部排空。端口规划测试不等同于完整仿真。

### 短 trace 检查未暂停业务

`tools/make_toe_local_smoke.py` 根据实际 topology 选取每个 leaf group 的代表 host，生成跨组流量，不替换原 trace；具体 TP 端口由仿真器选择：

```bash
python3 ../tools/make_toe_local_smoke.py --case-path scratch/mooncake_pd_ocs --output /tmp/toe_local_smoke.csv
```

将输出用于单独的测试 case 的 `traffic.csv`。trace 含热点预热、29.999999 秒启动的热点任务、30 秒之后每 100us 开始的其它跨组 pair 流量及 31 秒收尾任务。用 `--toe-history-windows=1 --toe-recompute-seconds=30 --toe-switch-delay-seconds=0.002` 做检查。热点优化可能同时调整其它 pair，因此日志中的计数才是判断依据，不假定某个指定 pair 必然不变。

本工作环境没有完整 ns-3 构建工具和运行产物：已执行端口规划回归、修改文件语法检查和 patch 应用检查；未执行此 trace 或大规模仿真。应用后先完成短 trace，再比较相同输入和参数下的 global/selective FCT。
