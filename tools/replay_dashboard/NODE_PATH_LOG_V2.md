# 路径日志 v2：实际源 Leaf 发送归属

输出 `replay_node_path_samples.csv`：

```
sample_id,case_id,start_ns,end_ns,topology_epoch,te_epoch,toe_decision_epoch,src_node,dst_node,src_leaf,dst_leaf,transit_leaf,tx_payload_bytes,tx_packets
```

窗口、sample_id、三个 epoch 与链路采样精确关联。时间单位 ns，保留 0.001 ns 精度。
基础窗口 500 ms；策略发布、重配阶段、物理连接生效和详情范围边界会拆开窗口。

源/目的设备从实际数据包 IPv4 地址恢复；接入 Leaf 从真实设备端口的物理连接恢复。
URMA WRITE 与 READ_RESPONSE 载荷均按实际包方向记录，不套用任务发起方向。
控制、ACK、READ 请求不作为业务载荷计入；当前实现不对 LDST/CTP 提供端点路径详情。

只在源 Leaf 的实际 fabric 发送成功后累计一次，包含重传，不需要维护 taskId/flowHash 生命周期表。
目的 Leaf 是实际目的设备端口所接 Leaf。transit=-1 表示发送到目的 Leaf；否则是实际第一跳的中转 Leaf。
后续最短路由由原仓库逻辑约束；不是逐跳接收确认，更不是端到端交付。
若重配后该中转到目的 Leaf 已无直接连接，不输出误导路径，并累计 `unattributed_path_packets` 元信息。
队列中的包在详情范围开始后才发送时，成功发送处可以恢复元数据，避免因入队时间较早漏计。

链路 CSV 默认 `aggregation=node_pair`，`src_port=dst_port=-1`，`circuit_count` 为并行链路数，
`data_rate_bps` 是总物理速率，`capacity_bits` 是当前分段容量时间积分。
初始/重配拓扑 CSV 仍保留物理端口明细用于校验和链路增减。

按窗口/端点/路径聚合，范围外不做详细路径计数。`--replay-detail-windows=none|all|220s:280s,400s:460s`。
`te_epoch` 表示发送窗口内当前生效策略，不能解释成所有已排队包最初选择路由时的策略版本。

后端批量写入临时磁盘表，与链路窗口/版本/拓扑连接校验后转到独立路径表。
`/api/frames` 不嵌入所有端点路径；`/api/paths` 按当前 Leaf 对和最多 10 秒范围返回。
