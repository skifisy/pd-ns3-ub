**中文** | [English](README_en.md) | [返回 tools 首页](../README.md)

# 集合通信流量生成

集合通信流量统一通过 tools 根目录的 `ub_tools.py traffic` 生成。命令写入 `traffic.csv`，不会启动
ns-3，也不会自动选择拓扑。

## 快速开始

在 tools 根目录执行：

```bash
python3 ub_tools.py traffic allreduce \
  --host-count 8 \
  --group-size 8 \
  --data-size 1GB \
  --algorithm ring \
  --group-layout contiguous \
  --output-dir ./case
```

可用命令：

```bash
python3 ub_tools.py traffic allreduce --help
python3 ub_tools.py traffic reduce-scatter --help
python3 ub_tools.py traffic allgather --help
python3 ub_tools.py traffic alltoall --help
python3 ub_tools.py traffic alltoallv --help
python3 ub_tools.py traffic convert-simai --help
python3 ub_tools.py traffic validate --help
```

## 通用参数

| 参数 | 含义 |
| --- | --- |
| `--host-count` | 参与的物理节点总数；使用 placement 文件时可以省略 |
| `--group-size` | 每个通信组的 rank 数，默认等于 host count |
| `--data-size` | 每个 rank 参与一次完整集合通信时处理的数据总量 |
| `--algorithm` | 当前集合通信采用的调度算法 |
| `--phase-delay` | 有依赖关系的 phase 之间增加的延迟，默认 `0ns` |
| `--output-dir` | 写入 `traffic.csv` 的 case 目录 |

数据单位支持 `B`、`KB`、`MB` 和 `GB`，使用二进制倍率。已有 `traffic.csv` 默认不会被覆盖。

## 算法

面向研究人员的完整通信过程、阶段复杂度、适用场景和当前建模边界见
[集合通信算法说明](ALGORITHMS.md)。

| 集合通信 | `--algorithm` |
| --- | --- |
| AllReduce | `ring`、`nhr`、`recursive-halving-doubling` |
| ReduceScatter | `ring`、`nhr`、`recursive-halving` |
| AllGather | `ring`、`nhr`、`recursive-doubling` |
| AllToAll | `pairwise` |
| AllToAllV | `pairwise` |

算法选择依据：

- `ring` 是 AllGather 和 ReduceScatter 的通用带宽型基线，NVIDIA NCCL 主线源码直接实现了
  两者的 ring kernel；
- AllGather 的 `recursive-doubling` 名称对应二进制 Recursive Exchange；它与
  `recursive-halving`、`recursive-halving-doubling` 一样，对非 2 次幂 rank 数执行完整
  FoldIn/active exchange/FoldOut；
- `nhr` 对齐 HCCL 官方独立 AllGather/ReduceScatter NHR template，包括非 2 次幂 rank 数的
  step、peer 和 slice 计算；它不是所有通信库共有的标准名称。

这里的 `--data-size` 表示每个 rank 的完整逻辑输入大小。AllGather 的最终聚合对象大小为
rank 数乘以该值；AllToAll 将每-rank 对象按目的 rank 分配。所有 builder 先生成统一的
`Chunk`/`Transfer`/`CollectiveContract` schedule，并在写 CSV 前自动验证。算法实现与依据见
[`algorithms/ALGORITHM_SOURCES.md`](algorithms/ALGORITHM_SOURCES.md)。

可独立运行语义验证器，查看每轮各 rank 交换的块及最终持有状态；它覆盖 AllGather、ReduceScatter、
AllReduce、AllToAll 和 AllToAllV：

```bash
python3 ub_tools.py traffic validate allgather \
  --rank-size 4 \
  --data-size 1KB \
  --algorithm ring
```

添加 `--verbose` 可输出 contract、chunk 和 phase 后 holdings。用 `--output trace.json` 可保存稳定
JSON 工件；默认直接写到 stdout。已有输出不会被覆盖，必须显式加 `--overwrite`。验证器的规则和
网络 fidelity 边界见 [集合通信算法说明](ALGORITHMS.md)。

AllToAll `pairwise` 可使用 `--peers-per-phase K` 合并连续 atomic phase；它不改变 chunk、peer、
transfer 集合或总 wire bytes，要求 `1 <= K < group-size`。`scatter` 不是当前算法名。

## Rank 放置

自动放置：

```bash
--group-layout contiguous
--group-layout interleaved
```

显式放置使用 CSV：

```csv
groupId,rankId,nodeId
0,0,17
0,1,9
1,0,3
1,1,12
```

```bash
python3 ub_tools.py traffic allreduce \
  --data-size 1MB \
  --algorithm nhr \
  --placement-file placement.csv \
  --output-dir ./case
```

`--group-layout` 和 `--placement-file` 不能同时使用。

## AllToAllV

AllToAllV 支持三种字节分布：

```text
matrix   用户提供 N x N 字节矩阵，不使用 --data-size
uniform  将 --data-size 稳定分配给各目的 rank
random   使用固定 --seed 随机分配 --data-size
```

矩阵示例：

```csv
0,1024,2048,4096
1024,0,512,2048
2048,512,0,1024
4096,2048,1024,0
```

```bash
python3 ub_tools.py traffic alltoallv \
  --host-count 4 \
  --algorithm pairwise \
  --distribution matrix \
  --traffic-matrix matrix.csv \
  --output-dir ./case
```

对角线上的本地数据始终由 contract 直接满足；添加 `--include-local` 时 CSV 才会包含本地写行。
不使用该选项时，单 rank AllToAllV 仍然合法，但不会产生 task。matrix 中的零单元不生成
chunk 或网络 transfer；`uniform` 与固定 `--seed` 的 `random` 都按每个 source 的 `--data-size` 生成。

## SimAI 转换

已有 SimAI MockNCCL flow 可以直接转换，不需要重新选择集合通信算法：

```bash
python3 ub_tools.py traffic convert-simai flows.jsonl \
  --output ./case/traffic.csv \
  --rank-map rank_map.csv \
  --host-count 8
```

## 已有流量迁移

已有 `traffic.csv` 可以原样用于结构化 case 或 CSV case，无需转换。使用
`build_traffic.py` 的命令可按以下关系迁移：

| 已有配置 | 统一入口中的参数 |
| --- | --- |
| 集合通信类型 | `traffic allreduce`、`reduce-scatter`、`allgather`、`alltoall` 或 `alltoallv` |
| 算法名称 | `--algorithm` |
| rank 数或节点数 | `--host-count` 和 `--group-size` |
| 数据量 | `--data-size`，表示每个 rank 的完整算子数据量 |
| rank 排列 | `--group-layout` 或 `--placement-file` |
| 输出位置 | `--output-dir` |

已有 runlog 不受流量生成入口影响，可直接使用 `ub_tools.py trace parse` 解析。

## 后续步骤

生成流量后回到统一流程：

```bash
python3 ub_tools.py case check ./case
```

仿真结束后解析结果：

```bash
python3 ub_tools.py trace parse ./case --output-dir ./analysis
```

完整流程见 [tools 首页](../README.md) 。`build_traffic.py`、`all2allv_maker.py` 和
`simai_flow_to_traffic.py` 仍可直接运行；需要它们特有的参数或输出布局时，可继续使用原命令。
