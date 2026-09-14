// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_QUEUE_MANAGER_H
#define UB_QUEUE_MANAGER_H

#include <cstddef>
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/ub-small-fifo-queue.h"
#include "ns3/nstime.h"
#include "ub-datatype.h"
#include "ub-network-address.h"

namespace ns3 {

class Node;
class UbPfcFixedDecisionHook;
class UbPfcDynamicDecisionHook;
class UbPfcPaperDynamicDecisionHook;

constexpr uint32_t DEFAULT_RESERVE_PER_QUEUE_BYTES = 1048576;  // 1MB
constexpr uint64_t DEFAULT_SHARED_POOL_BYTES       = 0;        // disabled
constexpr uint32_t DEFAULT_HEADROOM_PER_PORT_BYTES = 0;        // disabled
constexpr uint32_t DEFAULT_ALPHA_SHIFT             = 1;        // shared / 2
constexpr uint32_t DEFAULT_DYNAMIC_PFC_RESUME_GAP_BYTES = 4 * 1024;
constexpr uint32_t DEFAULT_PAPER_DYNAMIC_PFC_BETA = 8;
constexpr uint32_t DEFAULT_PAPER_DYNAMIC_PFC_PRIORITY_COUNT = 8;

enum class IngressQueueType {
    VOQ,        // Virtual Output Queue - 转发数据包队列
    TP,         // Transport Channel - 传输层可靠通道
    BASE        // 基类默认值 - 不应出现在运行时
};

struct UbIngressQueueOccupancy {
    uint64_t total_bytes {0};
    uint64_t shared_bytes {0};
    uint64_t headroom_bytes {0};
};

struct UbIngressPortOccupancy {
    uint64_t non_headroom_bytes {0};
    uint64_t headroom_bytes {0};
    uint64_t total_bytes {0};
};

struct UbSwitchBufferOccupancy {
    uint64_t shared_pool_used_bytes {0};
    uint64_t total_buffered_bytes {0};
};

struct UbBufferProfileView {
    uint64_t reserve_per_queue_bytes {0};
    uint64_t shared_pool_bytes {0};
    uint64_t headroom_per_port_bytes {0};
};

/**
 * @brief port的收包缓存队列
 */
class UbIngressQueue : public Object {
public:
    UbIngressQueue();
    virtual ~UbIngressQueue();
    static TypeId GetTypeId(void);
    
    virtual IngressQueueType GetIngressQueueType()
    {
        return IngressQueueType::BASE;
    }

    virtual bool IsEmpty();
    virtual bool IsLimited() { return false; }
    virtual Ptr<Packet> GetNextPacket();
    virtual uint32_t GetNextPacketSize();
    void SetInPortId(uint32_t inPortId) { m_inPortId = inPortId; }
    void SetIngressPriority(uint32_t priority) { m_ingressPriority = priority; }
    void SetOutPortId(uint32_t outPortId) { m_outPortId = outPortId; }
    uint32_t GetInPortId() { return m_inPortId; }
    uint32_t GetIngressPriority() { return m_ingressPriority; }
    uint32_t GetOutPortId() { return m_outPortId; }
    Time GetHeadArrivalTime() { return m_headArrivalTime; }
    bool IsControlFrame();
    bool IsForwardedDataPacket();
    bool IsGeneratedDataPacket();

protected:
    Time m_headArrivalTime = Seconds(0);

private:
    uint32_t m_ingressPriority;
    uint32_t m_inPortId;
    uint32_t m_outPortId;
};

/**
 * @brief VOQ (Virtual Output Queue) implementation of ingress queue
 */
class UbPacketQueue final : public UbIngressQueue {
public:
    UbPacketQueue();
    ~UbPacketQueue() final;
    static TypeId GetTypeId(void);

    bool IsEmpty() override;
    Ptr<Packet> GetNextPacket() override;
    Ptr<Packet> Front();
    void Pop();
    void Push(Ptr<Packet> p);
    IngressQueueType GetIngressQueueType() override;
    uint32_t GetNextPacketSize() override;

private:
    Ptr<Packet> m_frontPacket;
    UbSmallFifoQueue<Ptr<Packet>, 1> m_overflowQueue;
    size_t m_packetCount = 0;
    IngressQueueType m_ingressQueueType = IngressQueueType::VOQ;
};

/**
 * @brief VOQ Buffer统计管理模块（双视图：InPort + OutPort）
 * 
 * 架构说明：
 * - VOQ是Input-side的虚拟输出队列，按[outPort][priority][inPort]组织
 * - 本类提供两个统计视图：
 *   1. InPort视图：用于入端口流控（PFC检查入端口是否拥塞）
 *   2. OutPort视图：用于路由负载均衡和拥塞控制（检查出端口负载）
 * - 物理上只有一个包，但在两个视图中都有记录
 */
class UbQueueManager : public Object {
public:
    UbQueueManager();
    ~UbQueueManager() {}
    void Init();
    static TypeId GetTypeId(void);
    void SetOwnerNode(Ptr<Node> node)
    {
        m_ownerNode = node;
    }

    // Init
    void SetVLNum(uint32_t vlNum)
    {
        m_vlNum = vlNum;
    }
    void SetPortsNum(uint32_t portsNum)
    {
        m_portsNum = portsNum;
    }

    // ========== VOQ Dual-View Operations ==========
    
    /**
     * @brief 检查VOQ是否有空间容纳新包（检查双视图）
     * @param inPort 入端口
     * @param outPort 出端口
     * @param priority 优先级
     * @param pSize 包大小（字节）
     * @return true if both views have space
     */
    bool CheckVoqSpace(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 检查InPort视图是否有空间（用于丢包判断）
     * @param inPort 入端口
     * @param priority 优先级
     * @param pSize 包大小（字节）
     * @return true if InPort buffer has space
     */
    bool CheckInPortSpace(uint32_t inPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 查询OutPort视图的缓冲区占用（纯监控功能，不用于丢包决策）
     * @param outPort 出端口
     * @param priority 优先级
     * @param pSize 包大小（字节）
     * @return 统计信息，始终返回true（OutPort视图无物理缓冲区限制）
     */
    bool CheckOutPortSpace(uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 包进入VOQ时调用（同时更新双视图）
     */
    void PushToVoq(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    /**
     * @brief 包离开VOQ时调用（同时更新双视图）
     */
    void PopFromVoq(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    void PushToInPortProcessing(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    void MoveInPortProcessingToVoq(uint32_t inPort, uint32_t outPort, uint32_t priority, uint32_t pSize);
    
    // ========== 查询接口：InPort视图（用于流控） ==========
    
    /** InPort视图：某(inPort, priority)的 reserve+shared 占用（不含headroom） */
    uint64_t GetQueueIngressNonHeadroomBytes(uint32_t inPort, uint32_t priority) const;
    
    /** InPort视图：某inPort所有优先级的总 reserve+shared 占用 */
    uint64_t GetPortIngressNonHeadroomBytes(uint32_t inPort) const;
    
    // ========== 查询接口：OutPort视图（用于路由和拥塞控制） ==========
    
    /** OutPort视图：某(outPort, priority)的buffer占用 */
    uint64_t GetOutPortBufferUsed(uint32_t outPort, uint32_t priority);
    
    /** OutPort视图：某outPort所有优先级的总buffer占用 */
    uint64_t GetTotalOutPortBufferUsed(uint32_t outPort) const;
    
    uint32_t GetReservePerQueueBytes() const { return m_reservePerQueueBytes; }
    void SetReservePerQueueBytes(uint32_t size);
    void SetPaperDynamicAdmissionEnabled(bool enabled) { m_paperDynamicAdmissionEnabled = enabled; }
    bool IsPaperDynamicAdmissionEnabled() const { return m_paperDynamicAdmissionEnabled; }

    // ========== Three-Tier Buffer (reserve → shared → headroom) ==========

    bool CheckIngressAdmission(uint32_t inPort, uint32_t priority, uint32_t pSize);
    UbIngressQueueOccupancy GetIngressQueueOccupancy(uint32_t inPort, uint32_t priority) const;
    UbIngressPortOccupancy GetIngressPortOccupancy(uint32_t inPort) const;
    UbSwitchBufferOccupancy GetSwitchBufferOccupancy() const;
    UbBufferProfileView GetBufferProfileView() const;
    uint64_t GetQueueIngressSharedBytes(uint32_t inPort, uint32_t priority) const;
    uint64_t GetQueueIngressHeadroomBytes(uint32_t inPort, uint32_t priority) const;
    uint64_t GetQueueIngressTotalBytes(uint32_t inPort, uint32_t priority) const;
    uint64_t GetIngressControlBytes(uint32_t inPort, uint32_t priority) const;
    uint64_t GetOutPortControlBytes(uint32_t outPort, uint32_t priority) const;
    void AddEgressBufferedBytes(uint32_t bytes);
    void RemoveEgressBufferedBytes(uint32_t bytes);

private:
    using DarrayU64 = std::vector<std::vector<uint64_t>>;
    friend class UbPfcFixedDecisionHook;
    friend class UbPfcDynamicDecisionHook;
    friend class UbPfcPaperDynamicDecisionHook;
    bool IsLocallyGeneratedControlFrame(uint32_t inPort, uint32_t outPort, uint32_t priority) const;
    uint64_t GetIngressAdmissionThresholdBytes(uint32_t inPort, uint32_t priority) const;
    uint64_t GetDynamicPauseThresholdBytes() const;
    uint64_t GetDynamicResumeThresholdBytes() const;
    uint64_t GetPaperPauseThresholdBytes(uint64_t totalBufferedBytes) const;
    uint64_t GetPaperResumeThresholdBytes(uint64_t totalBufferedBytes) const;
    uint64_t GetSwitchTotalBufferedBytes() const;
    void UpdateIngressAdmission(uint32_t inPort, uint32_t priority, uint32_t pSize);
    void RemoveFromIngressAdmission(uint32_t inPort, uint32_t priority, uint32_t pSize);

    uint32_t m_vlNum = 0;
    uint32_t m_portsNum = 0;
    uint32_t m_reservePerQueueBytes {DEFAULT_RESERVE_PER_QUEUE_BYTES};

    // 双视图统计（同一个包在VOQ中，但从两个维度统计）
    DarrayU64 m_inPortBuffer;   // [inPort][priority] - 用于流控
    DarrayU64 m_outPortBuffer;  // [outPort][priority] - 用于路由和拥塞控制
    DarrayU64 m_inPortProcessingBytes;   // [inPort][priority]
    DarrayU64 m_outPortProcessingBytes;  // [outPort][priority]
    uint64_t m_totalProcessingBytes {0};

    // 三层 buffer 状态
    uint64_t m_sharedPoolBytes {DEFAULT_SHARED_POOL_BYTES};
    uint32_t m_alphaShift {DEFAULT_ALPHA_SHIFT};
    uint32_t m_headroomPerPortBytes {DEFAULT_HEADROOM_PER_PORT_BYTES};
    uint32_t m_dynamicPfcResumeGapBytes {DEFAULT_DYNAMIC_PFC_RESUME_GAP_BYTES};
    uint32_t m_paperDynamicPfcBeta {DEFAULT_PAPER_DYNAMIC_PFC_BETA};
    bool m_paperDynamicAdmissionEnabled {false};
    uint64_t m_totalHeadroomBytes {0};
    uint64_t m_totalReservedBytes {0};
    uint64_t m_sharedUsedBytes {0};
    uint64_t m_totalVoqBufferedBytes {0};
    uint64_t m_totalEgressBufferedBytes {0};
    std::vector<uint64_t> m_totalOutPortVoqBufferedBytes;
    DarrayU64 m_hdrmBytes;    // [inPort][priority] headroom usage
    DarrayU64 m_ingressControlBytes; // [inPort][priority] control-frame occupancy outside data-plane admission
    DarrayU64 m_outPortControlBytes; // [outPort][priority] control-frame occupancy outside data-plane admission
    Ptr<Node> m_ownerNode;
    TracedCallback<uint32_t, uint64_t> m_traceOutPortBufferBytes;
    TracedCallback<uint32_t, uint32_t, uint64_t> m_traceIngressQueueOccupancyBytes;
};
} // namespace ns3

#endif /* UB_QUEUE_MANAGER_H */
