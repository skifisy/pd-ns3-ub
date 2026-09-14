// SPDX-License-Identifier: GPL-2.0-only
#include <iostream>
#include <algorithm>
#include <limits>
#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-port.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-utils.h"

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbSwitch);
NS_LOG_COMPONENT_DEFINE("UbSwitch");

namespace {

UbFlowControlEventContext
MakeIngressFlowControlEventContext(Ptr<Packet> packet,
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

uint16_t
Cna24ToRoutingPortId(uint32_t cna)
{
    const uint32_t encodedPort = cna & 0xFF;
    return encodedPort == 0 ? 0 : static_cast<uint16_t>(encodedPort - 1);
}

} // namespace


/*-----------------------------------------UbSwitchNode----------------------------------------------*/

TypeId UbSwitch::GetTypeId (void)
{
    static TypeId tid = TypeId ("ns3::UbSwitch")
        .SetParent<Object> ()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbSwitch> ()
        .AddAttribute("FlowControl",
                      "Flow control mechanism (NONE, CBFC, CBFC_SHARED, PFC_FIXED, PFC_DYNAMIC, "
                      "or PFC_DYNAMIC_PAPER). PFC_DYNAMIC_PAPER is the paper-style dynamic PFC "
                      "mode used for DCQCN reproduction from \"Congestion Control for Large-Scale "
                      "RDMA Deployments\" (SIGCOMM 2015). "
                      "CBFC and PFC modes are peer policies over the same ingress accounting model.",
                      EnumValue(FcType::CBFC),
                      MakeEnumAccessor<FcType>(&UbSwitch::m_flowControlType),
                      MakeEnumChecker(FcType::NONE, "NONE",
                                      FcType::CBFC, "CBFC",
                                      FcType::CBFC_SHARED, "CBFC_SHARED",
                                      FcType::PFC_FIXED, "PFC_FIXED",
                                      FcType::PFC_DYNAMIC, "PFC_DYNAMIC",
                                      FcType::PFC_DYNAMIC_PAPER, "PFC_DYNAMIC_PAPER"))
        .AddAttribute("VlScheduler",
                      "VL inter-scheduling algorithm (SP or DWRR).",
                      EnumValue(SP),
                      MakeEnumAccessor<VlScheduler>(&UbSwitch::m_vlScheduler),
                      MakeEnumChecker(SP, "SP",
                                      DWRR, "DWRR"))
        .AddAttribute("InPortProcessingDelay",
                      "Fixed non-blocking input-port processing latency after route/output are "
                      "known and before the packet becomes visible in VOQ to the allocator.",
                      TimeValue(NanoSeconds(0)),
                      MakeTimeAccessor(&UbSwitch::m_inPortProcessingDelay),
                      MakeTimeChecker())
        .AddTraceSource("LastPacketTraversesNotify",
                        "Last Packet Traverses, NodeId",
                        MakeTraceSourceAccessor(&UbSwitch::m_traceLastPacketTraversesNotify),
                        "ns3::UbSwitch::LastPacketTraversesNotify");
    return tid;
}

void UbSwitch::SetReservePerQueueBytes(uint32_t bytes)
{
    m_bufferOverrides.reservePerQueueBytes = bytes;
}

void UbSwitch::SetSharedPoolBytes(uint64_t bytes)
{
    m_bufferOverrides.sharedPoolBytes = bytes;
}

void UbSwitch::SetHeadroomPerPortBytes(uint32_t bytes)
{
    m_bufferOverrides.headroomPerPortBytes = bytes;
}

void UbSwitch::SetDynamicPfcResumeGapBytes(uint32_t bytes)
{
    m_bufferOverrides.dynamicPfcResumeGapBytes = bytes;
}

void UbSwitch::SetDynamicThresholdAlphaShift(uint32_t shift)
{
    m_bufferOverrides.dynamicThresholdAlphaShift = shift;
}

void UbSwitch::SetPaperDynamicPfcBeta(uint32_t beta)
{
    m_bufferOverrides.paperDynamicPfcBeta = beta;
}

void UbSwitch::SetPfcThresholds(int32_t xoffBytes, int32_t xonBytes)
{
    m_flowControlOverrides.pfcXoffBytes = xoffBytes;
    m_flowControlOverrides.pfcXonBytes = xonBytes;
}

void UbSwitch::SetCbfcCellGeometry(uint8_t flitLenBytes, uint8_t flitsPerCell)
{
    m_flowControlOverrides.cbfcFlitLenBytes = flitLenBytes;
    m_flowControlOverrides.cbfcFlitsPerCell = flitsPerCell;
}

void UbSwitch::SetCbfcReturnCellGrain(uint8_t dataPacketCells, uint8_t controlPacketCells)
{
    m_flowControlOverrides.cbfcReturnGrainDataCells = dataPacketCells;
    m_flowControlOverrides.cbfcReturnGrainControlCells = controlPacketCells;
}

void UbSwitch::SetCbfcCredits(int32_t initCreditCells, int32_t sharedInitCreditCells)
{
    m_flowControlOverrides.cbfcInitCreditCells = initCreditCells;
    m_flowControlOverrides.cbfcSharedInitCreditCells = sharedInitCreditCells;
}

void UbSwitch::ApplyLocalQueueManagerConfig()
{
    NS_ASSERT_MSG(m_queueManager != nullptr, "QueueManager must exist before applying local config");

    if (m_bufferOverrides.reservePerQueueBytes.has_value()) {
        m_queueManager->SetAttribute("ReservePerQueueBytes",
                                     UintegerValue(m_bufferOverrides.reservePerQueueBytes.value()));
    }
    if (m_bufferOverrides.sharedPoolBytes.has_value()) {
        m_queueManager->SetAttribute("SharedPoolBytes",
                                     UintegerValue(m_bufferOverrides.sharedPoolBytes.value()));
    }
    if (m_bufferOverrides.headroomPerPortBytes.has_value()) {
        m_queueManager->SetAttribute("HeadroomPerPortBytes",
                                     UintegerValue(m_bufferOverrides.headroomPerPortBytes.value()));
    }
    if (m_bufferOverrides.dynamicPfcResumeGapBytes.has_value()) {
        m_queueManager->SetAttribute("DynamicPfcResumeGapBytes",
                                     UintegerValue(m_bufferOverrides.dynamicPfcResumeGapBytes.value()));
    }
    if (m_bufferOverrides.dynamicThresholdAlphaShift.has_value()) {
        m_queueManager->SetAttribute("AlphaShift",
                                     UintegerValue(m_bufferOverrides.dynamicThresholdAlphaShift.value()));
    }
    if (m_bufferOverrides.paperDynamicPfcBeta.has_value()) {
        m_queueManager->SetAttribute("PaperDynamicPfcBeta",
                                     UintegerValue(m_bufferOverrides.paperDynamicPfcBeta.value()));
    }
}

void UbSwitch::ApplyLocalPortFlowControlConfig(Ptr<UbPort> port)
{
    NS_ASSERT_MSG(port != nullptr, "Port must exist before applying local flow-control config");

    if (m_flowControlOverrides.pfcXoffBytes.has_value()) {
        port->SetAttribute("PfcUpThld", IntegerValue(m_flowControlOverrides.pfcXoffBytes.value()));
    }
    if (m_flowControlOverrides.pfcXonBytes.has_value()) {
        port->SetAttribute("PfcLowThld", IntegerValue(m_flowControlOverrides.pfcXonBytes.value()));
    }
    if (m_flowControlOverrides.cbfcFlitLenBytes.has_value()) {
        port->SetAttribute("CbfcFlitLenByte", UintegerValue(m_flowControlOverrides.cbfcFlitLenBytes.value()));
    }
    if (m_flowControlOverrides.cbfcFlitsPerCell.has_value()) {
        port->SetAttribute("CbfcFlitsPerCell", UintegerValue(m_flowControlOverrides.cbfcFlitsPerCell.value()));
    }
    if (m_flowControlOverrides.cbfcReturnGrainDataCells.has_value()) {
        port->SetAttribute("CbfcRetCellGrainDataPacket",
                           UintegerValue(m_flowControlOverrides.cbfcReturnGrainDataCells.value()));
    }
    if (m_flowControlOverrides.cbfcReturnGrainControlCells.has_value()) {
        port->SetAttribute("CbfcRetCellGrainControlPacket",
                           UintegerValue(m_flowControlOverrides.cbfcReturnGrainControlCells.value()));
    }
    if (m_flowControlOverrides.cbfcInitCreditCells.has_value()) {
        port->SetAttribute("CbfcInitCreditCell", IntegerValue(m_flowControlOverrides.cbfcInitCreditCells.value()));
    }
    if (m_flowControlOverrides.cbfcSharedInitCreditCells.has_value()) {
        port->SetAttribute("CbfcSharedInitCreditCell",
                           IntegerValue(m_flowControlOverrides.cbfcSharedInitCreditCells.value()));
    }
}

void UbSwitch::InitAllocator(Ptr<Node> node)
{
    switch (m_vlScheduler) {
        case DWRR:
            m_allocator = CreateObject<UbDwrrAllocator>();
            break;
        case SP:
        default:
            m_allocator = CreateObject<UbRoundRobinAllocator>();
            break;
    }
    m_allocator->SetNodeId(node->GetId());
    m_allocator->Init();
    VoqInit();
}

void UbSwitch::InitQueueManager(Ptr<Node> node)
{
    m_queueManager = CreateObject<UbQueueManager>();
    m_queueManager->SetOwnerNode(node);
    m_queueManager->SetVLNum(m_vlNum);
    m_queueManager->SetPortsNum(m_portsNum);
    m_queueManager->SetPaperDynamicAdmissionEnabled(m_flowControlType == FcType::PFC_DYNAMIC_PAPER);
    ApplyLocalQueueManagerConfig();
    m_queueManager->Init();
}

void UbSwitch::InitRoutingProcess(Ptr<Node> node)
{
    m_routingProcess = CreateObject<UbRoutingProcess>();
    m_routingProcess->SetNodeId(node->GetId());
    m_Ipv4Addr = utils::NodeIdToIp(node->GetId());
}
/**
 * @brief Init UbNode, create algorithm, queueManager, fc and so on
 */
void UbSwitch::Init()
{
    auto node = GetObject<Node>();
    m_portsNum = node->GetNDevices();
    NS_LOG_DEBUG("UbSwitch Init: nodeId=" << node->GetId()
                 << " flowControlMode=" << static_cast<int>(m_flowControlType)
                 << " ports=" << m_portsNum
                 << " vlNum=" << m_vlNum);

    InitAllocator(node);
    InitQueueManager(node);
    InitNodePortsFlowControl();
    InitRoutingProcess(node);
}

void UbSwitch::DoDispose()
{
    for (auto& event : m_inPortProcessingEvents) {
        if (event.IsPending()) {
            event.Cancel();
        }
    }
    m_inPortProcessingEvents.clear();
    m_queueManager = nullptr;
    m_congestionCtrl = nullptr;
    m_allocator = nullptr;
    m_voq.groups.clear();
    m_routingProcess = nullptr;
}

/**
 * @brief Init flow control for each port
 */
void UbSwitch::InitNodePortsFlowControl()
{
    NS_LOG_DEBUG("[UbSwitch InitNodePortsFlowControl] m_portsNum: " << m_portsNum
                << " m_flowControlType: " << static_cast<int>(m_flowControlType));

    for (uint32_t pidx = 0; pidx < m_portsNum; pidx++) {
        Ptr<UbPort> port = DynamicCast<ns3::UbPort>(GetObject<Node>()->GetDevice(pidx));
        ApplyLocalPortFlowControlConfig(port);
        port->CreateAndInitFc(m_flowControlType);
    }
}

/**
 * @brief 将tp放入调度算法中
 */
void UbSwitch::RegisterTpWithAllocator(Ptr<UbIngressQueue> tp, uint32_t outPort, uint32_t priority)
{
    if ((outPort >= m_portsNum) || (priority >= m_vlNum)) {
        NS_ASSERT_MSG(0, "Invalid indices (outPort, priority)!");
    }
    NS_LOG_DEBUG("[UbSwitch RegisterTpWithAllocator] TP: outPortIdx: " << outPort
                 << "priorityIdx: " << priority << "outPort: " << outPort);
    tp->SetOutPortId(outPort);
    tp->SetInPortId(outPort); // tp uses outPort as inPort, since tp has no inPort
    tp->SetIngressPriority(priority);
    m_allocator->RegisterUbIngressQueue(tp, outPort, priority);
}

/**
 * @brief 将tp从调度算法中删除
 */
void UbSwitch::RemoveTpFromAllocator(Ptr<UbIngressQueue> tp)
{
    uint32_t outPort = tp->GetOutPortId();
    uint32_t priority = tp->GetIngressPriority();
    m_allocator->UnregisterUbIngressQueue(tp, outPort, priority);
}

UbSwitch::UbSwitch()
{
}
UbSwitch::~UbSwitch()
{
}

Ptr<UbSwitchAllocator> UbSwitch::GetAllocator()
{
    return m_allocator;
}

/**
 * @brief init voq
 */
void UbSwitch::VoqInit()
{
    const auto groupCount =
        static_cast<VirtualOutputQueueGroupKey>(m_portsNum) * m_vlNum;
    NS_ASSERT_MSG(groupCount <= std::numeric_limits<size_t>::max(), "VOQ group count too large");
    m_voq.groups.clear();
    m_voq.groups.resize(static_cast<size_t>(groupCount));
}

/**
 * @brief push packet into voq
 */
void UbSwitch::PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    GetOrCreateVoq(outPort, priority, inPort)->Push(p);
}

Ptr<UbPacketQueue> UbSwitch::GetOrCreateVoq(uint32_t outPort, uint32_t priority, uint32_t inPort)
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        NS_ASSERT_MSG(0, "Invalid VOQ indices (outPort, priority, inPort)!");
    }
    const auto groupKey = BuildVoqGroupKey(outPort, priority);
    Ptr<UbPacketQueue>* slot = GetOrCreateVoqSlot(groupKey, inPort);
    if (*slot != nullptr) {
        return *slot;
    }

    Ptr<UbPacketQueue> voq = CreateObject<UbPacketQueue>();
    voq->SetOutPortId(outPort);
    voq->SetIngressPriority(priority);
    voq->SetInPortId(inPort);
    m_allocator->RegisterUbIngressQueue(voq, outPort, priority);
    *slot = voq;
    return voq;
}

UbSwitch::VirtualOutputQueueGroupKey
UbSwitch::BuildVoqGroupKey(uint32_t outPort, uint32_t priority) const
{
    return static_cast<VirtualOutputQueueGroupKey>(outPort) * m_vlNum + priority;
}

const Ptr<UbPacketQueue>*
UbSwitch::FindVoqSlot(UbSwitch::VirtualOutputQueueGroupKey groupKey, uint32_t inPort) const
{
    if (groupKey >= m_voq.groups.size()) {
        return nullptr;
    }

    const auto& group = m_voq.groups[static_cast<size_t>(groupKey)];
    for (const auto& entry : group) {
        if (entry.inPort == inPort) {
            return &entry.queue;
        }
    }
    return nullptr;
}

Ptr<UbPacketQueue>*
UbSwitch::GetOrCreateVoqSlot(UbSwitch::VirtualOutputQueueGroupKey groupKey, uint32_t inPort)
{
    NS_ASSERT_MSG(groupKey < m_voq.groups.size(), "VOQ group key is out of range");

    auto& group = m_voq.groups[static_cast<size_t>(groupKey)];
    for (auto& entry : group) {
        if (entry.inPort == inPort) {
            return &entry.queue;
        }
    }

    group.push_back({inPort, nullptr});
    return &group.back().queue;
}

void
UbSwitch::ForwardDataPacketAfterAdmission(Ptr<Packet> packet,
                                          uint32_t inPort,
                                          uint32_t outPort,
                                          uint32_t priority)
{
    NS_ASSERT_MSG(packet != nullptr, "ForwardDataPacketAfterAdmission requires a packet");
    NS_ASSERT_MSG(m_queueManager != nullptr, "ForwardDataPacketAfterAdmission requires QueueManager");
    NS_ASSERT_MSG(IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum),
                  "ForwardDataPacketAfterAdmission invalid VOQ indices");

    m_queueManager->PushToInPortProcessing(inPort, outPort, priority, packet->GetSize());

    if (m_inPortProcessingDelay <= NanoSeconds(0)) {
        MoveInPortProcessingToVoq(packet, inPort, outPort, priority);
        FinalizeForwardedPacketEnqueue(packet, inPort, outPort, priority);
        return;
    }

    m_inPortProcessingEvents.push_back(Simulator::Schedule(m_inPortProcessingDelay,
                                                           &UbSwitch::CompleteInPortProcessing,
                                                           this,
                                                           packet,
                                                           inPort,
                                                           outPort,
                                                           priority));
}

void
UbSwitch::MoveInPortProcessingToVoq(Ptr<Packet> packet,
                                    uint32_t inPort,
                                    uint32_t outPort,
                                    uint32_t priority)
{
    if (packet == nullptr || m_queueManager == nullptr) {
        return;
    }

    NS_ASSERT_MSG(IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum),
                  "MoveInPortProcessingToVoq invalid VOQ indices");
    m_queueManager->MoveInPortProcessingToVoq(inPort, outPort, priority, packet->GetSize());
    GetOrCreateVoq(outPort, priority, inPort)->Push(packet);
}

void
UbSwitch::FinalizeForwardedPacketEnqueue(Ptr<Packet> packet,
                                         uint32_t inPort,
                                         uint32_t outPort,
                                         uint32_t priority)
{
    if (packet == nullptr) {
        return;
    }

    auto node = GetObject<Node>();
    NS_ASSERT_MSG(node != nullptr, "FinalizeForwardedPacketEnqueue requires an owning node");
    Ptr<UbPort> recvPort = DynamicCast<UbPort>(node->GetDevice(inPort));
    Ptr<UbPort> outDevice = DynamicCast<UbPort>(node->GetDevice(outPort));
    NS_ASSERT_MSG(recvPort != nullptr, "FinalizeForwardedPacketEnqueue requires a valid ingress port");
    NS_ASSERT_MSG(outDevice != nullptr, "FinalizeForwardedPacketEnqueue requires a valid out port");

    if (m_congestionCtrl != nullptr)
    {
        m_congestionCtrl->OnSwitchPostEnqueue(inPort, outPort, packet);
    }

    if (IsPFCEnable()) {
        recvPort->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               GetOrCreateVoq(outPort, priority, inPort),
                                               inPort,
                                               outPort,
                                               priority));
    }

    outDevice->TriggerTransmit();
}

void
UbSwitch::CompleteInPortProcessing(Ptr<Packet> packet,
                                   uint32_t inPort,
                                   uint32_t outPort,
                                   uint32_t priority)
{
    m_inPortProcessingEvents.erase(
        std::remove_if(m_inPortProcessingEvents.begin(),
                       m_inPortProcessingEvents.end(),
                       [](const EventId& event) { return !event.IsPending(); }),
        m_inPortProcessingEvents.end());

    MoveInPortProcessingToVoq(packet, inPort, outPort, priority);
    FinalizeForwardedPacketEnqueue(packet, inPort, outPort, priority);
}

bool UbSwitch::IsValidVoqIndices(uint32_t outPort, uint32_t priority, uint32_t inPort, uint32_t portsNum, uint32_t vlNum)
{
    return outPort < portsNum && priority < vlNum && inPort < portsNum;
}

uint64_t UbSwitch::GetAllocatedVoqCountForTest() const
{
    uint64_t count = 0;
    for (const auto& group : m_voq.groups) {
        for (const auto& entry : group) {
            if (entry.queue != nullptr) {
                ++count;
            }
        }
    }
    return count;
}

bool UbSwitch::HasVoqForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        return false;
    }
    const Ptr<UbPacketQueue>* slot = FindVoqSlot(BuildVoqGroupKey(outPort, priority), inPort);
    return slot != nullptr && *slot != nullptr;
}

UbPacketType_t UbSwitch::GetPacketType(Ptr<Packet> packet)
{
    UbDatalinkHeader dlHeader;
    packet->PeekHeader(dlHeader);
    if (dlHeader.IsControlCreditHeader())
        return UB_CONTROL_FRAME;
    if (dlHeader.IsPacketIpv4Header())
        return UB_URMA_DATA_PACKET;
    if (dlHeader.GetConfig() == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16)) {
        Ptr<Packet> copy = packet->Copy();
        UbDatalinkPacketHeader packetHeader;
        UbCna16NetworkHeader cna16Header;
        copy->RemoveHeader(packetHeader);
        copy->RemoveHeader(cna16Header);
        if (cna16Header.GetNlp() == UB_CNA_NLP_CTPH) {
            return UB_CTP_DATA_PACKET;
        }
        return UB_LDST_DATA_PACKET;
    }
    if (dlHeader.GetConfig() == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA24)) {
        Ptr<Packet> copy = packet->Copy();
        UbDatalinkPacketHeader packetHeader;
        UbCna24NetworkHeader cna24Header;
        copy->RemoveHeader(packetHeader);
        copy->RemoveHeader(cna24Header);
        if (cna24Header.GetNlp() == UB_CNA_NLP_CTPH) {
            return UB_CTP_DATA_PACKET;
        }
    }
    if (dlHeader.IsPacketUbMemHeader())
        return UB_LDST_DATA_PACKET;
    return UNKOWN_TYPE;
}

/**
 * @brief Receive packet from port. Node handle packet
 */
void UbSwitch::SwitchHandlePacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // 帧类型判断
    auto packetType = GetPacketType(packet);
    switch (packetType) {
        case UB_CONTROL_FRAME:
            port->GetFlowControl()->OnControlFrameReceived(packet);
            break;
        case UB_URMA_DATA_PACKET:
            if (IsCBFCEnable() || IsCBFCSharedEnable()) {
                port->GetFlowControl()->OnDataPacketReceived(packet);
            }
            HandleURMADataPacket(port, packet);
            break;
        case UB_LDST_DATA_PACKET:
            if (IsCBFCEnable() || IsCBFCSharedEnable()) {
                port->GetFlowControl()->OnDataPacketReceived(packet);
            }
            HandleLdstDataPacket(port, packet);
            break;
        case UB_CTP_DATA_PACKET:
            if (IsCBFCEnable() || IsCBFCSharedEnable()) {
                port->GetFlowControl()->OnDataPacketReceived(packet);
            }
            HandleCtpDataPacket(port, packet);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Packet Type!");
    }
}

/**
 * @brief Handle URMA type data packet
 */
void UbSwitch::HandleURMADataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // Parse headers once for efficient reuse
    ParsedURMAHeaders headers;
    ParseURMAPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkTpDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Handle Ldst type data packet
 */
void UbSwitch::HandleLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    // Parse headers once for efficient reuse
    ParsedLdstHeaders headers;
    ParseLdstPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkLdstDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Handle CTP type data packet
 */
void UbSwitch::HandleCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet)
{
    ParsedCtpHeaders headers;
    ParseCtpPacketHeader(packet, headers);

    switch (GetNodeType()) {
        case UB_DEVICE:
            if (!SinkCtpDataPacket(port, packet, headers)) {
                ForwardDataPacket(port, packet, headers);
            }
            break;
        case UB_SWITCH:
            ForwardDataPacket(port, packet, headers);
            break;
        default:
            NS_ASSERT_MSG(0, "Invalid Node! ");
    }
}

/**
 * @brief Sink URMA type data packet
 */
bool UbSwitch::SinkTpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
    NS_LOG_DEBUG("[UbPort recv] Psn: " << headers.transportHeader.GetPsn());
    Ipv4Mask mask("255.255.255.0");

    // Forward
    if (!utils::IsInSameSubnet(headers.ipv4Header.GetDestination(), GetNodeIpv4Addr(), mask)) {
        return false;
    }
    // Sink
    NS_LOG_DEBUG("[UbPort recv] Pkt tb is local");
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               nullptr,
                                               port->GetIfIndex(),
                                               port->GetIfIndex(),
                                               headers.datalinkPacketHeader.GetPacketVL()));
    }

    const uint32_t srcTpn = headers.transportHeader.GetSrcTpn();
    uint32_t dstTpn = headers.transportHeader.GetDestTpn();
    Ptr<UbController> controller = GetObject<UbController>();
    auto targetTp = controller->ResolveInboundTp(srcTpn,
                                                 dstTpn,
                                                 headers.ipv4Header.GetSource(),
                                                 headers.ipv4Header.GetDestination(),
                                                 headers.datalinkPacketHeader.GetPacketVL());
    if (targetTp == nullptr) {
        NS_ABORT_MSG("Received unreserved or mismatched TP channel key. node="
                     << GetObject<Node>()->GetId() << " srcTpn=" << srcTpn << " dstTpn=" << dstTpn
                     << " sourceIp=" << headers.ipv4Header.GetSource()
                     << " destinationIp=" << headers.ipv4Header.GetDestination() << " priority="
                     << static_cast<uint32_t>(headers.datalinkPacketHeader.GetPacketVL())
                     << " packetUid=" << packet->GetUid());
    }
    const uint8_t tpOpcode = headers.transportHeader.GetTPOpcode();
    if (UbTransportChannel::IsTransportResponseOpcode(tpOpcode)) {
        NS_LOG_DEBUG("[UbPort recv] is ACK");
        UbDatalinkPacketHeader tempDlHeader;
        UbIpBasedNetworkHeader tempNetHeader;
        Ipv4Header tempIpv4Header;
        UdpHeader tempUdpHeader;
        packet->RemoveHeader(tempDlHeader);
        packet->RemoveHeader(tempNetHeader);
        packet->RemoveHeader(tempIpv4Header);
        packet->RemoveHeader(tempUdpHeader);
        targetTp->RecvTpAck(packet);
    } else if (tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_UNRELIABLE_TA) ||
               tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA)) {
        targetTp->RecvDataPacket(packet);
    } else {
        NS_LOG_WARN("Dropping reserved transport opcode " << static_cast<uint32_t>(tpOpcode));
        utils::UbUtils::RecordRuntimePacketDrop("reserved transport opcode");
    }
    return true;
}

/**
 * @brief Sink Ldst type data packet
 */
bool UbSwitch::SinkLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers)
{
    // Store/load request: DLH cNTH cTAH(0x03/0x06) [cMAETAH] Payload
    // Store/load response: DLH cNTH cATAH(0x11/0x12) Payload
    NS_LOG_DEBUG("[UbPort recv] ub ldst frame");
    uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
    uint32_t dnode = utils::Cna16ToNodeId(dCna);
    // Forward
    if (dnode != GetObject<Node>()->GetId()) {
        return false;
    }
    // Sink Packet
    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               nullptr,
                                               port->GetIfIndex(),
                                               port->GetIfIndex(),
                                               headers.datalinkPacketHeader.GetPacketVL()));
    }

    auto ldstApi = GetObject<Node>()->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
    NS_ASSERT_MSG(ldstApi != nullptr, "UbLdstApi can not be nullptr!");

    uint8_t type = headers.dummyTransactionHeader.GetTaOpcode();
    // 数据包
    if (type == (uint8_t)TaOpcode::TA_OPCODE_WRITE || type == (uint8_t)TaOpcode::TA_OPCODE_READ) {
        ldstApi->RecvDataPacket(packet);
    } else if (type == (uint8_t)TaOpcode::TA_OPCODE_TRANSACTION_ACK ||
               type == (uint8_t)TaOpcode::TA_OPCODE_READ_RESPONSE) {
        ldstApi->RecvResponse(packet);
        NS_LOG_DEBUG("ldst packet is ack!");
    } else {
        NS_ASSERT_MSG(0, "packet Ta Op code is wrong!");
    }
    return true;
}

/**
 * @brief Sink CTP type data packet. Task 6 wires real receive semantics.
 */
bool UbSwitch::SinkCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers)
{
    uint32_t dnode = headers.isCna24
                         ? utils::Cna24ToNodeId(headers.cna24NetworkHeader.GetDcna())
                         : utils::Cna16ToNodeId(headers.cna16NetworkHeader.GetDcna());
    if (dnode != GetObject<Node>()->GetId()) {
        return false;
    }

    if (IsCBFCEnable() || IsCBFCSharedEnable()) {
        port->GetFlowControl()->OnIngressEnqueued(
            MakeIngressFlowControlEventContext(packet,
                                               nullptr,
                                               port->GetIfIndex(),
                                               port->GetIfIndex(),
                                               headers.datalinkPacketHeader.GetPacketVL()));
    }

    Ptr<UbController> controller = GetObject<Node>()->GetObject<UbController>();
    if (controller != nullptr) {
        Ptr<UbCtpTransportService> ctpService = controller->GetCtpTransportService();
        if (ctpService != nullptr) {
            ctpService->HandleReceivedPacket(packet->Copy(), port->GetIfIndex());
        }
    }

    NS_LOG_DEBUG("[UbPort recv] local CTP packet consumed");
    return true;
}

/**
 * @brief Forward URMA data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers)
{
    // Log packet traversal
    LastPacketTraversesNotify(GetObject<Node>()->GetId(), headers.transportHeader);

    // Get routing key from parsed headers
    RoutingKey rtKey;
    GetURMARoutingKey(headers, rtKey);

    // Route
    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    if (outPort < 0) {
        utils::UbUtils::RecordRuntimePacketDrop("route cannot be found");
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        return;
    }

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();
    NS_ABORT_MSG_IF(priority == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        utils::UbUtils::RecordRuntimePacketDrop("ingress buffer full");
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

/**
 * @brief Forward LDST data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers)
{
    // Get routing key from parsed headers
    RoutingKey rtKey;
    GetLdstRoutingKey(headers, rtKey);

    // Route
    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    if (outPort < 0) {
        utils::UbUtils::RecordRuntimePacketDrop("route cannot be found");
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        return;
    }

    // If packet routed via non-shortest path, force subsequent hops to use shortest path
    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    // Buffer management: check input port buffer space
    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();
    NS_ABORT_MSG_IF(priority == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        utils::UbUtils::RecordRuntimePacketDrop("ingress buffer full");
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

/**
 * @brief Forward CTP data packet (headers already parsed)
 */
void UbSwitch::ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers)
{
    RoutingKey rtKey;
    GetCtpRoutingKey(headers, rtKey);

    bool selectedShortestPath = false;
    int outPort = m_routingProcess->GetOutPort(rtKey, selectedShortestPath, port->GetIfIndex());
    if (outPort < 0) {
        utils::UbUtils::RecordRuntimePacketDrop("route cannot be found");
        NS_LOG_WARN("The route cannot be found. Packet Dropped!");
        return;
    }

    if (!selectedShortestPath) {
        ForceShortestPathRouting(packet, headers.datalinkPacketHeader);
    }

    uint32_t inPort = port->GetIfIndex();
    uint8_t priority = headers.datalinkPacketHeader.GetPacketVL();
    uint32_t pSize = packet->GetSize();
    NS_ABORT_MSG_IF(priority == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    if (!m_queueManager->CheckInPortSpace(inPort, priority, pSize)) {
        utils::UbUtils::RecordRuntimePacketDrop("ingress buffer full");
        NS_LOG_WARN("NodeId " << GetObject<Node>()->GetId() << " InPort " << inPort << " pri=" << (uint32_t)priority
                    << " buffer full. Packet Dropped!");
        return;
    }

    SendPacket(packet, inPort, outPort, priority);
}

void UbSwitch::ForceShortestPathRouting(Ptr<Packet> packet, const UbDatalinkPacketHeader &parsedHeader)
{
    UbDatalinkPacketHeader modifiedHeader = parsedHeader;
    modifiedHeader.SetRoutingPolicy(true);  // Force shortest path

    UbDatalinkPacketHeader tempHeader;
    packet->RemoveHeader(tempHeader);
    packet->AddHeader(modifiedHeader);
}

void UbSwitch::ParseURMAPacketHeader(Ptr<Packet> packet, ParsedURMAHeaders &headers)
{
    // Parse headers needed by switch (store all that must be removed anyway)
    // Order: DLH -> NH -> IPv4 -> UDP -> TP -> ...
    packet->RemoveHeader(headers.datalinkPacketHeader);
    packet->RemoveHeader(headers.networkHeader);
    packet->RemoveHeader(headers.ipv4Header);
    packet->RemoveHeader(headers.udpHeader);
    packet->PeekHeader(headers.transportHeader);
    packet->AddHeader(headers.udpHeader);
    packet->AddHeader(headers.ipv4Header);
    packet->AddHeader(headers.networkHeader);
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::ParseLdstPacketHeader(Ptr<Packet> packet, ParsedLdstHeaders &headers)
{
    // Parse only headers needed by switch for routing and forwarding
    // Order: DLH -> CNA16NH -> dummyTA -> ...
    // Note: dummyTA can be either UbCompactTransactionHeader or UbCompactAckTransactionHeader
    packet->RemoveHeader(headers.datalinkPacketHeader);
    packet->RemoveHeader(headers.cna16NetworkHeader);
    packet->PeekHeader(headers.dummyTransactionHeader);
    packet->AddHeader(headers.cna16NetworkHeader);
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::ParseCtpPacketHeader(Ptr<Packet> packet, ParsedCtpHeaders &headers)
{
    packet->RemoveHeader(headers.datalinkPacketHeader);
    headers.isCna24 = headers.datalinkPacketHeader.GetConfig() ==
                      static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA24);
    if (headers.isCna24) {
        packet->RemoveHeader(headers.cna24NetworkHeader);
    } else {
        packet->RemoveHeader(headers.cna16NetworkHeader);
    }
    packet->PeekHeader(headers.ctpHeader);
    if (headers.isCna24) {
        packet->AddHeader(headers.cna24NetworkHeader);
    } else {
        packet->AddHeader(headers.cna16NetworkHeader);
    }
    packet->AddHeader(headers.datalinkPacketHeader);
}

void UbSwitch::GetURMARoutingKey(const ParsedURMAHeaders &headers, RoutingKey &rtKey)
{
    rtKey.sip = headers.ipv4Header.GetSource().Get();
    rtKey.dip = headers.ipv4Header.GetDestination().Get();
    rtKey.sport = headers.udpHeader.GetSourcePort();
    rtKey.dport = headers.udpHeader.GetDestinationPort();
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

void UbSwitch::GetLdstRoutingKey(const ParsedLdstHeaders &headers, RoutingKey &rtKey)
{
    uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
    uint16_t sCna = headers.cna16NetworkHeader.GetScna();
    uint32_t snode = utils::Cna16ToNodeId(sCna);
    uint32_t dnode = utils::Cna16ToNodeId(dCna);
    uint16_t sport = utils::Cna16ToPortId(sCna);
    uint16_t dport = 0;
    uint16_t lb = headers.cna16NetworkHeader.GetLb();
    rtKey.sip = utils::NodeIdToIp(snode, sport).Get();
    rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    rtKey.sport = lb;
    rtKey.dport = dport;
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

void UbSwitch::GetCtpRoutingKey(const ParsedCtpHeaders &headers, RoutingKey &rtKey)
{
    uint32_t snode = 0;
    uint32_t dnode = 0;
    uint16_t sport = 0;
    uint16_t dport = 0;
    uint16_t lb = 0;
    if (headers.isCna24) {
        const uint32_t dCna = headers.cna24NetworkHeader.GetDcna();
        const uint32_t sCna = headers.cna24NetworkHeader.GetScna();
        snode = utils::Cna24ToNodeId(sCna);
        dnode = utils::Cna24ToNodeId(dCna);
        sport = Cna24ToRoutingPortId(sCna);
        dport = Cna24ToRoutingPortId(dCna);
        lb = headers.cna24NetworkHeader.GetLb();
    } else {
        const uint16_t dCna = headers.cna16NetworkHeader.GetDcna();
        const uint16_t sCna = headers.cna16NetworkHeader.GetScna();
        snode = utils::Cna16ToNodeId(sCna);
        dnode = utils::Cna16ToNodeId(dCna);
        sport = utils::Cna16ToPortId(sCna);
        dport = (dCna & 0xF) == 0 ? 0 : utils::Cna16ToPortId(dCna);
        lb = headers.cna16NetworkHeader.GetLb();
    }
    rtKey.sip = utils::NodeIdToIp(snode, sport).Get();
    rtKey.dip = utils::NodeIdToIp(dnode, dport).Get();
    rtKey.sport = lb;
    rtKey.dport = dport;
    rtKey.priority = headers.datalinkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = headers.datalinkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = headers.datalinkPacketHeader.GetLoadBalanceMode();
}

/**
 * @brief Packet enters VOQ from input port
 * Place packet into VOQ[outPort][priority][inPort] and update buffer statistics
 */
void UbSwitch::SendPacket(Ptr<Packet> packet, uint32_t inPort, uint32_t outPort, uint32_t priority)
{
    ForwardDataPacketAfterAdmission(packet, inPort, outPort, priority);
}

// Control frames use priority 0 and inPort==outPort in this simulator model.
// This is a performance-oriented modeling tradeoff, not a UB protocol requirement.
// The switch peeks once here, then the VOQ path relies on queue identity instead of re-parsing
// packet headers at every scheduling/accounting step.
void UbSwitch::SendControlFrame(Ptr<Packet> packet, uint32_t portId)
{
    NS_ABORT_MSG_IF(GetPacketType(packet) != UB_CONTROL_FRAME,
                    "SendControlFrame expects a real UB control-credit packet");
    uint32_t priority = 0;
    GetOrCreateVoq(portId, priority, portId)->Push(packet);
    m_queueManager->PushToVoq(portId, portId, priority, packet->GetSize());

    Ptr<Node> node = GetObject<Node>();
    Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(portId));
    port->TriggerTransmit();
}

void UbSwitch::NotifySwitchDequeue(uint16_t inPortId, uint32_t outPort, uint32_t priority, Ptr<Packet> packet)
{
    m_queueManager->PopFromVoq(inPortId, outPort, priority, packet->GetSize());

    // 只有数据报文触发拥塞控制
    UbPacketType_t packetType = GetPacketType(packet);
    if (packetType != UB_CONTROL_FRAME && m_congestionCtrl != nullptr) {
        NS_LOG_DEBUG("[QMU] Node:" << GetObject<Node>()->GetId()
              << " port:" << outPort
              << " VOQ outPort buffer:" << m_queueManager->GetTotalOutPortBufferUsed(outPort));
        m_congestionCtrl->OnSwitchPostDequeue(inPortId, outPort, packet);
    }
}

bool UbSwitch::IsCBFCEnable()
{
    return m_flowControlType == FcType::CBFC;
}

bool UbSwitch::IsCBFCSharedEnable()
{
    return m_flowControlType == FcType::CBFC_SHARED;
}

bool UbSwitch::IsPFCEnable()
{
    return m_flowControlType == FcType::PFC_FIXED || m_flowControlType == FcType::PFC_DYNAMIC ||
           m_flowControlType == FcType::PFC_DYNAMIC_PAPER;
}

Ptr<UbQueueManager> UbSwitch::GetQueueManager()
{
    return m_queueManager;
}

void UbSwitch::SetCongestionCtrl(Ptr<UbCongestionControl> congestionCtrl)
{
    m_congestionCtrl = congestionCtrl;
}

Ptr<UbCongestionControl> UbSwitch::GetCongestionCtrl()
{
    return m_congestionCtrl;
}

RoutingKey
UbSwitch::GetLdstRoutingKeyForTest(Ptr<Packet> packet)
{
    ParsedLdstHeaders headers;
    ParseLdstPacketHeader(packet, headers);

    RoutingKey rtKey;
    GetLdstRoutingKey(headers, rtKey);
    return rtKey;
}

RoutingKey
UbSwitch::GetCtpRoutingKeyForTest(Ptr<Packet> packet)
{
    ParsedCtpHeaders headers;
    ParseCtpPacketHeader(packet, headers);

    RoutingKey rtKey;
    GetCtpRoutingKey(headers, rtKey);
    return rtKey;
}

void UbSwitch::LastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader)
{
    m_traceLastPacketTraversesNotify(nodeId, ubTpHeader);
}

uint32_t
UbSwitch::GetVoqPacketCountForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const
{
    if (!IsValidVoqIndices(outPort, priority, inPort, m_portsNum, m_vlNum)) {
        return 0;
    }
    const Ptr<UbPacketQueue>* queue = FindVoqSlot(BuildVoqGroupKey(outPort, priority), inPort);
    if (queue == nullptr || *queue == nullptr) {
        return 0;
    }
    return (*queue)->IsEmpty() ? 0u : 1u;
}

}  // namespace ns3
