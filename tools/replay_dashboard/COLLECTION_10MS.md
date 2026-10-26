# v8 日志采集设置：10 ms

此文件与 replay dashboard 代码分开说明采集侧设置，不是新的 C++ 完整 telemetry 补丁。
适用于已经安装此前 v2 replay telemetry 的仿真代码。当前 GitHub main 不含该采集器源码；
未安装时需先应用你持有的完整日志 patch，再使用以下参数。

## 三组共同设置

```text
--replay-log=1
--replay-interval=10ms
--replay-scope=all
--replay-link-aggregation=pair
--replay-queues=0
--replay-detail-windows=220s:280s,400s:460s
```

各组另加 `--replay-case-id=ecmp` / `te` / `toe`，保留原有路由和重配参数。
此处不更改线程数；线程兼容性取决于你实际使用的采集器版本。

manifest 顶层设置：

```json
{
  "window_ms": 10,
  "throughputScale": 100000
}
```

以上只是配置片段，实际使用需保留 topology、workload 和 cases 字段。
throughputScale=100000 仅适用于你确实希望将 4 Mbps 显示成 400 Gbps 的缩放实验；未缩放实验使用 1。

## 所需记录

- replay_run.csv：sample_interval_ns=10000000，正确 case-id 和模式。
- replay_topology.csv：完整初始拓扑和实际生效的重配拓扑，包含端口和真实带宽。
- replay_link_samples.csv：包括所有业务端接入和 Leaf 间链路，按有向节点对聚合。
- replay_events.csv：控制事件和精确生效时间。
- replay_node_path_samples.csv：详情范围内的源 Leaf 实际载荷发送及首跳归属。
- 原生 TaskTrace，或显式规范化的任务事件/统计 CSV。

接入吞吐需要每个业务端 → Leaf 出方向的采样。只有 Leaf 间日志时，页面显示吞吐“—”。
队列采集不是吞吐或利用率的前提，保持关闭即可。
链路和路径必须使用相同 sample-id、时间边界及 epoch。基础窗口 10 ms，事件/详情边界允许拆短。
采样字节按真实发送计数，不能用配置权重替代实际流量。接入总发送吞吐含重传和控制流量。
当前功能不要求新增目的端去重接收计数，也不将发送量解释为有效交付量。

## 旧数据与规模

旧 500 ms 日志需保持 manifest window_ms=500；不能拆成或插值成真实 10 ms。
要观察 10 ms 变化，需重新运行采集。
600 秒约 60000 个基础窗口/组，样本数量约为 500 ms 的 50 倍；路径详情仍建议限定范围。
本次未运行仿真、编译或性能测试，未验证你本地采集器的 10 ms 和多线程运行行为。
