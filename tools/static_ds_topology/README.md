# D–S 静态增容拓扑

直接生成默认 Mooncake 拓扑并完成一次静态容量重分配，无需输入 base case。
也支持从已有的原始 Mooncake case 生成。每个具体的 P–S pair 拆一根，
共 `12 × 4 = 48` 根；每个 P–D pair 同样拆一根，释放 48 个 D 端口。
16 个 D–S pair 各增加三根，每个跨组 P–P pair 各增加一根。
所有 leaf 仍保持 64 个 OCS 端口。脚本只生成文件，不启动仿真，不修改 ToE 控制器。

## 使用

需要 Python 3.9+，使用标准库及仓库现有的 `tools/generate_ocs_topology.py`、
`tools/generate_routing_table.py`。在仓库根目录执行即可：

```bash
python3 tools/static_ds_topology/build_static_ds_topology.py
```

结果写入当前工作目录下的 `static_ds_case/`，包含节点表、最终扩容拓扑和路由表。
内置原始拓扑只在临时目录中生成，不会写入原生成器的默认 case 目录。
可用 `--out-dir` 指定其他输出位置：

```bash
python3 tools/static_ds_topology/build_static_ds_topology.py \
  --out-dir /path/to/static_ds_case
```

将所有物理链路带宽缩小 100000 倍：

```bash
python3 tools/static_ds_topology/build_static_ds_topology.py \
  --out-dir /path/to/static_ds_4mbps_case \
  --bandwidth-divisor 100000
```

`400 Gbps / 100000 = 4 Mbps = 0.5 MB/s`，这里的单位是 Mbps，不是 MB/s。
该选项同时作用于全部服务器接入链路和 OCS 链路；默认除数为 1，保留 400 Gbps。
除数必须为正整数，缩放后每根链路必须是至少 1 bit/s 的整数速率。
链路时延、端口、路由结构及流量文件内容不会随带宽缩放而改变。

如需保留已有 case 的节点表和输入文件，可选传入 `--base-case`：

```bash
python3 tools/static_ds_topology/build_static_ds_topology.py \
  --base-case /path/to/original_case \
  --out-dir /path/to/static_ds_case
```

所有参数均可省略。输出目录必须不存在或为空；使用 `--base-case` 时，
输出目录不能与原 case 相同、互相嵌套。
脚本在临时目录内完成生成与验证，再发布最终目录；失败不会留下部分生成的 case。
不覆盖已有文件，不改写原 case。

## 输入与输出

默认无须提供输入文件。仅在传入 `--base-case` 时，原目录必须包含
`node.csv`、`topology.csv`。适用范围固定为当前 Mooncake：

- 计算服务器 0–15，各 64 端口；存储服务器 16–36，各 12 端口。
- Prefill leaf 37–48、Decode leaf 49–52、Storage leaf 53–56，各 128 端口。
- leaf 按连续四台分组，跨组每个 pair 初始恰好四根 OCS 电路。
- OCS 使用端口 64–127，链路 `bandwidth` 为 `400Gbps`，OCS 传播时延一致。
- 服务器接入关系与 `tools/generate_ocs_topology.py` 相同。

`--base-case` 只接受原始均匀的 400 Gbps 拓扑，不接受已扩容或已缩放的拓扑作为输入。
默认模式先构造上述原始拓扑，再应用同一套端口交换和可选带宽缩放。

| 输出文件 | 内容 |
|---|---|
| `node.csv` | 默认自动生成；指定 base case 时原样复制原节点表 |
| `topology.csv` | 新的静态连接，复用原物理端口，保留服务器接入链路 |
| `routing_table.csv` | 按新拓扑重新生成，检查所有目的端口的最短路输出集合 |
| `static_ds_changes.csv` | 48 组交换的实际拆除/新增端点，共 192 条记录 |
| `static_ds_summary.json` | 输入模式、带宽除数、前后容量、每 leaf 分配、D–S 矩阵、缺失项、SHA256 |

指定原 case 且存在 `traffic.csv`、`network_attribute.txt`、`transport_channel.csv` 时原样复制。
默认模式不生成业务流量或网络属性配置；这些实验输入需要另行提供。
前两项缺失时仍生成三个拓扑文件，但明确标记为 topology-only，不能直接运行完整实验。
汇总中的 before/after 容量均使用选定除数缩放后的链路带宽，便于在相同带宽下比较。
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
每个跨组 P–P pair 各增加一根，由四根增至五根。
本脚本一次性生成这些调整后的静态拓扑，仿真期间无需启用 ToE 重配。

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
以上容量为未缩放时的值。除数为 100000 时，D–S 每方向总容量从 256 Mbps 增至 448 Mbps。

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
端口复用、输入保留、确定性、路由实际出端口、重复应用/覆盖拒绝、异常清理、
无参数生成、全部链路的带宽缩放、无效除数和 CLI 输出。
无需编译 ns-3，也不依赖 HiGHS。
