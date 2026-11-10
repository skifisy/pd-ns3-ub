# TE / TOE 每轮变化限幅

本功能在求解与下发之间增加可选后处理，不修改原有 TE 两阶段 LP、TOE 拓扑求解目标或逐流路径缓存。

## 使用方式

| 参数 | 不指定 | 显式指定 |
|---|---|---|
| `--te-max-weight-step=0.10` | 原 TE 发布方式 | 每个有方向 OD 每轮最多迁移 0.10 的路径选择概率 |
| `--toe-max-prefill-remove=2` | 原 TOE 完整目标拓扑 | 每台 Prefill Leaf 每轮最多拆除 2 根旧 OCS 电路 |

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

每台 Prefill Leaf 的拆链数为 `sum_j max(0,current(i,j)-planned(i,j))`，包括 P–P、P–D、P–S；新增电路不能抵消拆除量。P–P 两端分别消耗各自预算。预算以每轮实际拓扑为起点，不跨轮累积。

规划器从当前拓扑向完整 LP 目标挑选交替拆建链闭合组合：先生成两拆两建候选，再寻找更长的交替闭合路径，包括同一 Leaf 以不同拆建状态出现的组合。每次接受一组后重新计算剩余差分和预算。每组均保证：

- 每台 Leaf 端口数守恒；所有原允许 Pair 至少 1 根，不新增禁止的 Pair。
- 各 Pair 数量只向目标单调移动；Prefill 累计拆链不超预算。
- 预测 MLU 相对起始拓扑及上一接受拓扑均不恶化（浮点容差 `1e-12 * max(1,initial_mlu)`）。

在搜索到的候选中选择 MLU 最小者；收益相当时选择拆链更少者，然后使用固定 Pair 排序打破平局。预算只限制上限，不保证用满；无可接受组合就保持当前拓扑。`0` 禁止任何 Prefill 拆链，但仍允许完全不触及 Prefill 的合法变化。

这是有限搜索的局部算法，不承诺预算内全局最优或找到所有长组合。每次选取迭代的长组合搜索至多扩展 100,000 个状态；每轮 ECMP 至多评估 20,000 个候选、WCMP 至多 128 个，触及内部上限记录 `search_limited=1`。这些固定内部保护不增加 CLI 参数。

ECMP 评估包含同组 OD 按第一跳物理端口数分配的两跳负载。WCMP 预览使用当时 TE 自身历史窗口生成的需求矩阵，执行原 TE 求解及相同 step、transit cap 后处理，再在 TOE 本轮需求矩阵上计算 MLU。预览只评估，不发布权重、不消耗历史窗口。预览不会将流路径缓存建模为可精确预测的负载。

最终仅执行一次选中的拓扑，沿用物理端口规划、排空/有损切换、超时回退和恢复流程。不会在同一轮自动追加多阶段切换。下一 TOE 周期基于那时的实际拓扑重新规划。

| 日志 | 含义 |
|---|---|
| `toe_topology.csv: target` | 完整 LP 目标 |
| `toe_topology.csv: planned` | 本轮受限目标，只在开启限制时出现 |
| `toe_topology.csv: applied` | 物理回读后的实际拓扑；未调整时为当前拓扑 |
| `toe_bounded.csv` | 起始/计划预测 MLU、LP/计划重连数、搜索量和状态 |
| `toe_prefill_remove.csv` | 每台 Prefill 的 planned 拆链数；成功应用后追加 applied 实际拆链数 |

开启限制时 `toe_summary.csv` 追加 `bounded,current_predicted_mlu,planned_predicted_mlu`；原有 `min_mlu,applied_mlu_bound,solver_optimal` 始终为完整 LP 目标属性。`solved` 行的重连数为 LP 目标，后续 `draining/applied/bounded-no-change` 行为实际执行计划数量。物理拆建一致性检查也使用计划数量。原 `toe-mlu-slack` 不保证约束受限拓扑。切换期间历史可能推进，候选预览是决策时快照，应用后 TE 会按应用时的最新窗口重新求解，不能保证两次策略或实际 MLU 完全相同。

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

新增测试覆盖 10,000 组概率分布、首次 fallback、冻结/反向/空窗口、路径失效、强制中转排除与全网 cap 冲突、六边闭合组合、重复 Leaf 组合、预算不足、MLU 退化拒绝，以及 30 组固定种子的 Mooncake 拓扑及物理端口拆建一致性。
