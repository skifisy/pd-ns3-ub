# 单脚本 ToE 分析 v3

运行入口：`tools/analyze_toe_v3.py`。Python 3.9+，仅标准库。
默认过程在生成任务对比记录时直接收集汇总；不重新读取 `task_compare.csv` 或 `task_link_usage.csv`。

代码已直接包含在仓库中，无需另行应用分析脚本 patch。需求矩阵日志和热力图说明见[需求矩阵文档](./TOE_DEMAND_HEATMAPS_V3.md)。

## 从原始实验数据生成全部结果

```bash
python3 tools/analyze_toe_v3.py \
  --baseline /path/to/ecmp_case \
  --toe /path/to/toe_case \
  --output /path/to/analyze_toe_result_v3
```

输出目录必须是新的或空的。原始 case 目录不受修改。
多个实验共享同一个 ECMP 基线：

```bash
python3 tools/analyze_toe_v3.py \
  --baseline /path/to/ecmp_case \
  --case ToE-30 /path/to/toe30_case \
  --case ToE-60 /path/to/toe60_case \
  --output /path/to/comparison_result
```

## 实际日志路径与格式

仓库控制器默认写到 `<case>/toe/`，不是 `<case>/output/`：

```text
<case>/toe/toe_summary.csv
<case>/toe/toe_topology.csv
<case>/toe/toe_reconfiguration.csv
<case>/toe/toe_circuits.csv
<case>/toe/toe_demand.csv
```

脚本同时兼容旧数据的 `<case>/output/`、case 根目录、`runlog/` 和 `output/toe/`。
同名日志内容有冲突会在任务或 trace 导入前报错；实际选用路径会写入 `analysis.log` 和 `analysis_summary.json`。
如果仿真使用了自定义 `--toe-output`：

```bash
python3 tools/analyze_toe_v3.py \
  --baseline /path/to/ecmp_case --toe /path/to/toe_case \
  --toe-log-dir ToE /path/to/custom_toe_output \
  --output /path/to/new_result
```

`--case NAME DIR` 对应 `--toe-log-dir NAME DIR`；ideal replay 可用 `Ideal:NAME`。
指定目录后该 case 的 `toe_*` 日志只从该目录读取，避免混用其他实验。

兼容实际拓扑表头：

```csv
epoch,time_seconds,status,src_leaf,dst_leaf,circuit_count,capacity_gbps_per_direction
```

v5 的 `paused_tp_channels`、`retained_circuits`、`rewired_circuits`、`unpaused_new_payload_bytes` 会映射到重配结果字段。
`toe_circuits.csv` 的 retained/remove/add 是局部规划片段，不会合并成 applied 全量快照。
未应用的 target 不能自行激活；存在独立 applied 事件时可按原分析口径确认该次 target。

ECMP 基线不需要任何 `toe_*` 日志。对于 ToE case，缺少 summary 或 topology 时默认提前失败并打印查找路径，避免运行很久后只得到空的重配表。
确实只需要 FCT 对比时显式使用 `--allow-missing-toe-logs`，缺失数据仍保留 NA。

## 同一次任务遍历的流程

1. 预检所有输入日志，读取任务、拓扑和 ToE 事件。
2. 原始 trace 每个文件按所选模式读取一次，构建观察数据。
3. 从已有 SQLite 任务表查询有效配对任务的精确 ECMP P99；仅读取 FCT 数值，供尾部分类使用。
4. 生成每一行 `task_compare.csv` 的同时更新任务累计值、候选样本和紧凑数值数组/分组索引。
5. 在数值数组上计算精确分位数及各组统计；成员完全相同的组复用结果，原 FCT 分组表直接使用这些统计。
6. 读取小型事件/汇总结果表排版，输出文本和重点 CSV。分组拓扑总量直接复用已解析的 before/after 状态。

精确分位数仍需要排序，SQLite 也有配对和事件范围查询；这里消除的是默认流程对巨大任务 CSV 的再次解析与重复分组统计。
数值数组和索引随有效配对任务数增长；不会保存全量任务行字典。数组在统计完成后释放。
`--include-link-samples` 在默认模式中从 SQLite 按所选 TaskId 查询，不扫描已生成的整张链路 CSV。

## 输出

原始分析表仍包含 `task_compare.csv`、`run_comparison_summary.csv`、`fct_group_summary.csv`、
`reconfiguration_events.csv`、`event_task_cohorts.csv`、`topology_changes.csv`、`hotspot_effect_summary.csv`、
`task_path_usage.csv`、`task_link_usage.csv`。
此外生成：

- `toe_ai_summary_v3.txt`：最多 1500 行、120000 字符。
- `topology_group_changes_v3.csv`：P↔P/P↔D/P↔S/D↔S 每次重配的全组 before/after 链路总量，以及增加、减少、反转等。
- `topology_group_totals_v3.csv`：累计重配统计。
- `task_focus_summary.csv`：统一 ECMP P99 阈值的尾部迁移、角色、操作、大小、容量、暂停暴露和 epoch 分组。
- `task_focus_samples.csv`：重点任务样本，默认全局最多 200 个。
- `demand_matrices.csv`、`demand_epoch_summary.csv`、`demand_heatmaps.html` 与 `demand_heatmaps/*.svg`：精确求解输入与热力图，缺失日志会明确标记。
- `analysis.log`、`analysis_summary.json`：实际输入路径、数据覆盖、分阶段耗时与 pipeline 计数。

默认模式应显示：

```text
pipeline task_compare_csv_read_passes=0 task_link_usage_csv_read_passes=0 task_rows_consumed=...
```

`stage_seconds` 分别列出输入导入、各 case 的 trace/指标、特征构建、P99 数值查询、任务对比与累计、分位数、分析表输出和摘要排版耗时。
缺少 TaskId 的历史 PortTrace 无法恢复每个任务的实际端口或路径；这些字段仍为未知，不会根据连接候选伪造。

## 仅重排已有分析结果

```bash
python3 tools/analyze_toe_v3.py --summary-only /path/to/existing_analysis
```

此显式模式会读取已有 `task_compare.csv`，不读取原始仿真 trace。
如果旧结果目录中的重配表因日志路径错误而为空，单独重汇总无法恢复事件；需用修正版从原始 case 重新分析。

## 验证

```bash
python3 -m unittest discover -s tools/tests -p test_analyze_toe_v3.py
```

测试覆盖真实 case/toe 目录与表头、v5 字段、缺日志提前失败、目录覆盖/冲突、默认零次任务 CSV 重读、
仅汇总与流式结果一致、全组链路数、精确分位数和分组上限回退。
