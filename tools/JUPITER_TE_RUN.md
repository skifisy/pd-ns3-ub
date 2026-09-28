# Jupiter Historical TE / WCMP 运行手册

本文档说明如何从脚本生成 case、单独生成 `routing_table.csv`、准备 Mooncake 流量、编译 ns-3，以及运行 Jupiter historical-TE/WCMP 仿真。

技术思路和优化模型见 [`JUPITER_TE_DESIGN.md`](./JUPITER_TE_DESIGN.md)，实现摘要见 [`JUPITER_TE.md`](./JUPITER_TE.md)。本文档只关注“命令怎么跑”。

## 1. 工作目录和依赖

下面的命令默认先进入仓库根目录：

```bash
REPO_ROOT=$(pwd)
CASE_DIR="$REPO_ROOT/ns-3-ub/scratch/mooncake_pd_ocs"
```

Jupiter TE 在 ns-3 进程内调用 HiGHS C API。安装 HiGHS C++ 开发包后，确认存在 `highs-config.cmake`；如果它不在 CMake 默认搜索路径，记录其目录：

```bash
HIGHS_CMAKE_DIR=/path/to/lib/cmake/highs
```

运行仿真不再需要 SciPy，也不会启动 Python solver。

## 2. 生成 OCS topology

从仓库根目录执行：

```bash
python3 tools/generate_ocs_topology.py
```

脚本固定生成：

```text
ns-3-ub/scratch/mooncake_pd_ocs/
├── node.csv
├── topology.csv
└── routing_table.csv
```

其中 host 为 `0..36`，leaf 为 `37..56`；每个允许的跨组 leaf pair 有 4 条并行 400 Gbps OCS 物理链路。`routing_table.csv` 对 destination host port 做精确匹配，并保留并行物理链路的 ECMP outports。

`generate_ocs_topology.py` 会调用 routing-table generator，所以第一次建 case 时通常只需要这一条命令。

## 3. 单独生成 routing_table.csv

如果 `topology.csv` 已存在，只想重建路由表，不需要重新生成 topology：

```bash
python3 tools/generate_routing_table.py --case-path "$CASE_DIR"
```

默认读取 `$CASE_DIR/topology.csv`，并生成/覆盖 `$CASE_DIR/routing_table.csv`。

也可以显式指定输入和输出：

```bash
python3 tools/generate_routing_table.py \
  --topology "$CASE_DIR/topology.csv" \
  --output "$CASE_DIR/routing_table.csv"
```

如果使用脚本默认的 case 路径，在仓库根目录直接执行即可：

```bash
python3 tools/generate_routing_table.py
```

兼容入口仍然保留：

```bash
python3 tools/generate_ocs_routing_table.py \
  "$CASE_DIR/topology.csv" \
  "$CASE_DIR/routing_table.csv"
```

注意：这里生成的是 ns-3 普通转发所需的静态 shortest-path/ECMP 表，并不是 Jupiter LP 每 30 秒求出的 WCMP 权重。TE 动态权重由进程内 `UbTeSolver` 计算并发布。

## 4. 准备 traffic.csv

ns-3 case 至少需要：

```text
network_attribute.txt
node.csv
topology.csv
routing_table.csv
traffic.csv
```

`generate_ocs_topology.py` 不会生成 `network_attribute.txt` 或 `traffic.csv`。

### 4.1 使用仓库内置的 TE smoke traffic

仓库提供 `tools/jupiter_te_smoke_traffic.csv`。它只有 24 个 1 MiB WRITE task，分三批运行：

- 第一批约在 0 秒启动，为 `[0,30)` 生成历史矩阵；
- 第二批约在 31 秒启动，使用不同 priority 形成新流；
- 第三批约在 62 秒启动。

三批 task 不使用 DAG 依赖，而是用绝对启动 delay，避免 smoke test 受到依赖可见性设置影响。
每批内部按 100 ms 错开任务，避免无重传模式下人为制造同步突发并触发运行器的丢包终止。
因此它可以快速检查“采集完整窗口 -> 30 秒求解 -> 后续新流查询 WCMP”整条链路。先保留
正式 trace，再将 smoke 文件复制进 case：

```bash
test ! -f "$CASE_DIR/traffic.csv" || \
  cp "$CASE_DIR/traffic.csv" "$CASE_DIR/traffic.full.csv"
cp tools/jupiter_te_smoke_traffic.csv "$CASE_DIR/traffic.csv"
```

这个文件使用当前 OCS case 的 host 编号：compute host `0..7`、storage host `16..23`。
它不能直接用于采用其他节点编号的 topology。

### 4.2 从 Mooncake trace 生成 DAG

仓库自带小 trace：

```text
input/mooncake/conversation_trace_200.jsonl
```

例如先做一个较小的 smoke case：

```bash
WORK_DIR=/tmp/mooncake_pd_jupiter
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

python3 tools/mooncake_trace_to_pd_store_dag_v6_layer_pipeline.py \
  input/mooncake/conversation_trace_200.jsonl \
  --config tools/mooncake_pd_store_config_v6_layer_pipeline.json \
  --max-requests 4 \
  --out-dir "$WORK_DIR"
```

完整实验可去掉 `--max-requests 4`，并按实验需要设置 `--warmup-requests` 等参数。

### 4.3 DAG 转 ns-3 traffic

```bash
python3 tools/task_dag_to_ub_traffic_v6_layer_pipeline_with_mapping.py \
  "$WORK_DIR/task_dag.csv" \
  --output "$CASE_DIR/traffic.csv" \
  --mapping-output "$WORK_DIR/traffic_task_mapping.csv"
```

`traffic_task_mapping.csv` 用于把 ns-3 task id 映射回 Mooncake request/DAG 语义，建议保留用于分析。

### 4.4 network_attribute.txt

把实验使用的 `network_attribute.txt` 放到 `$CASE_DIR`。Jupiter TE 不会生成这份文件。

TE 使用逐流 WCMP，不支持一个 TPN 同时开启 packet spray。用于 TE 的配置应确保对应 transport channel 的 packet spray 关闭，例如不要使用：

```text
default ns3::UbTransportChannel::UsePacketSpray "true"
```

仿真前可检查必要文件：

```bash
for f in network_attribute.txt node.csv topology.csv routing_table.csv traffic.csv; do
  test -f "$CASE_DIR/$f" || echo "missing: $CASE_DIR/$f"
done
```

## 5. 运行 Python 回归测试

从仓库根目录执行：

```bash
python3 -m unittest tools/test_jupiter_te.py -v
```

Python 测试覆盖 OCS 逻辑边、候选路径、destination-port routing、post-warmup utilization，以及 `generate_routing_table.py` 的独立命令行入口。LP/WCMP 数值测试在 unified-bus C++ test suite 中运行。

## 6. 编译 ns-3

Jupiter TE 可以单线程运行。下面同时打开 MTP，便于之后使用 `--mtp-threads=8`：

```bash
cd "$REPO_ROOT/ns-3-ub"

BUILD_JOBS=${BUILD_JOBS:-$(python3 -c 'import os; print(os.cpu_count() or 1)')}
./ns3 configure \
  --enable-modules=unified-bus \
  --enable-mtp \
  --disable-werror \
  -d release \
  -G Ninja \
  -- -Dhighs_DIR="$HIGHS_CMAKE_DIR"
./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

配置输出必须包含 `UnifiedBus Jupiter TE: in-process HiGHS backend enabled`。未找到 HiGHS 时普通 unified-bus 仍可编译，但启用 `--jupiter-te=1` 会在仿真开始前立即报错。

不需要 MTP 时可去掉 `--enable-mtp`，运行时也不要传 `--mtp-threads`。

## 7. 先跑普通 routing baseline

建议先关闭 Jupiter TE 跑一次，确认 topology、routing table 和 traffic 能正常完成：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --mtp-threads=8'
```

单线程 smoke test：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs'
```

## 8. 跑 Jupiter historical TE + WCMP

从 `ns-3-ub` 目录执行：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --jupiter-te=1 --te-recompute-seconds=30 --te-history-windows=120 --te-s=0 --mtp-threads=8'
```

主要参数：

- `--jupiter-te=1`：打开历史预测 TE 和逐流 WCMP；
- `--te-recompute-seconds=30`：每 30 秒重算一次；
- `--te-history-windows=120`：最多使用之前 120 个完整的 30 秒窗口；
- `--te-s=0`：关闭 Jupiter per-path share cap，范围 `[0,1]`；
- `--mtp-threads=8`：使用 8 个 MTP 线程，需要编译时打开 `--enable-mtp`。

在线求解不依赖工作目录；矩阵、LP 和 WCMP 策略均在当前 ns-3 进程内处理。

TE 当前要求单 MPI rank。不要用 `mpirun -np 2`（或更多 rank）运行该模式；MTP 多线程可以使用。

## 9. Debug 模式和输出文件

普通模式不打印 TE 过程日志，也不保留 TE CSV；当前矩阵和 LP 策略只存在于内存。
Debug 模式同样不会把 TE 过程信息
打印到控制台，而是集中写入 `te_debug.log`；致命错误仍会在控制台显示。

需要确认 TE 流程或保留实验数据时，加 `--debug-te=1`：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --jupiter-te=1 --debug-te=1 --te-recompute-seconds=30 --te-history-windows=120 --te-s=0 --progress-interval=10ms --mtp-threads=8'
```

`--debug-te` 必须和 `--jupiter-te=1` 一起使用。默认调试输出目录为：

```text
$CASE_DIR/jupiter_te/
```

`--progress-interval` 控制运行器多久检查一次 task 是否全部完成。默认值已经从 `100us`
调整为 `10ms`：对于一小时 trace，检查事件数从约 3600 万降到约 36 万；task 完成计数本身
也改为 O(1) 增量计数。它不改变数据包调度、TE 的 30 秒窗口或 WCMP，只可能让仿真检测到
“全部 task 已完成”的时刻最多延后一个检查间隔。排查时建议显式保留
`--progress-interval=10ms`，不建议再用 `100us`。

当进度检查确认所有 task 均已完成时，运行器会先写入最终 task 状态并取消 TE 的
`Recompute`/`SampleLinkWindow` 周期事件，再按原有流程停止仿真。控制台会明确显示：

```text
[COMPLETED: tasks=<completed>/<total>]
```

Debug 模式的 `te_debug.log` 还会记录
`[CONTROL] periodic_events=stopped reason=all-tasks-completed`。如果由于未开启重传时发生
丢包而提前停止，则不会出现上述完成标记，最终完成量也不会被误报为全部完成。

主要文件：

```text
observed.csv
observed-current.csv
prediction.csv
weights.csv
link_bytes.csv
epoch_summary.csv
live_status.csv
demand_events.csv
path_decisions.csv
WcmpSelectionTrace.csv
transit_forwards.csv
te_debug.log
```

- `te_debug.log`：初始化、每次重算和最终汇总，适合直接提供给 AI；
- `epoch_summary.csv`：每个 epoch 的历史窗口数、活跃 OD、预测总速率、LP 利用率和新流选路计数；
- `observed.csv`：发送端首次生成的 WRITE/READ-response 业务载荷，按 30 秒窗口和 source/destination leaf 聚合；READ request、transaction/transport ACK 和重传不进入矩阵；
- `observed-current.csv`：尚未到下一个 30 秒边界、仍保存在内存中的实时业务矩阵快照；
- `live_status.csv`：每秒、每 30 秒 OCS 采样点以及每 100 个新流的 task、业务包、OCS 包和选路累计计数；其中 `route_controller_calls` 是流首包真正进入全局 TE 控制器的累计次数；
- `demand_events.csv`：每类最多 20 个业务矩阵输入样本，区分 `accepted`、异常零载荷、同 leaf 和端口映射失败；
- `prediction.csv`：每个控制 epoch 使用的预测流量矩阵；
- `weights.csv`：每个 epoch 对直达/单中转候选路径求出的 WCMP 权重；
- `link_bytes.csv`：每 30 秒从 OCS 端口原子累计计数器采样得到的逻辑有向边线速字节，包含报文头和重传；不再为每个报文调用 TE 控制器。
- `path_decisions.csv`：每个新流在源 leaf 的 WCMP 决策；`decision_source` 区分初始 fallback 和历史策略；
- `WcmpSelectionTrace.csv`：把 `UbFlowTag.taskId` 与源 leaf 的实际 WCMP 决策关联起来；同一 task 的多个传输流可有多行，已复用固定路径的后续 task 标记为 `pinned`。该文件只在 `--debug-te=1` 时生成，是 Leaf-pair task 并发分析的输入；
- `transit_forwards.csv`：被分到中转路径的流在中转 leaf 的第二跳记录，用于检查是否按两跳终止。两个文件可用 `sip,dip,sport,dport,priority` 关联；因为沿用现有逐节点 ECMP 哈希，`flow_hash` 在源 leaf 和中转 leaf 可以不同。

最后一个不足 30 秒的窗口会以 `complete=0` 写出，不参与后续历史预测。如果仿真在 30 秒内结束，这是有效的 smoke test，但不会产生可供下一次 TE 重算使用的完整历史窗口。正式比较 TE 结果时，约定排除前 600 秒 warmup。

为避免 TE 成为逐包热路径，本版本还做了四项不改变算法语义的优化：

- 每个 leaf 的 routing process 同时缓存 WCMP 出端口和“本跳不归 TE 管理”的负查询；源/中转 leaf 的后续包复用物理端口，目的 leaf 后续包直接进入普通 host 路由；
- 只有携带 WRITE 或 READ-response 业务载荷的可靠 TA 报文进入 TE 路径，READ request 和 transaction/transport ACK 继续使用普通路由；
- 业务矩阵统计按执行线程分片，不再让所有数据包竞争同一把控制器锁；
- OCS 包数/字节数通过端口累计计数器按窗口采样，不再逐包回调控制器。

LP 求解仍只在 `--te-recompute-seconds` 指定的 epoch 运行；WCMP 权重仍只作用于策略发布后出现的新流，已存在流保持固定路径。
正常情况下 `route_controller_calls` 应接近“经过 source/transit/destination leaf 的业务流数”，远小于
`new_data_callbacks`；若两者同数量级，说明逐流缓存没有生效。

### 9.1 生成 Leaf-pair task 并发区间与 60 秒热点图

先确保仿真使用了 `--jupiter-te=1 --debug-te=1`。分析脚本直接读取
`jupiter_te/WcmpSelectionTrace.csv`，不需要也不读取旧的
`runlog/EcmpSelectionTrace_node_*.tr`：

```bash
python3 tools/leaf_pair_active_flow_analysis.py \
  "$CASE_DIR" \
  --leaf-nodes=37-56

python3 tools/analyze_leaf_pair_hotspot_duration_matrix_60s.py \
  "$CASE_DIR" \
  --leaf-nodes=37-56 \
  --bin-s=60 \
  --threshold=16
```

两个脚本默认分别写入：

```text
$CASE_DIR/output/leaf_pair_active_flow_analysis/
$CASE_DIR/output/leaf_pair_hotspot_duration_60s/
```

方向始终是真实 KV payload 方向：WRITE 为 task source→destination，READ 为 task
destination→source，因此不再提供 `--directed true/false`。`--include-same-leaf` 是开关参数：
需要时写出该选项，不需要时省略，不能在后面追加 `true` 或 `false`。若一个 task 的多条
WCMP trace 映射到不同 Leaf pair，脚本会把它写入 `unresolved_tasks.csv`，不会猜测。

可用 `--te-output` 修改 debug 输出目录，例如：

```bash
./ns3 run \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --jupiter-te=1 --debug-te=1 --te-output=/tmp/jupiter_te_run'
```

## 10. 怎么确认 TE 流程跑通

先看紧凑日志：

```bash
grep -E '\[INIT\]|\[SOLVER\]|\[EPOCH|\[FLUSH\]|\[FINAL\]' \
  "$CASE_DIR/jupiter_te/te_debug.log"
```

正常流程应满足：

1. 出现 `[INIT]`，且 leaf、逻辑边和物理 circuit 数量非零；
2. 每到一个重算周期出现一条 `[EPOCH N]`，epoch 连续递增；
3. 有完整历史窗口且有跨 leaf 业务后，出现 `[SOLVER]`，对应 epoch 为 `solver=ok`；
4. `prediction.csv` 中该 epoch 有 OD，`weights.csv` 中同一 `(epoch, src, dst)` 的权重和约为 1；
5. 后续新流的 `path_decisions.csv` 中出现 `decision_source=historical-policy`，说明 WCMP 已使用新策略；
6. 仿真结束出现 `[FLUSH]` 和 `[FINAL]`。如果运行时间不足 30 秒，只有 fallback 决策且 solver 被跳过是正常现象。

使用内置 smoke traffic 时，预期至少看到 epoch 1 和 epoch 2 的 `solver=ok`，并且
`observed.csv`、`prediction.csv`、`weights.csv`、`link_bytes.csv` 都不只有表头。由于 host
到 leaf 有多条 NIC/ECMP 组合，第二批新流不保证与第一批命中完全相同的 leaf OD；因此
`historical-policy` 行数可以随确定性随机流选择变化，不能把“每条第二批流都命中 policy”
作为通过条件。

`policy-current.csv` 发布前会强制检查每个活跃 OD 的权重均非负且权重和为 1。诊断脚本的
`POLICY_WEIGHT_SUMMARY` 应显示 `invalid_rows=0 invalid_weight_sums=0`。

`observed.csv` 和 `link_bytes.csv` 只在 30 秒窗口结束时落盘。例如业务从 411 秒开始，
它属于 `[390,420)` 窗口，在到达 420 秒之前这两个历史文件只有表头是正常的。此时应检查：

```bash
tail -n 20 "$CASE_DIR/jupiter_te/live_status.csv"
cat "$CASE_DIR/jupiter_te/observed-current.csv"
cat "$CASE_DIR/jupiter_te/demand_events.csv"
```

`live_status.csv` 同时记录仿真时间和墙钟毫秒时间；如果仿真时间不变而累计 packet/byte
计数增长，说明正在处理同一仿真时刻的大批事件，并非死锁。task 状态列可区分依赖等待、
已就绪、运行中和已完成。业务回调很多但 `accepted_data_packets=0` 时，可直接根据
mapping miss、same-leaf 和 zero-payload 计数定位矩阵为何为空。

运行器的 task 完成计数使用增量缓存，不再每 100 微秒扫描全部 task；控制台 warning
也会附带 `tasks=completed/total`，避免大型 DAG 的进度检查本身成为主要开销。

`transit_forwards.csv` 的每一行都表示“一条被选为中转的流已经在中转 leaf 直接发往目标
leaf”。它为空不一定是错误，因为某次 LP 可能全部选择直达。若 `path_decisions.csv` 中存在
`transit_leaf >= 0`，则应能在该流真正发包后找到对应的二跳记录；中转 leaf 不再执行
WCMP 重选，因此不会形成第三跳或在两个中转 leaf 之间循环。

如果需要把结果交给 AI 排查，优先提供 `te_debug.log`、`epoch_summary.csv`、发生异常的
epoch 在 `prediction.csv`/`weights.csv` 中的行，以及相关的 `path_decisions.csv` 和
`transit_forwards.csv` 行。

对于“仿真时间卡在某个点”的问题，单独看旧版 `te_debug.log` 不足以区分死锁和同一时刻
的事件风暴。使用本版本时，建议同时提供：

```bash
tail -n 100 "$CASE_DIR/jupiter_te/te_debug.log"
tail -n 30 "$CASE_DIR/jupiter_te/live_status.csv"
cat "$CASE_DIR/jupiter_te/demand_events.csv"
cat "$CASE_DIR/jupiter_te/observed-current.csv"
```

也可以在仿真仍运行时观察追加记录：

```bash
tail -f "$CASE_DIR/jupiter_te/live_status.csv"
```

判断方法：

- 同一个 `sim_time_seconds` 下，`new_data_callbacks`、`ocs_packets` 或字节数继续增长：正在
  处理同一仿真时刻的大批网络事件，不是程序锁死；
- `new_data_callbacks > 0`、`accepted_data_packets = 0`：查看四类拒绝计数和
  `demand_events.csv`，通常是 host port 映射或业务都在同 leaf；
- `accepted_data_packets > 0` 且 `current_od_entries > 0`：业务矩阵采集已正常工作；
- `running_tasks > 0` 且所有 packet/byte 计数在多个墙钟采样间完全不变：才更像传输状态机
  或流控停滞；
- `pending_tasks > 0`、`ready_tasks = running_tasks = 0`：优先检查 DAG phase 依赖是否能够
  被已完成 task 释放；
- 到 420 秒后，`observed.csv` 应出现 `[390,420)` 的完整窗口，并触发第一次有输入的 solver；
  在 420 秒之前 `prediction.csv`、`weights.csv` 为空是正常的。

## 11. 分析 OCS 链路利用率

仿真结束后回到仓库根目录：

```bash
cd "$REPO_ROOT"

python3 tools/analyze_jupiter_te.py \
  --topology "$CASE_DIR/topology.csv" \
  --link-bytes "$CASE_DIR/jupiter_te/link_bytes.csv" \
  --output "$CASE_DIR/jupiter_te/utilization.csv"
```

默认只统计完整窗口，并排除前 600 秒 warmup。`utilization.csv` 会给出每个窗口最忙的有向 OCS 逻辑边及其利用率。
该步骤依赖 `--debug-te=1` 生成的 `link_bytes.csv`；普通模式不会生成分析 CSV。

## 12. 推荐的最小验证顺序

第一次拉取或修改代码后，建议按下面顺序定位问题：

```text
1. python3 tools/generate_ocs_topology.py
2. python3 tools/generate_routing_table.py --case-path <case>
3. python3 -m unittest tools/test_jupiter_te.py -v
4. 准备 network_attribute.txt + traffic.csv
5. 编译 ub-quick-example
6. 不开 --jupiter-te 跑 baseline
7. 开 --jupiter-te=1 --debug-te=1 跑 TE
8. 按第 10 节检查 te_debug.log、epoch_summary.csv 和选路 CSV
9. analyze_jupiter_te.py 计算 post-warmup utilization
```

这样可以把 case 文件问题、routing table 问题、C++ LP 问题和 ns-3/TE 对接问题分开排查。
