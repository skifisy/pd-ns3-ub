# pd-ns3-ub 脚本与 ns-3 仿真运行指南

本文说明当前仓库中 Mooncake PD -> ns-3-UB 的常用运行流程，包括：

1. 生成 OCS 拓扑 (`node.csv` / `topology.csv`)；
2. 独立生成 `routing_table.csv`；
3. 从 Mooncake trace 生成 `task_dag.csv`；
4. 将 DAG 转换成 ns-3-UB 的 `traffic.csv`；
5. 编译并运行 ns-3 仿真。

除特别说明外，下面的命令都在仓库根目录 `pd-ns3-ub/` 下执行。

---

## 1. 生成 OCS 拓扑

运行：

```bash
python3 tools/generate_ocs_topology.py
```

默认会创建：

```text
ns-3-ub/scratch/mooncake_pd_storage_ocs_topology/
├── node.csv
└── topology.csv
```

后续示例统一使用：

```bash
CASE=ns-3-ub/scratch/mooncake_pd_storage_ocs_topology
```

可以先检查：

```bash
ls -lh "$CASE/node.csv" "$CASE/topology.csv"
```

`generate_ocs_topology.py` 只负责物理拓扑，不负责生成路由表；路由表使用下一节的独立脚本生成。

---

## 2. 独立生成 `routing_table.csv`

### 2.1 Mooncake OCS 默认 case

`tools/generate_routing_table.py` 可以单独运行。无参数时默认读取上一节生成的 Mooncake OCS case：

```bash
python3 tools/generate_routing_table.py
```

等价于：

```bash
python3 tools/generate_routing_table.py \
  ns-3-ub/scratch/mooncake_pd_storage_ocs_topology
```

输入：

```text
node.csv
topology.csv
```

输出：

```text
routing_table.csv
```

因此完整的拓扑 + 路由生成命令为：

```bash
python3 tools/generate_ocs_topology.py
python3 tools/generate_routing_table.py
```

检查结果：

```bash
head "$CASE/routing_table.csv"
```

### 2.2 给其他 case 生成路由表

只要 case 目录中已经有合法的 `node.csv` 和 `topology.csv`，就可以直接运行：

```bash
python3 tools/generate_routing_table.py ns-3-ub/scratch/<case-name>
```

查看参数：

```bash
python3 tools/generate_routing_table.py --help
```

也可以覆盖输入/输出文件名：

```bash
python3 tools/generate_routing_table.py ns-3-ub/scratch/<case-name> \
  --node-file node.csv \
  --topology-file topology.csv \
  --output routing_table.csv
```

脚本按 hop 数生成最短路，并保留所有等价最短路径的 egress ports。并行物理链路会作为不同的 `outPorts` 保留；对于多端口 DEVICE，也会保留 `dstPortId`，而不是只按目标 node 生成一条粗粒度路由。

输出格式与 ns-3-UB 当前读取格式一致：

```text
nodeId,dstNodeId,dstPortId,outPorts,metrics
```

其中多个 `outPorts` / `metrics` 使用空格分隔，`metric` 为对应最短路径的 hop 数。

---

## 3. 从 Mooncake trace 生成 `task_dag.csv`

仓库提供了一个小的示例 trace：

```text
input/mooncake/conversation_trace_200.jsonl
```

例如：

```bash
DAG_OUT=/tmp/mooncake_pd_store_dag

python3 tools/mooncake_trace_to_pd_store_dag_v6_layer_pipeline.py \
  input/mooncake/conversation_trace_200.jsonl \
  --config tools/mooncake_pd_store_config_v6_layer_pipeline.json \
  --out-dir "$DAG_OUT"
```

主要输出包括：

```text
$DAG_OUT/task_dag.csv
$DAG_OUT/requests_debug.csv
```

做 smoke test 时，不需要先生成完整 DAG 再用 pandas 手工截断，可以直接限制 request 数量：

```bash
python3 tools/mooncake_trace_to_pd_store_dag_v6_layer_pipeline.py \
  input/mooncake/conversation_trace_200.jsonl \
  --config tools/mooncake_pd_store_config_v6_layer_pipeline.json \
  --max-requests 4 \
  --out-dir "$DAG_OUT"
```

如果使用更大的生产 trace，需要跳过 warm-up request，可以增加：

```text
--warmup-requests <N>
```

注意 `N` 要小于输入 trace 中可用的 request 数量。

---

## 4. 把 DAG 转换成 ns-3-UB `traffic.csv`

推荐使用带 mapping 输出的版本，便于把 ns-3 task 映射回 Mooncake request / DAG task：

```bash
python3 tools/task_dag_to_ub_traffic_v6_layer_pipeline_with_mapping.py \
  "$DAG_OUT/task_dag.csv" \
  --output "$CASE/traffic.csv" \
  --mapping-output "$CASE/traffic_task_mapping.csv" \
  --debug-output "$CASE/traffic_phase_debug.csv"
```

仿真实际读取的是：

```text
$CASE/traffic.csv
```

`traffic_task_mapping.csv` 和 `traffic_phase_debug.csv` 是调试/分析辅助文件，不是 ns-3 case 的必需输入。

---

## 5. 仿真前检查 case 文件

`ub-quick-example` 的基本 case 至少需要：

```text
network_attribute.txt
node.csv
topology.csv
routing_table.csv
traffic.csv
```

`transport_channel.csv` 是可选的；缺失时，当前 runner 可以根据 routing table 为 traffic 自动预留 RTP connection records。

可以检查：

```bash
for f in network_attribute.txt node.csv topology.csv routing_table.csv traffic.csv; do
  test -f "$CASE/$f" || echo "missing: $CASE/$f"
done
```

注意：当前 `generate_ocs_topology.py`、`generate_routing_table.py` 和 DAG/traffic 转换脚本都不会生成 `network_attribute.txt`。运行仿真前需要为 case 准备一份与实验配置匹配的 `network_attribute.txt`。字段说明和已有 case 示例见：

```text
ns-3-ub/scratch/README.md
ns-3-ub/scratch/*/network_attribute.txt
```

不要只因为文件名相同就盲目复制其他实验的参数；尤其要确认 flow control、congestion control、transport、trace 开关等是否符合当前实验。

---

## 6. 编译 ns-3-UB

进入 ns-3 目录：

```bash
cd ns-3-ub
```

### 6.1 单线程 / 非 MTP 仿真

建议先做 UB-only release build：

```bash
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}

python3.12 ./ns3 configure \
  --enable-modules=unified-bus \
  --disable-werror \
  -d release \
  -G Ninja

python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

### 6.2 使用 `--mtp-threads`

如果要使用 MTP，例如 `--mtp-threads=8`，配置阶段必须打开 MTP：

```bash
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}

python3.12 ./ns3 configure \
  --enable-modules=unified-bus \
  --enable-mtp \
  --disable-werror \
  -d release \
  -G Ninja

python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

如果之前在同一个 workspace 中配置过 examples/tests/MPI/MTP 等选项，并且怀疑旧配置残留，可参考 `ns-3-ub/scratch/README.md` 中的显式 reset configure 命令。

---

## 7. 运行 ns-3 仿真

以下命令都在 `ns-3-ub/` 目录执行。

### 7.1 单线程 smoke test

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_storage_ocs_topology --dependency-visibility-delay=1ns'
```

### 7.2 MTP 8 线程

前提是上一节已经用 `--enable-mtp` 编译：

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_storage_ocs_topology --dependency-visibility-delay=1ns --mtp-threads=8'
```

推荐使用显式的 `--case-path=...`。当前 runner 仍兼容 positional case path，但显式参数更清楚，也与 `ns-3-ub/scratch/README.md` 中的当前示例一致。

如果修改了 C++ 后没有重新 build，去掉 `--no-build`，或者先单独执行：

```bash
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

---

## 8. 一套最短的 smoke-test 流程

在仓库根目录执行：

```bash
CASE=ns-3-ub/scratch/mooncake_pd_storage_ocs_topology
DAG_OUT=/tmp/mooncake_pd_store_dag_smoke

# 1) topology
python3 tools/generate_ocs_topology.py

# 2) routing table
python3 tools/generate_routing_table.py

# 3) small DAG
python3 tools/mooncake_trace_to_pd_store_dag_v6_layer_pipeline.py \
  input/mooncake/conversation_trace_200.jsonl \
  --config tools/mooncake_pd_store_config_v6_layer_pipeline.json \
  --max-requests 4 \
  --out-dir "$DAG_OUT"

# 4) traffic
python3 tools/task_dag_to_ub_traffic_v6_layer_pipeline_with_mapping.py \
  "$DAG_OUT/task_dag.csv" \
  --output "$CASE/traffic.csv" \
  --mapping-output "$CASE/traffic_task_mapping.csv" \
  --debug-output "$CASE/traffic_phase_debug.csv"

# 5) make sure network_attribute.txt has been prepared for this case
for f in network_attribute.txt node.csv topology.csv routing_table.csv traffic.csv; do
  test -f "$CASE/$f" || echo "missing: $CASE/$f"
done
```

然后进入 `ns-3-ub/`，完成一次 build 后运行：

```bash
python3.12 ./ns3 run --no-build \
  'scratch/ub-quick-example --case-path=scratch/mooncake_pd_storage_ocs_topology --dependency-visibility-delay=1ns'
```

如果要跑 MTP，则先用 `--enable-mtp` 重新 configure/build，然后增加 `--mtp-threads=<N>`。

---

## 9. 常见问题

### `generate_routing_table.py` 提示缺少输入文件

先确认 case 中有：

```text
node.csv
topology.csv
```

Mooncake OCS 默认 case 可以先执行：

```bash
python3 tools/generate_ocs_topology.py
```

### ns-3 提示 case path 缺失

推荐使用：

```text
--case-path=scratch/<case-name>
```

注意：运行 `./ns3` 时通常已经 `cd ns-3-ub`，因此 case path 是相对 `ns-3-ub/` 的 `scratch/...`，不是仓库根目录下的 `ns-3-ub/scratch/...`。

### 传入 `--mtp-threads` 后提示 MTP 不可用

需要重新 configure/build，并在 configure 时增加：

```text
--enable-mtp
```

### 仿真加载 case 时提示缺少配置文件

检查第 5 节列出的必需文件，尤其是 `network_attribute.txt`、`routing_table.csv` 和 `traffic.csv`。

更完整的 ns-3-UB case 文件格式、字段语义和运行参数请参考：

```text
ns-3-ub/scratch/README.md
ns-3-ub/QUICK_START.md
ns-3-ub/UB_PARALLEL_SIMULATION.md
```
