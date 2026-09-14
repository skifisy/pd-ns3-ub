// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-port.h"
#include "protocol/ub-routing-process.h"
#include "protocol/ub-transport.h"
#include "ub-queue-manager.h"

#include <algorithm>

namespace ns3 {

namespace {

uint32_t
ComputeInitialIngressPhase(uint32_t nodeId, uint32_t outPortId, size_t qSize)
{
    if (qSize == 0) {
        return 0;
    }
    return static_cast<uint32_t>((nodeId + outPortId) % qSize);
}

} // namespace

NS_OBJECT_ENSURE_REGISTERED(UbSwitchAllocator);
NS_OBJECT_ENSURE_REGISTERED(UbDwrrAllocator);
NS_LOG_COMPONENT_DEFINE("UbSwitchAllocator");

namespace {

UbFlowControlEventContext
MakeAllocatorFlowControlEventContext(Ptr<Packet> packet,
                                     Ptr<UbIngressQueue> ingressQueue,
                                     uint32_t inPortId,
                                     uint32_t outPortId,
                                     uint32_t priority)
{
    return {
        .packet = packet,
        .ingressQueue = ingressQueue,
        .inPortId = inPortId,
        .outPortId = outPortId,
        .priority = priority,
    };
}

} // namespace

TypeId UbSwitchAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbSwitchAllocator")
        .SetParent<Object>()
        .AddConstructor<UbSwitchAllocator>()
        .AddAttribute("AllocationTime",
                      "Latency of the switch allocation pipeline per scheduling round.",
                      TimeValue(NanoSeconds(10)),
                      MakeTimeAccessor(&UbSwitchAllocator::m_allocationTime),
                      MakeTimeChecker());
    return tid;
}
UbSwitchAllocator::UbSwitchAllocator()
{
}

UbSwitchAllocator::~UbSwitchAllocator()
{
}

void UbSwitchAllocator::DoDispose()
{
    m_ingressSources.clear();
    m_isRunning.clear();
    m_oneMoreRound.clear();
}

void UbSwitchAllocator::TriggerAllocator(Ptr<UbPort> outPort)
{
    std::string typeName = GetInstanceTypeId().GetName();
    NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] portId: " << outPort->GetIfIndex());

    auto outPortId = outPort->GetIfIndex();

    if (outPortId >= m_isRunning.size()) {
         NS_LOG_WARN("Port ID out of range in Allocator");
         return;
    }

    if (m_isRunning[outPortId]) {
        // one more round flag
        // 为了避免 running 过程中新生成的包：
        // 1. 无法被当前轮次调度
        // 2. 下一次 trigger 会被当前轮次的状态掩盖
        m_oneMoreRound[outPortId] = true;
        NS_LOG_DEBUG("[" << typeName << " TriggerAllocator] Allocator is running, will retrigger.");
        return;
    }
    m_isRunning[outPortId] = true;
    Simulator::Schedule(m_allocationTime, &UbSwitchAllocator::AllocateNextPacket, this, outPort);
}

void UbSwitchAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
}

void UbSwitchAllocator::Init()
{
    Simulator::Schedule(MilliSeconds(10), &UbSwitchAllocator::CheckDeadlock, this);
}

void UbSwitchAllocator::RegisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority)
{
    auto& queues = m_ingressSources[outPort][priority];
    if (ingressQueue->GetIngressQueueType() != IngressQueueType::VOQ) {
        queues.push_back(ingressQueue);
        return;
    }

    const uint32_t inPort = ingressQueue->GetInPortId();
    auto insertAt = queues.begin();
    for (; insertAt != queues.end(); ++insertAt) {
        Ptr<UbIngressQueue> existing = *insertAt;
        if (existing->GetIngressQueueType() != IngressQueueType::VOQ ||
            existing->GetInPortId() > inPort) {
            break;
        }
    }
    queues.insert(insertAt, ingressQueue);
}

uint32_t
UbSwitchAllocator::GetIngressQueueSlotCount(uint32_t outPort, uint32_t priority) const
{
    const auto& queues = m_ingressSources[outPort][priority];
    uint32_t nonVoqCount = 0;
    for (const auto& queue : queues) {
        if (queue->GetIngressQueueType() != IngressQueueType::VOQ) {
            ++nonVoqCount;
        }
    }
    return m_voqIngressSlotCount + nonVoqCount;
}

std::optional<UbSwitchAllocator::IngressCandidate>
UbSwitchAllocator::FindNextEligibleIngressQueue(uint32_t outPort,
                                                uint32_t priority,
                                                uint32_t startIngressQueueSlot,
                                                Ptr<UbPort> egressPort) const
{
    const auto& queues = m_ingressSources[outPort][priority];
    const uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPort, priority);
    if (ingressQueueSlotCount == 0 || queues.empty()) {
        return std::nullopt;
    }

    std::optional<IngressCandidate> bestAtOrAfterStart;
    std::optional<IngressCandidate> bestWrapped;
    uint32_t nonVoqQueueOrdinal = 0;

    for (const auto& queue : queues) {
        uint32_t ingressQueueSlot;
        if (queue->GetIngressQueueType() == IngressQueueType::VOQ) {
            ingressQueueSlot = queue->GetInPortId();
        } else {
            ingressQueueSlot = m_voqIngressSlotCount + nonVoqQueueOrdinal;
            ++nonVoqQueueOrdinal;
        }

        if (ingressQueueSlot >= ingressQueueSlotCount || queue->IsEmpty() || queue->IsLimited() ||
            egressPort->GetFlowControl()->IsFcLimited(queue)) {
            continue;
        }

        IngressCandidate candidate{queue, ingressQueueSlot};
        if (ingressQueueSlot >= startIngressQueueSlot) {
            if (!bestAtOrAfterStart.has_value() ||
                ingressQueueSlot < bestAtOrAfterStart->ingressQueueSlot) {
                bestAtOrAfterStart = candidate;
            }
        } else if (!bestWrapped.has_value() ||
                   ingressQueueSlot < bestWrapped->ingressQueueSlot) {
            bestWrapped = candidate;
        }
    }

    if (bestAtOrAfterStart.has_value()) {
        return bestAtOrAfterStart;
    }
    return bestWrapped;
}

void UbSwitchAllocator::CheckDeadlock()
{
    Time now = Simulator::Now();
    Time threshold = MilliSeconds(10);

    for (uint32_t outPort = 0; outPort < m_ingressSources.size(); ++outPort) {
        for (uint32_t pri = 0; pri < m_ingressSources[outPort].size(); ++pri) {
            for (const auto& queue : m_ingressSources[outPort][pri]) {
                if (queue && !queue->IsEmpty()) {
                    if (now - queue->GetHeadArrivalTime() > threshold) {
                        std::stringstream ss;
                        ss << "Potential Deadlock in Node " << m_nodeId
                           << " OutPort:" << outPort << " Pri:" << pri;

                        if (queue->GetIngressQueueType() == IngressQueueType::VOQ) {
                            ss << " QueueType:VOQ InPort:" << queue->GetInPortId();
                        } else if (queue->GetIngressQueueType() == IngressQueueType::TP &&
                                   !queue->IsGeneratedDataPacket()) {
                            auto tp = DynamicCast<UbTransportChannel>(queue);
                            if (tp) {
                                ss << " QueueType:TP TPN:" << tp->GetTpn();
                            } else {
                                ss << " QueueType:TP (Cast Failed)";
                            }
                        } else {
                            ss << " QueueType:Unknown(" << (int)queue->GetIngressQueueType() << ")";
                        }

                        ss << " Head packet stuck for " << (now - queue->GetHeadArrivalTime()).GetMilliSeconds() << " ms";
                        NS_LOG_WARN(ss.str());
                    }
                }
            }
        }
    }
    Simulator::Schedule(MilliSeconds(10), &UbSwitchAllocator::CheckDeadlock, this);
}

void UbSwitchAllocator::UnregisterUbIngressQueue(Ptr<UbIngressQueue> ingressQueue, uint32_t outPort, uint32_t priority)
{
    m_ingressSources[outPort][priority].erase(
        std::remove(m_ingressSources[outPort][priority].begin(), m_ingressSources[outPort][priority].end(), ingressQueue),
        m_ingressSources[outPort][priority].end());
}

void UbSwitchAllocator::RegisterEgressStauts(uint32_t portsNum)
{
    m_egressStatus.resize(portsNum, true);
}

void UbSwitchAllocator::SetEgressStatus(uint32_t portId, bool status)
{
    m_egressStatus[portId] = status;
}

bool UbSwitchAllocator::GetEgressStatus(uint32_t portId)
{
    return m_egressStatus[portId];
}

/*-----------------------------------------UbRoundRobinAllocator----------------------------------------------*/
TypeId UbRoundRobinAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRoundRobinAllocator")
        .SetParent<UbSwitchAllocator>()
        .AddConstructor<UbRoundRobinAllocator>();
    return tid;
}

void UbRoundRobinAllocator::Init()
{
    UbSwitchAllocator::Init();
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();
    m_rrIdx.resize(portsNum);
    m_rrPhaseSeeded.resize(portsNum);
    for (auto &v: m_rrIdx) {
        v.resize(vlNum, 0);
    }
    for (auto &v: m_rrPhaseSeeded) {
        v.resize(vlNum, false);
    }
    m_ingressSources.resize(portsNum);
    m_isRunning.assign(portsNum, false);
    m_oneMoreRound.assign(portsNum, false);
    m_voqIngressSlotCount = portsNum;
    for (auto &i : m_ingressSources) {
        i.resize(vlNum);
    }
}

void UbRoundRobinAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
    // 轮询调度
    NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] portId: " << outPort->GetIfIndex());
    auto outPortId = outPort->GetIfIndex();
    auto ingressQueue = SelectNextIngressQueue(outPort);
    // 调度得到的ingressqueue加入egressqueue
    if (ingressQueue != nullptr) {
        const uint32_t nextPacketBytes = ingressQueue->GetNextPacketSize();
        if (!outPort->GetUbQueue()->CanEnqueue(nextPacketBytes)) {
            NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] egress queue full, keep packet in ingress queue"
                         << " outPortId=" << outPortId
                         << " bytes=" << nextPacketBytes);
            m_isRunning[outPortId] = false;
            return;
        }

        auto packet = ingressQueue->GetNextPacket();
        auto inPortId = ingressQueue->GetInPortId();
        auto priority = ingressQueue->GetIngressPriority();
        auto packetEntry = std::make_tuple(inPortId, priority, packet);
        auto context = MakeAllocatorFlowControlEventContext(packet,
                                                            ingressQueue,
                                                            inPortId,
                                                            outPortId,
                                                            priority);
        const bool enqueueOk = outPort->EnqueueToEgress(packetEntry);
        NS_ASSERT_MSG(enqueueOk,
                      "allocator pre-check promised egress queue capacity, but DoEnqueue still failed");
        outPort->GetFlowControl()->OnEgressEnqueued(context);

        // Packet moved from VOQ to EgressQueue, notify Switch to update buffer statistics
        if (ingressQueue->GetIngressQueueType() != IngressQueueType::TP &&
            !ingressQueue->IsGeneratedDataPacket()) {
            // Forwarded packet (not locally generated)
            auto node = NodeList::GetNode(m_nodeId);
            auto inPort = DynamicCast<UbPort>(node->GetDevice(inPortId));
            node->GetObject<UbSwitch>()->NotifySwitchDequeue(inPortId, outPortId, priority, packet);
            inPort->GetFlowControl()->OnIngressReleased(context);
        }
    }
    m_isRunning[outPortId] = false;
    // 通知port发包
    Simulator::ScheduleNow(&UbPort::NotifyAllocationFinish, outPort);
    if (m_oneMoreRound[outPortId] == true) {
        m_oneMoreRound[outPortId] = false;
        Simulator::ScheduleNow(&UbRoundRobinAllocator::TriggerAllocator, this, outPort);
        NS_LOG_DEBUG("[UbRoundRobinAllocator AllocateNextPacket] ReTriggerAllocator portId: " << outPort->GetIfIndex());
        return;
    }
}

Ptr<UbIngressQueue> UbRoundRobinAllocator::SelectNextIngressQueue(Ptr<UbPort> outPort)
{
    uint32_t pi;
    uint32_t outPortId = outPort->GetIfIndex();
    auto node = NodeList::GetNode(m_nodeId);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();
    for (pi = 0 ; pi < vlNum; pi++) {
        const uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);
        if (ingressQueueSlotCount == 0) {
            continue;
        }
        if (!m_rrPhaseSeeded[outPortId][pi]) {
            m_rrIdx[outPortId][pi] =
                ComputeInitialIngressPhase(m_nodeId, outPortId, ingressQueueSlotCount);
            m_rrPhaseSeeded[outPortId][pi] = true;
        } else if (m_rrIdx[outPortId][pi] >= ingressQueueSlotCount) {
            m_rrIdx[outPortId][pi] %= ingressQueueSlotCount;
        }
        auto candidate =
            FindNextEligibleIngressQueue(outPortId, pi, m_rrIdx[outPortId][pi], outPort);
        if (candidate.has_value()) {
            m_rrIdx[outPortId][pi] = (candidate->ingressQueueSlot + 1) % ingressQueueSlotCount;
            NS_LOG_DEBUG("[UbSwitchAllocator DispatchPacket] " << " NodeId: " << node->GetId()
                                                              << " PortId: " << outPortId
                                                              << " ingressQueueSlot: "
                                                              << candidate->ingressQueueSlot);
            return candidate->queue;
        }
    }
    return nullptr;
}


/*-----------------------------------------UbDwrrAllocator----------------------------------------------*/

TypeId UbDwrrAllocator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDwrrAllocator")
        .SetParent<UbSwitchAllocator>()
        .AddConstructor<UbDwrrAllocator>()
        .AddAttribute("DefaultQuantum",
                      "Default DWRR quantum in bytes for all VLs.",
                      UintegerValue(1500),
                      MakeUintegerAccessor(&UbDwrrAllocator::m_defaultQuantum),
                      MakeUintegerChecker<uint32_t>(500, 1<<30))
        .AddAttribute("VlQuantums",
                      "Per-VL quantum overrides as 'vl:bytes,vl:bytes', e.g. '7:6000,8:12000'.",
                      StringValue(""),
                      MakeStringAccessor(&UbDwrrAllocator::m_vlQuantumsStr),
                      MakeStringChecker())
        ;
    return tid;
}

void UbDwrrAllocator::Init()
{
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    m_rrIdx.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_rrPhaseSeeded.assign(portsNum, std::vector<bool>(vlNum, false));
    m_quantum.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_deficit.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_lastSelectedIngressQueueSlot.assign(portsNum, std::vector<uint32_t>(vlNum, 0));
    m_currVlIdx.assign(portsNum, 0);

    m_isRunning.assign(portsNum, false);
    m_oneMoreRound.assign(portsNum, false);
    m_voqIngressSlotCount = portsNum;

    m_ingressSources.resize(portsNum);
    for (auto &i : m_ingressSources) {
        i.resize(vlNum);
    }

    ApplyDefaultQuantum();
    ParseAndApplyVlQuantums(m_vlQuantumsStr);
}

void UbDwrrAllocator::SetQuantum(uint32_t priority, uint32_t quantum)
{
    for (uint32_t port = 0; port < m_quantum.size(); ++port) {
        if (priority < m_quantum[port].size()) {
            m_quantum[port][priority] = quantum;
        }
    }
}

void UbDwrrAllocator::SetQuantum(uint32_t outPort, uint32_t priority, uint32_t quantum)
{
    if (outPort >= m_quantum.size()) {
        return;
    }
    if (priority >= m_quantum[outPort].size()) {
        return;
    }
    m_quantum[outPort][priority] = quantum;
}

Ptr<UbIngressQueue> UbDwrrAllocator::SelectNextIngressQueue(Ptr<UbPort> outPort)
{
    uint32_t outPortId = outPort->GetIfIndex();
    auto node = NodeList::GetNode(m_nodeId);
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    if (vlNum == 0) {
        return nullptr;
    }

    uint32_t startVl = m_currVlIdx[outPortId] % vlNum;

    for (uint32_t cnt = 0; cnt < vlNum; ++cnt) {
        uint32_t pi = (startVl + cnt) % vlNum;
        const uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);

        if (ingressQueueSlotCount == 0) {
            m_deficit[outPortId][pi] = 0;
            continue;
        }

        if (!m_rrPhaseSeeded[outPortId][pi]) {
            m_rrIdx[outPortId][pi] =
                ComputeInitialIngressPhase(m_nodeId, outPortId, ingressQueueSlotCount);
            m_rrPhaseSeeded[outPortId][pi] = true;
        } else if (m_rrIdx[outPortId][pi] >= ingressQueueSlotCount) {
            m_rrIdx[outPortId][pi] %= ingressQueueSlotCount;
        }

        auto candidate =
            FindNextEligibleIngressQueue(outPortId, pi, m_rrIdx[outPortId][pi], outPort);
        if (!candidate.has_value()) {
            m_deficit[outPortId][pi] = 0;
            continue;
        }

        m_deficit[outPortId][pi] += m_quantum[outPortId][pi];
        m_lastSelectedIngressQueueSlot[outPortId][pi] = candidate->ingressQueueSlot;
        m_rrIdx[outPortId][pi] = (candidate->ingressQueueSlot + 1) % ingressQueueSlotCount;
        m_currVlIdx[outPortId] = (pi + 1) % vlNum;

        NS_LOG_DEBUG("[UbDwrrAllocator SelectNextIngressQueue]"
                     << " NodeId: " << node->GetId()
                     << " OutPortId: " << outPortId
                     << " VL: " << pi
                     << " ingressQueueSlot: " << candidate->ingressQueueSlot
                     << " deficit: " << m_deficit[outPortId][pi]);
        return candidate->queue;
    }

    return nullptr;
}

void UbDwrrAllocator::AllocateNextPacket(Ptr<UbPort> outPort)
{
    NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] portId: " << outPort->GetIfIndex());
    auto outPortId = outPort->GetIfIndex();

    // 标记这轮调度是否实际发出了至少一个包
    bool sentAny = false;

    auto ingressQueue = SelectNextIngressQueue(outPort);
    if (ingressQueue != nullptr) {
        auto priority = ingressQueue->GetIngressPriority();
        uint32_t pi = priority;
        auto &queues = m_ingressSources[outPortId][pi];
        uint32_t ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);
        if (ingressQueueSlotCount > 0) {
            uint32_t ingressQueueSlot =
                m_lastSelectedIngressQueueSlot[outPortId][pi] % ingressQueueSlotCount;
            uint32_t &deficit = m_deficit[outPortId][pi];

            bool first = true;

            while (deficit > 0 && ingressQueueSlotCount > 0) {
                auto candidate =
                    FindNextEligibleIngressQueue(outPortId, pi, ingressQueueSlot, outPort);
                if (!candidate.has_value()) {
                    deficit = 0;
                    break;
                }
                auto q = candidate->queue;
                ingressQueueSlot = candidate->ingressQueueSlot;

                uint32_t pktSize = q->GetNextPacketSize();
                if (!outPort->GetUbQueue()->CanEnqueue(pktSize)) {
                    NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] egress queue full, keep packet in ingress queue"
                                 << " outPortId=" << outPortId
                                 << " bytes=" << pktSize);
                    break;
                }

                // 第一个包就发不出去：不扣赤字，并回滚 m_rrIdx
                if (pktSize > deficit) {
                    if (!sentAny) {
                        m_rrIdx[outPortId][pi] = ingressQueueSlot;
                    }
                    break;
                }

                auto packet = q->GetNextPacket();
                auto inPortId = q->GetInPortId();
                auto packetEntry = std::make_tuple(inPortId, priority, packet);
                auto context = MakeAllocatorFlowControlEventContext(packet,
                                                                    q,
                                                                    inPortId,
                                                                    outPortId,
                                                                    priority);
                const bool enqueueOk = outPort->EnqueueToEgress(packetEntry);
                NS_ASSERT_MSG(enqueueOk,
                              "allocator pre-check promised egress queue capacity, but DoEnqueue still failed");
                outPort->GetFlowControl()->OnEgressEnqueued(context);

                // Packet moved from VOQ to EgressQueue, notify Switch to update buffer statistics
                if (q->GetIngressQueueType() != IngressQueueType::TP &&
                    !q->IsGeneratedDataPacket()) {
                    // Forwarded packet (not locally generated)
                    auto node = NodeList::GetNode(m_nodeId);
                    auto inPort = DynamicCast<UbPort>(node->GetDevice(inPortId));
                    node->GetObject<UbSwitch>()->NotifySwitchDequeue(inPortId, outPortId, priority, packet);
                    inPort->GetFlowControl()->OnIngressReleased(context);
                }

                sentAny = true;
                deficit -= pktSize;

                if (first) {
                    ingressQueueSlot = m_rrIdx[outPortId][pi];
                    first = false;
                } else {
                    ingressQueueSlotCount = GetIngressQueueSlotCount(outPortId, pi);
                    if (ingressQueueSlotCount == 0) {
                        break;
                    }
                    m_rrIdx[outPortId][pi] =
                        (ingressQueueSlot + 1) % ingressQueueSlotCount;
                    ingressQueueSlot = m_rrIdx[outPortId][pi];
                }
            }

            bool vlanEmpty = true;
            for (auto &qq : queues) {
                if (!qq->IsEmpty() &&
                    !outPort->GetFlowControl()->IsFcLimited(qq)) {
                    vlanEmpty = false;
                    break;
                }
            }
            if (vlanEmpty) {
                deficit = 0;
            }
        }
    }

    if (ingressQueue != nullptr && !sentAny) {
        Simulator::Schedule(m_allocationTime,
                            &UbDwrrAllocator::AllocateNextPacket,
                            this, outPort);
        return;
    }

    m_isRunning[outPortId] = false;
    // 通知 port 发包
    Simulator::ScheduleNow(&UbPort::NotifyAllocationFinish, outPort);
    if (m_oneMoreRound[outPortId]) {
        m_oneMoreRound[outPortId] = false;
        Simulator::ScheduleNow(&UbDwrrAllocator::TriggerAllocator, this, outPort);
        NS_LOG_DEBUG("[UbDwrrAllocator AllocateNextPacket] ReTriggerAllocator portId: "
                     << outPort->GetIfIndex());
    }
}

void UbDwrrAllocator::ApplyDefaultQuantum()
{
    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    uint32_t q = std::max<uint32_t>(m_defaultQuantum, 64);
    for (uint32_t p = 0; p < portsNum; ++p) {
        for (uint32_t vl = 0; vl < vlNum; ++vl) {
            m_quantum[p][vl] = q;
        }
    }
}

void UbDwrrAllocator::ParseAndApplyVlQuantums(const std::string& s)
{
    if (s.empty()) return;

    std::istringstream ss(s);
    std::string token;

    auto node = NodeList::GetNode(m_nodeId);
    uint32_t portsNum = node->GetNDevices();
    auto vlNum = node->GetObject<UbSwitch>()->GetVLNum();

    while (std::getline(ss, token, ',')) {
        if (token.empty()) continue;

        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);

        auto pos = token.find(':');
        if (pos == std::string::npos) continue;

        std::string vlStr = token.substr(0, pos);
        std::string qStr  = token.substr(pos + 1);

        vlStr.erase(0, vlStr.find_first_not_of(" \t"));
        vlStr.erase(vlStr.find_last_not_of(" \t") + 1);
        qStr.erase(0, qStr.find_first_not_of(" \t"));
        qStr.erase(qStr.find_last_not_of(" \t") + 1);

        char* endp = nullptr;
        long vl = std::strtol(vlStr.c_str(), &endp, 10);
        if (endp == vlStr.c_str() || vl < 0 || (uint32_t)vl >= vlNum) {
            NS_LOG_WARN("UbDwrrAllocator::VlQuantums invalid vl: " << vlStr);
            continue;
        }

        endp = nullptr;
        long q = std::strtol(qStr.c_str(), &endp, 10);
        if (endp == qStr.c_str() || q <= 0) {
            NS_LOG_WARN("UbDwrrAllocator::VlQuantums invalid quantum: " << qStr);
            continue;
        }

        for (uint32_t p = 0; p < portsNum; ++p) {
            m_quantum[p][(uint32_t)vl] = (uint32_t)q;
        }
    }
}

} // namespae ns3
