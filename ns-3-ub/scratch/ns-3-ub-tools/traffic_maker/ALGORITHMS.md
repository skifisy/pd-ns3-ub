# 集合通信算法说明

本文描述 `ub_tools.py traffic` 当前实际生成的网络调度。算法来源和固定 revision 见
[算法来源](algorithms/ALGORITHM_SOURCES.md)；本页只说明工具的可观察行为和建模边界。

## 统一语义与写入门槛

所有 builder 先生成同一语义 IR：

- `Chunk` 是有稳定 ID 和字节数的逻辑数据块；
- `Transfer` 指明发送者、接收者、chunk、字节数、`COPY` 或 `REDUCE`，以及源数据是否保留；
- `CollectiveContract` 定义每个 rank 的初始/最终 holdings，以及归约类 collective 的必需贡献者；
- `CollectiveSchedule` 把 transfer 划分为严格有序的 atomic phase。

`traffic.csv` 不是这套语义的来源。每一次 `traffic ...` 写 CSV 前都会调用统一 validator；contract、
chunk 表、phase 内 ownership/贡献和最终 postcondition 任一不满足时，CSV 不会写出。

`traffic validate` 可独立检查 `allgather`、`reduce-scatter`、`allreduce`、`alltoall` 和
`alltoallv`。默认输出紧凑 JSON；`--verbose` 额外输出 contract、每个 chunk、每个 phase 的 transfer
及 phase 后 holdings。它验证 builder 语义，不读取已降级为端点/字节/phase 的 `traffic.csv`。

```bash
python3 ub_tools.py traffic validate allreduce \
  --rank-size 5 --data-size 1KB --algorithm recursive-halving-doubling --verbose

python3 ub_tools.py traffic validate alltoallv \
  --rank-size 4 --algorithm pairwise --distribution uniform --data-size 1KB --verbose
```

## 数据量、零值与本地项

除 AllToAllV 外，`--data-size D` 都表示**每个 rank 的完整逻辑输入对象**，不是单条网络传输、
单个分片或总 wire bytes。

| Collective | `D` 的含义 |
| --- | --- |
| AllReduce | 每个 rank 的完整归约输入和最终结果大小 |
| ReduceScatter | 每个 rank 的完整归约输入；最终只保留一个结果 chunk |
| AllGather | 每个 rank 的输入 chunk 大小；最终对象为 `N * D` |
| AllToAll | 每个 rank 按目的 rank 分配的完整发送对象，含本地目的项 |
| AllToAllV | 不使用统一 `D`；`N x N` matrix 的 `[src][dst]` 是该单元的字节数 |

`0B` 对全部 collective 都是合法输入。零字节 chunk 不进入
chunk 表，也不产生有效载荷 transfer；为保持算法的阶段结构，schedule 仍可包含空 phase。AllToAllV
matrix 中的零单元同样不产生 chunk 或 transfer。

AllToAll/AllToAllV 的本地 `src == dst` 数据由 contract 在同一个 rank 的初始和最终 holdings 中
直接满足，不属于远端传输。生成 CSV 时只有显式 `--include-local` 的 AllToAllV 才会输出本地
`URMA_WRITE` 行。

## 算法总览

令 `N` 是 group size，`P = 2^floor(log2 N)`。

| Collective | `--algorithm` | 网络 phase | 任意 `N` | 当前调度 |
| --- | --- | ---: | --- | --- |
| AllReduce | `ring` | `2(N-1)` | 是 | Ring ReduceScatter + Ring AllGather |
| AllReduce | `nhr` | `2 ceil(log2 N)` | 是 | HCCL NHR ReduceScatter + AllGather |
| AllReduce | `recursive-halving-doubling` | `2 log2 P`，非 2 次幂再加 FoldIn/FoldOut | 是 | MPICH/Rabenseifner 风格 |
| ReduceScatter | `ring` | `N-1` | 是 | 单向环归约 |
| ReduceScatter | `nhr` | `ceil(log2 N)` | 是 | HCCL NHR |
| ReduceScatter | `recursive-halving` | `log2 P`，必要时 FoldIn/FoldOut | 是 | 二进制 Recursive Exchange |
| AllGather | `ring` | `N-1` | 是 | 单向环转发 |
| AllGather | `nhr` | `ceil(log2 N)` | 是 | HCCL NHR |
| AllGather | `recursive-doubling` | `log2 P`，必要时 FoldIn/FoldOut | 是 | 二进制 Recursive Exchange |
| AllToAll | `pairwise` | `ceil((N-1)/K)` | 是 | cyclic pairwise；`K=--peers-per-phase` |
| AllToAllV | `pairwise` | `N-1` | 是 | matrix-defined cyclic pairwise |

`--peers-per-phase` 仅适用于 AllToAll `pairwise`，满足 `1 <= K < N`。它把连续的 atomic pairwise
phase 合并，**不改变** chunk、peer、transfer 集合或总 wire bytes。`scatter` 不是当前算法名。

## AllReduce 与 ReduceScatter

### Ring 与 NHR

Ring 使用固定前后继；AllReduce 先完成 `N-1` 个 ReduceScatter phase，再完成 `N-1` 个
AllGather phase。它是带宽型网络基线，不表达多 ring、channel 或 pipeline。

NHR 使用 HCCL standalone template 的 modulo peer 与 slice 集合。ReduceScatter 的 rank delta
从低位 2 的幂递增，AllGather 反向递减；`ceil(log2 N)` 使非 2 次幂 group 也有定义。NHR 是 HCCL
术语，不应泛称为每个通信库都实现的通用算法。

### Recursive Halving 与 Halving-Doubling

当 `N` 不是 2 次幂时，当前工具不是截断 XOR loop：

1. 对前 `2(N-P)` 个 rank，even `r` 向 odd `r+1` FoldIn；odd active rank 代表该 pair。
2. `P` 个 active rank 进行二进制 Recursive Exchange。ReduceScatter 的 mask 从高到低；
   AllReduce 的后半 AllGather mask 从低到高。
3. FoldOut 将结果从 active odd rank 发回对应 even rank。

ReduceScatter 的 FoldOut 只返还目标结果 chunk；AllReduce 的 FoldOut 返还完整结果。归约 transfer
在 IR 中记录为 `REDUCE`，并检查每个最终 chunk 的贡献者正好是所有 rank；网络仿真仍不计本地归约
计算时间。

## AllGather

Ring 逐轮转发一个已拥有 chunk。NHR 根据 HCCL 的递减 rank delta 与非连续 slice 集合传播 chunk。

CLI 名称 `recursive-doubling` 对应当前的**二进制 Recursive Exchange**：对于非 2 次幂 `N`，它先
FoldIn 到 `P` 个 active rank，active rank 再以 `active_rank xor distance` 配对，最后 FoldOut。因而
它支持任意正 rank 数。它不是 Open MPI 那个只在 2 次幂 communicator 上直接运行、否则 fallback
到 Bruck 的 classic recursive-doubling selector；不要把两者的支持边界混为一谈。

## AllToAll 与 AllToAllV

Pairwise AllToAll 在 distance `s = 1..N-1` 中让 source `r` 发送其 `r -> (r+s) mod N` 的唯一
chunk。每个远端 `src-dst` 单元恰好传输一次，且 `source_retains=False`，因此最终 ownership 恰为
每个 destination 收到的 source chunks。若 `K > 1`，只把相邻 distance 的 transfer 放入同一 phase。

AllToAllV 使用同一 pairwise peer 顺序，但 byte matrix 的每个非零 `[src][dst]` 是独立 chunk。
matrix 分布不使用 `--data-size`；`uniform` 将每个 source 的 `D` 确定性切分，`random` 用 `--seed`
产生可复现的非负切分。matrix 对角线始终作为 local ownership 进入 contract；只有
`--include-local` 控制 lowering 是否为它输出 self task。

## 网络流量 fidelity 边界

该工具的 fidelity 是“调度到网络”的 fidelity：它保留 logical peer、chunk identity、每条远端
transfer 的字节数与 phase dependency，并将其降级为 `traffic.csv` 的 `URMA_WRITE` 行。

verbose 结果会展示 AllToAllV byte matrix 推导出的逻辑 send/receive offset，便于检查布局；它不验证
物理 buffer overlap、alias 或越界，也不模拟 GPU/NPU buffer、in-place copy、local reduction cost、kernel
protocol、stream/channel 数、NIC batching、collective runtime selector 或跨 phase overlap。AllToAllV
matrix 的 byte layout 在 builder/validator 中存在，但 CSV 不携带 offset。因此结果应解释为给定
rank placement、拓扑和 phase barrier 下的网络传输时间，不是端到端 GPU collective latency。
