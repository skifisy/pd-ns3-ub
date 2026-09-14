# 快速开始

**语言**: [English](QUICK_START_en.md) | [中文](QUICK_START.md)

> 本项目基于 ns-3.44 构建，已在 Linux 与 Windows WSL 系统下验证。详细的平台支持、安装步骤、系统要求及编译选项，请参阅 [ns-3.44 文档](https://www.nsnam.org/releases/ns-3-44/documentation/)、[安装指南](https://www.nsnam.org/docs/release/3.44/installation/singlehtml/) 及 [ns-3.44 源码](https://gitlab.com/nsnam/ns-3-dev/-/tree/ns-3.44?ref_type=tags)。

## 环境要求

核心构建依赖如下工具。代码下载可通过 Git，或通过浏览器 / wget / curl 下载源码压缩包（tar + bunzip2 解压）。

| 目的       | 工具                          | 最低版本             |
| ---------- | ----------------------------- | -------------------- |
| 下载       | git（Git 下载）<br/>或：tar 与 bunzip2（Web 下载）               | 无最低版本要求       |
| 编译器     | g++<br/>或：clang++           | >= 10<br/>>= 11      |
| 配置       | python3                       | >= 3.8               |
| 构建系统   | cmake<br/>以及 make / ninja / Xcode 其一 | cmake >= 3.13<br/>无最低版本要求 |

如使用 Conda/virtualenv，请确保后续运行的 `python3` 与安装依赖的解释器一致。

### 快速检查版本

可在命令行中按下列方式检查版本：

| 工具    | 版本检查命令        |
| ------- | ------------------- |
| g++     | `g++ --version`     |
| clang++ | `clang++ --version` |
| python3 | `python3 -V`        |
| cmake   | `cmake --version`   |

## 获取代码

```bash
# 克隆项目
git clone https://gitcode.com/open-usim/ns-3-ub.git
cd ns-3-ub

# 初始化并更新子模块（包含 Python 分析工具）
git submodule update --init --recursive

# 如果上述命令失败，可以手动克隆：
# git clone https://gitcode.com/open-usim/ns-3-ub-tools.git scratch/ns-3-ub-tools

# 验证子模块状态
git submodule status
```

如需在仿真结束后自动触发 trace 分析，请在对应用例的 `network_attribute.txt` 中配置工具路径，例如：

```
global UB_PYTHON_SCRIPT_PATH "scratch/ns-3-ub-tools/trace_analysis/parse_trace.py"
```

## Python 工具与依赖

Python 工具集位于 `scratch/ns-3-ub-tools/`（[open-usim/ns-3-ub-tools](https://gitcode.com/open-usim/ns-3-ub-tools)），该项目提供仿真拓扑、路由表、网络流量等配置文件工具与仿真后日志解析工具，更多信息请参阅该项目主页README。工具集包括：

- 拓扑/可视化：`net_sim_builder.py`、`topo_plot.py`、`user_topo_*.py`
- 流量生成：`traffic_maker/*`
- Trace 分析：`trace_analysis/parse_trace.py`

依赖安装推荐使用项目内的 `requirements.txt`：

```bash
python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt
# 可能会遇到`externally-managed-environment`限制，此时请尝试使用虚拟环境
# 使用国内镜像源加速下载
pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r scratch/ns-3-ub-tools/requirements.txt
# 或使用 conda：
conda install pandas matplotlib seaborn
# 或者手动安装以上依赖
```

说明： 请在运行 `trace_analysis/parse_trace.py` 前通过 `requirements.txt` 预先安装所需第三方包。

提示：如果你使用支持 repo-local skill 的 Agent，本仓库在 ` .codex/skills/ ` 下提供 OpenUSim skills。它们会先按本页和 `README.md` 检查启动状态，再处理实验定义、case 生成、仿真执行和结果分析。普通 smoke run、旧 case 复现、单次调试、A/B 对比、参数 sweep 和控制变量实验都可以走这套流程；对比类实验会先定下 baseline、待比较配置、固定控制项、预期结果和证据来源。分析得到稳定根因或通用判断后，也可以在用户同意后写成知识卡。这些 skills 依赖当前 `ns-3-ub` 工作树与 `scratch/ns-3-ub-tools/`，不作为独立 submodule 维护。

## 配置与编译

```bash
# 已知问题：ns-3 launcher（./ns3）与 Python 3.14 存在 argparse 兼容问题，建议用 python3.12 运行。
# 单个 build 可用 -j 加速；不要同时启动多个 build/test 构建任务。
BUILD_JOBS=${BUILD_JOBS:-$(python3.12 -c 'import os; print(os.cpu_count() or 1)')}

# 配置 UB 仿真所需模块
python3.12 ./ns3 configure --enable-modules=unified-bus --disable-werror -d release -G Ninja

# 编译 UB 仿真入口及其依赖
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example
```

如果需要开发或验证 ns-3 其他模块，再使用不带 `--enable-modules` 的全量配置。

### （可选）启用 Unison 多线程并行仿真

如需启用 Unison for ns-3 的多线程并行仿真（MTP），请在配置阶段加入 `--enable-mtp`（可同时启用示例）：

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --enable-mtp --disable-werror -d release -G Ninja
python3.12 ./ns3 build -j "$BUILD_JOBS" ub-quick-example

# 运行时可通过 --mtp-threads 参数启用多线程（需 >= 2）
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp --mtp-threads=8'
```

更多 MTP 用法、行为边界和诊断参数请参阅 [UNISON_README.md](UNISON_README.md)。

## 运行简单示例

`ub-quick-example` 会读取 case 目录中的配置文件（如 `topology.csv`、`traffic.csv` 等），自动创建 unified-bus 场景并运行仿真。完成上面的单目标编译后，推荐命令形式为 `python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=...'`。

```bash
# 如使用 Conda，请确保其 bin 在 PATH 前（或先激活环境）
# export PATH=<your-conda-path>/bin:$PATH

# 安装依赖
python3 -m pip install --user -r scratch/ns-3-ub-tools/requirements.txt

# 运行小示例并触发 trace 分析
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'

# 验证输出
ls scratch/2nodes_single-tp/output/
# 预期包含：task_statistics.csv  throughput.csv
```

如遇到 `ModuleNotFoundError: No module named 'pandas'`，说明运行时的 `python3` 与安装依赖所用解释器不一致；请检查 PATH，或使用 `python3 -m pip install --user ...` 在当前解释器中安装依赖。

## scratch 目录下的示例

以下为当前仓库中已提供的可用用例目录及对应运行命令：

- 2 节点（单 TP）：
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_single-tp'
  ```

- 2 节点（多 TP）：
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_multiple-tp'
  ```

- 2 节点（包喷洒）：
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2nodes_packet-spray'
  ```

- 2D FullMesh 4x4（多路径 All-to-All）：
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-multipath_a2a'
  # 启用多线程加速（需 --enable-mtp 编译）
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-multipath_a2a --mtp-threads=8'
  ```

- 2D FullMesh 4x4（分层广播）：
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-hierarchical_broadcast'
  ```

- Clos（32 hosts / 4 leafs / 8 spines, pod2pod）：
  ```bash
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/clos_32hosts-4leafs-8spines_pod2pod'
  # 大型拓扑建议使用多线程
  python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/clos_32hosts-4leafs-8spines_pod2pod --mtp-threads=16'
  ```

说明：部分大型用例运行时间较长，可使用 `--mtp-threads=8` 启用多线程加速（需 `--enable-mtp` 编译）。

## 完整工作流程示例

```bash
# 运行完整示例，包含 Python 后处理
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/2dfm4x4-multipath_a2a'

# 预期输出：
[01:23:37]:Run case: scratch/2dfm4x4-multipath_a2a
[01:23:37]:Set component attributes
[01:23:37]:Create node.
[01:23:37]:Start Client.
[01:23:37]:Simulator finished!
[01:23:37]:Start Parse Trace File.
所有依赖已满足，开始执行脚本...
处理完成，结果已保存到 scratch/2dfm4_4-multipath_a2a/output/task_statistics.csv
处理完成，结果已保存到 scratch/2dfm4_4-multipath_a2a/output/throughput.csv
[01:23:37]:Program finished.

# 查看生成的结果文件
ls scratch/2dfm4x4-multipath_a2a/output/
# task_statistics.csv  throughput.csv
```

## 配置文件说明

每个用例目录通常包含如下文件（格式可参照现有样例）：

- `network_attribute.txt` - 网络全局参数（可配置 `UB_PYTHON_SCRIPT_PATH` 用于自动后处理）
- `node.csv` - 节点定义
- `topology.csv` - 拓扑连接
- `routing_table.csv` - 路由表
- `transport_channel.csv` - 可选的显式 RTP 传输通道；省略时会在流量加载阶段自动预留
- `traffic.csv` - 流量定义

有关 `ub-quick-example` 的入口说明、配置文件格式与典型命令，请参见 [scratch/README.md](scratch/README.md)。

---

## 相关文档

| 文档 | 描述 |
|------|------|
| [README.md](README.md) | 项目总览：UB 模块能力、目录结构与核心概念 |
| [scratch/README.md](scratch/README.md) | 配置驱动入口：配置文件语义、运行命令、用例执行与参数说明 |
| [open-usim/ns-3-ub-tools](https://gitcode.com/open-usim/ns-3-ub-tools) | 配套工具链（子模块）：用例配置生成与 trace 后处理与分析 |
