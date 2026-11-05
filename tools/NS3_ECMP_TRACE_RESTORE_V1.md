# ECMP 选路日志恢复（v1）

本补丁基于 main 提交 `f5afca0a14239e0595535161e5d36fa3bea41233`。
直接修改源码，无需再运行 `apply_ns3_ecmp_trace_restore.py`。补丁从仓库根目录应用，
源码路径以 `ns-3-ub/` 开头。此前的 Decode 修改与本补丁独立。

## 应用及启动

```bash
git apply --check /path/to/ns3-ecmp-selection-trace-restore-v1.patch
git apply /path/to/ns3-ecmp-selection-trace-restore-v1.patch
```

在实验目录的 `network_attribute.txt` 中设置（已有配置项请修改原值）：

```text
global UB_TRACE_ENABLE "true"
global UB_ECMP_TRACE_ENABLE "true"
```

可以继续使用 `global UB_RECORD_PKT_TRACE "false"`，无需开启全量逐包记录。
在仓库的 `ns-3-ub` 目录运行，正常启动器会按需重新编译：

```bash
./ns3 run 'scratch/ub-quick-example --case-path=/mnt/liujiaxin/scratch/mooncake_agent_v17 --dependency-visibility-delay=1ns --mtp-threads=8'
```

## 记录含义与范围

文件位于本次实验的 `runlog/EcmpSelectionTrace_node_<switch_id>.tr`。
沿用原分析格式，例如：

```text
[10.000000us] ECMP Selection, flowId: 30494, sip: 0.0.0.13, dip: 0.0.0.23, priority: 7, hash: 123, ecmpWidth: 4, selectedIndex: 3, selectedOutPort: 9, hashIncludesTransportPorts: 1
```

示例仅说明格式。`flowId` 是 `traffic.csv` 的 `taskId`，来自现有 `UbFlowTag`；
它不是 request_id，关联请求时应查询 `traffic_debug.csv`。一条 task 在不同交换机
分别记录首次哈希选择，同一交换机的后续包、重传或返回方向包不再重复记录。

记录直接读取现有 `hash64`、候选端口数量及 `hash64 % totalSize` 的结果，不重新
计算或替换哈希。`hashIncludesTransportPorts` 同时覆盖 packet spray 和可选的
per-flow 传输端口参与哈希模式。最短路径标记和选路结果沿用原逻辑。

这项日志覆盖普通 HASH 选路路径。TE/ToE 控制器直接返回的路径不属于该记录范围；
没有 task tag 或没有可用出端口的包不记录。开启该类别后，普通 ADAPTIVE 选路处理
带 task tag 的包会明确报错，避免把自适应选择误记成 ECMP 哈希选择。

## 去重和并发

每个交换机按 taskId 分配分页位图，每页包含 4096 个 task 的 512 字节标记位，
仅分配用到的页，不按最大 taskId 创建完整数组。位图由互斥锁保护；输出沿用已有
文件级锁和缓冲。保持 `--mtp-threads=8`，不会降为单线程。

位图保留到本次运行结束，防止后续重传重复记录；`UbUtils::Destroy()` 在工作线程
停止后清除位图并刷新日志，下一次运行可以重新记录相同 task。
这只保存首次选择，不用于还原同一 task 在重配前后的完整路径变化。

## 验证

新增 C++ 回归覆盖默认关闭、总开关、关闭逐包记录时输出、taskId=0、分页边界、
uint32 最大 taskId、八线程重复标记、不同交换机独立去重、Destroy 后重新记录，
以及不同哈希模式的实际 hash/index/port 与开关前后选路一致性。

本次补丁交付进行了源码静态检查与 `git apply --check`；未编译或运行 ns-3。
配置并构建了测试目标的环境，可运行 `unified-bus-runtime-tools` 中的新增回归。
