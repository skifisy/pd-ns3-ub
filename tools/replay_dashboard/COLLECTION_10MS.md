# Replay 日志采集：默认 10 ms，可配置间隔

采集器已包含在当前仓库中，不需要另行应用旧 telemetry 补丁。
用户入口为 `scratch/ub-quick-example`（也适用于 examples 入口）。

## 三组共同设置

保留原有路由、重配和线程参数，只增加：

```text
--replay
```

默认基础采样间隔为 **10ms**。可显式设置：

```text
--replay --replay-interval=10ms
--replay --replay-interval=20ms
--replay --replay-interval=500ms
```

`--replay-interval` 使用带单位的 ns-3 时间，必须严格大于零。
零、负值及在仿真时间精度下舍入为零的值会被拒绝。
该参数只有与 `--replay` 一起使用时生效，单独设置不会开启日志。
`--progress-interval` 只改变任务进度打印频率，与 replay 采样间隔独立。

模式和 case_id 自动推断：ECMP 为 ecmp，Jupiter TE 为 te，TOE-only 为 toe，TOE+TE 为 toe+te。
输出默认在 `<case>/replay/`，全程采集全部链路并按有向节点对聚合；队列采样关闭，路径采集为全程。
当前入口不接受旧版的 replay-log、replay-case-id、replay-scope、replay-link-aggregation、
replay-queues、replay-task-events 或 replay-detail-windows 参数。
ECMP / TE 可保留 `--mtp-threads=8`；TOE 仍有原有控制器单线程限制；MPI 日志暂不支持。
本次不改变线程选择、转发、路由或重配逻辑。

## Dashboard 配置

manifest 顶层窗口必须与实际采集一致：

```json
{
  "window_ms": 10,
  "throughputScale": 1
}
```

以上只是配置片段，实际使用保留 topology、workload 和 cases。
10ms 对应 window_ms=10；20ms 对应 20；500ms 对应 500。
throughputScale 仅负责显示换算，与采样窗口独立；需要把 4 Mbps 显示为 400 Gbps 时才设置 100000。

## 所需记录

- replay_run.csv：默认 sample_interval_ns 的数值为 10000000；覆盖间隔时写入实际解析值。
- replay_topology.csv：完整初始拓扑和实际生效拓扑，含端口和真实带宽。
- replay_link_samples.csv：业务端接入和 Leaf 间有向链路采样。
- replay_events.csv：精确控制事件时间。
- replay_node_path_samples.csv：源 Leaf 实际业务载荷发送与首跳归属。
- 原生 TaskTrace：已启用时自动复用；否则入口自动补写 replay_task_events.csv。

元信息 replay_interval_argument、replay_profile 和启动提示反映所选间隔。
链路和路径使用相同 sample-id、时间边界及 epoch，控制事件可把基础窗口拆短。
接入吞吐需要业务端 → Leaf 出方向采样；采样缺失显示“—”。
接入发送吞吐包含计数中的控制及重传，不等同于目的端去重的有效交付吞吐。

## 旧数据与规模

已有 500 ms 日志保持 manifest window_ms=500；无法拆分或插值为真实 10 ms。
观察 10 ms 变化必须使用新的采集设置重新运行。
600 秒约 60000 个基础窗口/组，基础样本数约为 500 ms 的 50 倍，事件拆分还会增加窗口。
当前入口采集全程路径，选取 dashboard 播放片段不减少采集成本。
本次未运行仿真、编译或测试，未实测 10 ms 全程日志的时间与存储开销。
