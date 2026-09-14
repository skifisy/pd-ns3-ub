# ns-3-UB 发布说明

**语言**: [English](RELEASE_NOTES_UB_en.md) | [中文](RELEASE_NOTES_UB.md)

## Release 1.3.0

**发布日期**: 2026 年 7 月

### 仿真功能

- **RTP 可靠传输**：新增选择性重传，覆盖静态/动态 RTO、快速选择性重传、Selective MarkPSN 以及 SACK/NAK 反馈处理。重传需显式开启（opt-in），不开时所有 case 行为与之前一致。
- **RTP 丢包与故障注入**：`retrans_fault.csv` 提供故障注入用例，可精确复现 DATA/ACK/SACK 等关键包的丢失、延迟与恢复行为。
- **MPI / MTP Traffic DAG**：TrafficGen DAG 从单进程扩展到了 MTP、MPI 和 MPI+MTP hybrid 三种模式。任务完成通知可跨 rank 传播，依赖可见性与 MPI lookahead 绑定，带依赖的 workload 在多线程、多进程、混合模式下行为一致。
- **大规模路由压缩**：支持用 range-based 压缩路由表描述大规模规则，新增 case 默认走压缩路径。1K-host 及以上拓扑不用再把重复的路由规则展开成百万行 CSV。
- **并行输出一致性校验**：新增 canonical 输出路径，对比 local、MTP、MPI、hybrid 四种模式的任务完成结果，方便发现并行运行导致的结果漂移。

### 仿真引擎效率提升

- **TrafficGen 启动加速**：重写了 traffic 记录解析、opcode/delay 缓存、source app cache 和 runtime task 结构，大规模 `traffic.csv` 读入和激活阶段的 CPU 与内存开销明显下降。
- **Traffic DAG 内存优化**：task 状态改用紧凑存储，依赖关系换成 vector-based 结构，替代原来开销较大的 presence bitmap 和 dense 辅助结构，DAG workload 常驻内存大幅减少。
- **并行调度加速**：调整了 ready task 收集、phase id 存储、跨 rank completion 可见性及 MTP 事件排序，无 trace 的大规模 workload 的 run 阶段耗时明显缩短。
- **压缩路由加载优化**：路由文件体积大幅减小，同时加载阶段的内存峰值和解析时间也降下来了，Clos 大 case 不再被路由 CSV 展开拖慢。

### Agent Skills

仓库内置的 OpenUSim Skills 是 5 个分阶段 Agent 辅助流程，覆盖一次 UB 仿真实验的完整生命周期：

- **welcome**：检查仓库、工具链和构建产物是否就绪。
- **plan-experiment**：把自然语言目标整理成可执行的实验描述，支持单 case 和对比组两种模式。
- **run-experiment**：根据实验描述生成 case、配置、仿真执行和显式失败处理。
- **analyze-results**：解读仿真输出，对比预期目标，定位异常原因。
- **capture-insights**：将稳定的根因或通用结论沉淀为知识卡，供后续复用。

本次新增**对比组实验模式**：A/B 对比、参数扫描、控制变量等实验要求在生成 case 前写好预测和判断标准，跑完后按预测-vs-实际归类（`matched` / `mismatched` / `inconclusive`），避免跑完再补原因。运行和分析阶段各司其职，不再混在一起。

### 关键验证指标

以下数据均关闭 trace 和 parse，每项重复运行取中位数。效率对比基线为 `749a09f`，本版本提交为 `a5a519e`。

<table>
  <thead>
    <tr>
      <th>类别</th>
      <th>验证项</th>
      <th>测试场景</th>
      <th>结果</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>仿真功能</td>
      <td>四模式输出一致性</td>
      <td>32-host Clos、16-task fan-in / fan-out DAG；local、MTP、MPI、hybrid 四种模式 canonical 输出对比。</td>
      <td>
        <ul>
          <li>四种模式输出完全一致。</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td>引擎效率</td>
      <td>1K-host Clos 压缩路由表</td>
      <td><code>clos_1024h_32l_32s</code>；expanded route 对比 compressed route。</td>
      <td>
        <ul>
          <li>route 文件缩小 177.61x：21.56 MB -> 121.36 KB。</li>
          <li>RSS 下降 44.81%：1.021 GB -> 563.4 MB。</li>
          <li>仿真 run 阶段加速 5.70x：1959.6 ms -> 344.1 ms。</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td>引擎效率</td>
      <td>多线程性能（MTP，4 线程，vs 基线 <code>749a09f</code>）</td>
      <td>32-host Clos，20,000 条独立 <code>URMA_WRITE</code>。</td>
      <td>
        <ul>
          <li>wall time 加速 1.26x：7.980 s -> 6.350 s。</li>
          <li>仿真 run 阶段加速 1.26x：7805 ms -> 6214 ms。</li>
          <li>RSS 下降 3.86%：466.0 MB -> 448.0 MB。</li>
        </ul>
      </td>
    </tr>
    <tr>
      <td>引擎效率</td>
      <td>单线程性能（local，vs 基线 <code>749a09f</code>）</td>
      <td>同上，local 模式。</td>
      <td>
        <ul>
          <li>wall time 加速 1.10x：14.776 s -> 13.452 s。</li>
          <li>仿真 run 阶段加速 1.09x：14502 ms -> 13313 ms。</li>
          <li>RSS 基本持平：428.0 MB -> 427.6 MB。</li>
        </ul>
      </td>
    </tr>
  </tbody>
</table>

### 兼容性与迁移

- 带依赖的 `traffic.csv` 在 MPI / hybrid 模式下，跨 rank 任务完成的通知可见时间受 lookahead 约束，需据此设置依赖可见性延迟。
- `EnableRetrans=false` 时发生丢包，`ub-quick-example` 会直接 fail-fast。验证丢包恢复需要显式开启 `EnableRetrans` 并选好 `RetransmissionMode`。
- `SelectiveAckBitmapBits=0` 即 AUTO，运行时自动根据接收端乱序窗口确定 ack bitmap 编码位数。如果环境中存在 packet spray 或多路径乱序，建议开启 `EnableFastSelectiveRetrans`。

### 修复与文档

- 修复序列号 wrap 时的比较和窗口判断问题。
- 修复 Traffic DAG 并行运行时依赖维护、任务激活和优先级字段校验的问题。
- 修复 DCQCN 阈值边界标记，高于 `kmax` 的报文现在能正确标记。
- 修复路由 hash salt，不同节点的 packet spray 路径选择不再互相干扰。
- 更新 Quick Start、scratch case 文档和 `ns-3-ub-tools` 子模块，明确了 UB-only focused build 方式、`--no-build` 运行方式、路由范围和 traffic 数值字段含义。

---

## Release 1.2.1

**发布日期**: 2026 年 4 月

### 新特性与行为变化

- 完成拥塞控制、流量控制统一 hook 架构重构。拥塞控制算法通过 `OnSender*`、`OnSwitch*`、`OnReceiver*`、`OnTpAttached` 等 hook 接入发送端、交换机和接收端事件；流量控制算法通过 `OnIngress*`、`OnEgress*`、`OnControlFrameReceived`、`OnDataPacketReceived` 等 hook 接入口入队、出队、控制帧和数据信用事件。用户新增算法时，可复用现有拓扑、队列、trace 与配置文件路径，把算法逻辑集中在对应算法类和必要枚举/配置项里，减少对交换机、传输层和 case 模板的侵入式修改。当前内置支持 DCQCN 与 C-AQM 拥塞控制算法，以及 CBFC 与 PFC 流量控制算法。
- 新增 RTP 侧 DCQCN 支持，并新增 `PFC_DYNAMIC_PAPER` 作为 DCQCN 论文 **"Congestion Control for Large-Scale RDMA Deployments"** (SIGCOMM 2015) 的 paper-style dynamic PFC 阈值复现模式。
- `ub-quick-example` 在 `EnableRetrans=false` 且发生丢包时会提前停止，并提示检查路由、缓冲区和流控配置，避免无恢复能力的运行继续产生不可解释结果。

### 兼容性与迁移

- 本版本继续收拢旧 `scratch` 用例迁移路径，把常见旧配置项转换为运行前诊断提示，便于复制旧 case 后定位需要修改的参数。
- `network_attribute.txt` 现在会在 `ConfigStore` 前检查已知旧 key，并输出迁移提示。已知迁移包括：`ns3::UbQueueManager::ResumeOffset` → `ns3::UbQueueManager::DynamicPfcResumeGapBytes`，`ns3::UbSwitch::EnableCBFC/EnablePFC` → `ns3::UbSwitch::FlowControl`，`ns3::UbApiThread::*` → `ns3::UbLdstThread::*`。
- 如果旧 case 依赖旧的 `CbfcRetCellGrainControlPacket=1` 行为，请在 `network_attribute.txt` 里显式设置该值；当前 repo 默认值为 `32`。
- 细粒度 trace 文件由新开关控制：`UB_QUEUE_TRACE_ENABLE`、`UB_FLOW_CONTROL_TRACE_ENABLE`、`UB_CONGESTION_CONTROL_TRACE_ENABLE`。旧 case 不写这些开关可以继续运行，但不会自动生成对应的 `QueueTrace_*`、`PfcTrace_*`、`CbfcTrace_*`、`Dcqcn*` 或 `Caqm*` 文件。

---

## Release 1.2.0

**发布日期**: 2026 年 3 月

### 新特性

- **OpenUSim Agent Skill 体系**：新增仓库内置的四阶段 AI Agent Skill，让 AI 编码助手（Codex / Claude Code / Cursor 等）能够端到端地完成 UB 仿真实验。四个阶段分别为：环境就绪检查（welcome）、实验规划与参数收敛（plan-experiment）、case 生成与仿真执行（run-experiment）、结果解读与根因分析（analyze-results）。配套提供 AGENTS.md 路由规则和共享知识库（拓扑选项、负载模式、trace 可观测性等 7 份参考文档），以及仿真配置文件自动生成脚本

- **URMA Read 全链路支持**：实现完整的 URMA Read 请求/响应数据通路。Read 请求以零 payload 方式发送并携带逻辑字节数，对端收到后自动生成 Read Response 回传实际数据。Transport 层支持多包 Read 响应的重组与完成判定，Transaction 层区分 Request/Response 方向并正确处理 Read 与 Write 的不同完成语义

- **流控与缓冲区管理重构**：
  - **共享缓冲区动态准入控制**：重构入口缓冲区管理，采用 Reserve → Shared → Headroom 三层准入模型。每个入口队列拥有独立保留配额（Reserve），超出部分按动态阈值（Alpha）从全局共享池竞争分配，PFC 场景下进一步使用端口级 Headroom 吸收在途报文。支持 XOFF/XON 水位查询与防振荡偏移
  - **CBFC / PFC 流控模式完善**：流控模式扩展为五种——NONE、CBFC（独占 credit）、CBFC_SHARED（共享 credit 池）、PFC_FIXED（固定阈值反压）、PFC_DYNAMIC（基于缓冲区占用的动态阈值反压）。CBFC 与 PFC 作为对等的流控策略共用同一套入口缓冲区准入模型，可按场景灵活选择

- **MPI 多进程数据通路**：新增远程链路抽象，支持通过 MPI 跨进程传输 UB 报文，实现分布式多进程仿真。配合统一的 quick-example 入口，支持 MPI 配置驱动的多机拓扑仿真

### 功能优化

- **仿真进度停滞告警**：case-runner 实时监控任务完成进度，当长时间无任务完成时输出潜在死锁警告，便于快速定位流控死锁或路由环路等问题
- **细粒度 Tracing**：支持模块级 trace 开关，按需启用或禁用不同协议层的 trace 输出，降低大规模仿真的 I/O 开销
- **可观测性分级预设**：提供多级观测预设，在快速验证和深度分析场景间一键切换日志详细程度
- **TrafficGen 线程安全**：流量生成器支持 UNISON 多线程并发调度下的安全调用
- **TrafficGen 支持 URMA Read**：流量描述文件支持指定 URMA_READ 操作类型
- **统一仿真入口**：ub-quick-example 重构为配置驱动的统一入口，同时支持 MPI 多进程和 MTP 多线程两种运行模式

### Bug 修复

- 修复 TA 层调度 WQE Segment 时的公平性问题，消除部分 segment 饥饿
- 修复多包 URMA Read 请求的切片重组逻辑，保证数据完整性
- 修复路由 trace 记录的端口信息错误和 VOQ 索引越界检查
- 修复部分构建配置下 unified-bus 库链接缺失导致的编译失败
- 修复初始化阶段的竞态条件，提升启动稳定性
- MPI 相关测试按构建标志条件编译，非 MPI 构建不再报错

### 构建与 CI

- CI 流水线简化为 Ubuntu 单平台
- 新增 uv.lock 依赖锁文件，指定 Python 3.11
- 更新 ns-3-ub-tools 子模块

### 测试

- 新增 URMA Read、共享缓冲区准入、MPI CBFC 混合模式等多组回归测试
- 新增 TrafficGen 和 quick-example 入口边界测试
- 新增 Agent Skill 文档与脚本辅助函数测试
- 测试代码净增约 2800 行

---

## Release 1.1.0

**发布日期**: 2026 年 1 月

### 新特性

- **UNISON 多线程并行仿真**：集成 UNISON 框架，支持多线程并行仿真
- **DWRR 调度算法**：在网络层和数据链路层新增基于 DWRR（Deficit Weighted Round Robin）的 VL 间调度支持
- **自适应路由**：实现基于端口负载感知的自适应路由，支持可配置的路由属性
- **死锁检测**：在交换机和传输层新增潜在死锁检测，增强报文到达时间追踪
- **CBFC 共享信用模式**：引入 CBFC 共享信用模式，提供更灵活的流控配置

### 优化与 Bug 修复

- 优化 DWRR 用户配置方式
- 重构缓冲区管理架构，统一 VOQ 管理（双视图 + 出口统计）
- 优化路由表查找流程
- 改进队列管理，支持基于字节的出口队列限制
- 修复 LDST 与 CBFC 的兼容性问题
- 优化流量控制配置接口
- 修复交换仲裁器中的 TP 移除与信用恢复问题
- 支持无配置文件自动生成 TP
- 支持无用 TP 自动移除优化

---

## Release 1.0.0

ns-3-UB 仿真器初始版本，实现了灵衢基础规范中功能层、事务层、传输层、网络层和数据链路层的完整协议栈支持。

**核心特性：**
- 完整的 UB 协议栈实现
- 支持 Load/Store 和 URMA 编程接口
- 拥塞控制与流量控制机制
- 多路径路由与负载均衡
- 基于 SP 调度的 QoS 支持
- 基于信用的流量控制（CBFC）
