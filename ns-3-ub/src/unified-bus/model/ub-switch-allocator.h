// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_UBSWITCH_ALLOCATOR_H
#define UB_UBSWITCH_ALLOCATOR_H

#include <optional>
#include <vector>
#include <unordered_map>
#include "ns3/object.h"
#include "ns3/nstime.h"
#include "ns3/node.h"

namespace ns3 {

class UbSwitch;
class UbIngressQueue;
class UbPort;

// outport, priority, voq/TpChannel/ctrlq
typedef std::vector<std::vector<std::vector<Ptr<UbIngressQueue> > > > IngressSource_t;
typedef std::vector<bool> EgressStatus_t;

/**
 * @brief 交换机调度器基类
 */
class UbSwitchAllocator : public Object {
public:
    UbSwitchAllocator();
    virtual ~UbSwitchAllocator();
    static TypeId GetTypeId (void);
    void TriggerAllocator(Ptr<UbPort> outPort);
    virtual void AllocateNextPacket(Ptr<UbPort> outPort);
    virtual void Init();
    void SetNodeId(uint32_t nodeId) {m_nodeId = nodeId;}
    void RegisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority);
    void UnregisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority);
    void RegisterEgressStauts(uint32_t portsNum);
    void SetEgressStatus(uint32_t portId, bool status);
    bool GetEgressStatus(uint32_t portId);
    void DoDispose() override;
    void CheckDeadlock();

protected:
    struct IngressCandidate
    {
        Ptr<UbIngressQueue> queue;
        uint32_t ingressQueueSlot;
    };

    uint32_t GetIngressQueueSlotCount(uint32_t outPort, uint32_t priority) const;
    std::optional<IngressCandidate> FindNextEligibleIngressQueue(uint32_t outPort,
                                                                 uint32_t priority,
                                                                 uint32_t startIngressQueueSlot,
                                                                 Ptr<UbPort> egressPort) const;

    Time m_allocationTime;
    uint32_t m_nodeId;
    IngressSource_t m_ingressSources;
    EgressStatus_t m_egressStatus;
    std::vector<bool> m_isRunning;	
    std::vector<bool> m_oneMoreRound;
    uint32_t m_voqIngressSlotCount = 0;
};


/**
 * @brief 轮询算法的交换机调度器
 */
class UbRoundRobinAllocator : public UbSwitchAllocator {
public:
    UbRoundRobinAllocator() {}
    virtual ~UbRoundRobinAllocator() {}
    static TypeId GetTypeId(void);

    virtual void Init() override;
    Ptr<UbIngressQueue> SelectNextIngressQueue(Ptr<UbPort> outPort);
    virtual void AllocateNextPacket(Ptr<UbPort> outPort) override;

private:
    std::vector<std::vector<uint32_t> > m_rrIdx;
    std::vector<std::vector<bool> > m_rrPhaseSeeded;
};


/**
 * @brief 基于 DWRR 的 VL 间调度器
 *
 *  - 每个 [outPort][priority(VL)] 维护：
 *      - m_quantum
 *      - m_deficit  
 *  - m_currVlIdx[outPort]：当前在 VL 间 DWRR 轮询到的 VLAN
 *  - 相同 VLAN 内多个 IngressQueue 按 RR 轮询（m_rrIdx）
 */
class UbDwrrAllocator : public UbSwitchAllocator {
public:
    UbDwrrAllocator() {}
    virtual ~UbDwrrAllocator() {}
    static TypeId GetTypeId(void);

    virtual void Init() override;
    Ptr<UbIngressQueue> SelectNextIngressQueue(Ptr<UbPort> outPort);
    virtual void AllocateNextPacket(Ptr<UbPort> outPort) override;

    void SetQuantum(uint32_t priority, uint32_t quantum);
    void SetQuantum(uint32_t outPort, uint32_t priority, uint32_t quantum);

private:
    std::vector<std::vector<uint32_t> > m_rrIdx;
    std::vector<std::vector<bool> > m_rrPhaseSeeded;
    std::vector<std::vector<uint32_t> > m_quantum;
    std::vector<std::vector<uint32_t> > m_deficit;
    std::vector<uint32_t> m_currVlIdx;

    // 记录最近一次在某个 VLAN 下选中的 ingress queue slot，用于额度不足时回滚 m_rrIdx
    std::vector<std::vector<uint32_t> > m_lastSelectedIngressQueueSlot;

    uint32_t    m_defaultQuantum;
    std::string m_vlQuantumsStr;
    void ApplyDefaultQuantum();
    void ParseAndApplyVlQuantums(const std::string& s);
};

} /* namespace ns3 */

#endif /* UB_UbSwitchAllocator_H */
