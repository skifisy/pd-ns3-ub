# Jupiter Historical TE / WCMP 运行手册

本文档针对分支 `feat/jupiter-historical-te-wcmp`，说明如何从脚本生成 case、单独生成 `routing_table.csv`、准备 Mooncake 流量、编译 ns-3，以及分别运行普通路由 baseline 和 Jupiter historical-TE/WCMP 仿真。

算法和实现细节见 [`JUPITER_TE.md`](./JUPITER_TE.md)。本文档只关注“命令怎么跑”。

## 1. 工作目录和依赖

下面的命令默认先进入仓库根目录：

```bash
git checkout feat/jupiter-historical-te-wcmp

REPO_ROOT=$(pwd)
CASE_DIR="$REPO_ROOT/ns-3-ub/scratch/mooncake_pd_storage_ocs_topology"
```

Jupiter TE 在线求解器使用 SciPy HiGHS，因此运行 TE 前要保证当前 Python 环境能够 import `scipy`：

```bash
python3 -c 'import scipy; print(scipy.__version__)'
```

如果环境中尚未安装 SciPy，请在用于运行 ns-3 的 Python 环境中安装它。

## 2. 生成 OCS topology

从仓库根目录执行：

```bash
python3 tools/generate_ocs_topology.py
```

脚本固定生成下面的 case 目录：

```text
ns-3-ub/scratch/mooncake_pd_storage_ocs_topology/
```

并生成：

```text
node.csv
topology.csv
routing_table.csv
```

其中：

- host 为 `0..36`；
- leaf 为 `37..56`；
- 每个允许的跨组 leaf pair 有 4 条并行 400 Gbps OCS 物理链路；
- `routing_table.csv` 对 destination host port 做精确匹配，并保留并行物理链路的 ECMP outports。

`generate_ocs_topology.py` 会调用 routing-table generator，所以第一次建 case 时通常只需要这一条命令。

## 3. 单独生成 routing_table.csv

如果 `topology.csv` 已存在，只想重建路由表，不需要重新生成 topology：

```bash
python3 tools/generate_routing_table.py --case-path "$CASE_DIR"
```

默认读取：

```text
$CASE_DIR/topology.csv
```

并覆盖/生成：

```text
$CASE_DIR/routing_table.csv
```

也可以显式指定输入和输出：

```bash
python3 tools/generate_routing_table.py \
  --topology "$CASE_DIR/topology.csv" \
  --output "$CASE_DIR/routing_table.csv"
```

如果使用脚本默认的 case 路径，那么在仓库根目录下直接执行也可以：

```bash
python3 tools/generate_routing_table.py
```

兼容入口 `tools/generate_ocs_routing_table.py <topology.csv> <routing_table.csv>` 仍然保留，但新脚本建议统一使用 `generate_routing_table.py`。

注意：这个 routing table 是 ns-3 普通转发所需的静态 shortest-path/ECMP 表，并不是 Jupiter LP 每 30 秒求出的 WCMP 权重。TE 的动态权重由仿真过程中 `jupiter_te_solver.py` 计算并发布。

## 4. 准备 traffic.csv

ns-3 case 至少需要：

```text
network_attribute.txt
node.csv
topology.csv
routing_table.csv
traffic.csv
```

`generate_ocs_topology.py` 只负责前三个生成结果中的 `node.csv`、`topology.csv` 和 `routing_table.csv`；它不会生成 `network_attribute.txt` 或 `traffic.csv`。

### 4.1 从 Mooncake trace 生成 DAG

仓库自带一个小 trace：

```text
input/mooncake/conversation_trace_200.jsonl
```

例如做一个较小的 smoke case：

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

### 4.2 DAG 转 ns-3 traffic

```bash
python3 tools/task_dag_to_ub_traffic_v6_layer_pipeline_with_mapping.py \
  "$WORK_DIR/task_dag.csv" \
  --output "$CASE_DIR/traffic.csv" \
  --mapping-output "$WORK_DIR/traffic_task_mapping.csv"
```

`traffic_task_mapping.csv` 用于把 ns-3 task id 映射回 Mooncake request/DAG 语义，建议保留用于分析。

### 4.3 network_attribute.txt

把实验使用的 `network_attribute.txt` 放到 `$CASE_DIR`。Jupiter TE 不会替你生成这份文件。

TE 使用逐流 WCMP，不支持一个 TPN 同时开启 packet spray。用于 TE 的配置应确保对应 transport channel 的 packet spray 是关闭的，例如不要把：

```text
default ns3::UbTransportChannel::UsePacketSpray "true"
```

作为该实验的传输模式。

仿真前可以检查：

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

该测试覆盖 OCS 逻辑边、候选路径、LP/WCMP 权重、destination-port routing、post-warmup utilization，以及 `generate_routing_table.py` 独立命令行入口。

## 6. 编译 ns-3

Jupiter TE 本身可以单线程运行；下面同时打开 MTP，便于之后用 `--mtp-threads=8`。

```bash
cd "$REPO_ROOT/ns-3-ub"

BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}
python3.12 ./ns3 configure \
  --enable-modules=unified-bus \
  --enable-mtp \
  --disable-werror \
  -d release \
  -G Ninja
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

如果不需要 MTP，可以去掉 `--enable-mtp`，并在运行命令中不要传 `--mtp-threads`。

## 7. 先跑普通 routing baseline

建议先关闭 Jupiter TE 跑一次，确认 topology、routing table 和 traffic 本身能够正常完成：

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_storage_ocs_topology --mtp-threads=8'
```

如果只想单线程 smoke test：

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_storage_ocs_topology'
```

## 8. 跑 Jupiter historical TE + WCMP

从 `ns-3-ub` 目录执行：

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example \
   --case-path=scratch/mooncake_pd_storage_ocs_topology \
   --jupiter-te=1 \
   --te-solver=../tools/jupiter_te_solver.py \
   --te-recompute-seconds=30 \
   --te-history-windows=120 \
   --te-s=0 \
   --mtp-threads=8'
```

主要参数：

- `--jupiter-te=1`：打开历史预测 TE 和逐流 WCMP；
- `--te-solver=...`：Python LP solver 路径；从 `ns-3-ub` 目录运行时也能自动找到 `../tools/jupiter_te_solver.py`，但显式指定更容易复现；
- `--te-recompute-seconds=30`：每 30 秒重算一次；
- `--te-history-windows=120`：最多使用之前 120 个完整的 30 秒窗口；
- `--te-s=0`：关闭 Jupiter 的 per-path share cap；范围是 `[0,1]`；
- `--mtp-threads=8`：使用 8 个 MTP 线程，需要编译时打开 `--enable-mtp`。

TE 当前要求单 MPI rank。不要用 `mpirun -np 2`（或更大的 rank 数）运行该模式；MTP 多线程可以使用。

## 9. 输出文件

默认 TE 输出目录为：

```text
$CASE_DIR/jupiter_te/
```

主要文件：

```text
observed.csv
prediction.csv
weights.csv
link_bytes.csv
```

含义：

- `observed.csv`：源端成功发出的业务载荷，按 30 秒窗口和 source/destination leaf 聚合；
- `prediction.csv`：每个控制 epoch 使用的预测流量矩阵；
- `weights.csv`：每个 epoch 对直达/单中转候选路径求出的 WCMP 权重；
- `link_bytes.csv`：OCS 逻辑有向边实际成功发送的线速字节，包含报文头和重传。

最后一个不足 30 秒的窗口会以 `complete=0` 写出，不参与后续历史预测。

如果仿真在 30 秒内就结束，这是有效的 smoke test，但不会产生可用于下一次 TE 重算的完整历史窗口。正式比较 TE 结果时，该分支约定排除前 600 秒 warmup。

可以用 `--te-output` 改写输出目录，例如：

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example \
   --case-path=scratch/mooncake_pd_storage_ocs_topology \
   --jupiter-te=1 \
   --te-solver=../tools/jupiter_te_solver.py \
   --te-output=/tmp/jupiter_te_run'
```

## 10. 分析 OCS 链路利用率

仿真结束后回到仓库根目录：

```bash
cd "$REPO_ROOT"

python3 tools/analyze_jupiter_te.py \
  --topology "$CASE_DIR/topology.csv" \
  --link-bytes "$CASE_DIR/jupiter_te/link_bytes.csv" \
  --output "$CASE_DIR/jupiter_te/utilization.csv"
```

默认只统计完整窗口，并排除前 600 秒 warmup。输出 `utilization.csv` 会给出每个窗口最忙的有向 OCS 逻辑边及其利用率。

## 11. 推荐的最小验证顺序

第一次拉取或修改该分支时，推荐按下面顺序定位问题：

```text
1. python3 tools/generate_ocs_topology.py
2. python3 tools/generate_routing_table.py --case-path <case>
3. python3 -m unittest tools/test_jupiter_te.py -v
4. 准备 network_attribute.txt + traffic.csv
5. 编译 ub-quick-example
6. 不开 --jupiter-te 跑 baseline
7. 开 --jupiter-te=1 跑 TE
8. 检查 jupiter_te/weights.csv、observed.csv、link_bytes.csv
9. analyze_jupiter_te.py 计算 post-warmup utilization
```

这样可以把“case 文件问题”“routing table 问题”“Python LP 问题”和“ns-3/TE 对接问题”分开排查。
