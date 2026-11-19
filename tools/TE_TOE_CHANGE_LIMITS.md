# TE / TOE 每轮变化限幅

本功能在求解与下发之间增加可选后处理，不修改原有 TE 两阶段 LP、TOE 拓扑求解目标或逐流路径缓存。

## 使用方式

| 参数 | 不指定 | 显式指定 |
|---|---|---|
| `--te-max-weight-step=0.10` | 原 TE 发布方式 | 每个有方向 OD 每轮最多迁移 0.10 的路径选择概率 |
| `--toe-max-prefill-remove=2` | 原 TOE 完整目标拓扑 | 每个涉及 Prefill 的具体 Leaf Pair 每轮最多拆除 2 根旧 OCS 电路 |

```bash
# TE，其他 case / 仿真参数按原实验保留
./ns3 run 'scratch/ub-quick-example --case-path=<case> --jupiter-te=1 --te-max-weight-step=0.10 --debug-te=1'

# TOE 使用 ECMP，一次只应用预算内的拓扑
./ns3 run 'scratch/ub-quick-example --case-path=<case> --toe-mode=only --toe-max-prefill-remove=2'

# 联用；沿用 TOE 对运行线程、可靠传输、排空方式的原有要求
./ns3 run 'scratch/ub-quick-example --case-path=<case> --toe-mode=te --toe-max-prefill-remove=2 --te-max-weight-step=0.10 --debug-te=1'
```

CLI 只接受有限数值 `te-max-weight-step ∈ [0,1]`、十进制整数 `toe-max-prefill-remove ∈ [0,64]`。未指定与显式指定 `0` 不同。指定限制却未启用对应控制器会报错。C++ Configure API 以 `-1` 表示未启用，CLI 不接受这个哨兵值。

## TE 权重发布

对每个 OD，以最近实际发布的分布为 old，以本轮 LP 结果为 target，路径取并集、缺失值补零：

\[
D=\tfrac12\sum_p|w_p^{target}-w_p^{old}|,\quad
\alpha=\begin{cases}1&D\le\delta\\\delta/D&D>\delta\end{cases},\quad
w^{new}=(1-\alpha)w^{old}+\alpha w^{target}.
\]

例如 `[直达=1,A=0,B=0] → [0.4,0.4,0.2]`，step 为 0.10 时发布 `[0.9,0.0666667,0.0333333]`。每轮重新求 LP，向最新目标移动；不继续追赶旧目标。只在插值前处理求解器归一化误差，插值后不截断、再归一化。

- 首次出现的 OD 从当时实际 fallback 起步：有直达时为直达 1，否则按两跳瓶颈容量分配，匹配现有运行逻辑。
- 空历史窗口、暂时无需求的 OD 保留有效的已发布策略；LP 失败时保留旧策略并输出错误。
- step 为 0 冻结普通重算的权重变化；step 为 1 可一步到达本轮目标。
- TOE 后刷新物理端口和原有路径缓存，开启 step 时保留逻辑策略作为下一轮基准。若旧逻辑路径失效，先去掉失效概率并恢复可达性；这种强制变化单独记录，不计入普通 step。
- `--te-max-transit-ratio` 仍是全网需求加权的可选中转比例上限，强制中转 OD 不进入分母。限幅后重新检查；冲突时保留旧策略。若旧策略在最新需求下也已超上限，状态为 `held-cap-conflict-old-over-cap`，不会将它误报为满足上限。此保守策略可能长期保持旧权重。

step 约束概率分布的总变差距离，不约束实际迁移流数、字节数或哈希区间变化。当前已有流的 `m_flowTransit` 缓存保持不变，普通发布后已有流仍按原机制固定路径。TOE 清缓存后则重新选路。

启用 step 且 `--debug-te=1` 时新增：

| 文件 | 含义 |
|---|---|
| `target_weights.csv` | 归一化后的完整 LP 目标 |
| `weights.csv` / `policy-current.csv` | 本轮实际发布的完整策略，包括保留的无需求 OD |
| `weight_step.csv` | 每个 OD 的目标距离、普通发布距离、强制修复距离和发布状态 |
| `publication_summary.csv` | LP 最优 MLU、实际策略在本轮预测下的 MLU、旧/新可选中转比例及发布状态 |

`weight_step.csv` 的普通距离以可达性修复后的分布为基准；强制距离单列。既有 `epoch_summary.csv` 的 `min_max_utilization` 仍表示 LP 目标，不能解释为限幅策略的负载。全部值为连续权重，没有新增整数 WCMP 桶量化。

## TOE 受限拓扑

固定 Mooncake 映射：L37–L48 为 Prefill；20 个 Leaf 的分组和每 Leaf 64 个 OCS 端口沿用现有控制器要求。

`--toe-max-prefill-remove=k` 对每个涉及 Prefill 的无向具体 Leaf Pair `{i,j}` 分别施加 `max(0,current(i,j)-planned(i,j)) <= k`，包括 P–P、P–D、P–S。新增电路不能抵消拆除量，同一 Pair 在本轮多次接受的候选中累计拆除也不能超过 k；不同 Pair 不共用预算。P–P 按一个无向 Pair 检查，D–S 不受此 Prefill 拆链参数限制。预算以每轮实际拓扑为起点，不跨轮累积。

**口径变更：**旧版本按每台 P 的拆链总和限幅；当前 `remove1`（`--toe-max-prefill-remove=1`）改为每个具体 P 相邻 Pair 最多拆一根。同名旧实验与新实验不可直接视为相同限制；回放元数据新增 `toe_prefill_remove_scope=pair`，审计 CSV 同时写出 `budget_scope=pair`。

例如从原始每 Pair 四根的均匀拓扑出发，各 P–D、P–S Pair 拆一根，所有允许的跨组 P–P Pair 各加一根，所有 D–S Pair 各加三根，可得到：

| 电路类别 | 原始 | 调整后 | 变化 |
|---|---:|---:|---:|
| P–P | 192 | 240 | +48 |
| P–D | 192 | 144 | −48 |
| P–S | 192 | 144 | −48 |
| D–S | 64 | 112 | +48 |
| 总计 | 640 | 640 | 0 |

每台 P 共拆八根，但每个 P–D、P–S Pair 只拆一根，符合新 `remove1`；每台 D、S 各释放十二个连接 P 的端口并增加十二个 D–S 端口。每台 Leaf 仍使用 64 个 OCS 端口。实际物理计划保留 544 根、拆除 96 根、新建 96 根；D–S 的 48 根增量不是整个计划的重连总数。这里的 P–P 新增只发生在原有允许的跨组 Pair，不增加组内直连。

这是满足预算和端口约束的示例，不强制每个 D–S Pair 增加三根。当前规划器只使用本轮原求解器的结果，不执行第二次拓扑求解，也不对候选调用 TE 预览求解器。原 `UbToeSolver::Solve` 内部的 MLU 搜索和分阶段优化保持原样；应用后的常规 TE 刷新仍沿用原流程。

### 单次求解后的 D–S 分配

本模式专门执行 D–S 扩容及其配套重连，规则如下：

| Pair | 本轮规则 |
|---|---|
| P–D | 可从任何原有 P–D Pair 回收，最多 `min(k,current-1)` 根，不受原 LP 目标限制 |
| P–S | 仅在 LP 要求减少时回收，最多 `min(k,current-target)` 根，不越过 LP 目标；LP 保持或增加的 Pair 不拆 |
| D–S | 从 current 开始逐根增加，不超过该 Pair 的 LP target；不主动拆除已有 D–S |
| P–P | 每新增一根 D–S，配套增加一根合法跨组 P–P，不受其原 LP 目标限制；不主动拆 P–P |

将未达到目标的 D–S Pair 按 `target/current_planned` 降序排列。代码用 64 位整数交叉乘积比较比率，避免浮点平局；比率相同时优先剩余缺口更大的 Pair，再按固定 Pair ID 排序。

每轮扫描找到优先级最高且有合法捐赠组合的 Pair，执行：拆 `Pa–D` 和 `Pb–S`，建 `D–S` 和 `Pa–Pb`。两台 P 之间必须已有合法跨组 Pair，禁止自环和组内补边。捐赠组合优先选择当前电路数较少的 P–P，再按捐赠 Pair 的已用预算和固定 ID 打破平局。这只用于端口平衡，不改变 D–S 的目标满足率优先级。

每补一根就扣减具体 P–D、P–S Pair 的剩余预算，重新排序；最高优先级 Pair 找不到配套组合时跳过，继续尝试其它 Pair。所有目标已满足、预算用完或找不到合法组合时结束。每步保持所有 Leaf 度数、Pair 覆盖和最低一根连接，释放的端口全部复用。`k=0` 时不进行本模式的重连。

例如一台 D 当前为 `4/4/4/4`，LP 目标为 `10/8/6/6`，D 侧可回收十二个端口，且 S 侧和 P–P 配套允许时，本轮可得到 `9/7/6/6`。目标十根也可达到十根，没有六/七/九根的分档阈值。

第 4 轮附件回归中，LP 的 D–S target 全部为九根，P–S target 全部低于四根。remove1 能将 D–S 从 64 根扩到 112 根、每 Pair 七根，虽然部分 P–D 的原 LP target 仍是四根，P–P 目标也只集中在少数 Pair。算法不会把这些原目标当作捐赠/补边约束。

这是确定性贪心分配，不保证预算内全局最优或用满所有理论可回收端口。它不再执行旧版的逐候选 MLU 单调验收：target 比率是容量期望的启发式，不能保证全网 MLU 或实际 FCT 改善。`current_predicted_mlu` 和 `planned_predicted_mlu` 仅作算术诊断；only 模式标记 `ecmp`，with-TE 模式标记 `ecmp_proxy`，不能把后者解释为 WCMP 预测或验收结果。ECMP 诊断包含同组 OD 的单中转负载。

最终仅执行一次选中的拓扑，沿用物理端口规划、排空/有损切换、超时回退和恢复流程。不会在同一轮自动追加多阶段切换。下一 TOE 周期基于那时的实际拓扑重新规划。

| 日志 | 含义 |
|---|---|
| `toe_topology.csv: target` | 完整 LP 目标 |
| `toe_topology.csv: planned` | 本轮受限目标，只在开启限制时出现 |
| `toe_topology.csv: applied` | 物理回读后的实际拓扑；未调整时为当前拓扑 |
| `toe_bounded.csv` | 起始/计划 MLU 诊断、LP/计划重连数、`allocated_ds_circuits` 和 `mlu_model`；兼容字段 `candidate_evaluations,search_limited` 固定为零 |
| `toe_ds_allocation.csv` | planned 逐步分配记录：D–S 的分配前/LP目标/分配后条数、比率优先级、P–D/P–S 捐赠 P 和 P–P 补边；是否实际生效仍看 applied 日志 |
| `toe_prefill_remove.csv` | 每台 Prefill 的 planned/applied 拆链总数，以及 `max_removed_per_pair`；总数可以大于预算，`max_prefill_remove` 是每 Pair 上限，`budget_scope=pair` |
| `toe_prefill_pair_remove.csv` | 每个涉及 Prefill 的无向具体 Pair 的 planned/applied 拆链数和预算，包括零拆链 Pair；applied 按实际物理拆链计数，并逐 Pair 验证上限 |

开启限制时 `toe_summary.csv` 追加 `bounded,current_predicted_mlu,planned_predicted_mlu,bounded_mlu_model`；原有 `min_mlu,applied_mlu_bound,solver_optimal` 始终为完整 LP 目标属性。`solved` 行的重连数为 LP 目标，后续 `draining/applied/bounded-no-change` 行为实际执行计划数量。物理拆建一致性检查也使用计划数量。原 `toe-mlu-slack` 不保证约束受限拓扑。应用后 TE 会按应用时的最新窗口重新求解，算术诊断不预测这些新权重或实际流哈希的效果。

分析脚本 `analyze_toe_v3.py` 优先使用 applied 快照；有应用事件但缺少 applied 快照时优先回退 planned。明确标记 bounded 的 epoch 不允许再回退完整 target，避免把未执行电路算成已执行。

## 验证

在仓库根目录运行纯 C++ 约束与物理规划测试（不运行 ns-3 仿真）：

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=undefined tools/tests/test_te_toe_change_limits.cc -o /tmp/test_te_toe_change_limits
/tmp/test_te_toe_change_limits
g++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=undefined tools/tests/test_toe_reconfiguration.cc -o /tmp/test_toe_reconfiguration
/tmp/test_toe_reconfiguration
python3 -m unittest discover -s tools/tests -p test_analyze_toe_v3.py
```

测试覆盖 10,000 组 TE 概率分布、fallback/冻结/反向/空窗口/失效路径/cap；附件第 4 轮目标重放、P–D 和 P–P 脱离原目标、P–S LP 约束、比率优先级、9/7/6/6 和十根目标、耗尽 Pair 跳过、零/双倍预算、跨轮预算重置、输入校验、100 组固定种子目标、全部物理端口唯一性及回读。另一个物理规划测试覆盖 1,000 次连续重连。
