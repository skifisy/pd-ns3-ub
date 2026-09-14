# UB 多线程与多进程仿真指南

本文面向希望使用并行运行加速仿真，且使用 `scratch/ub-quick-example` 运行 UB case 的用户。说明四种运行方式的区别、case 需要怎样配置、为什么并行结果有时会与单线程结果不同，以及三个 timing offset 参数分别解决什么问题。

## 为什么需要并行仿真

小型 case 适合直接用本地单线程运行。拓扑、流量或 trace 规模增大后，可以使用多线程、MPI 多进程，或把两者结合，以缩短运行时间或利用多台机器的资源。

UB 提供四种运行方式：

| 运行方式 | 进程数 | 每个进程的线程数 | 适用情况 |
| --- | ---: | ---: | --- |
| 本地单线程 | 1 | 1 | 建立基准结果、调试或运行小型 case |
| 本地多线程 | 1 | 2 个或更多 | 在一台机器上加速仿真 |
| MPI 多进程 | 2 个或更多 | 把 case 分配到多个进程或多台机器 |
| MPI + 多线程 | 2 个或更多 | 同时使用进程级和线程级并行 |

命令行和构建选项使用 `MTP` 表示多线程运行方式。四种方式都使用同一个用户入口：

```text
scratch/ub-quick-example
```

## 为什么需要 timing offset

并行仿真的目的是加速计算、扩展可运行的 case 规模，而不主动改变模型行为。但离散事件仿真器允许数据流开始、报文接收或状态更新等多个事件拥有完全相同的仿真时间，而单线程和并行运行可能以不同但都有效的顺序处理这些同时发生的操作。如果仿真用例中存在较多这类同时发生的事件，处理先后的差异就可能导致单线程与并行运行的结果产生漂移。

本项目通过基于 seed 的确定性小幅错开，降低大量事件在同一时间同步发生的概率，具体由以下参数控制：

```text
--initial-task-start-offset-window=<Time>
--link-delay-offset-window=<Time>
--timing-offset-seed=<uint32>
```

initial task start offset 的目的是把原本同时启动的无依赖 task 分散到一个很小的时间窗口内，降低仿真起点对同时操作顺序的敏感性。

link delay offset 的目的是降低来自不同进程或不同路径的流量在后续再次同时到达的概率。它补充 initial task start offset，但不会替代它。

共享 seed 的目的是让单线程、多线程和多进程运行得到相同的 offset 分配，从而能够进行可重复的配对比较。

两个 offset window 默认关闭。seed 默认是 `1`，只有至少一个 window 开启时才影响 offset 分配。只有当目标 workload 对同时操作的处理顺序敏感，并且实验明确接受这种小幅时间调整时，才需要启用 offset。

## 配置与构建

根据需要选择对应的配置命令。

本地单线程：

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --disable-mtp --disable-werror -d release -G Ninja
```

本地多线程：

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --disable-mpi --enable-mtp --disable-werror -d release -G Ninja
```

MPI 多进程：

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --enable-mpi --disable-mtp --disable-werror -d release -G Ninja
```

MPI + 多线程：

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-examples --disable-tests --enable-mpi --enable-mtp --disable-werror -d release -G Ninja
```

完成配置后构建 UB 用户入口：

```bash
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

使用 MPI 模式前，需要安装能够提供 `mpirun` 的 MPI 运行环境。

## 运行四种模式

本地单线程：

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'
```

本地多线程：

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/clos_32hosts-4leafs-8spines_pod2pod --mtp-threads=4'
```

`--mtp-threads` 表示当前进程使用的工作线程数。`0` 和 `1` 使用本地单线程；`2` 或更大的值启用多线程。

MPI 多进程：

```bash
mpirun -np 2 build/scratch/ns3.44-ub-quick-example \
  --case-path=scratch/ub-mpi-minimal
```

MPI + 多线程：

```bash
mpirun -np 2 build/scratch/ns3.44-ub-quick-example \
  --case-path=scratch/ub-mpi-minimal \
  --mtp-threads=2
```

`-np` 表示 MPI 进程数。在混合模式中，`--mtp-threads` 对每个进程分别生效。例如，`-np 2` 和 `--mtp-threads=4` 表示两个进程，每个进程使用四个工作线程。

可执行文件名包含 ns-3 版本号。如果版本发生变化，请使用当前构建在 `build/scratch/` 下生成的 `ns3.<version>-ub-quick-example` 文件。

## 准备多进程 case

四种运行方式使用相同的 case 目录格式。多进程运行还需要满足以下要求：

- `node.csv` 的每一行都应显式填写 `systemId`。该值表示节点所属的 MPI 进程编号，从 `0` 开始。
- 在 MPI + 多线程模式下，`systemId` 仍然只表示进程编号，不要把线程编号编码到该字段中。线程分配由运行时自动完成。
- `topology.csv` 可以连接属于不同进程的节点，仿真器会自动建立本地连接或跨进程连接。
- 所有进程都会读取完整的 `traffic.csv`，但 task 只会在拥有其 `sourceNode` 的进程上启动。
- `transport_channel.csv` 描述端点之间的连接关系，不需要为每个进程重复填写同一条连接。

最大的 `systemId` 必须小于 `mpirun -np` 指定的进程数。

MPI + 多线程模式要求每个启动的 MPI 进程至少拥有一个节点。如果某个进程没有节点，程序会在分区阶段明确退出；此时应减少 `-np`，或调整 `node.csv` 中的 `systemId` 分配。

## 配置 task 依赖

如果 `traffic.csv` 使用 phase 依赖，并行运行会根据 UB 链路时延自动选择一个保守的正可见延迟，并用它约束并行同步步长。通常不需要额外配置。

如需固定实验参数，可以显式覆盖自动值：

```text
--dependency-visibility-delay=<Time>
```

一个 task 完成后，依赖它的 task 会在该延迟之后看到完成状态。显式值在并行运行中必须大于零。

显式覆盖主要用于需要固定该参数的对照实验。对比单线程与并行结果时，两边需使用相同的显式值。

## 对比单线程与并行结果

先使用完全相同的 case 文件和模型参数，分别运行一次本地单线程基准和目标并行模式。优先比较实验真正关心的业务指标，而不是只比较日志行顺序。

如需逐 task 对比，可以分别输出 canonical task events：

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=<case-dir> --canonical-output=/tmp/ub_local'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=<case-dir> --mtp-threads=4 --canonical-output=/tmp/ub_mtp4'
```

开始大型实验前，需要先为目标 workload 定义可接受的差异。一个 case 上观察到的误差范围不能直接套用到另一个 case。

## 可选 timing offset

当大量 task 在同一仿真时刻启动，或者多条路径的流量在同一时刻到达一个节点时，可以使用小幅、确定性的 timing offset，降低结果对同时操作顺序的敏感性。

两个 offset window 默认关闭；seed 默认是 `1`，仅在至少一个 window 开启时生效：

```text
--initial-task-start-offset-window=<Time>
--link-delay-offset-window=<Time>
--timing-offset-seed=<uint32>
```

### 初始 task start offset

`--initial-task-start-offset-window` 只作用于 `dependOnPhases` 为空的初始 task。来自同一 source 的初始 task 使用相同的非负 offset，取值范围为 `[0, window)`。有依赖的 task 不会增加该 offset。

这个参数处理的是大量 source 同时释放初始 task 的情况。它不会修改后续依赖完成时间，也不会对所有 task 普遍增加延迟。

### Link delay offset

`--link-delay-offset-window` 为每条原始延迟大于零的物理 link 分配一个固定的非负 offset，取值范围为 `[0, window)`。同一条 link 上的所有 packet 都使用相同的调整后 delay，而不是为每个 packet 重新取值。

原始 delay 为 `0ns` 的 link 保持不变。

### 共享 seed

`--timing-offset-seed` 同时控制上述两种 offset，使相同输入和 seed 能够得到相同的 offset 分配。

属于同一组对比的单线程、多线程和多进程运行，必须使用相同的两个 window 和 seed。否则它们代表的是不同的模型时间配置，不能直接配对比较。

window 没有适用于所有 workload 的通用推荐值。使用能够满足实验目的的最小窗口即可。窗口越大，不同对象获得相同 offset 的概率通常越低，但对模型时间的改动也越大。

这些 offset 是提高可重复性的 workaround，不是经过校准的硬件延迟模型。如果实验启用了它们，必须随结果记录两个 window 和 seed。报告汇总结果时可以使用多个 seed，但每一组单线程与并行对比必须保持 seed 相同。

## 常见问题

| 现象 | 检查项 |
| --- | --- |
| 传入 `--mtp-threads` 后提示 MTP 不可用 | 使用 `--enable-mtp` 重新配置并构建用户入口 |
| `mpirun` 无法启动程序 | 检查 MPI 环境，并确认配置时使用了 `--enable-mpi` |
| 带依赖的 traffic case 在仿真前退出 | 确认相关 UB 链路具有正时延；必要时显式设置正的 `--dependency-visibility-delay` |
| 节点被分配给不存在的进程 | 确认每个 `systemId` 都小于 `-np` 的值 |
| 单线程与并行输出不同 | 检查 case 文件、依赖延迟、两个 offset window、seed 和其他模型参数是否一致 |
| 日志提示多个对象获得相同 offset | 只有在实验能够接受更大时间改动时，才增大对应 window |

基础本地运行请参阅 [QUICK_START.md](QUICK_START.md)。Unison 的通用示例和接入方式请参阅 [UNISON_README.md](UNISON_README.md)。
