# Jupiter 流量工程技术思路

本文说明当前 Mooncake OCS 场景中，Jupiter 风格的历史预测流量工程（TE）如何接入 Unified Bus 仿真：从实际业务中观测需求，按拓扑容量计算路径权重，再由源 leaf 对新流执行 WCMP。

## 一、整体架构

静态 `routing_table.csv` 为普通转发提供 shortest-path/ECMP 路由，但不随业务需求变化。加入 TE 后，控制器周期性地利用已发生的业务构造 leaf 到 leaf 的有向需求矩阵，在 OCS 容量约束下计算路径份额。转发面只在新流的首包上按份额选路，随后固定该流的路径，避免一次策略更新把同一流的报文分散到不同路径。

```mermaid
flowchart LR
    A["传输层首次生成<br/>WRITE / READ-response 数据包"] --> B["按实际 host 端口<br/>映射有向 leaf OD"]
    B --> C["30 秒观测窗口"]
    C --> D["历史窗口逐 OD 取最大值"]
    T["topology.csv<br/>OCS 端口容量"] --> E["有向逻辑边容量"]
    D --> F["两阶段 LP<br/>最小化 MLU，再最小化中转流量"]
    E --> F
    F --> G["原子发布 OD 路径权重"]
    G --> H["源 leaf 对新流执行 WCMP"]
    H --> I["直达或单中转<br/>物理链路逐流 ECMP"]
    I --> J["OCS 端口线速统计<br/>调试与效果分析"]
```

这个闭环不预读 `traffic.csv` 中未来的任务，也不根据事后完整流量回放 oracle 策略。矩阵和策略在 ns-3 进程内传递，CSV 只在调试模式下作为审计输出。

## 二、拓扑抽象与适用范围

当前实现面向 `tools/generate_ocs_topology.py` 生成的静态 OCS case。节点 37–56 是 20 个 leaf，分为 5 组，每组 4 个；组内没有 OCS 直连，跨组每对 leaf 有 4 条并行的 400 Gbps 物理链路。LP 将这 4 条链路合并成一条容量为 1.6 Tbps 的**有向**逻辑边，两个方向分别建模。容量由仿真中各 `UbPort` 的实际速率累加，而不是在 LP 中写死。

任意两个不同组各有 4 个 leaf，因此每对组包含 16 对跨组 leaf；每对 leaf 的 4 条并行物理链路在 LP 中合并为每方向 1.6 Tbps 的有向逻辑边。组内 leaf 之间没有 OCS 直连。

一个业务 OD（origin–destination）是 `(源 leaf, 目的 leaf)`。候选路径只有两类：有直连时的 `src → dst`，以及 `src → transit → dst` 的单中转路径。跨组 OD 有 1 条直达和 12 条单中转候选；同组 OD 无直达，有 16 条单中转候选。路径容量取所经过逻辑边容量的最小值。当前代码依赖这一 host/leaf 编号和静态拓扑约定，不能直接把任意 Clos case 当成受支持的 TE case。

拓扑脚本同时生成 `node.csv`、`topology.csv` 和 `routing_table.csv`。后者仍是普通路由所需的静态表，不包含 LP 算出的动态 WCMP 权重。

## 三、详细设计

### 3.1 流量矩阵观测

`UbTransportChannel::SendNewDataPacket` 在首次生成 WRITE 或 READ-response 数据包时，将业务载荷字节送入 `UbTeController::OnNewDataPacket`。控制器以实际 source/destination host 端口查 `topology.csv`，映射到有向 leaf OD，再按仿真时间累加到对应的 30 秒窗口。READ 的响应数据沿响应发送端到请求端方向计入矩阵。无载荷包、READ request、ACK、控制包和重传不计入需求；同 leaf 业务也不占用 OCS OD 矩阵。

每次重算只提取**已完成**的 30 秒窗口。对每个 OD，从最近 `--te-history-windows` 个完整窗口中取业务字节数最大值，换算为预测速率：

$$
D_{s,d}=\frac{8}{30}\max_{w\in\mathcal{H}} B_{w,s,d}\quad\text{bit/s}
$$

本轮参与最大值计算的历史完整窗口集合定义为：

$$
\mathcal{H}_T=\{w\in\mathbb{Z}\mid T-H\le w<T\}
$$

式中，`T` 是本轮重算时已完成窗口的边界编号，`H` 是 `--te-history-windows` 指定的回看窗口数；`w` 是一个 30 秒窗口的编号，`s` 和 `d` 分别是源 leaf 与目的 leaf。$$B_{w,s,d}$$ 是该窗口内从 `s` 到 `d` 首次生成的 WRITE 或 READ-response 数据包的业务载荷总字节数。$$D_{s,d}$$ 是该有向 leaf 对的**预测需求速率**（bit/s）：取上述窗口中的最大业务字节数，乘 8 换算成比特，再除以 30 秒。它不是当前窗口的实时速率，也不是 OCS 链路的线速利用率。

默认回看 120 个窗口，即最多 1 小时；`--te-recompute-seconds` 默认 30 秒，控制重算频率，不改变观测窗口长度。没有历史业务的 OD 不进入本轮 LP。启动时或该 OD 不在当前策略中时，源 leaf 采用保底路由：有直连就走直连；没有直连就从可行单中转路径中，按两段链路的瓶颈容量比例选取。若本轮完全没有预测需求，控制器清空策略，后续新流继续走保底路由。

这里的需求是**源端已经发起的数据量**，不是链路实际线速流量。控制器另在调试模式下从 OCS 发送端口统计线速字节，按有向逻辑边汇总；该值包含报文头、控制包和重传，用于事后评估，不反馈给 LP。因此预测需求与实际链路字节数不要求相等。

### 3.2 最小化MLU

`UbTeSolver` 在 ns-3 进程内调用 HiGHS C API。对每个有预测需求的 OD，定义候选路径份额、各有向逻辑边容量和最大链路利用率。第一阶段直接最小化 MLU：

$$
\begin{aligned}
\min_{x,U}\quad &U\\
\text{s.t.}\quad
&\sum_{p\in P_o}x_{o,p}=1 &&\forall o\in\mathcal{O},\\
&\sum_{o\in\mathcal{O}}\sum_{p\in P_o:\,e\in p}D_o x_{o,p}\le U C_e
&&\forall e\in\mathcal{E},\\
&x_{o,p}\ge 0 &&\forall o\in\mathcal{O},\ p\in P_o.
\end{aligned}
$$

式中分别使用 OD 的候选路径集合、预测速率、有向逻辑边容量和路径份额。链路负载按所有经过该边的 OD 路径流量相加。

第二阶段把最大利用率限制在第一阶段最优值附近，再最小化单中转路径承载的预测业务量：

$$
\begin{aligned}
\min_{x,U}\quad &\sum_{o\in\mathcal{O}}\sum_{p\in P_o^{\mathrm{transit}}}D_o x_{o,p}\\
\text{s.t.}\quad &U\le U^\star+\max\!\left(10^{-9},10^{-6}U^\star\right).
\end{aligned}
$$

其他流量守恒和链路容量约束与第一阶段相同。这里的容差用于消化数值误差，使策略在近似相同的瓶颈利用率下偏向直达路径。

**路径权重来自第二阶段解出的路径份额。** 第一阶段的 `U*` 只表示理论上可达到的最小最大链路利用率，本身不是权重。第二阶段在 `U` 基本保持最优的条件下，重新求出各 OD 的路径份额 `x*`；这些份额就是待下发的权重：

$$
w_{o,p}=x^*_{o,p},\qquad w_{o,p}\ge 0,\qquad \sum_{p\in P_o}w_{o,p}=1
$$

例如，某个 OD 的第二阶段解为直达 `0.60`、经 leaf 45 为 `0.30`、经 leaf 53 为 `0.10`，控制器就把这三个值作为该 OD 的路径权重。求解器会把极小的数值误差归零，并按每个 OD 的权重和归一化；控制器在发布前再次检查权重和为 1，然后一次性替换内存中的当前策略。策略按 OD 和路径保存：直达路径用 `transit_leaf = -1` 表示，单中转路径用实际中转 leaf ID 表示。3.3 节说明新流如何根据这些权重选中路径，再映射到物理出端口。

`--te-s` 是可选的路径份额约束，取值 `[0,1]`。当 `S>0` 时，每条候选路径的份额还满足：

$$
x_{o,p}\le\frac{C_p}{S\sum_{q\in P_o}C_q},\qquad S>0
$$

其中，路径容量取其所经逻辑边容量的最小值。

`S=0`（默认）关闭该上限；`S=1` 会强制同容量候选路径的份额接近均分，不能理解为“只用直达”。LP 使用无量纲路径份额，使每个 OD 的流守恒行都归一为 1，避免低速 OD 在求解容差下被当成零流量。求解失败会终止本次仿真。

### 3.3 WCMP选路

TE 的权重针对**逻辑路径**，不是交换机上的物理端口。求解器对每个源/目的 leaf 对输出一组路径权重，表示一个新流被分配到每条候选路径的比例；同一 leaf 对的路径权重和为 1。控制器将整组策略保存在进程内，并一次性替换当前策略。没有向每个交换机发送单独的控制报文。

新流经过源 leaf 时，选路按以下步骤进行：

1. **查 OD 权重。** `UbSwitch` 只把 WRITE 和 READ-response 的 IP 业务数据标记为 TE 流。源 leaf 根据实际源、目的 leaf 查询该 OD 的当前权重。控制流量和其他报文走普通路由。
2. **按权重选逻辑路径。** 对新流计算现有 `CalcHash` 流哈希，再把哈希映射到 `[0,1)` 的数值。该数值落入哪个路径的累计权重区间，就选中哪条路径。举例来说，假设求解器给 `leaf 37 → leaf 41` 下发以下权重：

   | 候选逻辑路径 | 权重 | 哈希区间 |
   | --- | ---: | --- |
   | 直达 `37 → 41` | 0.60 | `[0.00, 0.60)` |
   | 经 leaf 45：`37 → 45 → 41` | 0.30 | `[0.60, 0.90)` |
   | 经 leaf 53：`37 → 53 → 41` | 0.10 | `[0.90, 1.00)` |

   如果该流的哈希抽样值是 `0.73`，就选中 `37 → 45 → 41`。这个示例只解释权重如何转换为选择结果；真实权重由每轮 LP 根据预测需求和链路容量求出。
3. **把逻辑路径转成出端口。** 路径确定后，源 leaf 找到通往下一跳 leaf 的物理端口集合。直达路径的下一跳是目的 leaf；中转路径的下一跳是选中的 transit leaf。然后用同一流哈希在这组并行端口中选出一个具体出端口。在当前拓扑中，每对跨组 leaf 有 4 个物理端口，选出的端口相当于该 leaf 对上的逐流 ECMP。
4. **转发并固定该流。** 若选中中转，源 leaf 将包头 `RoutingPolicy` 设为 shortest；中转 leaf 只向目的 leaf 直达转发。源 leaf 缓存该流选中的路径和物理出端口，后续数据包沿用相同结果。下一轮权重更新只作用于新流，不会把已有流迁移到另一条路径。

概括来说，权重决定下一跳 leaf（目的 leaf 或中转 leaf），流哈希再从对应的 4 个物理端口中选一个。WCMP 按流分配路径，多个流的路径数量比例会接近权重；承载字节比例还会受到流大小差异影响。若策略中没有可用路径，控制器使用保底选路。

TE 要求业务使用逐流路由；若一个 TE 流启用了 packet spray，控制器会拒绝。当前只有单 MPI rank 才能得到完整的全局需求矩阵，因此 `--jupiter-te=1` 与多 rank MPI 不兼容。MTP 线程并行可用：需求按线程分片累加，控制器用锁保护历史、策略和路径选择。

## 四、运行方式

先按[运行手册](./JUPITER_TE_RUN.md)准备 HiGHS C++ 库并编译 `ub-quick-example`；case 目录需包含 `topology.csv`、`routing_table.csv`、`network_attribute.txt`、`traffic.csv` 等完整配置。从仓库根目录生成静态 OCS 拓扑后，进入 `ns-3-ub` 运行：

```bash
python3 tools/generate_ocs_topology.py
cd ns-3-ub
./ns3 run 'scratch/ub-quick-example --case-path=scratch/mooncake_pd_ocs --jupiter-te=1 --debug-te=1 --te-recompute-seconds=30 --te-history-windows=120 --te-s=0'
```

`--jupiter-te=1` 开启历史预测 TE 和逐流 WCMP；其余三个参数分别设置重算间隔、最多回看的 30 秒窗口数和可选路径份额上限。需要多线程时，可在启用 MTP 的构建中追加 `--mtp-threads=8`；TE 当前要求单 MPI rank。

默认调试输出在 `<case>/jupiter_te/`，可用 `--te-output` 改目录。主要文件如下：

| 文件 | 用途 |
| --- | --- |
| `observed.csv` / `prediction.csv` | 已完成窗口的源端业务字节 / 各 epoch 的预测需求 |
| `weights.csv` / `path_decisions.csv` | 发布的 OD 路径份额 / 新流实际选路 |
| `WcmpSelectionTrace.csv` | 通过 `UbFlowTag.taskId` 关联 task 与 WCMP 路径 |
| `link_bytes.csv` | OCS 发送端口汇总的有向逻辑边线速字节 |
| `epoch_summary.csv` / `te_debug.log` | 求解规模、瓶颈利用率、选路计数和控制过程 |
| `matrix-current.csv` / `policy-current.csv` / `solver-summary-current.csv` | 内存状态的当前审计快照，不参与求解 |
| `live_status.csv` / `observed-current.csv` / `demand_events.csv` | task 与报文进度、尚未结算的窗口和需求接受情况 |

最后一个不足 30 秒的观测窗口以 `complete=0` 写出，不进入预测。`tools/analyze_jupiter_te.py` 可用 `link_bytes.csv` 计算最忙有向链路利用率；比较 TE 与普通路由时，应使用同一拓扑、业务和仿真配置，并排除前 600 秒的预热区间。预热期间 TE 仍正常观测和重算，600 秒只是当前实验的评估分界，不是控制器的启动门槛。

