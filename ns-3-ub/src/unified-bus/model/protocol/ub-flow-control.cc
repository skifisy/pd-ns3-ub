// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-port.h"
#include "ns3/ub-link.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-utils.h"
#include "ns3/ub-datatype.h"
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbFlowControl);
NS_LOG_COMPONENT_DEFINE("UbFlowControl");

namespace {

bool
IsValidCbfcCellGrain(uint8_t grain)
{
    switch (grain)
    {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
    case 32:
    case 64:
    case 128:
        return true;
    default:
        return false;
    }
}

} // namespace

TypeId UbFlowControl::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFlowControl").SetParent<Object>().AddConstructor<UbFlowControl>();
    return tid;
}

TypeId UbPfcDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcDecisionHook").SetParent<Object>();
    return tid;
}

TypeId UbPfcFixedDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcFixedDecisionHook")
        .SetParent<UbPfcDecisionHook>()
        .AddConstructor<UbPfcFixedDecisionHook>();
    return tid;
}

void UbPfcFixedDecisionHook::Init(uint64_t xoffThresholdBytes, uint64_t xonThresholdBytes)
{
    m_xoffThresholdBytes = xoffThresholdBytes;
    m_xonThresholdBytes = xonThresholdBytes;
}

UbPfcDecision UbPfcFixedDecisionHook::Evaluate(Ptr<UbQueueManager> queueManager,
                                               uint32_t inPort,
                                               uint32_t priority) const
{
    auto queueOcc = queueManager->GetIngressQueueOccupancy(inPort, priority);
    auto switchOcc = queueManager->GetSwitchBufferOccupancy();
    return {
        .pause = queueOcc.total_bytes >= m_xoffThresholdBytes,
        .resume = queueOcc.total_bytes < m_xonThresholdBytes,
        .ingress_total_bytes = queueOcc.total_bytes,
        .ingress_shared_bytes = queueOcc.shared_bytes,
        .ingress_headroom_bytes = queueOcc.headroom_bytes,
        .switch_shared_pool_used_bytes = switchOcc.shared_pool_used_bytes,
        .switch_total_buffered_bytes = switchOcc.total_buffered_bytes,
        .xoff_threshold_bytes = m_xoffThresholdBytes,
        .xon_threshold_bytes = m_xonThresholdBytes,
    };
}

TypeId UbPfcDynamicDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcDynamicDecisionHook")
        .SetParent<UbPfcDecisionHook>()
        .AddConstructor<UbPfcDynamicDecisionHook>();
    return tid;
}

UbPfcDecision UbPfcDynamicDecisionHook::Evaluate(Ptr<UbQueueManager> queueManager,
                                                 uint32_t inPort,
                                                 uint32_t priority) const
{
    auto queueOcc = queueManager->GetIngressQueueOccupancy(inPort, priority);
    auto switchOcc = queueManager->GetSwitchBufferOccupancy();
    const uint64_t xoff = queueManager->GetDynamicPauseThresholdBytes();
    const uint64_t xon = queueManager->GetDynamicResumeThresholdBytes();
    const bool pause = queueOcc.headroom_bytes > 0 || (queueOcc.shared_bytes >= xoff && queueOcc.shared_bytes > 0);
    return {
        .pause = pause,
        .resume = !pause && queueOcc.shared_bytes <= xon,
        .ingress_total_bytes = queueOcc.total_bytes,
        .ingress_shared_bytes = queueOcc.shared_bytes,
        .ingress_headroom_bytes = queueOcc.headroom_bytes,
        .switch_shared_pool_used_bytes = switchOcc.shared_pool_used_bytes,
        .switch_total_buffered_bytes = switchOcc.total_buffered_bytes,
        .xoff_threshold_bytes = xoff,
        .xon_threshold_bytes = xon,
    };
}

TypeId UbPfcPaperDynamicDecisionHook::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfcPaperDynamicDecisionHook")
        .SetParent<UbPfcDecisionHook>()
        .AddConstructor<UbPfcPaperDynamicDecisionHook>();
    return tid;
}

UbPfcDecision UbPfcPaperDynamicDecisionHook::Evaluate(Ptr<UbQueueManager> queueManager,
                                                      uint32_t inPort,
                                                      uint32_t priority) const
{
    auto queueOcc = queueManager->GetIngressQueueOccupancy(inPort, priority);
    auto switchOcc = queueManager->GetSwitchBufferOccupancy();
    const uint64_t xoff = queueManager->GetPaperPauseThresholdBytes(switchOcc.total_buffered_bytes);
    const uint64_t xon = queueManager->GetPaperResumeThresholdBytes(switchOcc.total_buffered_bytes);
    const bool pause = queueOcc.headroom_bytes > 0 || (queueOcc.total_bytes >= xoff && queueOcc.total_bytes > 0);
    return {
        .pause = pause,
        .resume = !pause && queueOcc.total_bytes <= xon,
        .ingress_total_bytes = queueOcc.total_bytes,
        .ingress_shared_bytes = queueOcc.shared_bytes,
        .ingress_headroom_bytes = queueOcc.headroom_bytes,
        .switch_shared_pool_used_bytes = switchOcc.shared_pool_used_bytes,
        .switch_total_buffered_bytes = switchOcc.total_buffered_bytes,
        .xoff_threshold_bytes = xoff,
        .xon_threshold_bytes = xon,
    };
}

TypeId UbCbfc::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbCbfc")
            .SetParent<UbFlowControl>()
            .AddConstructor<UbCbfc>()
            .AddTraceSource("ControlCreditRestoreNotify",
                            "Observed restored control credits at the real flow-control receive path.",
                            MakeTraceSourceAccessor(&UbCbfc::m_traceControlCreditRestoreNotify),
                            "ns3::UbCbfc::ControlCreditRestoreNotify");
    return tid;
}

void UbCbfc::Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
                  uint8_t retCellGrainControlPacket, int32_t portTxfree, int32_t ctrlCrdRtrThldCells,
                  uint32_t nodeId, uint32_t portId)
{
    // 基础参数配置
    m_cbfcCfg.m_flitLen = flitLen;
    m_cbfcCfg.m_nFlitPerCell = nFlitPerCell;
    m_cbfcCfg.m_retCellGrainDataPacket = retCellGrainDataPacket;
    m_cbfcCfg.m_retCellGrainControlPacket = retCellGrainControlPacket;
    m_cbfcCfg.m_ctrlCrdRtrThldCells = ctrlCrdRtrThldCells;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    m_crdTxfree.resize(ubVlNum, portTxfree);
    m_crdToReturn.resize(ubVlNum, 0);
    m_creditBlockedLast.resize(ubVlNum, false);
    m_nodeId = nodeId;
    m_portId = portId;
    NS_LOG_DEBUG("NodeId: " << m_nodeId
                 << " PortId: " << m_portId
                 << " Init Cbfc"
                 << " flitLen=" << static_cast<uint32_t>(m_cbfcCfg.m_flitLen)
                 << " flitsPerCell=" << static_cast<uint32_t>(m_cbfcCfg.m_nFlitPerCell)
                 << " retCellGrainData=" << static_cast<uint32_t>(m_cbfcCfg.m_retCellGrainDataPacket)
                 << " retCellGrainCtrl=" << static_cast<uint32_t>(m_cbfcCfg.m_retCellGrainControlPacket)
                 << " ctrlCrdRtrThld=" << m_cbfcCfg.m_ctrlCrdRtrThldCells
                 << " initCreditPerVl=" << portTxfree);

    NS_ABORT_MSG_IF(m_cbfcCfg.m_flitLen == 0 || m_cbfcCfg.m_nFlitPerCell == 0,
                    "CBFC requires non-zero cell geometry (flitLen and nFlitPerCell)");
    NS_ABORT_MSG_IF(m_cbfcCfg.m_retCellGrainDataPacket == 0 ||
                        m_cbfcCfg.m_retCellGrainControlPacket == 0,
                    "CBFC requires non-zero return grain for both data and control packets");
    NS_ABORT_MSG_IF(!IsValidCbfcCellGrain(m_cbfcCfg.m_nFlitPerCell),
                    "CBFC flits-per-cell must be one of {1,2,4,8,16,32,64,128}");
    NS_ABORT_MSG_IF(!IsValidCbfcCellGrain(m_cbfcCfg.m_retCellGrainDataPacket),
                    "CBFC data-packet credit grain must be one of {1,2,4,8,16,32,64,128}");
    NS_ABORT_MSG_IF(!IsValidCbfcCellGrain(m_cbfcCfg.m_retCellGrainControlPacket),
                    "CBFC control-packet credit grain must be one of {1,2,4,8,16,32,64,128}");
    NS_ABORT_MSG_IF(m_cbfcCfg.m_ctrlCrdRtrThldCells <= 0,
                    "CBFC requires a positive control-frame force threshold");
    NS_ABORT_MSG_IF(portTxfree < 0,
                    "CBFC requires non-negative initial transmit credit per VL");

    if (portTxfree == 0)
    {
        NS_LOG_WARN("CBFC initial credit is zero on NodeId=" << m_nodeId
                    << " PortId=" << m_portId
                    << " initCreditPerVl=" << portTxfree);
    }
}

void UbCbfc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    Object::DoDispose();
}

bool UbCbfc::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    uint32_t nextPktSize = 0;
    const uint32_t priority = ingressQ->GetIngressPriority();
    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return false;
    }

    nextPktSize = ingressQ->GetNextPacketSize();
    NS_LOG_DEBUG("nextPktSize:" << nextPktSize);

    int32_t consumeCellNum = ceil((float)nextPktSize / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
    const bool blocked = m_crdTxfree[priority] < consumeCellNum;
    if (m_creditBlockedLast[priority] != blocked) {
        utils::UbUtils::CbfcStateNotify(m_nodeId,
                                        m_portId,
                                        blocked ? "BLOCK" : "RESUME",
                                        priority,
                                        m_crdTxfree[priority],
                                        nextPktSize);
        m_creditBlockedLast[priority] = blocked;
    }
    if (blocked) {
        NS_LOG_INFO("Flow Control Credit Limited,outPort:{" << ingressQ->GetOutPortId() << "} VL:{"
                                                            << priority << "}");
        NS_LOG_DEBUG("m_crdTxfree[ " << priority << " ]: " << m_crdTxfree[priority]
                                     << "is insufficient");
        return true;
    }
    NS_LOG_DEBUG("m_crdTxfree[ " << priority << " ]: " << m_crdTxfree[priority]
                                 << "is enough");

    return false;
}

void UbCbfc::OnIngressReleased(const UbFlowControlEventContext& context)
{
    if (context.inPortId != context.outPortId) { // 转发的报文
        AccumulateReturnedCredit(context.packet, m_portId);
        MaybeQueueControlReturn(m_portId);
    }
}

void UbCbfc::OnEgressEnqueued(const UbFlowControlEventContext& context)
{
    if (context.ingressQueue->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return;
    }
    CbfcConsumeCrd(context.packet); // 计算消耗的信用证
    TryAttachPiggybackCredit(context.packet);
}

void UbCbfc::OnControlFrameReceived(Ptr<Packet> p)
{
    CbfcRestoreCrd(p);
}

void UbCbfc::OnDataPacketReceived(Ptr<Packet> p)
{
    RestoreDataPacketCredit(p);
}

void
UbCbfc::ControlCreditRestoreNotify(uint32_t nodeId,
                                   uint32_t portId,
                                   const std::vector<uint8_t>& credits)
{
    m_traceControlCreditRestoreNotify(nodeId, portId, credits);
}

void UbCbfc::OnIngressEnqueued(const UbFlowControlEventContext& context)
{
    AccumulateReturnedCredit(context.packet, m_portId);
    MaybeQueueControlReturn(m_portId);
}

int32_t UbCbfc::GetCrdToReturn(uint8_t vlId)
{
    int32_t crdToReturnCell = m_crdToReturn[vlId];

    return crdToReturnCell;
}

void UbCbfc::SetCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort)
{
    NS_LOG_DEBUG("NodeId: " << targetPort->GetNode()->GetId() << " PortId: " << targetPort->GetIfIndex());
    int32_t &vlRxbuf = m_crdToReturn[vlId];
    NS_LOG_DEBUG("before set m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]
                                              << " consumeCell: " << consumeCell);

    vlRxbuf += consumeCell;
    NS_LOG_DEBUG("after set m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]);
}

void UbCbfc::UpdateCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort)
{
    NS_LOG_DEBUG("NodeId: " << targetPort->GetNode()->GetId() << " PortId: " << targetPort->GetIfIndex()
                 << " vlId: " << (uint32_t)vlId);

    int32_t &vlRxbuf = m_crdToReturn[vlId];
    NS_LOG_DEBUG("before set: "
                 << "m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]);
    if (vlRxbuf >= consumeCell) {
        vlRxbuf -= consumeCell;
        NS_LOG_DEBUG("after set: "
                     << "m_crdToReturn[ " << (uint32_t)vlId << " ]: " << m_crdToReturn[vlId]);
    }
}

bool UbCbfc::CbfcConsumeCrd(Ptr<Packet> p)
{
    uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId << " pktSize: " << pktSize);
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    int32_t consumeCellNum = ceil((float)(pktSize) / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
    NS_LOG_DEBUG("befor consume, m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
    if (m_crdTxfree[vlId] >= consumeCellNum) {
        m_crdTxfree[vlId] -= consumeCellNum;
        utils::UbUtils::CbfcCreditLevelNotify(m_nodeId,
                                              m_portId,
                                              "CONSUME",
                                              vlId,
                                              m_crdTxfree[vlId],
                                              -consumeCellNum);
        NS_LOG_DEBUG("left m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
        return true;
    }

    return false;
}

bool UbCbfc::CbfcRestoreCrd(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId);
    port->ResetCredits();
    UbDatalinkControlCreditHeader crdHeader = UbDataLink::ParseCreditHeader(p, port);

    uint32_t resumeCellGrainNum = 0;
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    std::vector<uint8_t> restoredCredits;
    restoredCredits.reserve(ubVlNum);
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("port m_credits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
        restoredCredits.push_back(port->m_credits[index]);
    }

    for (int index = 0; index < ubVlNum; index++) {
        if (port->m_credits[index] > 0) {
            resumeCellGrainNum = port->m_credits[index];
            NS_LOG_DEBUG("before resume m_crdTxfree[ " << (uint32_t)index << " ]: " << m_crdTxfree[index]);
            m_crdTxfree[index] += resumeCellGrainNum * m_cbfcCfg.m_retCellGrainControlPacket;  // 粒度数量 * 粒度大小
            utils::UbUtils::CbfcCreditLevelNotify(m_nodeId,
                                                  m_portId,
                                                  "RESTORE",
                                                  index,
                                                  m_crdTxfree[index],
                                                  static_cast<int32_t>(resumeCellGrainNum * m_cbfcCfg.m_retCellGrainControlPacket));
            NS_LOG_DEBUG("left m_crdTxfree[ " << (uint32_t)index << " ]: " << m_crdTxfree[index]);
            ret = true;
        }
    }

    if (ret)
    {
        ControlCreditRestoreNotify(m_nodeId, m_portId, restoredCredits);
        utils::UbUtils::CbfcCreditRestoreTraceNotify(m_nodeId, m_portId, restoredCredits);
    }

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return ret;
}

void UbCbfc::SendCrdAck(Ptr<Packet> cbfcPkt, uint32_t targetPortId)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    
    node->GetObject<UbSwitch>()->SendControlFrame(cbfcPkt, targetPortId);
    NS_LOG_DEBUG("send crd pkt");
}

void UbCbfc::AccumulateReturnedCredit(Ptr<Packet> p, uint32_t targetPortId)
{
    if (p != nullptr) {
        uint32_t pktSize = p->GetSize();
        Ptr<Node> node = NodeList::GetNode(m_nodeId);
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
        UbDatalinkPacketHeader pktHeader;
        p->PeekHeader(pktHeader);
        uint8_t vlId = pktHeader.GetPacketVL();
        NS_LOG_DEBUG("NodeId: " << node->GetId() << " PortId: " << port->GetIfIndex()
                     << " vlId: " << (uint32_t)vlId << " pktSize: " << pktSize);

        int32_t consumeCellNum = ceil((float)(pktSize) / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
        SetCrdToReturn(vlId, consumeCellNum, port);
    }
}

Ptr<Packet> UbCbfc::BuildControlReturnPacket(uint32_t targetPortId,
                                             int32_t minPendingCells,
                                             const std::string& reason)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
    if (port == nullptr) {
        return nullptr;
    }

    int32_t leftCrdToReturn = 0;
    int32_t crdSndGrains = 0;
    bool shouldReturnCredit = false;
    port->ResetCredits();
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    std::vector<int32_t> pendingSnapshot;
    pendingSnapshot.reserve(ubVlNum);

    for (int index = 0; index < ubVlNum; index++) {
        leftCrdToReturn = GetCrdToReturn(index);
        pendingSnapshot.push_back(leftCrdToReturn);
        if (leftCrdToReturn >= minPendingCells) {
            const int32_t pendingGrains = leftCrdToReturn / m_cbfcCfg.m_retCellGrainControlPacket;
            crdSndGrains = std::min(pendingGrains, static_cast<int32_t>(kMaxCtrlCreditGrainsPerFrame));
            NS_LOG_DEBUG("index: " << (uint32_t)index << " m_cbfcCfg->m_retCellGrainControlPacket: "
                         << (uint32_t)m_cbfcCfg.m_retCellGrainControlPacket
                         << " crdSndGrains: " << crdSndGrains);
            if (crdSndGrains > 0) {
                port->SetCredits(index, static_cast<uint8_t>(crdSndGrains));
                UpdateCrdToReturn(index, crdSndGrains * m_cbfcCfg.m_retCellGrainControlPacket, port);
                shouldReturnCredit = true;
            }
        }
    }

    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("SndCredits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
    }

    if (!shouldReturnCredit) {
        return nullptr;
    }
    std::vector<uint8_t> sendCredits;
    sendCredits.reserve(ubVlNum);
    for (int index = 0; index < ubVlNum; ++index) {
        sendCredits.push_back(port->m_credits[index]);
    }
    utils::UbUtils::CbfcControlSendNotify(
        m_nodeId,
        targetPortId,
        reason,
        m_cbfcCfg.m_ctrlCrdRtrThldCells,
        minPendingCells,
        pendingSnapshot,
        sendCredits);
    return UbDataLink::GenControlCreditPacket(port->m_credits);
}

Ptr<Packet>
UbCbfc::MaybeBuildControlReturnPacket(uint32_t targetPortId)
{
    if (ShouldForceControlReturn()) {
        return BuildControlReturnPacket(targetPortId,
                                        m_cbfcCfg.m_retCellGrainControlPacket,
                                        "force_threshold");
    }
    return nullptr;
}

uint8_t
UbCbfc::SelectPiggybackCreditVl() const
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    const uint8_t ubVlNum = static_cast<uint8_t>(val.Get());

    for (uint8_t offset = 1; offset <= ubVlNum; ++offset) {
        uint8_t candidate = static_cast<uint8_t>((m_lastPiggybackVl + offset) % ubVlNum);
        if (m_crdToReturn[candidate] >= m_cbfcCfg.m_retCellGrainDataPacket) {
            return candidate;
        }
    }
    return UB_PRIORITY_NUM_DEFAULT;
}

bool
UbCbfc::TryAttachPiggybackCredit(Ptr<Packet> p)
{
    if (p == nullptr) {
        return false;
    }

    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    if (header.GetCredit()) {
        p->AddHeader(header);
        return false;
    }

    const uint8_t targetVl = SelectPiggybackCreditVl();
    if (targetVl >= UB_PRIORITY_NUM_DEFAULT) {
        p->AddHeader(header);
        return false;
    }

    header.SetCredit(true);
    header.SetCreditTargetVL(targetVl);
    p->AddHeader(header);
    m_crdToReturn[targetVl] -= m_cbfcCfg.m_retCellGrainDataPacket;
    m_lastPiggybackVl = targetVl;
    return true;
}

bool
UbCbfc::RestoreDataPacketCredit(Ptr<Packet> p)
{
    if (p == nullptr) {
        return false;
    }

    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    if (!header.GetCredit()) {
        p->AddHeader(header);
        return false;
    }

    const uint8_t targetVl = header.GetCreditTargetVL();
    const int32_t restoreCells = m_cbfcCfg.m_retCellGrainDataPacket;
    m_crdTxfree[targetVl] += restoreCells;
    utils::UbUtils::CbfcCreditLevelNotify(m_nodeId,
                                          m_portId,
                                          "RESTORE",
                                          targetVl,
                                          m_crdTxfree[targetVl],
                                          restoreCells);

    header.SetCredit(false);
    header.SetCreditTargetVL(0);
    p->AddHeader(header);
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return true;
}

bool
UbCbfc::ShouldForceControlReturn() const
{
    for (int32_t pending : m_crdToReturn) {
        if (pending >= m_cbfcCfg.m_ctrlCrdRtrThldCells) {
            return true;
        }
    }
    return false;
}

void
UbCbfc::MaybeQueueControlReturn(uint32_t targetPortId)
{
    if (!ShouldForceControlReturn()) {
        return;
    }
    while (Ptr<Packet> cbfcPkt =
               BuildControlReturnPacket(targetPortId,
                                        m_cbfcCfg.m_retCellGrainControlPacket,
                                        "force_threshold")) {
        SendCrdAck(cbfcPkt, targetPortId);
    }
}

FcType UbCbfc::GetFcType()
{
    return m_fcType;
}


TypeId UbCbfcSharedCredit::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCbfcSharedCredit")
        .SetParent<UbCbfc>()
        .AddConstructor<UbCbfcSharedCredit>();
    return tid;
}

void UbCbfcSharedCredit::Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
                            uint8_t retCellGrainControlPacket, int32_t reservedPerVlCells,
                            int32_t ctrlCrdRtrThldCells,
                            int32_t sharedInitCells, uint32_t nodeId, uint32_t portId)
{
    UbCbfc::Init(flitLen, nFlitPerCell, retCellGrainDataPacket, retCellGrainControlPacket,
                 reservedPerVlCells, ctrlCrdRtrThldCells, nodeId, portId);

    m_shareCrd = sharedInitCells;
    m_reservedPerVlCells = reservedPerVlCells;

    NS_LOG_DEBUG("NodeId: " << m_nodeId
                 << " PortId: " << m_portId
                 << " Init CbfcSharedMode"
                 << " reservedPerVlCells=" << m_reservedPerVlCells
                 << " sharedInitCells=" << m_shareCrd);

    NS_ABORT_MSG_IF(m_reservedPerVlCells < 0 || m_shareCrd < 0,
                    "CBFC_SHARED requires non-negative reserved and shared initial credits");
    if (m_reservedPerVlCells == 0 && m_shareCrd == 0)
    {
        NS_LOG_WARN("CBFC shared-credit configuration has zero total transmit credit on NodeId="
                    << m_nodeId
                    << " PortId=" << m_portId);
    }
}

FcType UbCbfcSharedCredit::GetFcType()
{
    return FcType::CBFC_SHARED;
}

bool UbCbfcSharedCredit::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    uint32_t nextPktSize = 0;

    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return false;
    }
    nextPktSize = ingressQ->GetNextPacketSize();
    NS_LOG_DEBUG("nextPktSize: " << nextPktSize);

    const int32_t consumeCellNum =
        ceil((float)nextPktSize / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));

    const uint8_t vlId = ingressQ->GetIngressPriority();
    const int32_t totalAvail = m_shareCrd + m_crdTxfree[vlId];

    if (totalAvail < consumeCellNum) {
        NS_LOG_INFO("Flow Control Credit Limited,outPort:{" << ingressQ->GetOutPortId()
                                                            << "} VL:{" << (uint32_t)vlId << "}");
        NS_LOG_DEBUG("TotalAvailable[ " << (uint32_t)vlId << " ]: " << totalAvail
                                     << " is insufficient. Need: " << consumeCellNum);
        return true;
    }
    NS_LOG_DEBUG("TotalAvailable[ " << (uint32_t)vlId << " ]: " << totalAvail
                                 << " is enough. Need: " << consumeCellNum);

    return false;
}

void UbCbfcSharedCredit::OnEgressEnqueued(const UbFlowControlEventContext& context)
{
    if (context.ingressQueue->IsControlFrame()) {
        NS_LOG_DEBUG("is crd pkt");
        return;
    }
    CbfcSharedConsumeCrd(context.packet);
    TryAttachPiggybackCredit(context.packet);
}

void UbCbfcSharedCredit::OnControlFrameReceived(Ptr<Packet> p)
{
    CbfcSharedRestoreCrd(p);
}

void UbCbfcSharedCredit::OnDataPacketReceived(Ptr<Packet> p)
{
    RestoreSharedDataPacketCredit(p);
}

// CBFC 信用共享模式：信用消耗（Consume）逻辑
// 1. 计算当前报文需要消耗的 Cell 数量
// 2. 优先尝试从共享信用池 (m_shareCrd) 中扣除
// 3. 如果共享池信用充足，完成消耗并返回
// 4. 如果共享池信用不足，将共享池清零，并从该 VL 独占的信用证 (m_crdTxfree[vlId]) 中补充扣除剩余部分
// 5. 如果独占信用证依然不足，记录警告并归零
bool UbCbfcSharedCredit::CbfcSharedConsumeCrd(Ptr<Packet> p)
{
    const uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId << " pktSize: " << pktSize);
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    const uint8_t vlId = pktHeader.GetPacketVL();

    const int32_t consumeCellNum =
        ceil((float)pktSize / (m_cbfcCfg.m_flitLen * m_cbfcCfg.m_nFlitPerCell));
    
    NS_LOG_DEBUG("befor consume, m_shareCrd: " << m_shareCrd << " m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);

    if (m_shareCrd >= consumeCellNum) {
        m_shareCrd -= consumeCellNum;
        NS_LOG_DEBUG("left m_shareCrd: " << m_shareCrd << " left m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
        return true;
    }

    const int32_t remainder = consumeCellNum - m_shareCrd;
    m_shareCrd = 0;

    if (m_crdTxfree[vlId] >= remainder) {
        m_crdTxfree[vlId] -= remainder;
        NS_LOG_DEBUG("left m_shareCrd: " << m_shareCrd << " left m_crdTxfree[ " << (uint32_t)vlId << " ]: " << m_crdTxfree[vlId]);
        return true;
    }

    NS_LOG_WARN("CbfcSharedConsumeCrd underflow, vlId: " << (uint32_t)vlId);
    m_crdTxfree[vlId] = 0;
    return false;
}

// CBFC 信用共享模式：信用归还（Restore）逻辑
// 1. 解析控制报文，统计当前端口收到的所有 VL 归还的信用证总数
// 2. 将所有归还的信用证统一填充到共享信用池 (m_shareCrd) 中
// 3. 遍历所有优先级队列 (VL)，检查各 VL 的独占信用证是否达到预留阈值 (m_reservedPerVlCells)
// 4. 若某个 VL 的独占信用不足，则从共享池中拨付信用进行补充，直到达到阈值或共享池耗尽
// 4.1 补充顺序：可根据实际场景自定义。目前实现为按照 VL 优先级索引从小到大依次进行补充
// 5. 触发端口的发送流程 (TriggerTransmit) 以尝试发送因信用不足而积压的报文
bool UbCbfcSharedCredit::CbfcSharedRestoreCrd(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    NS_LOG_DEBUG("NodeId: " << m_nodeId << " PortId: " << m_portId);
    port->ResetCredits();
    UbDatalinkControlCreditHeader crdHeader = UbDataLink::ParseCreditHeader(p, port);

    uint32_t resumeCellGrainNum = 0;
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    std::vector<uint8_t> restoredCredits;
    restoredCredits.reserve(ubVlNum);
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG("port m_credits[ " << (uint32_t)index << " ]: " << (uint32_t)port->m_credits[index]);
        restoredCredits.push_back(port->m_credits[index]);
    }

    int32_t totalReturned = 0;

    for (int index = 0; index < ubVlNum; index++) {
        if (port->m_credits[index] > 0) {
            resumeCellGrainNum = port->m_credits[index];
            int32_t cells = resumeCellGrainNum * m_cbfcCfg.m_retCellGrainControlPacket;
            totalReturned += cells;
        }
    }

    if (totalReturned > 0) {
        NS_LOG_DEBUG("before resume Share: " << m_shareCrd << " TotalReturned: " << totalReturned);
        m_shareCrd += totalReturned;
        
        for (int vl = 0; vl < ubVlNum; vl++) {
            if (m_shareCrd <= 0) break;

            if (m_crdTxfree[vl] < m_reservedPerVlCells) {
                int32_t needed = m_reservedPerVlCells - m_crdTxfree[vl];
                int32_t canGive = std::min(needed, m_shareCrd);
                
                NS_LOG_DEBUG("Refill VL " << vl << " before: " << m_crdTxfree[vl] << " add: " << canGive);
                m_crdTxfree[vl] += canGive;
                m_shareCrd -= canGive;
                NS_LOG_DEBUG("Refill VL " << vl << " after: " << m_crdTxfree[vl]);
            }
        }
        NS_LOG_DEBUG("left Share: " << m_shareCrd);
        ret = true;
    }

    if (ret)
    {
        ControlCreditRestoreNotify(m_nodeId, m_portId, restoredCredits);
    }

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return ret;
}

bool
UbCbfcSharedCredit::RestoreSharedDataPacketCredit(Ptr<Packet> p)
{
    if (p == nullptr) {
        return false;
    }

    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    if (!header.GetCredit()) {
        p->AddHeader(header);
        return false;
    }

    m_shareCrd += m_cbfcCfg.m_retCellGrainDataPacket;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    const int ubVlNum = val.Get();
    for (int vl = 0; vl < ubVlNum && m_shareCrd > 0; ++vl) {
        if (m_crdTxfree[vl] < m_reservedPerVlCells) {
            const int32_t need = m_reservedPerVlCells - m_crdTxfree[vl];
            const int32_t grant = std::min<int32_t>(need, m_shareCrd);
            m_crdTxfree[vl] += grant;
            m_shareCrd -= grant;
        }
    }

    header.SetCredit(false);
    header.SetCreditTargetVL(0);
    p->AddHeader(header);
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
    return true;
}

TypeId UbPfc::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPfc")
        .SetParent<UbFlowControl>()
        .AddConstructor<UbPfc>();
    return tid;
}

FcType UbPfc::GetFcType()
{
    return m_fcType;
}

void UbPfc::InitRuntimeState(FcType mode,
                             int32_t portpfcUpThld,
                             int32_t portpfcLowThld,
                             uint32_t nodeId,
                             uint32_t portId,
                             uint32_t vlNum)
{
    m_pfcStatus = pfcStatus_t(vlNum);
    m_pfcCfg.m_portpfcUpThld = portpfcUpThld;
    m_pfcCfg.m_portpfcLowThld = portpfcLowThld;
    m_nodeId = nodeId;
    m_portId = portId;
    m_fcType = mode;
}

void UbPfc::BindFixedDecisionHook()
{
    NS_ABORT_MSG_IF(m_pfcCfg.m_portpfcUpThld < 0 || m_pfcCfg.m_portpfcLowThld < 0,
                    "PFC_FIXED requires non-negative hi/lo thresholds");
    NS_ABORT_MSG_IF(m_pfcCfg.m_portpfcLowThld >= m_pfcCfg.m_portpfcUpThld,
                    "PFC_FIXED requires PfcLowThld < PfcUpThld");
    auto hook = CreateObject<UbPfcFixedDecisionHook>();
    hook->Init(m_pfcCfg.m_portpfcUpThld, m_pfcCfg.m_portpfcLowThld);
    m_decisionHook = hook;
}

void UbPfc::BindDynamicDecisionHook(Ptr<UbQueueManager> queueManager)
{
    const auto profile = queueManager->GetBufferProfileView();
    NS_ABORT_MSG_IF(profile.shared_pool_bytes == 0,
                    "PFC_DYNAMIC requires SharedPoolBytes > 0");
    NS_ABORT_MSG_IF(profile.headroom_per_port_bytes == 0,
                    "PFC_DYNAMIC requires HeadroomPerPortBytes > 0");
    m_decisionHook = CreateObject<UbPfcDynamicDecisionHook>();
}

void UbPfc::BindPaperDynamicDecisionHook(Ptr<UbQueueManager> queueManager)
{
    const auto profile = queueManager->GetBufferProfileView();
    NS_ABORT_MSG_IF(profile.shared_pool_bytes == 0,
                    "PFC_DYNAMIC_PAPER requires SharedPoolBytes > 0");
    m_decisionHook = CreateObject<UbPfcPaperDynamicDecisionHook>();
}

void UbPfc::BindDecisionHook(Ptr<UbQueueManager> queueManager)
{
    NS_ABORT_MSG_IF(m_fcType != FcType::PFC_FIXED && m_fcType != FcType::PFC_DYNAMIC &&
                        m_fcType != FcType::PFC_DYNAMIC_PAPER,
                    "UbPfc must be initialized with PFC_FIXED, PFC_DYNAMIC, or PFC_DYNAMIC_PAPER mode");

    switch (m_fcType) {
        case FcType::PFC_FIXED:
            BindFixedDecisionHook();
            break;
        case FcType::PFC_DYNAMIC:
            BindDynamicDecisionHook(queueManager);
            break;
        case FcType::PFC_DYNAMIC_PAPER:
            BindPaperDynamicDecisionHook(queueManager);
            break;
        default:
            NS_ABORT_MSG("Unsupported PFC mode in BindDecisionHook");
    }
}

void UbPfc::Init(FcType mode,
                 int32_t portpfcUpThld,
                 int32_t portpfcLowThld,
                 uint32_t nodeId,
                 uint32_t portId)
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    const uint32_t ubVlNum = static_cast<uint32_t>(val.Get());
    InitRuntimeState(mode, portpfcUpThld, portpfcLowThld, nodeId, portId, ubVlNum);

    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));
    auto queueManager = port->GetNode()->GetObject<UbSwitch>()->GetQueueManager();

    NS_LOG_DEBUG("NodeId: " << m_nodeId
                 << " PortId: " << m_portId
                 << " Init Pfc mode="
                 << (m_fcType == FcType::PFC_FIXED ? "PFC_FIXED"
                     : (m_fcType == FcType::PFC_DYNAMIC ? "PFC_DYNAMIC" : "PFC_DYNAMIC_PAPER"))
                 << " hi=" << m_pfcCfg.m_portpfcUpThld
                 << " lo=" << m_pfcCfg.m_portpfcLowThld);

    BindDecisionHook(queueManager);
}

void UbPfc::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_decisionHook = nullptr;
    Object::DoDispose();
}

bool UbPfc::IsFcLimited(Ptr<UbIngressQueue> ingressQ)
{
    if (ingressQ->GetIngressQueueType() == IngressQueueType::VOQ && ingressQ->IsControlFrame()) {
        NS_LOG_DEBUG("is Pfc pkt");
        return false;
    }
    if (m_pfcStatus.m_portCredits[ingressQ->GetIngressPriority()] == 0) {
        NS_LOG_INFO("Flow Control Pfc Limited! NodeId: " << m_nodeId << ",outPort:{" << ingressQ->GetOutPortId() << "} VL:{"
                    << ingressQ->GetIngressPriority() << "}");
        return true;  // 不允许发送
    }

    return false;
}

void UbPfc::OnIngressReleased(const UbFlowControlEventContext& context)
{
    if (context.inPortId != context.outPortId) { // 转发的报文
        Ptr<Packet> pfcPkt = CheckPfcThreshold(context.packet, context.inPortId);
        if (pfcPkt != nullptr) {
            SendPfc(pfcPkt, context.inPortId);
        }
    }
}

void UbPfc::OnEgressEnqueued(const UbFlowControlEventContext& context)
{
}

void UbPfc::OnControlFrameReceived(Ptr<Packet> p)
{
    UpdatePfcStatus(p);
}

void UbPfc::OnIngressEnqueued(const UbFlowControlEventContext& context)
{
    Ptr<Packet> pfcPkt = CheckPfcThreshold(context.packet, m_portId);
    if (pfcPkt != nullptr) {
        SendPfc(pfcPkt, m_portId);
    }
}

bool UbPfc::UpdatePfcStatus(Ptr<Packet> p)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(m_portId));

    UbDatalinkControlCreditHeader pfcHeader = UbDataLink::ParseCreditHeader(p, port);
    bool ret = false;
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    for (int index = 0; index < ubVlNum; index++) {
        if (m_pfcStatus.m_portCredits[index] != port->m_credits[index]) {
            m_pfcStatus.m_portCredits[index] = port->m_credits[index];
            ret = true;
        }
    }

    NS_LOG_DEBUG("Recv Pfc uid: " << p->GetUid() << " NodeId: " << port->GetNode()->GetId() << " PortId: "
                << port->GetIfIndex() << " m_pfcStatus->m_portCredits:{"
                << (uint32_t)m_pfcStatus.m_portCredits[0] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[1] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[2] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[3] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[4] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[5] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[6] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[7] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[8] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[9] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[10] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[11] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[12] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[13] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[14] << " "
                << (uint32_t)m_pfcStatus.m_portCredits[15] << "}");

    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);

    return ret;
}

void UbPfc::SendPfc(Ptr<Packet> pfcPacket, uint32_t targetPortId)
{
    Ptr<Node> node = NodeList::GetNode(m_nodeId);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(targetPortId));
    
    node->GetObject<UbSwitch>()->SendControlFrame(pfcPacket, targetPortId);
    
    auto flowControl = DynamicCast<UbPfc>(port->GetFlowControl());
    flowControl->m_pfcStatus.m_pfcSndCnt++;
}

Ptr<Packet> UbPfc::CheckPfcThreshold(Ptr<Packet> p, uint32_t portId)
{
    Ptr<Packet> pfcPkt = nullptr;
    Ptr<Node> node = NodeList::GetNode(m_nodeId);

    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
    NS_LOG_DEBUG("NodeId: " << node->GetId() << " PortId: " << portId);
    auto flowControl = DynamicCast<UbPfc>(port->GetFlowControl());

    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();

    NS_ASSERT_MSG(flowControl->m_decisionHook != nullptr,
                  "UbPfc must have a decision hook before threshold evaluation");

    auto queueManager = node->GetObject<UbSwitch>()->GetQueueManager();
    for (int pri = 0; pri < ubVlNum; pri++) {
        UbPfcDecision decision = flowControl->m_decisionHook->Evaluate(queueManager, portId, pri);
        const uint8_t currentPauseState = decision.pause ? 1 : 0;

        if (flowControl->m_fcType == FcType::PFC_DYNAMIC &&
            flowControl->m_pfcStatus.m_pfcDynamicLastTracePause[pri] != currentPauseState) {
            utils::UbUtils::PfcDynamicStateNotify(port->GetNode()->GetId(),
                                                  portId,
                                                  static_cast<uint32_t>(pri),
                                                  decision.ingress_total_bytes,
                                                  decision.ingress_shared_bytes,
                                                  decision.ingress_headroom_bytes,
                                                  decision.xoff_threshold_bytes,
                                                  decision.xon_threshold_bytes,
                                                  decision.pause);
            flowControl->m_pfcStatus.m_pfcDynamicLastTracePause[pri] = currentPauseState;
        }

        if (decision.pause) {
            flowControl->m_pfcStatus.m_pfcSndCredits[pri] = 0;
        } else if (decision.resume) {
            flowControl->m_pfcStatus.m_pfcSndCredits[pri] = UB_CREDIT_MAX_VALUE;
        }
    }

    if (flowControl->m_pfcStatus.m_pfcSndCredits == flowControl->m_pfcStatus.m_pfcLastSndCredits) {
        NS_LOG_DEBUG("State Preservation");
        return pfcPkt;
    }
    for (int pri = 0; pri < ubVlNum; pri++) {
        if (flowControl->m_pfcStatus.m_pfcSndCredits[pri] ==
            flowControl->m_pfcStatus.m_pfcLastSndCredits[pri]) {
            continue;
        }
        const bool pause = flowControl->m_pfcStatus.m_pfcSndCredits[pri] == 0;
        const uint64_t ingressBytes = queueManager != nullptr
                                          ? queueManager->GetQueueIngressTotalBytes(portId, pri)
                                          : 0;
        UbUtils::PfcStateNotify(node->GetId(),
                                portId,
                                pause ? "PAUSE" : "RESUME",
                                static_cast<uint32_t>(pri),
                                ingressBytes);
    }

    port->ResetCredits();
    for (int pri = 0; pri < ubVlNum; pri++) {
        if (flowControl->m_pfcStatus.m_pfcSndCredits[pri]) {
            port->SetCredits(pri, flowControl->m_pfcStatus.m_pfcSndCredits[pri]);
        }
    }

    NS_LOG_DEBUG("m_pfcStatus->m_pfcSndCredits: ");
    for (int index = 0; index < ubVlNum; index++) {
        NS_LOG_DEBUG((uint32_t)flowControl->m_pfcStatus.m_pfcSndCredits[index] << " ");
    }

    flowControl->m_pfcStatus.m_pfcLastSndCredits = flowControl->m_pfcStatus.m_pfcSndCredits;

    NS_LOG_DEBUG("Port credits changed. NodeId: " << node->GetId() << " inPort:{" << portId
            << "} VL:{" << (uint32_t)port->m_credits[0] << " " << (uint32_t)port->m_credits[1] << " "
            << (uint32_t)port->m_credits[2] << " " << (uint32_t)port->m_credits[3] << " "
            << (uint32_t)port->m_credits[4] << " " << (uint32_t)port->m_credits[5] << " "
            << (uint32_t)port->m_credits[6] << " " << (uint32_t)port->m_credits[7] << " "
            << (uint32_t)port->m_credits[8] << " " << (uint32_t)port->m_credits[9] << " "
            << (uint32_t)port->m_credits[10] << " " << (uint32_t)port->m_credits[11] << " "
            << (uint32_t)port->m_credits[12] << " " << (uint32_t)port->m_credits[13] << " "
            << (uint32_t)port->m_credits[14] << " " << (uint32_t)port->m_credits[15] << "}");
    pfcPkt = UbDataLink::GenControlCreditPacket(port->m_credits);
    NS_LOG_DEBUG("Create pfcpkt uid: " << pfcPkt->GetUid());

    return pfcPkt;
}
}  // namespace ns3
