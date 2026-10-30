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

强制复用已有 SQLite，不检查或重新导入原始 trace / task_statistics.csv：

```bash
python server.py --manifest /data/experiment.json --cache /data/replay-cache/replay-v8.sqlite --use-cache --host 0.0.0.0 --port 8000
```

`--use-cache` 仍需要 manifest（读取显示倍率等设置），但其中指向的原始数据文件可以不在本机。
程序只读检查缓存表结构和摘要是否兼容；缓存不存在或不兼容时直接报错，不自动重建。
该模式直接使用缓存内的实验数据，不比较 manifest 与缓存的数据身份，请指定对应的缓存。
`--force` 强制重新导入，与 `--use-cache` 互斥。两项也支持环境变量
`REPLAY_USE_CACHE=1` / `REPLAY_FORCE=1`，适用于直接启动 ASGI 应用。
`prepare.py` 同样支持这两个互斥选项。

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

正常导入模式下，缺少 CSV、CSV 列不完整、时间戳无效、没有有效完成任务或各组没有共同完成任务时，程序会在终端报错并以非零退出码结束，不启动可继续使用的空 Dashboard。`--use-cache` 跳过原始 CSV 检查，直接使用已有统计。

沿用 `traffic.csv` 的全局 `taskId` 校验任务身份。顶部 P50/P90/P99 使用所有实验组共同完成任务的原始 FCT 计算，页面以 ms 展示并保留两位小数；同时保留各组总数、完成数和未完成数。
任务以最多 20000 条一批写入 SQLite；精确分位数使用磁盘排序。数据库缓存约 64 MiB，临时排序落盘。
该配置不是整个进程内存的硬上限，仍需为任务、索引、临时排序和旧/新缓存预留磁盘。
## 吞吐、容量和利用率

顶部每组 ECMP / TE / TOE 卡片统一展示 **P50 / P90 / P99 FCT、全程存储侧双向利用率、全程存储侧双向平均吞吐**。
存储侧统一包含全部 `storage-server → Leaf` 和 `Leaf → storage-server` 有向接入端口：
双向利用率 = `SUM(双向 tx_wire_bytes × 8) / SUM(双向 capacity_bits)`，容量加权；
双向平均吞吐 = `SUM(双向 tx_wire_bytes × 8) / 仿真总时长`。
包含 READ/WRITE、协议头和重传；任一窗口缺少任一预期方向的端口采样则显示「—」，不将缺失值当零。
这里的存储 RX 来自 Leaf 发送计数，表示向存储端发出，不代表应用层已可靠交付。

中间每组网络左侧展示 **存储侧双向吞吐、Fabric 利用率、最大利用率**。

| 指标 | 口径 |
|---|---|
| 存储侧双向吞吐 | 存储节点发送 + Leaf 发往存储节点的 `tx_wire_bytes`，乘 8 再除以真实窗口秒数 |
| 全程存储侧利用率 | 完整实验期间存储接入链路双向总发送比特 / 同口径双向链路的容量时间积分 |
| Fabric 整体利用率 | Leaf 间所有有向链路发送比特数 / 容量时间积分总和 |
| 最大利用率 | 当前窗口已采样 Leaf 间有向链路的最高利用率 |
| 焦点链路发送 | 焦点 A→B 直连物理链路（含并行链路）的窗口平均发送速率 |
| 焦点载荷发送 | 所选 Leaf 方向上，源 Leaf 按实际首跳归属的端点业务载荷发送量，含重传 |

拓扑中每段 Leaf 链路的数字均为该有向链路的**总发送速率 / 容量及利用率**，包含其他
Leaf 对的流量、协议开销和重传，不能解释为当前焦点独占的流量。
节点旁“焦点 发 / 发往”与端点列表只统计当前焦点的载荷，标签按真实数据包方向生成，
例如 `S16 → P0` 或 `S16 → D12`；不按任务发起方向反转 READ_RESPONSE。
路径动画根据源 Leaf 的发送归属展示载荷路径，不代表各跳同一时刻的实测速率，也不代表
接收端去重后的交付速率。

存储侧指标复用两个方向现有的 `tx_wire_bytes` 与 `capacity_bits`，不要求 `read_response_tx_wire_bytes` 字段；
因此可以重处理已有的**完整双向接入采样** Replay 日志，不用重新编译或重新运行仿真。
该指标是物理链路发送口径，包含控制流量、协议开销和重传，并非去重后的业务 Payload Goodput。

不能累加所有 Fabric 链路当作端到端吞吐；中转可能重复计数同一份数据。
存储节点识别与 `topology.py` 一致：非 SWITCH 节点中，64 个 UbPort 为计算端，其余为存储端；请检查实际实验的 node.csv 角色符合这一约定。

`"throughputScale": 100000` 可将 4 Mbps 显示为 400 Gbps。显示速率、容量、路径速率和绘图速率尺度同步缩放。
利用率、时间、FCT、任务数、原始载荷字节和队列大小保持原值。倍率必须为正有限数。
显示换算本身不证明缩小规模仿真与原网络性能等价。

播放新增 0.05×、0.02×、0.01×。Frame 预取按倍率与请求延迟动态调整，默认保留
至少一个前向数据块，最多提前 3 秒；Path 独立预取，Path 请求延迟不阻塞全网播放。
Frame 缓存限 12 块或约 32 MiB，Path 限 8 块或约 16 MiB，优先淘汰远处历史数据。
连续拖动时间轴时 180ms 防抖，松开时立即定位。打开 `?debug=1` 可在浏览器控制台
每 10 秒查看 Frame/Path 延迟、缓存大小、未完成请求和累计缓冲时间。
浏览器内存预算是软限制：当前显示块与刚加载块受保护，单块过大时可能暂时超预算。

## 播放、热点开关与中转节点

- 支持开始、暂停、拖动、从头播放、1/0.5/0.2/0.1 倍播放。
- 新增上一帧、下一帧：按三组实际窗口边界移动，包括事件拆分边界。
- 时间轴是日志中的仿真秒；主时钟显示三位小数，窗口范围显示六位小数。
- 基础统计 10 ms，拓扑最多约 30 次/秒绘制。原速播放可能跳过部分短帧，暂停逐帧或 0.1 倍播放可检查细节。
- 热点开关默认开：背景颜色表示双向较大的利用率，线宽表示该方向的链路总发送速率，各组同尺度。
- 关闭热点：背景为细灰结构线，焦点路径保留颜色、方向、数值；指标和趋势继续更新。
- 实际中转 Leaf 使用独立颜色、节点标签和路径描边，同一 Leaf 在三组中颜色一致。
- 计算节点身份固定：0–11 显示为 `P0–P11`（Prefill），12–15 显示为 `D12–D15`（Decode）。P 使用圆形、D 使用菱形；切换焦点或发送方向不会改变身份和形状。存储节点标记为 S，Leaf 标记为 L；其他编号的计算节点保留 C。
- 焦点面板显示正向与反向利用率；端点节点对按载荷速率聚合排序，最多展示前三项，更多记录只显示省略号。
- TOE 重配同时展示全局新增/拆除连接数量和受影响 Leaf 对数量，并突出当前焦点 Leaf 对的连接数及单方向容量变化。
- 角色颜色仍区分端点，TE/TOE 重配标记独立保留。
- 无端点路径详情时明确标注；不凭热度推断源目的或中转路径，也不会因没有采样而默认显示“直达”。

WCMP 策略路径与当前观测的载荷路径同时展示：

- 虚线为当前生效 epoch 中权重大于零的 WCMP 路径，标注权重，无流量时仍保留。
- 实线描边及动画为当前窗口源 Leaf 观测到的焦点载荷路径；没有载荷时不播放该方向的流量动画。
- 策略按发布事件与窗口的 `te_epoch` 对齐；新 epoch 没有该方向的权重时，不继承旧 epoch 的路径。已固定到旧路径的流量仍可由实际采样独立展示。
- 明细从 `jupiter_te/weights.csv`（或 `.gz`）与发布事件 / `epoch_summary.csv` 导入 SQLite，按焦点与时间块查询；不把全部权重明细发送到初始化响应。
- 缺少策略日志或强制使用不含策略明细的旧缓存时，明确显示“WCMP 策略路径未知”，不会从历史传输推测策略。要显示无流量时的策略路径，需要在具备权重日志时重新导入一次缓存。

## 加载、趋势与缓存

初始化只返回摘要，不发送任务时间戳或全程任务进度数组。
默认按 0.5 仿真秒加载拓扑、当前焦点路径和任务进度（约 50 个基础窗口，可含事件子窗口）。
暂停后保持画面，连续播放预取，拖动/切换焦点取消旧请求，过期响应不会覆盖当前位置。
每类最多缓存 4 块；帧与路径分别以 16 MiB / 8 MiB 原始 JSON 为目标预算，淘汰远处数据。
当前位置和正在预取的块可暂时超过预算；这不是浏览器总堆内存的硬上限。
首次准备仍要读完所需日志。默认通过输入文件路径、大小和修改时间判断数据是否变化；
数据处理代码通过内容 SHA-256 判断，不使用代码文件的修改时间，复制文件或仅执行 `touch`
不会因此触发重新导入。前端显示代码、服务查询代码的变化不使导入缓存失效。
输入、处理代码内容或缓存版本变化时重新导入；`--use-cache` 跳过上述指纹比较。

新增接入吞吐与整体利用率趋势，保留累计完成数、焦点发送/容量、全网热点趋势。
默认每组趋势点数按图表宽度限制，最多 2000 个桶（完成趋势另含起点）。
吞吐按发送总量/总时长聚合；利用率按总发送量/总容量积分聚合；热点保留区间峰值。
趋势区间以原始样本起点筛选，不推算窗口内部的瞬时状态。
点击图表定位播放；“当前位置前后 1 秒”和滚轮放大会重新请求更细数据；“全程趋势”恢复。
三组事件标记与播放游标同步；全程缩略曲线不替代原始 10 ms 窗口数据。

API v8：`/api/frames` 含 cases 和 progress，帧及路径请求最多 2 秒。
`/api/paths` 还返回当前块及块起点生效的 WCMP 策略 `policies`；不含该明细的旧缓存返回 null。
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
