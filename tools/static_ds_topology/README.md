# D–S 静态增容拓扑

从原始 Mooncake case 生成一次静态容量重分配：每个具体的 P–S pair 拆一根，
共 `12 × 4 = 48` 根；每个 P–D pair 同样拆一根，释放 48 个 D 端口。
16 个 D–S pair 各增加三根，每个跨组 P–P pair 各增加一根。
所有 leaf 仍保持 64 个 OCS 端口。脚本只生成文件，不启动仿真，不修改 ToE 控制器。

## 使用

需要 Python 3.9+，仅使用标准库和仓库现有的 `tools/generate_routing_table.py`。
在仓库根目录执行：

```bash
python3 tools/static_ds_topology/build_static_ds_topology.py \
  --base-case /path/to/original_case \
  --out-dir /path/to/static_ds_case
```

两个参数均必填。输出目录必须不存在或为空，且不能与原 case 相同、互相嵌套。
脚本在临时目录内完成生成与验证，再发布最终目录；失败不会留下部分生成的 case。
不覆盖已有文件，不改写原 case。

## 输入与输出

必需输入为 `node.csv`、`topology.csv`。适用范围固定为当前 Mooncake：

- 计算服务器 0–15，各 64 端口；存储服务器 16–36，各 12 端口。
- Prefill leaf 37–48、Decode leaf 49–52、Storage leaf 53–56，各 128 端口。
- leaf 按连续四台分组，跨组每个 pair 初始恰好四根 OCS 电路。
- OCS 使用端口 64–127，链路 `bandwidth` 为 `400Gbps`，OCS 传播时延一致。
- 服务器接入关系与 `tools/generate_ocs_topology.py` 相同。

只接受原始均匀拓扑，不接受将已扩容的拓扑再次作为输入。

| 输出文件 | 内容 |
|---|---|
| `node.csv` | 原样复制原节点表 |
| `topology.csv` | 新的静态连接，复用原物理端口，保留服务器接入链路 |
| `routing_table.csv` | 按新拓扑重新生成，检查所有目的端口的最短路输出集合 |
| `static_ds_changes.csv` | 48 组交换的实际拆除/新增端点，共 192 条记录 |
| `static_ds_summary.json` | 前后容量、每 leaf 分配、D–S 矩阵、输入缺失项、文件 SHA256 |

若原 case 存在 `traffic.csv`、`network_attribute.txt`、`transport_channel.csv`，则原样复制。
前两项缺失时仍生成三个拓扑文件，但明确标记为 topology-only，不能直接运行完整实验。
不会复制旧的路由表、仿真输出或大型 DAG 分析文件。

## 一次调整的确定性规则

对 `i = 0..11`、`t = 0..3`：

```python
p = 37 + i
g, r = divmod(i, 4)
q = 37 + 4 * ((g + 1) % 3) + ((r + t) % 4)
d = 49 + t
s = 53 + r
# 拆 p–d、q–s，建 d–s、p–q；每个 donor bundle 选择端口序最小的电路。
```

`p` 与 `q` 始终在不同组。D–S 使用释放的 D、S 端口；P–P 使用释放的 P 端口。
每台 P 对四台 D、四台 S 分别各拆一根，共拆八根；这些 pair 均从四根降至三根。
每个跨组 P–P pair 各增加一根，由四根增至五根。这不是现有
`--toe-max-prefill-remove=1` 的计数语义。

| 电路类别 | 原始 | 调整后 |
|---|---:|---:|
| P–P | 192 | 240 |
| P–D | 192 | 144 |
| P–S | 192 | 144 |
| D–S | 64 | 112 |
| 总 OCS 电路 | 640 | 640 |

| D / S | 53 | 54 | 55 | 56 | 合计 |
|---|---:|---:|---:|---:|---:|
| 49 | 7 | 7 | 7 | 7 | 28 |
| 50 | 7 | 7 | 7 | 7 | 28 |
| 51 | 7 | 7 | 7 | 7 | 28 |
| 52 | 7 | 7 | 7 | 7 | 28 |

D–S 每方向总容量从 25.6 Tbps 增至 44.8 Tbps，每台 D/S 的 D–S 电路从 16 根增至 28 根。
每个 D–S pair 从四根增至七根，每方向容量从 1.6 Tbps 增至 2.8 Tbps（增加 75%）。
P–S 总容量同时减少 25%，需要在仿真中评估对 Prefill 读写性能的影响。
保留所有跨组 pair、所有物理端口和接入链路，验证端口唯一性与路由完整性。

## 仿真对照

原始与新拓扑两组均设置 `--toe-mode=off --jupiter-te=0`，并使用相同 workload、
计算时延、流控、重传、随机种子和线程数。沿用原实验的其他必要命令行参数。
不要在该静态对照中传 `--toe-max-prefill-remove`，因为 ToE 已关闭。

脚本不核实 P–D 的业务/中转负载；容量变化不能代替实测性能。
先比较 D–S 读写 FCT、Prefill 读写 FCT、请求完成率与完成时间，再检查物理端口利用率。
生成后的路由表是新输入；不能继续使用原始路由表。

## 验证

```bash
python3 -m unittest discover \
  -s tools/static_ds_topology/tests \
  -p 'test_build_static_ds_topology.py' -v
```

测试使用原始拓扑生成器建立完整输入，覆盖精确容量、每个具体 pair 的链路数、
端口复用、输入保留、确定性、路由实际出端口、重复应用/覆盖拒绝、异常清理和 CLI 输出。
无需编译 ns-3，也不依赖 HiGHS。
