**中文** | [English](README_en.md)

# ns-3-ub-tools

`ns-3-ub-tools` 用于准备 OpenUSim / ns-3 Unified Bus 仿真目录。`ub_tools.py` 提供统一入口，
负责拓扑、路由、流量、case 检查和结果解析；仿真由 `ns-3-ub` 仓库中的 scratch runner
读取准备好的目录并独立启动。

## 安装

要求 Python 3.11 或更高版本。

```bash
uv venv
source .venv/bin/activate
uv pip install -r requirements.txt
```

查看所有命令：

```bash
python3 ub_tools.py --help
python3 ub_tools.py topology --help
python3 ub_tools.py traffic --help
python3 ub_tools.py traffic validate --help
python3 ub_tools.py case --help
python3 ub_tools.py trace --help
```

## 推荐使用路径

以下命令在 `ns-3-ub` 仓库根目录执行。

先激活 tools 的 Python 环境：

```bash
source scratch/ns-3-ub-tools/.venv/bin/activate
```

### 1. 准备结构化拓扑

拓扑可以来自 Python/NetworkX builder、JSON/YAML 文件，或者已有 CSV 拓扑目录：

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py topology check \
  scratch/structured_topo_example.py
```

输出：

```text
{"devices": 4, "links": 14, "nodes": 6, "switches": 2}
```

生成 case：

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py topology build \
  scratch/structured_topo_example.py \
  --output-dir path/to/case \
  --route-generation mixed
```

输出：

```text
structured case prepared: path/to/case nodes=6 links=14 route-generation=mixed
```

此时目录中包含：

```text
path/to/case/compressed_routing.bin
path/to/case/structured_summary.json
path/to/case/structured_topology.txt
```

默认输出：

- `structured_topology.txt`：物理拓扑和经过验证的结构规则；
- `compressed_routing.bin`：C++ 运行时直接加载的压缩候选端口索引；
- `structured_summary.json`：版本、结构覆盖率、规则数量和路由统计。

`topology build` 只准备拓扑与路由。一个可运行 case 还需要
`network_attribute.txt` 和 `traffic.csv`，`transport_channel.csv` 为可选文件。

### 2. 生成流量

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py traffic allreduce \
  --host-count 4 \
  --data-size 1MB \
  --algorithm ring \
  --group-layout contiguous \
  --output-dir path/to/case
```

`--data-size` 表示每个 rank 参与一次完整集合通信时处理的逻辑数据总量，不是单次传输或
单个 phase 的大小。

支持的集合通信和算法：

| 集合通信 | 算法或分布 |
| --- | --- |
| AllReduce | `ring`、`nhr`、`recursive-halving-doubling` |
| ReduceScatter | `ring`、`nhr`、`recursive-halving` |
| AllGather | `ring`、`nhr`、`recursive-doubling` |
| AllToAll | `pairwise`（可选 `--peers-per-phase`） |
| AllToAllV | `matrix`、`uniform`、`random` |

完整参数和 placement 示例见 [流量生成说明](traffic_maker/README.md) 。
各算法的通信过程、阶段复杂度和建模边界见
[集合通信算法说明](traffic_maker/ALGORITHMS.md) 。

所有 collective 在写入 `traffic.csv` 前都会自动验证 Chunk/Transfer/Contract 的语义。也可从同一入口
独立输出逐轮状态；验证覆盖 AllGather、ReduceScatter、AllReduce、AllToAll 和 AllToAllV：

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py traffic validate allgather \
  --rank-size 4 \
  --data-size 1KB \
  --algorithm ring
```

默认 JSON 是紧凑摘要。添加 `--verbose` 可检查 contract、每个 transfer 和 phase 后 holdings；使用
`--output trace.json` 保存工件，已有文件必须显式加 `--overwrite` 才会替换。`0B` 对非 V collective
是合法的每-rank 输入；matrix AllToAllV 的每个零单元不会产生网络 transfer。

### 3. 检查 case

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py case show path/to/case
python3 scratch/ns-3-ub-tools/ub_tools.py case check path/to/case --json
```

- `case show` 可以在目录尚未准备完整时查看拓扑和路由摘要；
- `case check` 检查运行时必需文件、节点引用、端口、流量和路由工件；
- `case check` 成功后再启动仿真。

### 4. 启动仿真

结构化 case 使用 `ns-3-ub` 仓库中的独立 runner：

```bash
python3 scratch/run_structured_topo_case.py \
  --case-path path/to/case \
  --route-runtime-mode compressed-table-lookup
```

runner 不导入 NetworkX，也不会在仿真进程中重新编译路由。tools 与 C++ 通过 case 目录中的
文件交接，不存在 stdin 或跨语言运行时调用。

### 5. 解析结果

```bash
python3 scratch/ns-3-ub-tools/ub_tools.py trace parse \
  path/to/case \
  --output-dir path/to/analysis
```

输出 `task_statistics.csv` 和 `throughput.csv`。

完整的仿真启动说明见 `ns-3-ub` 仓库的
[scratch 使用说明](https://gitcode.com/open-usim/ns-3-ub/blob/next/scratch/README.md) 。

## Python 拓扑接口

推荐让 Python 文件提供无参数 `build_topology()`：

```python
from net_sim_builder import NetworkSimulationGraph


def build_topology() -> NetworkSimulationGraph:
    graph = NetworkSimulationGraph()
    graph.add_netisim_host(0, allocation_delay="1ns", forward_delay="0ns")
    graph.add_netisim_host(1, allocation_delay="1ns", forward_delay="0ns")
    graph.add_netisim_edge(0, 1, bandwidth="400Gbps", delay="20ns")
    return graph
```

输入形式：

```text
path.py            -> 调用 build_topology()
path.py:function   -> 调用指定函数
path.json/yaml     -> 读取结构化声明
CSV case 目录       -> 读取 node.csv 和 topology.csv
```

`user_topo_example.py`、`user_topo_2layer_clos.py` 和 `user_topo_4x4_2DFM.py`
均可直接作为拓扑输入，也可作为自定义拓扑的起点。将建图逻辑放在 `build_topology()` 中并
返回 `NetworkSimulationGraph`；如需保留脚本直接生成 CSV 的能力，可在 `main()` 中调用该函数
并执行文件写入。

## 路由生成与运行

离线路由生成：

| `--route-generation` | 行为 |
| --- | --- |
| `mixed` | 默认值。已验证结构使用规则，其余部分自动回退到图派生候选 |
| `structured` | 要求全部路由查询都由已验证结构负责，否则报错 |
| `full` | 全部候选从物理图生成，用于兼容、对照和完整验证 |

运行时取候选端口：

| `--route-runtime-mode` | 行为 |
| --- | --- |
| `compressed-table-lookup` | 默认值。直接查询 `compressed_routing.bin`，适用于三种生成方式 |
| `structured-rule-compute` | 运行时根据结构规则计算，要求 case 中存在可用结构规则 |

两种运行方式只改变候选端口的来源，不改变 `GetOutPort()` 之后的 HASH、round-robin、
adaptive、packet spray 和一次绕路语义。

需要一次性完整正确性检查时添加：

```bash
--verify-routes
```

它会将最终压缩索引的端口级和节点级查询逐项与物理图最短路基准比较。大规模拓扑下会显著
增加准备时间，不建议作为每次普通运行的默认选项。

## 自动结构识别

可自动识别并验证：

- FullMesh、Ring、Mesh、Torus 和 N 维 Dimension FullMesh；
- 直接交换网络；
- 两层交换网络及独立多平面、局部加共享上层交换机；
- 经典 Dragonfly；
- Balanced Sparse Tree 一类 pair-design incidence 网络；
- 交换机端口扩展和经过验证的安全组合。

识别不依赖用户提供的 group 名称、节点编号范围或端口编号规律。候选结构只有在物理端口、
并行链路、链路归属和路由行为验证通过后才会进入规则路由。未识别部分由 `mixed` 自动处理。

## JSON/YAML 输入

声明式输入包含四类互相独立的连接描述：

- `nodeGroups`：节点数量和类型；
- `groupConnections`：组内或组间连接；
- `layeredConnections`：多层交换网络；
- `explicitConnections`：少量显式物理连接。

最小示例见 [structured_2devices.yaml](examples/structured_2devices.yaml) 。声明只用于生成物理图，
不会被直接当作路由正确性的证明。

## 已有用例迁移

### Python 拓扑脚本

将已有建图代码收进无参数函数，并返回图对象：

```python
def build_topology() -> NetworkSimulationGraph:
    graph = NetworkSimulationGraph()
    # 保留已有的节点和连接构造代码
    return graph
```

随后直接使用统一入口：

```bash
python3 ub_tools.py topology build existing_topology.py \
  --output-dir path/to/case \
  --route-generation mixed
```

如脚本已经提供其他 builder，可使用 `existing_topology.py:function_name`。需要继续直接执行
脚本生成 CSV 时，将写文件操作保留在 `main()` 中即可，两种入口可以共用同一份建图逻辑。

### CSV 拓扑和 case

已有 CSV case 无需修改，仍可由 `ub-quick-example` 读取。也可以把包含 `node.csv` 和
`topology.csv` 的目录传给 `topology build`，生成结构化 case；已有物理端口、未使用端口和
并行链路会原样参与分析。

### 流量和 trace

已有 `traffic.csv` 可以直接放入新 case。需要重新生成集合通信流量时，使用
`ub_tools.py traffic`；已有 runlog 可直接交给 `ub_tools.py trace parse`，无需重新运行仿真。

## 兼容路径

需要传统 CSV 拓扑和完整路由工件时：

```bash
python3 ub_tools.py topology build topology.py \
  --output-dir path/to/case \
  --case-format csv
```

CSV 输出固定使用 `full` 路由生成，并且仍需补齐共享的 `network_attribute.txt` 和
`traffic.csv` 才能运行。`--export-csv` 只导出诊断快照，不是 runner 输入。

已有 topology、traffic 和 trace 脚本仍可直接运行。需要这些脚本特有的参数、时间戳目录或
输出布局时，可继续使用原命令；其他场景使用 `ub_tools.py` 即可。

## 相关文档

- [流量生成说明](traffic_maker/README.md)
- [English README](README_en.md)
- [Parent scratch 使用说明](https://gitcode.com/open-usim/ns-3-ub/blob/next/scratch/README.md)
