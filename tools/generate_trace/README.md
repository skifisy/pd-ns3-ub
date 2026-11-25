# Mooncake trace 与物理端口

`mooncake_trace_to_dag_v15.py` 生成 DAG，随后
`mooncake_dag_to_traffic_v15.py` 生成可直接执行的 traffic。两个脚本及配置
均位于本目录；`run_mooncake_v15_glm51.sh` 从自身目录解析脚本位置。

## 存储节点与端口

- 计算端口使用选定 TP 组中每个 rank 对应的 `(node, port)`。
- 相同配置下，KV block 的存储节点保持旧映射：
  `slot = stable_hash("kv-storage-endpoint", key) % (节点数 * 端口数)`，
  `node = storage.nodes[slot // 端口数]`。
- 一个存储节点的所有端口共享该节点的全部数据；数据不归属于某个端口。
- 每次访问的起始端口下标为
  `stable_hash("storage-access-port-v1", seed, request_id, key, stage) % 端口数`。
- 实际端口取 `storage.ports[(起始下标 + max(layer,0) + rank) % 端口数]`。
  使用列表下标，支持不连续的物理端口编号。无层编号的最终 Decode 写入按层偏移零处理。
- `storage.port_selection_seed` 控制端口分配，默认沿用 `scheduling.random_seed`。
  节点映射不受此 seed 影响。相同输入、配置和 seed 可复现，不依赖 taskId、线程顺序或路由权重。

Prefill 写入与 Decode 读取同一个 key 时，节点相同，端口可以不同。
不同 block 仍按 key 分散到不同节点，也可能碰撞到同一个节点。
TP8 在至少八个端口时，同层各 rank 使用不同端口；TP2 的单层访问使用两个端口。
后续层轮换端口；本次不增加每个 rank 的传输流数。

生成器的任务预测和候选 TP 组评分查询相同规则选出的物理端口。
评分保持原有估计方法，预测不等于 ns-3 实测时间。端口并行度变化可能影响预测、
排队和调度结果；策略及 KV 数据依赖语义保持原样。
`summary.json` 包含 `storage_port_policy`、`storage_port_selection_seed` 和
按阶段、节点、端口统计的 `storage_access` 流数及原始字节数。

## traffic 字段与方向

```csv
taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases,sourcePort,destPort
```

网络任务必须有物理端口，TIMER 的端口留空。完整 DAG 使用 `src_port/dst_port`，
逻辑 TP DAG 使用每条 `tp_members` 的端点。READ 转换时同时交换节点和端口：
DAG 数据方向 `storage:9 -> compute:6`，生成 READ 操作 `compute:6 -> storage:9`。
超过 uint32 的传输拆成多行后，每行保留同一端点；`--data-size-divisor` 不改变端点。
`traffic_debug.csv` 与原生 traffic 使用同一列序，再附加 `request_id`；mapping 的
`src_port/dst_port` 与原生 traffic 的 `sourcePort/destPort` 一致。

新 traffic 需要应用本次仿真端补丁。仿真按表头解析字段，旧九列格式继续自动选端口。
已有实体字段与物理端口独立。显式端口目前支持 RTP URMA READ/WRITE；CTP 和 MEM
收到显式端口时明确报错。CTP 的 64 端口支持还需要扩展其 CNA16 寻址。
物理端口限制为 `0..254`，并必须指向对应节点的 UB 设备。

## 验证

在仓库根目录：

```bash
python3 -m unittest discover -s tools/tests -p 'test_mooncake_*.py'
```

在 ns-3-ub 目录构建后：

```bash
./ns3 build -j 4 test-runner ub-quick-example
./build/utils/ns3.44-test-runner --suite=unified-bus-traffic-ports --verbose
```

在仓库根目录执行实际收发回归（临时小用例，不运行完整 trace）：

```bash
python3 tools/tests/check_traffic_physical_ports.py \
  --binary ns-3-ub/build/scratch/ns3.44-ub-quick-example
```

该回归检查 multipath 和逐包选路下的端口绑定、READ 返回数据的实际出口、
旧 CSV、列重排，以及缺失、越界、不可达端口和 CTP 显式端口的报错行为。

端口回归覆盖新旧 CSV、列重排、端口零、实体字段共存、runtime 字段传递、
TP 缓存命中、首次建连、反向端点及连接复用。Python 回归覆盖 rank 分散、逐层轮转、
存储节点映射不变、READ 方向以及拆分与缩放后的端口保留。
