# Switch Buffer Management Architecture

## 1. 概述

本文档描述 Unified-Bus 交换机中的虚拟输出队列（VOQ, Virtual Output Queue）缓冲区管理架构。该架构的核心设计是**双视图统计系统**，从入端口（InPort）和出端口（OutPort）两个维度对同一个包进行记账，分别服务于不同的控制平面功能。

### 1.1 设计目标

- **准确的流控决策**：通过 InPort 视图统计，准确判断每个入端口的缓冲区占用，支持 PFC（Priority Flow Control）等流控机制
- **精确的负载均衡**：通过 OutPort 视图统计，实时掌握每个出端口的负载情况，支持自适应路由选择
- **拥塞控制集成**：OutPort 视图 + EgressQueue 统计为 CAQM 等拥塞控制算法提供准确的队列深度信息
- **灵活的优先级管理**：支持多优先级队列，两个视图均按优先级独立统计

### 1.2 关键组件

| 组件 | 文件位置 | 职责 |
|------|---------|------|
| **UbQueueManager** | `ub-queue-manager.h/cc` | 双视图缓冲区统计管理 |
| **UbSwitch** | `ub-switch.h/cc` | VOQ 数据结构维护和包调度 |
| **UbPort::m_ubQueue** | `ub-port.h/cc` | 出端口 EgressQueue（512KB 字节限制） |
| **UbCaqm** | `protocol/ub-caqm.h/cc` | CAQM 拥塞控制算法 |

---

## 2. VOQ 双视图架构

### 2.1 架构原理

VOQ 是输入端侧的虚拟输出队列，数据结构按 `[outPort][priority][inPort]` 组织。每个数据包**物理上只存在一份**，但在统计系统中**同时记录在两个视图**：

```
物理存储：VOQ[outPort][priority][inPort] = Queue<Packet>
统计视图：
  ├─ InPort 视图：m_inPortBuffer[inPort][priority] += packetSize
  └─ OutPort 视图：m_outPortBuffer[outPort][priority] += packetSize
```

#### 示例场景

假设一个数据包从 Port 0 进入，目标是 Port 2，优先级为 1：

```cpp
// 包入队时
VOQ[2][1][0].push(packet);  // 物理存储在 VOQ[outPort=2][priority=1][inPort=0]
m_inPortBuffer[0][1] += packetSize;   // InPort 视图记账：Port 0 增加
m_outPortBuffer[2][1] += packetSize;  // OutPort 视图记账：Port 2 增加

// 包出队时
VOQ[2][1][0].pop();
m_inPortBuffer[0][1] -= packetSize;   // InPort 视图减账
m_outPortBuffer[2][1] -= packetSize;  // OutPort 视图减账
```

### 2.2 双视图的作用分工

#### InPort 视图 (`m_inPortBuffer[inPort][priority]`)

**用途**：**入端口流控和丢包决策**

- **流控（PFC）**：当某入端口的某优先级队列占用超过阈值时，向上游发送暂停帧
- **丢包决策**：`CheckInPortSpace()` 检查入端口缓冲区是否还有空间接收新包
- **物理限制**：每个 `(inPort, priority)` 队列有物理缓冲区限制 `m_inPortPriorityBufferLimit`

**关键 API**：
```cpp
bool CheckInPortSpace(uint32_t inPort, uint32_t priority, uint32_t pSize);
uint64_t GetQueueIngressNonHeadroomBytes(uint32_t inPort, uint32_t priority);
uint64_t GetPortIngressNonHeadroomBytes(uint32_t inPort);
```

#### OutPort 视图 (`m_outPortBuffer[outPort][priority]`)

**用途**：**出端口负载均衡和拥塞控制**

- **多路径路由**：比较候选出端口的 OutPort 视图统计，选择负载最轻的路径
- **拥塞控制（CAQM）**：OutPort 视图 + EgressQueue 字节数 = 总队列深度，用于计算拥塞信用
- **监控统计**：纯监控功能，**不影响丢包决策**（OutPort 视图无物理缓冲区限制）

**关键 API**：
```cpp
bool CheckOutPortSpace(uint32_t outPort, uint32_t priority, uint32_t pSize);  // 始终返回 true
uint64_t GetOutPortBufferUsed(uint32_t outPort, uint32_t priority);
uint64_t GetTotalOutPortBufferUsed(uint32_t outPort);
```

---

## 3. 代码实现详解

### 3.1 数据结构定义

#### UbQueueManager 类（`ub-queue-manager.h`）

```cpp
class UbQueueManager : public Object {
private:
    using DarrayU64 = std::vector<std::vector<uint64_t>>;
    
    uint32_t m_vlNum;       // 优先级数量
    uint32_t m_portsNum;    // 端口数量
    uint32_t m_inPortPriorityBufferLimit;  // InPort 每队列缓冲区限制（字节）
    
    // 双视图统计：同一个包在 VOQ 中，但从两个维度统计
    DarrayU64 m_inPortBuffer;   // [inPort][priority] - 用于流控
    DarrayU64 m_outPortBuffer;  // [outPort][priority] - 用于路由和拥塞控制
};
```

#### UbSwitch 类（`ub-switch.h`）

```cpp
class UbSwitch : public Object {
private:
    // VOQ 物理数据结构：[outPort][priority][inPort]
    using VirtualOutputQueue_t = std::vector<std::vector<std::vector<std::queue<Ptr<Packet>>>>>;
    VirtualOutputQueue_t m_voq;
    
    Ptr<UbQueueManager> m_queueManager;  // 双视图统计管理器
};
```

### 3.2 核心操作流程

#### 3.2.1 包入队（`PushToVoq`）

**调用链**：`UbSwitch::PushPacketToVoq()` → `UbQueueManager::PushToVoq()`

```cpp
// ub-queue-manager.cc
void UbQueueManager::PushToVoq(uint32_t inPort, uint32_t outPort, 
                               uint32_t priority, uint32_t pSize)
{
    // 同时更新两个视图
    m_inPortBuffer[inPort][priority] += pSize;    // InPort 视图
    m_outPortBuffer[outPort][priority] += pSize;  // OutPort 视图
}

// ub-switch.cc
void UbSwitch::PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, 
                               uint32_t priority, uint32_t inPort)
{
    m_voq[outPort][priority][inPort].push(p);  // 物理入队
    m_queueManager->PushToVoq(inPort, outPort, priority, p->GetSize());  // 统计
}
```

#### 3.2.2 包出队（`PopFromVoq`）

**调用链**：`UbSwitch::ForwardDataPacket()` → `UbQueueManager::PopFromVoq()`

```cpp
// ub-queue-manager.cc
void UbQueueManager::PopFromVoq(uint32_t inPort, uint32_t outPort, 
                                uint32_t priority, uint32_t pSize)
{
    // 同时更新两个视图
    m_inPortBuffer[inPort][priority] -= pSize;    // InPort 视图
    m_outPortBuffer[outPort][priority] -= pSize;  // OutPort 视图
}

// ub-switch.cc
void UbSwitch::ForwardDataPacket(uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    auto& queue = m_voq[outPort][priority][inPort];
    Ptr<Packet> packet = queue.front();
    queue.pop();  // 物理出队
    m_queueManager->PopFromVoq(inPort, outPort, priority, packet->GetSize());  // 统计
    
    // 发送到出端口
    SendPacketToPort(outPort, packet);
}
```

#### 3.2.3 空间检查（双视图检查）

**场景**：包到达交换机后，决定是否入队或丢弃

```cpp
// ub-queue-manager.cc
bool UbQueueManager::CheckVoqSpace(uint32_t inPort, uint32_t outPort, 
                                   uint32_t priority, uint32_t pSize)
{
    // 必须同时满足两个视图的空间要求
    bool inPortOk = CheckInPortSpace(inPort, priority, pSize);
    bool outPortOk = CheckOutPortSpace(outPort, priority, pSize);  // 实际始终返回 true
    return inPortOk && outPortOk;
}

bool UbQueueManager::CheckInPortSpace(uint32_t inPort, uint32_t priority, uint32_t pSize)
{
    uint64_t currentUsed = m_inPortBuffer[inPort][priority];
    // InPort 有物理限制，超过则丢包
    return (currentUsed + pSize) <= m_inPortPriorityBufferLimit;
}

bool UbQueueManager::CheckOutPortSpace(uint32_t outPort, uint32_t priority, uint32_t pSize)
{
    uint64_t currentUsed = m_outPortBuffer[outPort][priority];
    // OutPort 视图仅做统计，始终返回 true（无物理限制）
    // 统计信息用于路由负载均衡和拥塞控制
    return true;
}
```

---

## 4. 与 EgressQueue 的集成

### 4.1 EgressQueue 角色

每个出端口有一个 **EgressQueue**（`UbPort::m_ubQueue`），位于 VOQ 之后：

```
[Packet Flow]
Input Port → VOQ[outPort][priority][inPort] → EgressQueue[outPort] → Output Line
             ↑                                  ↑
             InPort 视图统计               512KB 字节限制
             OutPort 视图统计              （基于字节的尾丢）
```

**EgressQueue 特性**：
- **容量限制**：512KB（固定值，见 `ub-port.cc` 中 `UbQueue::SetBufferSize(512000)`）
- **丢包策略**：基于字节的尾丢（Tail Drop），超过 512KB 时丢弃新包。当前实现中，交换机的 allocator 不会激进地向 EgressQueue 中发送数据包（ port 在 BUSY 状态时不会调度包），因此 EgressQueue 丢包概率较小
- **独立于 VOQ**：EgressQueue 是出端口的物理发送队列，与 VOQ 独立统计

### 4.2 总队列深度计算

**公式**：`总队列深度 = VOQ OutPort 视图 + EgressQueue 字节数`

#### 实现示例（CAQM 拥塞控制）

```cpp
// ub-caqm.cc:ResetLocalCc()
void UbSwitchCaqm::ResetLocalCc()
{
    for (uint32_t portId = 0; portId < ndevice; portId++) {
        // 1. 使用 OutPort 视图统计 VOQ 占用
        uint64_t voqUsed = sw->GetQueueManager()->GetTotalOutPortBufferUsed(portId);
        
        // 2. 加上 EgressQueue 的字节占用
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
        uint64_t egressUsed = port->GetUbQueue()->GetCurrentBytes();
        
        // 3. 总队列占用 = VOQ + EgressQueue
        uint64_t totalQueueSize = voqUsed + egressUsed;
        
        // 4. 计算拥塞信用（CC）
        uint64_t cc = uint64_t(m_lambda *
                            (m_ccUpdatePeriod.GetSeconds()
                            * m_bps[portId].GetBitRate() / 8
                            - m_txSize[portId]
                            + m_idealQueueSize
                            - totalQueueSize          // ← 关键：使用总队列深度
                            - m_creditAllocated[portId]));
        m_cc[portId] = cc;
    }
}
```

**设计原因**：
- VOQ 中的包正在等待仲裁调度，EgressQueue 中的包正在等待线路发送
- 两者都会占用出端口的"逻辑缓冲区"，必须合并统计才能准确反映出端口拥塞状态
- CAQM 算法需要这个总队列深度来计算拥塞信用，从而调节发送速率

---

## 5. 路由决策中的应用

### 5.1 多路径路由负载均衡

**场景**：自适应路由算法需要选择负载最轻的出端口

```cpp
// 伪代码：选择负载最轻的候选路径
uint32_t SelectBestPath(std::vector<uint32_t> candidatePorts, uint32_t priority)
{
    uint32_t bestPort = candidatePorts[0];
    uint64_t minLoad = GetQueueManager()->GetOutPortBufferUsed(bestPort, priority);
    
    for (uint32_t port : candidatePorts) {
        // 使用 OutPort 视图统计比较负载
        uint64_t load = GetQueueManager()->GetOutPortBufferUsed(port, priority);
        if (load < minLoad) {
            minLoad = load;
            bestPort = port;
        }
    }
    return bestPort;
}
```

---

## 6. 使用示例

### 7.1 包接收和入队

```cpp
// ub-switch.cc:HandleURMADataPacket()
void UbSwitch::HandleURMADataPacket(Ptr<Packet> packet, uint32_t inPort)
{
    // 1. 解析 header，确定 outPort 和 priority
    ParsedURMAHeaders headers;
    ParseURMAPacketHeader(packet, headers);
    uint32_t outPort = headers.datalinkPacketHeader.GetPacketOutPort();
    uint32_t priority = headers.datalinkPacketHeader.GetPriority();
    
    // 2. 检查 VOQ 空间（双视图检查）
    if (!m_queueManager->CheckVoqSpace(inPort, outPort, priority, packet->GetSize())) {
        // InPort 缓冲区满，丢包
        DropPacket(packet, "VOQ buffer full");
        return;
    }
    
    // 3. 入队到 VOQ，同时更新双视图统计
    PushPacketToVoq(packet, outPort, priority, inPort);
}
```

### 6.2 调度和出队

```cpp
// ub-switch.cc:ForwardDataPacket()
void UbSwitch::ForwardDataPacket(uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    // 1. 从 VOQ 取出包
    auto& queue = m_voq[outPort][priority][inPort];
    if (queue.empty()) return;
    
    Ptr<Packet> packet = queue.front();
    queue.pop();
    
    // 2. 更新双视图统计
    m_queueManager->PopFromVoq(inPort, outPort, priority, packet->GetSize());
    
    // 3. 发送到 EgressQueue（如果满则由 EgressQueue 丢弃）
    Ptr<UbPort> port = DynamicCast<UbPort>(GetDevice(outPort));
    port->SendPacket(packet);  // 内部会调用 UbQueue::Enqueue()
}
```

### 6.3 流控（PFC）检查

```cpp
// 伪代码：PFC 暂停帧生成
void CheckPfcCondition(uint32_t inPort, uint32_t priority)
{
    if (flowControlMode == PFC_FIXED) {
        uint64_t ingressBytes = m_queueManager->GetQueueIngressTotalBytes(inPort, priority);
        if (ingressBytes >= port->GetPfcUpThld()) {
            SendPfcPause(inPort, priority);
        }
    } else if (flowControlMode == PFC_DYNAMIC) {
        uint64_t hdrmUsed = m_queueManager->GetQueueIngressHeadroomBytes(inPort, priority);
        uint64_t sharedUsed = m_queueManager->GetQueueIngressSharedBytes(inPort, priority);
        if (hdrmUsed > 0 || sharedUsed >= m_queueManager->GetXoffThreshold()) {
            SendPfcPause(inPort, priority);
        }
    }
}
```

---

## 7. 总结

### 7.1 核心设计原则

1. **双视图分离**：InPort 视图管流控，OutPort 视图管路由，各司其职
2. **统计一致性**：包入队/出队时同步更新两个视图，保证统计准确
3. **物理 vs 虚拟**：InPort 视图有物理限制（丢包），OutPort 视图无限制（纯统计）
4. **与 EgressQueue 协同**：总队列深度 = VOQ OutPort 视图 + EgressQueue

### 7.2 关键 API 总结

| API | 功能 | 返回值 | 用途 |
|-----|------|--------|------|
| `CheckVoqSpace()` | 双视图空间检查 | bool | 包入队前判断 |
| `CheckInPortSpace()` | InPort 视图检查 | bool（有限制） | 丢包决策 |
| `CheckOutPortSpace()` | OutPort 视图检查 | bool（始终 true） | 统计监控 |
| `PushToVoq()` | 包入队统计 | void | 更新双视图 |
| `PopFromVoq()` | 包出队统计 | void | 更新双视图 |
| `GetQueueIngressNonHeadroomBytes()` | Queue ingress 非 headroom 占用查询 | uint64_t | 流控决策 |
| `GetOutPortBufferUsed()` | OutPort 占用查询 | uint64_t | 路由负载均衡 |
| `GetTotalOutPortBufferUsed()` | OutPort 总占用 | uint64_t | CAQM 拥塞控制 |

### 7.3 注意事项

- **双视图同步**：每次 VOQ 操作必须同时更新两个视图
- **EgressQueue 独立**：EgressQueue 有自己的丢包逻辑，与 VOQ 解耦
- **统计粒度**：两个视图都支持按优先级独立统计

---

## 8. 相关代码文件

| 文件路径 | 主要内容 |
|---------|---------|
| `src/unified-bus/model/ub-queue-manager.h` | UbQueueManager 类定义 |
| `src/unified-bus/model/ub-queue-manager.cc` | 双视图统计实现 |
| `src/unified-bus/model/ub-switch.h` | UbSwitch 和 VOQ 数据结构 |
| `src/unified-bus/model/ub-switch.cc` | 包处理和调度逻辑 |
| `src/unified-bus/model/ub-port.h` | UbPort 和 EgressQueue 定义 |
| `src/unified-bus/model/protocol/ub-caqm.cc` | CAQM 拥塞控制实现 |
