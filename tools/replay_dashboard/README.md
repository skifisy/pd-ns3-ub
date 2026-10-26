# TE / TOE 实际日志回放 v8

FastAPI + 本地 ECharts。默认基础窗口 **10 ms**，三组共用仿真时间轴、拓扑位置和热度尺度。
本次交付未运行仿真、导入器、网页或测试；附带合成 CSV 和供你自行执行的检查。

## 安装与启动

将本目录放到仓库根目录 `tools/replay_dashboard/`。代码包包含本地 ECharts，不依赖 CDN。

```bash
cd tools/replay_dashboard
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python server.py --smoke --host 0.0.0.0 --port 8000
```

`--smoke` 使用新目录 `smoke10ms/` 和独立缓存 `cache/smoke-v8.sqlite`。
这是明确标记的 1 秒合成 CSV，并非 ns-3 实验结果。原来的 `smoke/` 不会被覆盖。

实际实验：

```bash
python server.py --manifest /data/experiment.json --cache /data/replay-cache/replay-v8.sqlite --host 0.0.0.0 --port 8000
```

## 10 ms 采集与旧日志

仿真采集器已包含在仓库中。保留原有 ECMP / TE / TOE 参数，加 `--replay` 即按默认 10 ms 采集。
可通过可选参数覆盖间隔：

```text
--replay                         # 默认 10ms
--replay --replay-interval=10ms   # 显式指定 10ms
--replay --replay-interval=500ms  # 使用旧粒度
```

间隔使用带单位的 ns-3 时间，必须严格大于零。单独设置 replay-interval 不会开启采集。
`--progress-interval` 是独立的终端进度打印间隔，不改变 replay 窗口。
模式和 case-id 根据原有 TE/TOE 参数自动识别；全程采集所有链路并按有向节点对聚合，队列采样关闭。
当前入口固定采集全程路径，不接受旧版 `--replay-log`、`--replay-case-id`、`--replay-scope`、
`--replay-link-aggregation`、`--replay-queues` 或 `--replay-detail-windows` 参数。
本地 MTP（包括请求 8 线程）可用于 ECMP / TE 日志；TOE 仍受原有控制器单线程限制，MPI 日志不支持。

在 manifest 中设置 `"window_ms": 10`。运行元数据 `sample_interval_ns` 和实际窗口长度会被校验。
TE/TOE 发布和拓扑生效仍按精确时间拆分，实际窗口可以小于所配置的基础间隔。
旧 500 ms 数据不能还原成 10 ms；如需观看旧数据，显式保留 `"window_ms": 500`。
三组采样间隔必须与同一个 manifest 一致，禁止把粗采样改标签后充当细采样。
约 600 秒对应每组 60000 个基础窗口，采样记录数约为 500 ms 的 50 倍；未进行大规模开销实测。
详见 [COLLECTION_10MS.md](COLLECTION_10MS.md)。

## 数据来源与任务口径

复制 `experiment.example.json`，路径可为绝对路径或相对于 manifest：

- `topology`：实际运行使用的 node.csv / topology.csv 目录。
- `workload`：共同的 traffic.csv，用于任务总数和稳定任务标识。
- 每组 `id`：必须与采集时 case-id 一致。
- 每组必须提供对应的 `output/task_statistics.csv`；可通过 `task_statistics` 显式指定文件路径。
- `te_logs` / `toe_logs`：可指定搬移后的控制日志目录。
- `recording.start_seconds/end_seconds`：限定播放范围，不缩减全程 FCT 统计。

**任务/FCT 数据唯一来源是每组的 `output/task_statistics.csv`。** 不读取 `.tr`，不读取 `replay_task_events.csv`，也不会在文件缺失时自动生成或回退到其他来源。

CSV 必须包含以下列：

```text
taskId,sourceNode,destNode,dataSize(Byte),opType,taskStartTime(us),taskCompletesTime(us)
```

`taskStartTime(us)` 和 `taskCompletesTime(us)` 的单位是微秒，导入时直接换算；空值或 `NaN` 表示对应时间缺失。每组 CSV 至少要有一个开始、完成时间均有效且完成时间不早于开始时间的任务；各组还必须至少有一个共同完成的 taskId，否则准备过程报错。

如果未设置 `task_statistics`，程序会按 case 的 `path` 推导固定文件位置：当 `path` 以 `replay/` 结尾时读取其上一级目录下的 `output/task_statistics.csv`，否则读取 `path/output/task_statistics.csv`。显式路径也必须指向 `output/task_statistics.csv`，不会接受其他文件名或 `.gz` 文件。

缺少 CSV、CSV 列不完整、时间戳无效、没有有效完成任务或各组没有共同完成任务时，程序会在终端报错并以非零退出码结束，不启动可继续使用的空 Dashboard。

沿用 `traffic.csv` 的全局 `taskId` 校验任务身份。顶部 P50/P90/P99 使用所有实验组共同完成任务的原始 FCT；同时保留各组总数、完成数和未完成数。
任务以最多 20000 条一批写入 SQLite；精确分位数使用磁盘排序。数据库缓存约 64 MiB，临时排序落盘。
该配置不是整个进程内存的硬上限，仍需为任务、索引、临时排序和旧/新缓存预留磁盘。
## 吞吐、容量和利用率

每组拓扑左侧新增接入发送吞吐、Fabric 整体利用率，并保留最大利用率和热点方向数。

| 指标 | 口径 |
|---|---|
| 网络接入发送吞吐 | 所有业务端 → Leaf 的 tx_wire_bytes 合计 × 8 / 实际窗口秒数 |
| Fabric 整体利用率 | Leaf 间所有有向链路发送比特数 / 容量时间积分总和 |
| 最大利用率 | 当前窗口已采样 Leaf 间有向链路的最高利用率 |
| 焦点链路发送 | 焦点 A→B 直连物理链路（含并行链路）的窗口平均发送速率 |
| 节点载荷发送 | 源 Leaf 按实际首跳归属的端点业务载荷发送量，含重传 |

接入发送吞吐包含发送计数中的控制、重传，不是目的端去重的有效吞吐 Goodput。
不能累加所有 Fabric 链路当作端到端吞吐；中转会重复计数同一份数据。
只有全部预期业务端出方向都有采样时才展示接入总吞吐；缺失显示“—”，不填零。
整体利用率只在预期 Fabric 有向节点对都有采样时给出，使用容量加权，避免小链路与大链路等权。
利用率分母是容量时间积分，窗口中发生重配时不会将新带宽套到旧时段。
链路的实际序列化开销口径由采集器 tx_wire_bytes 定义，不推算目的端是否交付。

`"throughputScale": 100000` 可将 4 Mbps 显示为 400 Gbps。显示速率、容量、路径速率和绘图速率尺度同步缩放。
利用率、时间、FCT、任务数、原始载荷字节和队列大小保持原值。倍率必须为正有限数。
显示换算本身不证明缩小规模仿真与原网络性能等价。

## 播放、热点开关与中转节点

- 支持开始、暂停、拖动、从头播放、1/0.5/0.2/0.1 倍播放。
- 新增上一帧、下一帧：按三组实际窗口边界移动，包括事件拆分边界。
- 时间轴是日志中的仿真秒；主时钟显示三位小数，窗口范围显示六位小数。
- 基础统计 10 ms，拓扑最多约 30 次/秒绘制。原速播放可能跳过部分短帧，暂停逐帧或 0.1 倍播放可检查细节。
- 热点开关默认开：背景颜色表示双向较大的利用率，线宽表示发送速率。
- 关闭热点：背景为细灰结构线，焦点路径保留颜色、方向、数值；指标和趋势继续更新。
- 实际中转 Leaf 使用独立颜色、节点标签和路径描边，同一 Leaf 在三组中颜色一致。
- 角色颜色仍区分端点，TE/TOE 重配标记独立保留。
- 无端点路径详情时明确标注；不凭热度推断源目的或中转路径。

## 加载、趋势与缓存

初始化只返回摘要，不发送任务时间戳或全程任务进度数组。
默认按 0.5 仿真秒加载拓扑、当前焦点路径和任务进度（约 50 个基础窗口，可含事件子窗口）。
暂停后保持画面，连续播放预取，拖动/切换焦点取消旧请求，过期响应不会覆盖当前位置。
每类最多缓存 4 块；帧与路径分别以 16 MiB / 8 MiB 原始 JSON 为目标预算，淘汰远处数据。
当前位置和正在预取的块可暂时超过预算；这不是浏览器总堆内存的硬上限。
首次准备仍要读完所需日志。输入或处理代码变化使缓存失效；正常重开复用缓存。

新增接入吞吐与整体利用率趋势，保留累计完成数、焦点发送/容量、全网热点趋势。
默认每组趋势点数按图表宽度限制，最多 2000 个桶（完成趋势另含起点）。
吞吐按发送总量/总时长聚合；利用率按总发送量/总容量积分聚合；热点保留区间峰值。
趋势区间以原始样本起点筛选，不推算窗口内部的瞬时状态。
点击图表定位播放；“当前位置前后 1 秒”和滚轮放大会重新请求更细数据；“全程趋势”恢复。
三组事件标记与播放游标同步；全程缩略曲线不替代原始 10 ms 窗口数据。

API v8：`/api/frames` 含 cases 和 progress，帧及路径请求最多 2 秒。
`/api/completion`、`/api/trend`、`/api/global-trend` 接受 start/end/points，按需返回有限趋势点。
`/api/bootstrap` 不再包含 completionBins；使用旧 API 的自定义前端需同步更新。

## 自行执行检查

```bash
python tests/check_scalable_v6.py
python tests/check_replay_v8.py
python prepare.py --manifest smoke10ms/experiment.json --cache cache/smoke-v8.sqlite --force
python server.py --smoke
```

v8 检查覆盖 10 ms 窗口、事件拆分、精确 FCT、缺失接入采样、吞吐避免多跳重复、
容量加权、显示倍率换算、拖动时任务进度种子及采样间隔不匹配。
网页还请手工检查开始/暂停/拖动、逐帧、热点开关、中转颜色和趋势缩放。
以上命令本次均未执行，未进行真实 3000 万任务或 10 ms 全程日志的性能验证。
