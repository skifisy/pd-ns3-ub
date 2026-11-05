# TTFT / TPS 分析脚本说明

这个目录主要放的是针对 Prefill / Decode 相关链路的离线分析脚本，目的是在已有 ns-3 trace、DAG 和 traffic 输出的前提下，比较不同路由方案（ECMP / TE / TOE / EPS-2L）的请求级和流水线级表现。

其中包含两个核心脚本：

- `analyze_prefill_pipeline_v5.py`
- `stat_request_compare_v4.py`

它们都不是模拟器本身，不会去跑 ns-3；它们的作用是读取现有输出结果，对已有 trace 做后处理与汇总分析。

## 1. `analyze_prefill_pipeline_v5.py`

这个脚本更偏“流水线视角”，关注某个 Prefill batch 在读、计算、写过程中的时序分布。

### 主要作用

- 读取当前逻辑 DAG 和 task statistics，分析 Prefill 过程中各阶段的时间占比。
- 把网络读、计算和写入拆成更细的流水线结构，看它们在各个 batch / layer 中如何交错。
- 统计每个 Prefill batch 的关键时间窗口，包括：
  - PREFIX_READ
  - PREFIX_STORE_WRITE
  - 计算阶段推断/对齐
  - 细粒度 request / layer 整体时序
- 对某些 batch 做筛选、聚合和明细分析，便于定位瓶颈。

### 典型输出

脚本会输出：

- `pipeline_report.html`：交互式 HTML 报告，方便查看整体时序和各阶段的趋势。
- `pipeline_batch_summary.csv`：按 batch 汇总的结果。
- `pipeline_layer_detail.csv`：按 layer 级别的详细分析。
- `pipeline_request_detail.csv`：按 request 级别的明细。

### 适合看什么

如果你想回答下面这些问题，这个脚本最适用：

- 这个 Prefill batch 在计算和网络写入之间有没有明显等待？
- 某一层的读/写时序是 bottleneck 吗？
- 不同方案下同一个 batch 的阶段重叠程度是否不同？
- 哪些请求/层的异常拖慢了整个 Prefill 流水线？

### 调用方式

脚本文件头部给了一个示例：

```bash
python3 analyze_prefill_pipeline_v5.py \
  --case DAG_OUTPUT_DIR \
  --ecmp-dir ECMP --te-dir TE --toe-dir TOE --eps2-dir EPS2 \
  --mapping DAG_OUTPUT_DIR/mapping.csv \
  --out-dir pipeline_analysis
```

它本质上是“基于现有 DAG + mapping + task_statistics 做后验分析”，不是单纯的统计汇总脚本，而是更强调“流水线结构”。

## 2. `stat_request_compare_v4.py`

这个脚本更偏“跨方案对比”，关注请求级的指标，并把不同拓扑/调度方案进行统一比较。

### 主要作用

- 对比 ECMP / TE / TOE / EPS-2L 四种方案下的请求表现。
- 逐请求计算关键指标，包括：
  - TTFT（Time To First Token）
  - decode load / compute 时间
  - TPS
  - completion time
  - logical read throughput
- 统计每个 scheme 的 percentiles，例如：
  - TTFT P50 / P90 / P99 / P999
  - FCT P50 / P90 / P99 / P999
- 过滤出适合所有方案共同比较的有效请求子集，保证结果对比公平。

### 典型输出

脚本输出两份 CSV：

- `request_comparison.csv`：按请求维度给出每种方案的结果，适合详细查看单个请求表现。
- `summary_comparison.csv`：按 scheme 汇总统计，适合快速比较整体性能。

### 适合看什么

如果你想回答下面这些问题，这个脚本最适用：

- 在不同方案下，TTFT 的中位数和尾部延迟分别怎样？
- 哪种方案的 TPS 更高？
- decode 阶段的负载和吞吐是否有明显差异？
- 终端请求流完成时间（FCT）在四种方案之间有什么差别？

### 调用方式

脚本头部给了示例：

```bash
python3 stat_request_compare_v4.py \
  --case /data/CASE \
  --ecmp-dir /data/ecmp --te-dir /data/te --toe-dir /data/toe \
  --eps2-dir /data/eps_2layer \
  --data-size-divisor 100000 --num-layers 78 \
  --out-dir /data/compare
```

它是“按请求和方案做统一指标比较”的脚本，更偏结果对比和性能归纳，而不是阶段时序拆解。

## 3. 两者的关系

这两个脚本互补：

- `analyze_prefill_pipeline_v5.py`：看“哪里慢、为什么慢、慢在Pipeline的哪一段”。
- `stat_request_compare_v4.py`：看“不同方案到底在请求级和整体上差多少”。

如果你在研究某个方案表现异常，通常的顺序是：

1. 先跑 `stat_request_compare_v4.py`，快速看各方案在 TTFT / TPS / FCT 上是否存在显著差异。
2. 然后用 `analyze_prefill_pipeline_v5.py`，定位具体 batch / layer / request 上的瓶颈来源。

## 4. 输入数据特点

这两份脚本都依赖以下类数据：

- `task_dag.csv` / DAG 输出
- `traffic.csv` 或 mapping
- `requests_debug.csv`
- 各方案下的 `output/task_statistics.csv`
- 对应的 batch / request / layer 级元信息

它们都采用“读取已有 trace，做离线分析”的方式，不改变 simulation 逻辑，也不重跑模拟器。

## 5. 结论

这个目录的核心价值在于：把 ns-3 生成的 trace 转换成更容易解释的性能指标与时序视图。对研究 Prefill pipeline、TTFT、TPS 和整体网络 FCT 非常有用。

如果你要做规模比较、性能调优或瓶颈定位，这两个脚本是最关键的后处理工具。