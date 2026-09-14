# Parallel timing offset regression demo

这是一个公开的小型合成用例，用来回归 MTP 同时间戳任务释放问题，并展示 initial task start 与 link delay 两层 deterministic offset 的共享 seed 语义。用例由 `scratch/ns-3-ub-tools` 生成，不包含私有 workload 数据。

- 拓扑：8 hosts、4 leaf、2 spines 的两层 Clos。
- 流量：All-to-All scatter，2 个 4-rank 通信域，`scatter-k=2`，每 rank `1MB`。
- network attributes：从当前构建的运行时 parameter catalog 写入完整快照。
- 显式 override：`UbSwitchAllocator::AllocationTime=1ns`、
  `UbJetty::UbJettyInflightMax=10000`。
- trace：只保留 task trace，避免 packet/port trace 把示例变重。

重新生成：

```bash
python3.12 scratch/ub_parallel_timing_offset_demo/generate_case.py
```

这个 case 有 phase dependency，下面的命令显式使用 `--dependency-visibility-delay=10ns`。

## 回归矩阵

不开 offset：

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/ub_parallel_timing_offset_demo --dependency-visibility-delay=10ns --canonical-output=/tmp/mtp_noj_single'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/ub_parallel_timing_offset_demo --dependency-visibility-delay=10ns --mtp-threads=4 --canonical-output=/tmp/mtp_noj_mtp4'
diff -q /tmp/mtp_noj_single.rank0.txt /tmp/mtp_noj_mtp4.rank0.txt
```

打开 initial task start offset 和 positive-link delay offset：

```bash
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/ub_parallel_timing_offset_demo --dependency-visibility-delay=10ns --initial-task-start-offset-window=32ps --link-delay-offset-window=8ps --timing-offset-seed=1 --canonical-output=/tmp/mtp_offset_single'
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=scratch/ub_parallel_timing_offset_demo --dependency-visibility-delay=10ns --mtp-threads=4 --initial-task-start-offset-window=32ps --link-delay-offset-window=8ps --timing-offset-seed=1 --canonical-output=/tmp/mtp_offset_mtp4'
diff -q /tmp/mtp_offset_single.rank0.txt /tmp/mtp_offset_mtp4.rank0.txt
```

同一 case 的实测结果：

| 代码与参数 | single vs MTP 4-thread |
| --- | --- |
| `origin/main`，offset 关闭 | 不一致 |
| 当前 runtime 修复，offset 关闭 | 字节级一致 |
| 当前修复，initial task `32ps` + link `8ps`，seed 1 | 字节级一致 |
| 当前修复，initial task `32ps` + link `8ps`，seed 2 | 字节级一致 |

当前实现的 seed 1 canonical SHA-256 为 `97a89e1715952efb0ada08957df943a06081dcdfc1a45af5a53ca74db1645be9`，seed 2 为 `a9dad361301f845bff15c318fde9db0aa6d85ef852b06c6be43ce3b7a4785a5e`。同 seed 的 single/MTP hash 相同，不同 seed 的 hash 不同。

这说明 runtime 修复已经消除了这个公开 case 的默认漂移；offset 不是让它通过所必需的参数。两层 offset 保留为显式、默认关闭的去同步 workaround。

## Offset 参数

`--initial-task-start-offset-window` 只作用于原始 `dependOnPhases` 为空的初始 task；同一 source 的初始 task 共用一个由 seed 和 source node 决定的 `[0, window)` offset，dependent task 不加 offset。

`--link-delay-offset-window` 在 topology 加载时为每条正延迟物理 link 计算一个静态 `[0, window)` offset。端点 node/port identity 和 seed 决定 offset，同一条双向 link 上的所有 packet 使用同一个调整后 delay。它不在 packet 路径调用 RNG。原始 `0ns` link 保持不变，避免破坏 MTP 的零延迟邻居同 LP 约束。

`--timing-offset-seed` 同时控制两种 offset；两者使用不同哈希域。

slot 可以碰撞；日志会报告已占 slot、碰撞数和最大占用。更大的窗口会降低碰撞概率，但也会扩大模型扰动，不能把窗口越大简单理解为越正确。

## 统计边界

这两个窗口都会改变模型时间，不是经过校准的物理硬件延迟模型。single/MTP 必须使用相同的两个 window 和 seed 做 paired comparison。多 seed 可以降低所选 offset ensemble 下汇总指标的 Monte Carlo 不确定性，但不能消除 task-level 模型偏差，也不能恢复 offset 关闭时的轨迹。5 个 seed 只适合 pilot；正式统计建议使用 15--20 个独立 seed，并报告 mean、SD 和置信区间。

## English notes

This public synthetic case demonstrates two default-off deterministic offset windows. The initial task start offset assigns one `[0, window)` offset per source whose tasks have no phase dependencies and leaves dependent tasks unchanged. The positive-link delay offset assigns one static offset per canonicalized physical link identity; it is not a per-packet RNG, and original zero-delay links remain zero to preserve MTP co-location semantics. Both mechanisms use `--timing-offset-seed` with separate hash domains.

The runtime fixes make offset-disabled single/MTP output match for this case. With initial-task `32ps` plus link `8ps`, seeds 1 and 2 are each byte-identical between single and MTP, while the two seeds produce different canonical hashes. These settings are model-perturbing workarounds, not a calibrated physical delay model. Pair runtimes by seed; use multiple seeds only to estimate the selected offset ensemble, not to claim that model bias has disappeared.
