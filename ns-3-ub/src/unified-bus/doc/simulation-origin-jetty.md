# 仿真内部的完整 Jetty 编号传递

TA 报头的 Initiator RC ID 只有 20 位。各节点创建的 Jetty 编号是 32 位，
超过 `1048575` 后，仅靠报头会与低编号碰撞，例如 `0` 与 `1048576`。
事务响应无法找到正确的原始 Jetty，会导致任务完成回调及其 phase 依赖停滞。
这里的边界针对每个节点的 Jetty 编号，不是 traffic 文件的总流数。

## 实现

- `UbSimOriginJettyTag` 保存完整的 `uint32_t` origin Jetty。
- `UbTransportChannel::GenDataPacket` 对超过 `0xFFFFF` 的编号添加 PacketTag。
  请求、READ_RESPONSE、WRITE 的事务响应和 GBN 重新生成的报文都经过此函数。
- `ParseReceivedDataPacket` 解析 TA 报头后检查 Tag，校验其低 20 位与报头一致。
  不一致时立即终止仿真，避免错误归属；检查在 release 构建中同样生效。
- `UbTransactionHeader::GetSimulationOriginJetty()` 优先返回恢复的完整编号，
  无 Tag 的普通报文回退到原始 RC ID。事务序号展开状态、接收事务的 Jetty 和
  origin Jetty 均使用此接口。事务层据此生成响应并找到原始 Jetty 的完成任务。
- 恢复发生在乱序缓存之前，报头对象的值复制保留完整编号。
  `SetIniRcId` 和 `Deserialize` 清除旧的恢复状态，防止对象复用污染。
- 选择性重传的 `Packet::Copy()` 保留 Tag；GBN 重新生成时再次附加 Tag。

PacketTag 是仿真元数据，不进入 `Packet::GetSize()` 或 `Packet::CopyData()` 的
线上字节；TA 报头仍为 8 字节，链路序列化时延、负载字节数和 MTU 不变。
低编号无需 Tag。已有 traffic、任务依赖与传输粒度无需修改。

## 范围

这是仿真内部编号扩展，不是硬件 RCID 资源模型。RTP 的任务完成回调会回收该任务独占的 Jetty；CTP 共享 Jetty 会保留。这里没有实现 RCID 的分配、编号复用策略或容量约束，也没有改变其他位置已有的 `UINT32_MAX` 无效值约定。完整 32 位 Tag 的编码本身支持 `UINT32_MAX`；这不意味着该保留值可作为正常任务的 Jetty。

## 回归测试

在 `ns-3-ub/` 下启用 unified-bus 单元测试并构建：

```bash
python3.12 ./ns3 configure --enable-modules=unified-bus --enable-tests \
  --disable-examples --disable-mpi --disable-mtp --disable-werror -d default -G Ninja
ninja -C cmake-cache test-runner
./build/utils/ns3.44-test-runner-default --suite=unified-bus-sim-origin-jetty --verbose
```

此测试套件覆盖：

- 边界编号 `0`、`1`、`1048575`、`1048576`、`1048577`、`2097152`；
  Tag 编码另覆盖 `UINT32_MAX`，端到端任务另覆盖 `UINT32_MAX - 1`。
- 低 20 位碰撞的多个 Jetty 共用一个 TP，分包 READ/WRITE 的任务完成回调正确且仅一次。
- 乱序缓存释放后的编号保留，以及高低 Jetty 独立的 TA SSN 展开状态。
- GBN 重建、选择性重传的 Tag 和报文字节保持。
- 报头重新解析、重新设置编号后恢复状态被清除，以及不匹配的 Tag 被拒绝。
