# ToE 需求矩阵热力图：analyze_toe_v3.py

运行入口仍是一个 Python 文件，保留 v2 的任务/重配分析与流式汇总。
新增需求矩阵 CSV、20×20 Leaf 热力图、3×3 P/D/S 有向汇总图及相邻重配决策的需求差值图。
Python 3.9+，仅标准库；SVG 与 HTML 无须安装 matplotlib。

## 先记录精确的求解器输入

历史 toe_summary.csv、toe_topology.csv、toe_circuits.csv 均不含求解器需求矩阵。
不能根据拓扑配额或 task 的逻辑数据量精确反推这一矩阵。
本版本控制器已内置需求矩阵记录。更新代码后，按[运行手册](./TOE_DYNAMIC_TOPOLOGY_RUN.md)重新编译，
再按原命令运行仿真，无需另行应用 patch。此改动只新增日志，不修改需求计算或求解/重配策略。
日志写到原有 ToE 输出目录（默认 `<case>/toe/toe_demand.csv`）；自定义 `--toe-output` 也生效。

控制器在每次调用 Solve 前，按同一个 epoch 导出实际使用的有向输入，每轮完整导出 20×20 个单元，包含零值和对角线。
即使本轮没有改变拓扑，也记录输入，便于对照需求稳定时求解器是否保持拓扑。
日志使用 17 位有效数字保留 double 输入精度。

CSV 字段：

```csv
epoch,decision_seconds,src_leaf,dst_leaf,demand_bps,demand_gbps,peak_window_bytes,latest_window_bytes,latest_gbps,sample_window_seconds,history_windows,completed_window,matrix_complete,matrix_kind
```

`demand_bps` 是传给 Solve 的数值；`demand_gbps=demand_bps/1e9`。
当前预测规则为历史 30 秒窗口中逐 OD 的最大已发送 payload 字节数 ×8÷30；不同 OD 的最大值可能来自不同窗口。
这张矩阵是求解输入，不是重配生效时刻的瞬时流量，也不表示尚未发送的全部积压需求。
`latest_gbps` 单独保存最近已完成窗口值，与历史峰值输入区分。

## 分析时自动输出

```bash
python3 tools/analyze_toe_v3.py \
  --baseline /path/to/ecmp_case --toe /path/to/toe_case \
  --output /path/to/new_result_v3
```

默认只画实际重配对应的需求矩阵，保留全部决策的 CSV。
要画包括 no_change 在内的全部决策：增加 `--demand-heatmaps all`。
已有 `--toe-log-dir CASE DIR` 目录覆盖也用于查找 toe_demand.csv。
可用 `--demand-file CASE /path/to/toe_demand.csv` 显式指定。

新增输出：

- `demand_matrices.csv`：全部已记录 epoch 的有向原始数值，保留决策时间、窗口信息、最新窗口值与覆盖情况。
- `demand_epoch_summary.csv`：每轮决策时间、拓扑生效时间、重配状态、已知需求总和、最大 OD 和图文件路径。
- `demand_heatmaps.html`：浏览入口，每次重配同时展示 Leaf 矩阵、P/D/S 汇总、需求差值，以及分组链路数 before/after。
- `demand_heatmaps/*.svg`：可单独导出的矢量图。
- 原有 1500 行以内的 AI 摘要增加每轮矩阵覆盖、需求总和及最大 OD 概览。

横轴是目的 Leaf，纵轴是源 Leaf；P=37–48、D=49–52、S=53–56。
需求矩阵保留方向，与无向链路配额分组区分。
所有已展示 Leaf 矩阵使用同一线性色标；角色汇总图有自己的全局色标，需求差值图有共享的正负色标。
需求差值=当前决策输入−上一次展示的决策输入；默认是相邻两次实际重配，all 模式则是相邻决策。
上一轮缺少矩阵时中断差值比较，不跨缺失数据补算。

完整矩阵中明确记录的 0 表示零值；稀疏输入的未提供单元用 NA/阴影表示。
声称 matrix_complete=1 却不足 400 单元的输入会报错，防止截断文件被当成零需求。
有 solved 时间时会检查矩阵的 decision_seconds 与对应事件一致。

## 只画图，不重新分析任务

已经有同一次实验的分析结果和需求日志时：

```bash
python3 tools/analyze_toe_v3.py --demand-only /path/to/analysis_result \
  --demand-file ToE /path/to/toe_demand.csv
```

此模式只读取小型事件/拓扑结果和矩阵文件，不读 task_compare.csv，也不扫描原始 trace。
已归档了 demand_matrices.csv 时，省略 --demand-file 即可。
`--output /path/to/figures` 可将 CSV、HTML 与 SVG 放到其他目录。

旧实验没有精确矩阵日志时，输出会标记 missing_solver_demand_log，不能用全零图代替缺失数据。
仅重新运行分析脚本无法补出过去没有记录的求解输入。

## 验证

```bash
python3 -m unittest discover -s tools/tests -p test_analyze_toe_v3.py
```

新增测试覆盖有向矩阵、Bps/Gbps 单位、完整性/缺失、决策与生效时间区分、no_change 展示、
差值参照、单次矩阵读取、demand-only 零任务/trace 扫描及 SVG XML 转义。
控制器日志改动已独立编译和执行新增的日志代码块，验证 400 单元及数值；未进行完整 ns-3 构建或仿真。
