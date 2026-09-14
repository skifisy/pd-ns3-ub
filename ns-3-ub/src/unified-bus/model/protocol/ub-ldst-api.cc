// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/log.h"
#include "ns3/simulator.h"

#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-ldst-api.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbLdstApi");

NS_OBJECT_ENSURE_REGISTERED(UbLdstApi);

TypeId UbLdstApi::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbLdstApi")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddAttribute("UsePacketSpray",
                                          "Enable per-packet load balancing across equal-cost paths.",
                                          BooleanValue(false),
                                          MakeBooleanAccessor(&UbLdstApi::m_usePacketSpray),
                                          MakeBooleanChecker())
                            .AddAttribute("UseShortestPaths",
                                          "Sets a packet header flag that instructs switches to restrict forwarding to shortest paths (true) or allow non-shortest paths (false).",
                                          BooleanValue(true),
                                          MakeBooleanAccessor(&UbLdstApi::m_useShortestPaths),
                                          MakeBooleanChecker())
                            .AddTraceSource("LdstRecvNotify",
                                            "Fires on Ldst data or ACK reception (provides info and trace tags).",
                                            MakeTraceSourceAccessor(&UbLdstApi::m_ldstRecvNotify),
                                            "ns3::UbLdstApi::LdstRecvNotify");

    return tid;
}

UbLdstApi::UbLdstApi()
{
    BooleanValue val;
    if (GlobalValue::GetValueByNameFailSafe("UB_RECORD_PKT_TRACE", val)) {
        GlobalValue::GetValueByName("UB_RECORD_PKT_TRACE", val);
        m_pktTraceEnabled = val.Get();
    } else {
        m_pktTraceEnabled = false;
    }
}

UbLdstApi::~UbLdstApi()
{
}

void UbLdstApi::SetNodeId(uint32_t nodeId)
{
    m_nodeId = nodeId;
}

void UbLdstApi::LdstProcess(Ptr<UbLdstTaskSegment> taskSegment)
{
    // genpacket
    auto packet = GenDataPacket(taskSegment);
    // sendpacket
    SendPacket(taskSegment, packet);
}

void UbLdstApi::SendPacket(Ptr<UbLdstTaskSegment> taskSegment, Ptr<Packet> packet)
{
    RoutingKey rtKey;
    rtKey.sip = utils::NodeIdToIp(taskSegment->GetSrc()).Get();
    rtKey.dip = utils::NodeIdToIp(taskSegment->GetDest()).Get();
    rtKey.sport = m_lbHashSalt;
    rtKey.dport = 0;
    rtKey.priority = taskSegment->GetPriority();
    rtKey.useShortestPath = m_useShortestPaths;
    rtKey.usePacketSpray = m_usePacketSpray;

    auto node = NodeList::GetNode(m_nodeId);
    auto sw = node->GetObject<UbSwitch>();
    bool selectedShortestPath = false;
    int outPort = sw->GetRoutingProcess()->GetOutPort(rtKey, selectedShortestPath);
    if (outPort < 0) {
        // Route failed
        NS_ASSERT_MSG(0, "The route cannot be found");
    }
    uint16_t destPort = outPort;
    sw->PushPacketToVoq(packet, destPort, taskSegment->GetPriority(), destPort);
    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(destPort));
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, port);
}

Ptr<Packet> UbLdstApi::GenDataPacket(Ptr<UbLdstTaskSegment> taskSegment)
{
    // Store/load request: DLH cNTH cTAH(0x03/0x06) [cMAETAH] Payload
    UbCompactMAExtTah cMAETah;
    UbCompactTransactionHeader cTaHeader;
    UbCna16NetworkHeader cna16NetworkHeader;
    uint32_t length = taskSegment->GetLength();
    uint32_t payloadSize = 0;
    uint32_t dataSize = taskSegment->PeekNextDataSize();
    // add cTAH (Compact Transaction Header)
    if (taskSegment->GetType() == UbMemOperationType::STORE) {
        cTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_WRITE);
        payloadSize = dataSize;
    } else if (taskSegment->GetType() == UbMemOperationType::LOAD) {
        cTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_READ);
        payloadSize = taskSegment->GetPacketSize();
    }

    if (m_usePacketSpray) {
        if (m_lbHashSalt == MAX_LB) {
            m_lbHashSalt = MIN_LB;
        } else {
            m_lbHashSalt++;
        }
    }
    Ptr<Packet> packet = Create<Packet>(payloadSize);
    taskSegment->UpdateSentBytes(dataSize);
    // Gen Headers
    cMAETah.SetLength((uint8_t)length);
    cTaHeader.SetIniTaSsn(taskSegment->GetTaskSegmentId()); // taskid
    uint16_t scna = static_cast<uint16_t>(utils::NodeIdToCna16(taskSegment->GetSrc()));
    cna16NetworkHeader.SetScna(scna);
    uint16_t dcna = static_cast<uint16_t>(utils::NodeIdToCna16(taskSegment->GetDest()));
    cna16NetworkHeader.SetDcna(dcna);
    cna16NetworkHeader.SetLb(m_lbHashSalt);
    cna16NetworkHeader.SetServiceLevel(taskSegment->GetPriority());

    packet->AddHeader(cMAETah);
    packet->AddHeader(cTaHeader);
    packet->AddHeader(cna16NetworkHeader);

    // add dl header
    UbDataLink::GenPacketHeader(packet, false, false, taskSegment->GetPriority(), taskSegment->GetPriority(),
                                m_usePacketSpray, m_useShortestPaths, UbDatalinkHeaderConfig::PACKET_UB_MEM);
    UbFlowTag flowTag(taskSegment->GetTaskId(), taskSegment->GetSize());
    packet->AddPacketTag(flowTag);
    NS_LOG_DEBUG("[UbLdstApi GenDataPacket] packetUid: " << packet->GetUid() << " payload size:" << payloadSize);
    return packet;
}

/**
 * 接收到一个数据包，调用此函数处理，产生ack
 */
void UbLdstApi::RecvDataPacket(Ptr<Packet> packet)
{
    // Store/load request: DLH cNTH cTAH(0x03/0x06) [cMAETAH] Payload
    // Store/load response: DLH cNTH cATAH(0x11/0x12) Payload
    if (packet == nullptr) {
        NS_LOG_ERROR("Null packet received");
        return;
    }

    NS_LOG_DEBUG("[UbLdstApi RecvDataPacket] nodeId: " << m_nodeId << " packetUid: " << packet->GetUid());
    UbDatalinkPacketHeader linkPacketHeader;
    UbCompactAckTransactionHeader caTaHeader;
    UbCna16NetworkHeader cna16NetworkHeader;
    UbCompactTransactionHeader cTaHeader;
    UbCompactMAExtTah cMAETah;
    
    packet->RemoveHeader(linkPacketHeader);
    packet->RemoveHeader(cna16NetworkHeader);
    packet->RemoveHeader(cTaHeader);
    packet->PeekHeader(cMAETah);

    Ptr<Packet> ackp;
    // 收到store数据包
    if (cTaHeader.GetTaOpcode() == static_cast<uint8_t>(TaOpcode::TA_OPCODE_WRITE)) {
        ackp = Create<Packet>(0);
        caTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    } else if (cTaHeader.GetTaOpcode() == static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ)) {
        caTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_READ_RESPONSE);
        uint32_t payloadSize = 64 * (1 << (uint32_t)cMAETah.GetLength());
        NS_LOG_DEBUG("[UbLdstApi RecvDataPacket] Load payloadSize: " << payloadSize);
        ackp = Create<Packet>(payloadSize);
    }
    uint16_t tassn = cTaHeader.GetIniTaSsn();
    caTaHeader.SetIniTaSsn(tassn);

    uint16_t tmp = cna16NetworkHeader.GetScna();
    cna16NetworkHeader.SetScna(cna16NetworkHeader.GetDcna());
    cna16NetworkHeader.SetDcna(tmp);

    ackp->AddHeader(caTaHeader);
    ackp->AddHeader(cna16NetworkHeader);
    UbDataLink::GenPacketHeader(ackp, false, true, linkPacketHeader.GetCreditTargetVL(), linkPacketHeader.GetPacketVL(),
                                linkPacketHeader.GetLoadBalanceMode(), linkPacketHeader.GetRoutingPolicy(),
                                UbDatalinkHeaderConfig::PACKET_UB_MEM);

    RoutingKey rtKey;
    rtKey.sip = utils::Cna16ToIp(cna16NetworkHeader.GetScna()).Get();
    rtKey.dip = utils::Cna16ToIp(cna16NetworkHeader.GetDcna()).Get();
    rtKey.sport = cna16NetworkHeader.GetLb();
    rtKey.dport = 0;
    rtKey.priority = linkPacketHeader.GetPacketVL();
    rtKey.useShortestPath = linkPacketHeader.GetRoutingPolicy();
    rtKey.usePacketSpray = linkPacketHeader.GetLoadBalanceMode();
    
    auto node = NodeList::GetNode(m_nodeId);
    auto sw = node->GetObject<UbSwitch>();

    bool selectedShortestPath = false;
    int destPort = sw->GetRoutingProcess()->GetOutPort(rtKey, selectedShortestPath);
    if (destPort < 0) {
        // Route failed
        NS_ASSERT_MSG(0, "The route cannot be found");
    }

    sw->PushPacketToVoq(ackp, destPort, linkPacketHeader.GetPacketVL(), destPort);

    NS_LOG_DEBUG("[UbLdstApi RecvDataPacket] Send Ack. NodeId: " << m_nodeId << " PacketUid: "
                  << ackp->GetUid() << " packetSize: " << ackp->GetSize() << " destPort: " << destPort);
    if (m_pktTraceEnabled) {
        UbFlowTag flowTag;
        packet->PeekPacketTag(flowTag);
        UbPacketTraceTag traceTag;
        packet->PeekPacketTag(traceTag);
        LdstRecvNotify(packet->GetUid(), utils::Cna16ToNodeId(cna16NetworkHeader.GetDcna()),
                       utils::Cna16ToNodeId(cna16NetworkHeader.GetScna()),
                       PacketType::PACKET, packet->GetSize(), flowTag.GetFlowId(), traceTag);
    }
    Ptr<UbPort> triggerPort = DynamicCast<UbPort>(node->GetDevice(destPort));
    triggerPort->TriggerTransmit(); // 触发发送
}

void UbLdstApi::RecvResponse(Ptr<Packet> packet)
{
    if (packet == nullptr) {
        NS_LOG_ERROR("in RecvResponse, packet is null");
        return;
    }
    NS_LOG_DEBUG("[UbLdstApi RecvResponse] packetUid: " << packet->GetUid());
    // Store/load response: DLH cNTH cATAH(0x11/0x12) Payload
    UbDatalinkPacketHeader linkPacketHeader;
    UbCna16NetworkHeader cna16NetworkHeader;
    UbCompactAckTransactionHeader caTaHeader;
    packet->RemoveHeader(linkPacketHeader);
    packet->RemoveHeader(cna16NetworkHeader);
    packet->RemoveHeader(caTaHeader);

    if (m_pktTraceEnabled) {
        UbFlowTag flowTag;
        packet->PeekPacketTag(flowTag);
        UbPacketTraceTag traceTag;
        packet->PeekPacketTag(traceTag);
        LdstRecvNotify(packet->GetUid(), utils::Cna16ToNodeId(cna16NetworkHeader.GetDcna()),
                       utils::Cna16ToNodeId(cna16NetworkHeader.GetScna()),
                       PacketType::ACK, packet->GetSize(), flowTag.GetFlowId(), traceTag);
    }
    uint32_t taskSegmentId = caTaHeader.GetIniTaSsn();
    auto ldstInst = NodeList::GetNode(m_nodeId)->GetObject<UbLdstInstance>();
    Simulator::ScheduleNow(&UbLdstInstance::OnRecvAck, ldstInst, taskSegmentId);
}

void UbLdstApi::SetUsePacketSpray(bool usePacketSpray)
{
    m_usePacketSpray = usePacketSpray;
}

void UbLdstApi::SetUseShortestPaths(bool useShortestPaths)
{
    m_useShortestPaths = useShortestPaths;
}

void UbLdstApi::LdstRecvNotify(uint32_t packetUid, uint32_t src, uint32_t dst,
                               PacketType type, uint32_t size, uint32_t taskId, UbPacketTraceTag traceTag)
{
    m_ldstRecvNotify(packetUid, src, dst, type, size, taskId, traceTag);
}

} // namespace ns3