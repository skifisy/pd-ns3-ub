# Replay telemetry v4：保留本地 MTP 8 线程

提供两份补丁：

- `te-toe-replay-telemetry-v3-to-v4-mtp8.patch`：已应用 v3 单开关补丁的工作树继续应用这份。
- `te-toe-replay-telemetry-v4-mtp8-from-original.patch`：原始仓库 bcb10862fc5ea0ab1d64add84e725fc293e71fe8 使用这份完整补丁。

两份选择一份，不要同时应用。原始基线之后若仅增加 dashboard.zip，不影响这些改动。
本次未运行编译、仿真、smoke 或测试；下面的检查命令供使用方自行执行。

## 你的 ECMP 命令

从仓库 ns-3-ub/ 目录重新编译，然后保留 8 线程：

```bash
python3.12 ./ns3 build ub-quick-example
python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=/mnt/yuhaoze/pd-ns3-ub/ns-3-ub/scratch/mooncake_v14_synthetic_ecmp_5min --jupiter-te=0 --toe-mode=off --replay --progress-interval=1000ms --mtp-threads=8'
```

TE 同样保留 --mtp-threads=8，使用原来的 --jupiter-te=1。
不修改 --replay 单开关、不要求额外日志参数，自动识别和 v3 一样。
8 是 MTP 请求的最大线程数；底层实际分区数不足时，仍遵循原有 MTP 调度规则。

## 并发与窗口

1. `m_shards` 在 Run 前分配，以逻辑分区 LP ID 为索引；不是 OS 线程 ID。
   MTP 的一个 LP 同时只有一个执行者，LP 在线程间迁移不会丢失或共享错误计数。
2. 成功发送的路径计数只更新当前 LP 分片，不写共享CSV，不调用 Sample，不修改全局窗口。
   工作阶段只读拓扑映射及当前发布的版本；控制状态只在公共 LP 阶段更新。
3. 初始采样事件在 NO_CONTEXT / LP0 创建，后续 Tick 也只在 LP0 调度。
   MTP 已有调度器以公共事件时间限制工作 LP 前进，并在工作线程屏障之后处理公共 LP。
   在此读取已有原子端口计数、写出各 LP 路径聚合、清空本窗口计数。
   对控制边界，时间相同且先于公共控制更新完成的工作线程发送计入更新前的版本。
4. 任务日志复用原生 TaskTrace；需要补写时，先写当前 LP 的有界字符串缓冲，
   约64KiB才对公共任务文件加锁写入。不是每个数据包争抢一把全局锁。
   任务CSV行允许交错，dashboard v6本来就是按task_id/时间恢复，不依赖全局行顺序。
5. Finish 在 Simulator::Run 返回、工作线程join后执行。
   最后一个不满500ms窗口用所有LP已执行时间的最大值收尾，避免公共LP时钟落后导致漏记。

元信息保留 schema_version=2（兼容dashboard），新增 replay_logger_version=4、
采样上下文、任务行顺序语义、请求线程数；首次周期采样补记实际 worker LP 数。
默认不增加逐包文本、每任务常驻map或全量任务时间数组。

## 支持范围

- ECMP + 本地 MTP：日志采集器支持。
- TE/WCMP + 本地 MTP：日志采集器支持，TE原有依赖/后端要求不变。
- MPI：日志还没有做分rank归并，仍然拒绝。
- TOE：原始重配控制器有独立的单线程限制，本补丁保留它。
  本补丁没有宣称或实现 TOE + 8线程重配，不能把采集器并发支持当作控制器并发支持。

## 可选的合成采集器检查

补丁包含 scratch/ub-replay-mtp-smoke.cc。它是采集器回归夹具，**不是端到端网络性能实验**：

```bash
python3.12 ./ns3 build ub-replay-mtp-smoke
python3.12 ./ns3 run --no-build 'scratch/ub-replay-mtp-smoke --output=/tmp/replay-mtp-smoke-v4'
```

夹具建立8个LP，注入发送计数和路径观测，在490/500/510ms同时产生任务与路径计数，
检查500ms边界、同一时刻策略更新归属、750ms最终部分窗口、总字节数和任务CSV完整性。
每个LP产生足够任务行触发多次缓冲写盘。通过会输出 PASSED，否则返回失败。
这不代替真实数据包收发、不同线程数结果对比、ThreadSanitizer或3000万任务性能验证。
