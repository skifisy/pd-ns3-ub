# TE / TOE 实际日志回放 v6

FastAPI + 本地 ECharts，固定两排实际拓扑；500 ms 统计，ECMP/TE/TOE 同时钟比较。
本次交付没有运行编译、仿真、网页或测试。smoke 是明确标注的合成输入，不是实验结果。

## 安装与直接观看 smoke

将本目录放到仓库根目录的 `tools/replay_dashboard/`（可覆盖旧版）。

```bash
cd tools/replay_dashboard
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python server.py --smoke --host 0.0.0.0 --port 8000
```

在浏览器打开 `http://服务器IP:8000`。首次准备有文件读取百分比，完成后显示网页。
点击开始播放；支持暂停、拖动、1/0.5/0.2/0.1 倍播放和切换有向 Leaf 对。
可使用 `?autoplay=1` 自动开始。ECharts 已放在本地，不需要 CDN。

## 实际实验：先应用独立的日志 patch

日志 patch 基于原始仓库根目录，基线为：
`bcb10862fc5ea0ab1d64add84e725fc293e71fe8`。
这是完整 patch，不依赖旧版 replay telemetry patch；已经应用旧 telemetry 的工作树不要重复应用。
不修改 TE/TOE 算法、排空方式或路由结果。

保留你原有的 ECMP / TE / TOE 运行参数，每组增加以下参数，例如：

```text
--replay-log=1 --replay-case-id=ecmp --replay-interval=500ms
--replay-scope=all --replay-link-aggregation=pair --replay-queues=0
--replay-detail-windows=220s:280s,400s:460s
```

另两组将 `replay-case-id` 改成 `te` 和 `toe`，保持相同详情范围。
详情范围 `none` 默认不采集；`all` 全程采集，输出量和逐包统计成本也会增加。
时间范围应在实验运行前确定；全局链路热度、拓扑和控制事件仍记录全程。
当前采集器只支持单进程、单线程，MPI/MTP 会显式拒绝。

### 复用任务数据

默认 `--replay-task-events=0`，不重复生成完整 replay 任务事件。
确保原有 TaskTrace 采集开启，实际路径通常是 `<case>/runlog/TaskTrace_node_*.tr`。
任务源优先级：显式 `task_events` / `task_statistics` → 非空 replay_task_events → native TaskTrace。
可通过 `task_trace_directory` 指定搬移后的 TaskTrace 目录。

如不使用原有 TaskTrace，可以显式开启 `--replay-task-events=1`，并在网络属性中关闭重复的原生任务 trace。
本 patch 不会自动修改已有 trace 配置，也不自动开启 TE 的逐流 debug 输出。
两类任务日志均未采集时会报错，不能凭 workload 的投入时间虚构任务完成时间。

`task_statistics` 支持显式规范化 CSV：

```text
task_id,start_ns,end_ns,task_kind,node_id
```

开始或完成时间可留空。原分析脚本的 `task_statistics.csv` 如列名不同，需要先转换到此格式；
不猜测时间单位。任务标识沿用此仓库 traffic.csv 的全局 taskId，重复 workload ID 会报错。

## 实际日志 manifest

复制 `experiment.example.json`，使用绝对路径或相对于 manifest 的路径：

```bash
python server.py --manifest /data/experiment.json --host 0.0.0.0 --port 8000 --cache /data/replay-cache/replay.sqlite
```

`topology` 指向实际使用的 node.csv / topology.csv 目录，`workload` 指向同一份 traffic.csv。
三个 `id` 必须分别匹配各组采集时的 `--replay-case-id`。
`recording.start_seconds/end_seconds` 可限定视频观看范围；不缩减全程 FCT 的统计口径。
输入不同的 traffic 指纹会被拒绝；搬移后的 TE/TOE 审计日志可以使用 `te_logs` / `toe_logs` 指定。

### 吞吐显示倍率与原生任务日志

manifest 顶层可设置 `"throughputScale": 100000`，例如将仿真中的 4 Mbps 显示为 400 Gbps；
不设置时默认为 1。仅链路速率、容量、路径速率和绘图速率尺度按此倍率显示。
利用率、时间、FCT、任务数、原始字节数及队列大小保持原日志值。
这是显示换算，不代表验证了缩小规模仿真与原始网络的性能等价性。

原生 TaskTrace 导入只识别任务级 `MEM Task/WQE Starts/Completes`；
包含 `Segment` 的分段事件会在格式检查前跳过，避免误报或误计任务完成。
其余含 `taskId:` 但不符合任务格式的记录仍会报错。任务解析代码改变后缓存会自动失效并重建。

## 页面口径

- 顶部固定展示全程三组共同完成任务的精确 P50/P90/P99，及相对 ECMP 改善。
- 当前完成数、进行中任务每 500 ms 更新；时间桶向上取整，不提前展示未来完成事件。
- 上排 Leaf，下排计算/存储节点；接入链路也有真实链路流量，行间距压缩。
- 背景颜色为双向较大利用率，线宽为该方向发送速率，三组共用固定尺度；焦点保留方向标注。
- TOE 的链路增减来自实际生效的物理拓扑快照；计划与已生效明确区分。
- 路径表示源 Leaf 成功发送的业务载荷和当时实际第一跳，直达/中转沿仓库的后续最短路约束解释。
  它不是目的端交付量；途中丢包、重配和重传不能被当作已交付。详见 NODE_PATH_LOG_V2.md。
- 详情范围外明确显示未采集；没有路径记录不推算端点归属。
- TE/TOE 精确事件会拆分 500 ms 的基础窗口，页面显示实际窗口区间；短窗口可能小于 500 ms。
  利用率使用分段实际物理容量，不把暂停业务等同于容量归零。
- v1 逐端口日志仍可读取，但保留其原采样间隔，不通过插值伪造 500 ms 的实际数据。

## 3000 万任务的处理方式

workload / 任务记录以最多 20000 行为一批导入 SQLite，任务明细不进入全量 Python 字典。
数据库缓存约 64 MiB；临时排序落盘。这个数值不是整个进程内存的硬上限。
共同任务匹配、精确 FCT 排序使用磁盘表；开始/完成计数生成 500 ms 小型汇总。
首次准备仍要完整读取所需任务日志，需要为明细、索引、临时排序和旧/新缓存同时存在预留磁盘。
没有做 3000 万任务实测，不承诺固定耗时或峰值内存。

网页初始化只接收摘要和每组约 1200 条进度数据（600 秒实验）。
拓扑按 5 秒分块读取，端点路径只查询当前焦点，每类最多缓存 4 块；拖动取消过期请求。
原始 WcmpSelectionTrace 不再全量导入，避免任务级逐流证据撑大内存。
原始节点路径先分批落盘，通过索引按实际窗口匹配；旧格式 path_samples 仅作兼容。
输入或处理代码变化会使缓存失效；正常重开复用已有缓存。

## 供你自行执行的检查

```bash
python tests/check_scalable_v6.py
python prepare.py --manifest smoke/experiment.json --cache cache/smoke-v6.sqlite --force
python server.py --smoke
```

旧版 tests/check_actual_replay.py / check_player.cjs 的断言包含旧任务 API，不作为 v6 验收入口。
本版提供任务磁盘导入、共同集合、精确分位数、500 ms 边界的独立检查。
网页、C++ 编译和真实大 trace 仍请在你的环境验证。
