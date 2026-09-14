// SPDX-License-Identifier: GPL-2.0-only
#include "ub-ctp.h"

#include "ns3/boolean.h"
#include "ns3/global-value.h"
#include "ns3/hash.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-function.h"
#include "ns3/ub-header.h"
#include "ns3/ub-modulo-sequence.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-utils.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UbCtpTransportService");
NS_OBJECT_ENSURE_REGISTERED(UbCompactTransportChannel);
NS_OBJECT_ENSURE_REGISTERED(UbCtpTransportService);

namespace
{

using UbTaSsnSequence = UbModuloSequence<16, uint32_t>;

uint32_t
CalcCompactLength(uint32_t bytes)
{
    if (bytes <= 64)
    {
        return 0;
    }

    bytes = (bytes - 1) / 64;
    uint32_t length = 0;
    while (bytes > 0)
    {
        bytes >>= 1;
        ++length;
    }
    return length;
}

uint16_t
EntityLoadBalanceSalt(const UbCtpEntityKey& key)
{
    uint8_t buf[17];
    buf[0] = static_cast<uint8_t>((key.srcNodeId >> 24) & 0xff);
    buf[1] = static_cast<uint8_t>((key.srcNodeId >> 16) & 0xff);
    buf[2] = static_cast<uint8_t>((key.srcNodeId >> 8) & 0xff);
    buf[3] = static_cast<uint8_t>(key.srcNodeId & 0xff);
    buf[4] = static_cast<uint8_t>((key.dstNodeId >> 24) & 0xff);
    buf[5] = static_cast<uint8_t>((key.dstNodeId >> 16) & 0xff);
    buf[6] = static_cast<uint8_t>((key.dstNodeId >> 8) & 0xff);
    buf[7] = static_cast<uint8_t>(key.dstNodeId & 0xff);
    buf[8] = static_cast<uint8_t>((key.srcEntityId >> 24) & 0xff);
    buf[9] = static_cast<uint8_t>((key.srcEntityId >> 16) & 0xff);
    buf[10] = static_cast<uint8_t>((key.srcEntityId >> 8) & 0xff);
    buf[11] = static_cast<uint8_t>(key.srcEntityId & 0xff);
    buf[12] = static_cast<uint8_t>((key.dstEntityId >> 24) & 0xff);
    buf[13] = static_cast<uint8_t>((key.dstEntityId >> 16) & 0xff);
    buf[14] = static_cast<uint8_t>((key.dstEntityId >> 8) & 0xff);
    buf[15] = static_cast<uint8_t>(key.dstEntityId & 0xff);
    buf[16] = key.vl;
    return static_cast<uint16_t>(Hash64(std::string(reinterpret_cast<const char*>(buf), sizeof(buf))) &
                                 0xffff);
}

void
ValidateCna16PortHint(uint32_t port, const char* label)
{
    NS_ABORT_MSG_IF(port >= 15,
                    "CTP " << label << " port hint cannot be encoded as CNA16");
}

void
PadTaAckPacketForExperiment(Ptr<Packet> packet, uint32_t experimentTaAckPacketBytes)
{
    if (experimentTaAckPacketBytes == 0 || packet == nullptr)
    {
        return;
    }

    const uint32_t currentBytes = packet->GetSize();
    NS_ABORT_MSG_IF(experimentTaAckPacketBytes < currentBytes,
                    "ExperimentTaAckPacketBytes is smaller than the encoded CTP TAACK header stack");
    if (experimentTaAckPacketBytes > currentBytes)
    {
        packet->AddPaddingAtEnd(experimentTaAckPacketBytes - currentBytes);
    }
}

Ptr<Packet>
BuildCtpPacket(Ptr<UbWqeSegment> segment,
               const UbCtpEntityKey& key,
               uint32_t dstPortHint,
               uint32_t taSsn,
               uint32_t progressBytes,
               const UbCtpRoutingPolicy& routing,
               uint8_t loadBalanceSalt)
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP packet requires WQE segment");

    const bool isAckResponse =
        segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE &&
        segment->GetType() == TaOpcode::TA_OPCODE_TRANSACTION_ACK;
    const bool isReadRequest = segment->GetType() == TaOpcode::TA_OPCODE_READ &&
                               segment->GetSegmentKind() == UbTransactionSegmentKind::REQUEST;
    const uint32_t payloadBytes = isAckResponse || isReadRequest ? 0 : progressBytes;
    const uint32_t logicalBytes = isReadRequest ? segment->GetLogicalBytes() : payloadBytes;
    NS_ABORT_MSG_IF(payloadBytes > UB_MTU_BYTE,
                    "CTP packet payload must not exceed one MTU-sized transaction segment");
    NS_ABORT_MSG_IF(isReadRequest && logicalBytes > UB_MTU_BYTE,
                    "CTP read request segment must not describe more than one MTU response");

    Ptr<Packet> packet = Create<Packet>(payloadBytes);
    UbFlowTag flowTag(segment->GetTaskId(), segment->GetWqeSize());
    packet->AddPacketTag(flowTag);

    if (segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE)
    {
        UbCompactAckTransactionHeader ackHeader;
        ackHeader.SetTaOpcode(segment->GetType());
        ackHeader.SetIniTaSsn(static_cast<uint16_t>(taSsn));
        packet->AddHeader(ackHeader);
    }
    else
    {
        UbCompactTransactionHeader taHeader;
        taHeader.SetTaOpcode(segment->GetType());
        taHeader.SetIniTaSsn(static_cast<uint16_t>(taSsn));
        UbCompactMAExtTah maHeader;
        maHeader.SetLength(static_cast<uint8_t>(CalcCompactLength(logicalBytes)));
        packet->AddHeader(maHeader);
        packet->AddHeader(taHeader);
    }

    UbCompactEidHeader eidHeader;
    eidHeader.SetSourceEid(key.srcEntityId);
    eidHeader.SetDestinationEid(key.dstEntityId);
    packet->AddHeader(eidHeader);

    UbCompactUpiHeader upiHeader;
    upiHeader.SetUpi(0);
    packet->AddHeader(upiHeader);

    UbCtpHeader ctpHeader;
    ctpHeader.SetTPOpcode(CtpOpcode::CTP_DATA);
    ctpHeader.SetPadding(0);
    ctpHeader.SetNlp(UB_CTPH_NLP_UPI16_EID40_TAH);
    packet->AddHeader(ctpHeader);

    UbCna16NetworkHeader cnaHeader;
    cnaHeader.SetScna(static_cast<uint16_t>(utils::NodeIdToCna16(key.srcNodeId)));
    const uint32_t dstCna =
        dstPortHint == UINT32_MAX ? utils::NodeIdToCna16(key.dstNodeId)
                                  : utils::NodeIdToCna16(key.dstNodeId, dstPortHint);
    cnaHeader.SetDcna(static_cast<uint16_t>(dstCna));
    cnaHeader.SetLb(loadBalanceSalt);
    cnaHeader.SetServiceLevel(key.vl);
    cnaHeader.SetNlp(UB_CNA_NLP_CTPH);
    packet->AddHeader(cnaHeader);

    UbDataLink::GenPacketHeader(packet,
                                false,
                                false,
                                key.vl,
                                key.vl,
                                routing.usePacketSpray,
                                routing.useShortestPaths,
                                UbDatalinkHeaderConfig::PACKET_CNA16);
    return packet;
}

} // namespace

bool
UbCtpEntityKey::operator<(const UbCtpEntityKey& other) const
{
    return std::tie(srcNodeId, srcEntityId, dstNodeId, dstEntityId, vl) <
           std::tie(other.srcNodeId,
                    other.srcEntityId,
                    other.dstNodeId,
                    other.dstEntityId,
                    other.vl);
}

bool
UbCtpEntityKey::operator==(const UbCtpEntityKey& other) const
{
    return srcNodeId == other.srcNodeId && srcEntityId == other.srcEntityId &&
           dstNodeId == other.dstNodeId && dstEntityId == other.dstEntityId && vl == other.vl;
}

bool
UbCtpCongestionKey::operator<(const UbCtpCongestionKey& other) const
{
    return std::tie(srcNodeId, srcEntityId, dstNodeId, dstEntityId, vl) <
           std::tie(other.srcNodeId,
                    other.srcEntityId,
                    other.dstNodeId,
                    other.dstEntityId,
                    other.vl);
}

UbCtpTransactionContext::UbCtpTransactionContext(UbCtpEntityKey key,
                                                 uint32_t ackWindowCapacity)
    : m_key(key)
{
    NS_ABORT_MSG_IF(ackWindowCapacity == 0,
                    "CTP transaction context requires a non-zero ack window");
    m_ackWindow.Resize(ackWindowCapacity);
    m_ackWindow.Reset(0);
}

bool
UbCtpTransactionContext::CanAdmit(uint32_t sequence) const
{
    if (sequence != m_sendNext)
    {
        return false;
    }
    return true;
}

bool
UbCtpTransactionContext::TryAdmit(uint32_t sequence)
{
    if (!CanAdmit(sequence))
    {
        return false;
    }
    ++m_sendNext;
    return true;
}

void
UbCtpTransactionContext::MarkTaAck(uint32_t sequence)
{
    if (sequence < m_completeUna || sequence >= m_sendNext)
    {
        return;
    }
    const bool marked = m_ackWindow.Mark(sequence);
    NS_ASSERT_MSG(marked, "CTP TAACK sequence must fit in window");
    m_completeUna += m_ackWindow.AdvanceContiguous();
}

std::optional<uint32_t>
UbCtpTransactionContext::MarkTaAckWire(uint16_t wireSequence)
{
    auto logicalSequence =
        UbTaSsnSequence::UnwrapInWindow(wireSequence, m_completeUna, m_sendNext);
    if (!logicalSequence.has_value())
    {
        return std::nullopt;
    }

    MarkTaAck(*logicalSequence);
    return logicalSequence;
}

uint32_t
UbCtpTransactionContext::GetCompleteUna() const
{
    return m_completeUna;
}

uint32_t
UbCtpTransactionContext::GetSendNext() const
{
    return m_sendNext;
}

uint32_t
UbCtpTransactionContext::GetOutstandingCount() const
{
    return m_sendNext - m_completeUna;
}

void
UbCtpTransactionContext::SetWindowForTest(uint32_t completeUna, uint32_t sendNext)
{
    NS_ABORT_MSG_IF(sendNext < completeUna, "CTP test window sendNext must not precede completeUna");
    m_completeUna = completeUna;
    m_sendNext = sendNext;
    m_ackWindow.Reset(completeUna);
}

UbCtpEntityState::UbCtpEntityState(UbCtpEntityKey key)
    : m_key(key)
{
}

UbCtpTxState&
UbCtpEntityState::Tx()
{
    return m_tx;
}

const UbCtpTxState&
UbCtpEntityState::Tx() const
{
    return m_tx;
}

UbCtpRxState&
UbCtpEntityState::Rx()
{
    return m_rx;
}

const UbCtpRxState&
UbCtpEntityState::Rx() const
{
    return m_rx;
}

const UbCtpEntityKey&
UbCtpEntityState::GetKey() const
{
    return m_key;
}

void
UbCtpEntityState::AddOutPort(uint32_t outPort)
{
    if (std::find(m_tx.outPorts.begin(), m_tx.outPorts.end(), outPort) == m_tx.outPorts.end())
    {
        m_tx.outPorts.push_back(outPort);
    }
}

const std::vector<uint32_t>&
UbCtpEntityState::GetOutPorts() const
{
    return m_tx.outPorts;
}

TypeId
UbCompactTransportChannel::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCompactTransportChannel")
                            .SetParent<UbIngressQueue>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCompactTransportChannel>();
    return tid;
}

UbCompactTransportChannel::UbCompactTransportChannel() = default;

UbCompactTransportChannel::~UbCompactTransportChannel() = default;

IngressQueueType
UbCompactTransportChannel::GetIngressQueueType()
{
    return IngressQueueType::TP;
}

bool
UbCompactTransportChannel::IsEmpty()
{
    return m_cnpQueue.empty() && m_ackQueue.empty() && m_dataQueue.empty();
}

Ptr<Packet>
UbCompactTransportChannel::GetNextPacket()
{
    std::queue<Ptr<Packet>>* queue = GetPriorityQueue();
    if (queue == nullptr)
    {
        return nullptr;
    }

    Ptr<Packet> packet = queue->front();
    queue->pop();
    if (!IsEmpty())
    {
        m_headArrivalTime = Simulator::Now();
    }
    return packet;
}

uint32_t
UbCompactTransportChannel::GetNextPacketSize()
{
    const std::queue<Ptr<Packet>>* queue = GetPriorityQueue();
    return queue == nullptr ? 0 : queue->front()->GetSize();
}

void
UbCompactTransportChannel::SetCtpEntity(Ptr<Node> node,
                                        const UbCtpEntityKey& key,
                                        uint32_t outPort,
                                        uint32_t dstPortHint,
                                        UbCtpEntityState* state)
{
    m_node = node;
    m_key = key;
    m_outPort = outPort;
    m_dstPortHint = dstPortHint;
    m_state = state;
    SetOutPortId(outPort);
    SetInPortId(outPort);
    SetIngressPriority(key.vl);
}

void
UbCompactTransportChannel::SetExperimentTaAckPacketBytes(uint32_t packetBytes)
{
    NS_ABORT_MSG_IF(packetBytes > UB_MTU_BYTE,
                    "ExperimentTaAckPacketBytes must not exceed UB_MTU_BYTE");
    m_experimentTaAckPacketBytes = packetBytes;
}

Ptr<Packet>
UbCompactTransportChannel::BuildDataPacket(Ptr<UbWqeSegment> segment,
                                           uint32_t taSsn,
                                           uint32_t progressBytes,
                                           uint8_t loadBalanceSalt) const
{
    NS_ABORT_MSG_IF(m_state == nullptr, "CTP compact channel requires entity state");
    return BuildCtpPacket(segment,
                          m_key,
                          m_dstPortHint,
                          taSsn,
                          progressBytes,
                          m_state->Tx().routing,
                          loadBalanceSalt);
}

Ptr<Packet>
UbCompactTransportChannel::BuildResponsePacket(Ptr<UbWqeSegment> response,
                                               uint32_t taSsn,
                                               uint32_t progressBytes,
                                               uint8_t loadBalanceSalt) const
{
    Ptr<Packet> packet = BuildDataPacket(response, taSsn, progressBytes, loadBalanceSalt);
    PadTaAckPacketForExperiment(packet, m_experimentTaAckPacketBytes);
    return packet;
}

void
UbCompactTransportChannel::EnqueueDataPacket(Ptr<UbWqeSegment> segment,
                                             uint32_t taSsn,
                                             uint32_t progressBytes,
                                             uint8_t loadBalanceSalt)
{
    EnqueueData(BuildDataPacket(segment, taSsn, progressBytes, loadBalanceSalt));
}

void
UbCompactTransportChannel::EnqueueResponsePacket(Ptr<UbWqeSegment> response,
                                                 uint32_t taSsn,
                                                 uint32_t progressBytes,
                                                 uint8_t loadBalanceSalt)
{
    NS_ABORT_MSG_IF(response == nullptr, "CTP response packet requires segment");
    Ptr<Packet> packet = BuildResponsePacket(response, taSsn, progressBytes, loadBalanceSalt);
    if (response->GetType() == TaOpcode::TA_OPCODE_TRANSACTION_ACK)
    {
        EnqueueAck(packet);
    }
    else
    {
        EnqueueData(packet);
    }
}

void
UbCompactTransportChannel::EnqueueData(Ptr<Packet> packet)
{
    EnqueueToClass(UbCtpQueueClass::DATA, packet);
}

void
UbCompactTransportChannel::EnqueueAck(Ptr<Packet> packet)
{
    EnqueueToClass(UbCtpQueueClass::ACK, packet);
}

void
UbCompactTransportChannel::EnqueueCnp(Ptr<Packet> packet)
{
    EnqueueToClass(UbCtpQueueClass::CNP, packet);
}

void
UbCompactTransportChannel::Enqueue(Ptr<Packet> packet)
{
    EnqueueData(packet);
}

Ptr<Packet>
UbCompactTransportChannel::PeekNextPacket() const
{
    const std::queue<Ptr<Packet>>* queue = GetPriorityQueue();
    return queue == nullptr ? nullptr : queue->front();
}

uint32_t
UbCompactTransportChannel::GetQueuedPacketCountForTest() const
{
    return static_cast<uint32_t>(m_dataQueue.size() + m_ackQueue.size() + m_cnpQueue.size());
}

std::queue<Ptr<Packet>>&
UbCompactTransportChannel::QueueForClass(UbCtpQueueClass queueClass)
{
    switch (queueClass)
    {
    case UbCtpQueueClass::CNP:
        return m_cnpQueue;
    case UbCtpQueueClass::ACK:
        return m_ackQueue;
    case UbCtpQueueClass::DATA:
    default:
        return m_dataQueue;
    }
}

const std::queue<Ptr<Packet>>&
UbCompactTransportChannel::QueueForClass(UbCtpQueueClass queueClass) const
{
    switch (queueClass)
    {
    case UbCtpQueueClass::CNP:
        return m_cnpQueue;
    case UbCtpQueueClass::ACK:
        return m_ackQueue;
    case UbCtpQueueClass::DATA:
    default:
        return m_dataQueue;
    }
}

std::queue<Ptr<Packet>>*
UbCompactTransportChannel::GetPriorityQueue()
{
    return const_cast<std::queue<Ptr<Packet>>*>(
        static_cast<const UbCompactTransportChannel*>(this)->GetPriorityQueue());
}

const std::queue<Ptr<Packet>>*
UbCompactTransportChannel::GetPriorityQueue() const
{
    if (!m_cnpQueue.empty())
    {
        return &m_cnpQueue;
    }
    if (!m_ackQueue.empty())
    {
        return &m_ackQueue;
    }
    if (!m_dataQueue.empty())
    {
        return &m_dataQueue;
    }
    return nullptr;
}

void
UbCompactTransportChannel::EnqueueToClass(UbCtpQueueClass queueClass, Ptr<Packet> packet)
{
    NS_ABORT_MSG_IF(packet == nullptr, "Cannot enqueue null CTP packet");
    if (IsEmpty())
    {
        m_headArrivalTime = Simulator::Now();
    }
    QueueForClass(queueClass).push(packet);
}

TypeId
UbCtpTransportService::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCtpTransportService")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCtpTransportService>()
                            .AddAttribute("BoundOutPortCount",
                                          "Maximum number of shortest-path outports that a CTP "
                                          "Entity/VL may bind. Zero means no explicit limit.",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_boundOutPortCount),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("ExperimentTaAckPacketBytes",
                                          "Temporary experiment hook: pad CTP TAACK packets to this "
                                          "total wire packet size in bytes when non-zero.",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_experimentTaAckPacketBytes),
                                          MakeUintegerChecker<uint32_t>(0, UB_MTU_BYTE))
                            .AddAttribute("WindowTraceEnabled",
                                          "Enable CTP transaction admission and TAACK window trace.",
                                          BooleanValue(false),
                                          MakeBooleanAccessor(
                                              &UbCtpTransportService::m_windowTraceEnabled),
                                          MakeBooleanChecker())
                            .AddAttribute("WindowTraceSrcNode",
                                          "Trace only this CTP source node. UINT32_MAX traces any source.",
                                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_windowTraceSrcNode),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("WindowTraceDstNode",
                                          "Trace only this CTP destination node. UINT32_MAX traces any destination.",
                                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_windowTraceDstNode),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DelayTaAckSrcNode",
                                          "Experiment hook: delay TAACK completion only for this CTP source node.",
                                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_delayTaAckSrcNode),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DelayTaAckDstNode",
                                          "Experiment hook: delay TAACK completion only for this CTP destination node.",
                                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_delayTaAckDstNode),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DelayTaAckSequence",
                                          "Experiment hook: delay this TAACK sequence. UINT32_MAX disables it.",
                                          UintegerValue(std::numeric_limits<uint32_t>::max()),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_delayTaAckSequence),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DelayTaAckModulo",
                                          "Experiment hook: delay TAACKs matching sequence modulo.",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_delayTaAckModulo),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DelayTaAckRemainder",
                                          "Experiment hook: modulo remainder for DelayTaAckModulo.",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(
                                              &UbCtpTransportService::m_delayTaAckRemainder),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("DelayTaAckTime",
                                          "Experiment hook: delay duration for matching TAACK.",
                                          TimeValue(NanoSeconds(0)),
                                          MakeTimeAccessor(
                                              &UbCtpTransportService::m_delayTaAckTime),
                                          MakeTimeChecker())
                            .AddTraceSource(
                                "FirstPacketSendsNotify",
                                "Fires when the first packet of a CTP TA unit is enqueued.",
                                MakeTraceSourceAccessor(
                                    &UbCtpTransportService::m_traceFirstPacketSendsNotify),
                                "ns3::UbCtpTransportService::FirstPacketSendsNotify")
                            .AddTraceSource(
                                "LastPacketACKsNotify",
                                "Fires when the last TAACK for a CTP TA unit is processed.",
                                MakeTraceSourceAccessor(
                                    &UbCtpTransportService::m_traceLastPacketACKsNotify),
                                "ns3::UbCtpTransportService::LastPacketACKsNotify")
                            .AddTraceSource(
                                "CtpRecvNotify",
                                "Fires when a CTP data or ACK packet is received with path trace.",
                                MakeTraceSourceAccessor(&UbCtpTransportService::m_ctpRecvNotify),
                                "ns3::UbCtpTransportService::CtpRecvNotify");
    return tid;
}

UbCtpTransportService::UbCtpTransportService()
{
    BooleanValue traceEnable(false);
    BooleanValue packetTraceEnable(false);
    const bool traceValueExists =
        GlobalValue::GetValueByNameFailSafe("UB_TRACE_ENABLE", traceEnable);
    const bool packetTraceValueExists =
        GlobalValue::GetValueByNameFailSafe("UB_PACKET_TRACE_ENABLE", packetTraceEnable);
    m_pktTraceEnabled =
        traceValueExists && packetTraceValueExists && traceEnable.Get() && packetTraceEnable.Get();
}

UbCtpTransportService::~UbCtpTransportService() = default;

void
UbCtpTransportService::SetNode(Ptr<Node> node)
{
    m_node = node;
}

Ptr<Node>
UbCtpTransportService::GetNode() const
{
    return m_node;
}

void
UbCtpTransportService::SetUseShortestPaths(bool useShortestPaths)
{
    m_defaultRoutingPolicy.useShortestPaths = useShortestPaths;
}

void
UbCtpTransportService::SetUsePacketSpray(bool usePacketSpray)
{
    m_defaultRoutingPolicy.usePacketSpray = usePacketSpray;
}

void
UbCtpTransportService::SetBoundOutPortCountForTest(uint32_t boundOutPortCount)
{
    m_boundOutPortCount = boundOutPortCount;
}

void
UbCtpTransportService::SetExperimentTaAckPacketBytesForTest(uint32_t packetBytes)
{
    NS_ABORT_MSG_IF(packetBytes > UB_MTU_BYTE,
                    "ExperimentTaAckPacketBytes must not exceed UB_MTU_BYTE");
    m_experimentTaAckPacketBytes = packetBytes;
}

void
UbCtpTransportService::SetSourcePortHint(const UbCtpEntityKey& key, uint32_t port)
{
    ValidateCna16PortHint(port, "source");
    m_srcPortHints[key] = port;
}

void
UbCtpTransportService::ClearSourcePortHint(const UbCtpEntityKey& key)
{
    m_srcPortHints.erase(key);
}

void
UbCtpTransportService::SetDestinationPortHint(const UbCtpEntityKey& key, uint32_t port)
{
    ValidateCna16PortHint(port, "destination");
    m_dstPortHints[key] = port;
}

void
UbCtpTransportService::ClearDestinationPortHint(const UbCtpEntityKey& key)
{
    m_dstPortHints.erase(key);
}

UbCtpEntityState&
UbCtpTransportService::GetOrCreateEntityState(const UbCtpEntityKey& key)
{
    auto [it, inserted] = m_entityStates.try_emplace(key, key);
    if (inserted)
    {
        it->second.Tx().routing = m_defaultRoutingPolicy;
    }
    if (it->second.Tx().context == nullptr)
    {
        it->second.Tx().context =
            Create<UbCtpTransactionContext>(key, UB_JETTY_TASSN_OOO_THRESHOLD);
    }
    return it->second;
}

void
UbCtpTransportService::ConfigureRoutingPolicy(const UbCtpEntityKey& key,
                                              bool useShortestPaths,
                                              bool usePacketSpray)
{
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    state.Tx().routing.useShortestPaths = useShortestPaths;
    state.Tx().routing.usePacketSpray = usePacketSpray;
}

uint32_t
UbCtpTransportService::GetEntityStateCount() const
{
    return static_cast<uint32_t>(m_entityStates.size());
}

Ptr<UbCtpTransactionContext>
UbCtpTransportService::GetOrCreateTransactionContext(const UbCtpEntityKey& key)
{
    return GetOrCreateEntityState(key).Tx().context;
}

bool
UbCtpTransportService::HasTransactionContextForTesting(const UbCtpEntityKey& key) const
{
    auto it = m_entityStates.find(key);
    return it != m_entityStates.end() && it->second.Tx().context != nullptr;
}

Ptr<UbCompactTransportChannel>
UbCtpTransportService::GetOrCreateQueue(const UbCtpEntityKey& key,
                                        uint32_t outPort,
                                        uint32_t priority)
{
    QueueKey queueKey = std::make_tuple(key, outPort, priority);
    auto it = m_queues.find(queueKey);
    if (it != m_queues.end())
    {
        return it->second;
    }

    Ptr<UbCompactTransportChannel> queue = CreateObject<UbCompactTransportChannel>();
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    queue->SetCtpEntity(m_node, key, outPort, ResolveDestinationPortHint(key), &state);
    queue->SetExperimentTaAckPacketBytes(m_experimentTaAckPacketBytes);
    m_queues.emplace(queueKey, queue);
    state.AddOutPort(outPort);
    return queue;
}

Ptr<Packet>
UbCtpTransportService::BuildDataPacket(Ptr<UbWqeSegment> segment, const UbCtpEntityKey& key)
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP packet requires WQE segment");

    const uint32_t taSsn = segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE
                               ? segment->GetRequestTassn()
                               : segment->GetTaSsn();
    const uint32_t progressBytes =
        segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE &&
                segment->GetType() == TaOpcode::TA_OPCODE_TRANSACTION_ACK
            ? 0
            : static_cast<uint32_t>(segment->GetBytesLeft());
    const UbCtpEntityKey packetKey = MakeTxKeyForSegment(key, segment);
    UbCtpEntityState& state = GetOrCreateEntityState(packetKey);
    return BuildDataPacketWithTaSsn(segment, state, packetKey, taSsn, progressBytes);
}

Ptr<Packet>
UbCtpTransportService::BuildDataPacketWithTaSsn(Ptr<UbWqeSegment> segment,
                                                UbCtpEntityState& state,
                                                const UbCtpEntityKey& packetKey,
                                                uint32_t taSsn,
                                                uint32_t progressBytes)
{
    const uint16_t lbSalt = NextLoadBalanceSalt(state, packetKey);
    Ptr<Packet> packet = BuildCtpPacket(segment,
                                        packetKey,
                                        ResolveDestinationPortHint(packetKey),
                                        taSsn,
                                        progressBytes,
                                        state.Tx().routing,
                                        static_cast<uint8_t>(lbSalt & 0xff));
    PadTaAckPacketForExperiment(packet);
    return packet;
}

void
UbCtpTransportService::PadTaAckPacketForExperiment(Ptr<Packet> packet) const
{
    ns3::PadTaAckPacketForExperiment(packet, m_experimentTaAckPacketBytes);
}

Ptr<Packet>
UbCtpTransportService::BuildResponsePacketForTest(Ptr<UbWqeSegment> response,
                                                  const UbCtpEntityKey& key)
{
    return BuildDataPacket(response, key);
}

void
UbCtpTransportService::AddCtpNetworkHeaders(Ptr<Packet> packet,
                                            const UbCtpEntityKey& key,
                                            CtpOpcode opcode,
                                            uint8_t loadBalanceSalt)
{
    AddCtpNetworkHeaders(packet,
                         key,
                         opcode,
                         loadBalanceSalt,
                         m_defaultRoutingPolicy.useShortestPaths,
                         m_defaultRoutingPolicy.usePacketSpray);
}

void
UbCtpTransportService::AddCtpNetworkHeaders(Ptr<Packet> packet,
                                            const UbCtpEntityKey& key,
                                            CtpOpcode opcode,
                                            uint8_t loadBalanceSalt,
                                            bool useShortestPaths,
                                            bool usePacketSpray)
{
    UbCtpRoutingPolicy routing{.useShortestPaths = useShortestPaths,
                               .usePacketSpray = usePacketSpray,
                               .nextSprayIndex = 0};
    Ptr<UbWqeSegment> empty = CreateObject<UbWqeSegment>();
    empty->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    empty->SetType(TaOpcode::TA_OPCODE_WRITE);
    empty->SetPayloadBytes(packet->GetSize());
    empty->SetCarrierBytes(packet->GetSize());
    Ptr<Packet> headers = BuildCtpPacket(empty,
                                         key,
                                         ResolveDestinationPortHint(key),
                                         0,
                                         packet->GetSize(),
                                         routing,
                                         loadBalanceSalt);
    (void)headers;
    NS_ABORT_MSG_IF(opcode != CtpOpcode::CTP_DATA,
                    "manual CTP header helper only supports CTP_DATA in the entity path");
}

bool
UbCtpTransportService::SendSegment(Ptr<UbWqeSegment> segment, const UbCtpEntityKey& key)
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP send requires WQE segment");

    UbCtpEntityState& state = GetOrCreateEntityState(key);
    Ptr<UbCtpTransactionContext> context = state.Tx().context;
    while (!segment->IsSentCompleted())
    {
        const uint32_t progressBytes = static_cast<uint32_t>(segment->GetBytesLeft());
        if (progressBytes == 0)
        {
            return true;
        }
        const uint32_t taSsn = segment->GetTaSsn();
        if (!context->TryAdmit(taSsn))
        {
            TraceWindowEvent(key, "admit-blocked", taSsn, *context);
            return false;
        }
        TraceWindowEvent(key, "admit", taSsn, *context);
        const UbCtpEntityKey packetKey = MakeTxKeyForSegment(key, segment);
        if (!SendAdmittedFragment(segment, state, packetKey, taSsn, progressBytes))
        {
            return false;
        }
    }
    return true;
}

void
UbCtpTransportService::PrepareJetty(Ptr<UbJetty> jetty)
{
    NS_ABORT_MSG_IF(jetty == nullptr, "CTP send requires jetty");
    jetty->SetTaSegmentBytes(UB_MTU_BYTE);
}

void
UbCtpTransportService::StartJetty(Ptr<UbJetty> jetty, const UbCtpEntityKey& key)
{
    PrepareJetty(jetty);

    auto& jettys = m_keyJettys[key];
    if (std::find(jettys.begin(), jettys.end(), jetty) == jettys.end())
    {
        jettys.push_back(jetty);
    }
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    state.Tx().routing.useShortestPaths = m_defaultRoutingPolicy.useShortestPaths;
    state.Tx().routing.usePacketSpray = m_defaultRoutingPolicy.usePacketSpray;
    DrainJettys(state);
}

UbCtpEntityKey
UbCtpTransportService::MakeSourceGroupKeyForRequest(const UbCtpEntityKey& key) const
{
    UbCtpEntityKey sourceKey = key;
    sourceKey.dstNodeId = 0;
    sourceKey.dstEntityId = 0;
    return sourceKey;
}

UbCtpEntityKey
UbCtpTransportService::MakeTxKeyForSegment(const UbCtpEntityKey& jettyKey,
                                           Ptr<UbWqeSegment> segment) const
{
    NS_ABORT_MSG_IF(segment == nullptr, "CTP segment key requires segment");
    UbCtpEntityKey txKey = jettyKey;
    txKey.dstNodeId = segment->GetDest();
    if (segment->HasSrcEntityId())
    {
        txKey.srcEntityId = segment->GetSrcEntityId();
    }
    if (segment->HasDstEntityId())
    {
        txKey.dstEntityId = segment->GetDstEntityId();
    }
    txKey.vl = static_cast<uint8_t>(segment->GetPriority());
    return txKey;
}

bool
UbCtpTransportService::SendAdmittedFragment(Ptr<UbWqeSegment> segment,
                                            UbCtpEntityState& state,
                                            const UbCtpEntityKey& packetKey,
                                            uint32_t taSsn,
                                            uint32_t progressBytes)
{
    if (state.Tx().outstandingSegments.find(taSsn) == state.Tx().outstandingSegments.end())
    {
        state.Tx().outstandingSegments[taSsn] = segment;
        ++state.Tx().segmentOutstandingPackets[segment];
    }

    const uint16_t lbSalt = NextLoadBalanceSalt(state, packetKey);
    const uint32_t outPort = SelectOutPort(state, packetKey, lbSalt);
    Ptr<UbCompactTransportChannel> queue = GetOrCreateQueue(packetKey, outPort, packetKey.vl);
    RegisterQueueIfNeeded(queue, outPort, packetKey.vl);
    queue->EnqueueDataPacket(segment, taSsn, progressBytes, static_cast<uint8_t>(lbSalt & 0xff));
    FirstPacketSendsNotify(segment->GetTaskId(),
                           packetKey,
                           taSsn,
                           outPort,
                           progressBytes,
                           segment->GetType());
    segment->UpdateSentBytes(progressBytes);
    TraceWindowEvent(packetKey, "data-enqueued", taSsn, *state.Tx().context, outPort);

    Ptr<UbPort> port = DynamicCast<UbPort>(m_node->GetDevice(outPort));
    NS_ABORT_MSG_IF(port == nullptr, "CTP send selected a non-UB source port");
    port->TriggerTransmit();
    return true;
}

bool
UbCtpTransportService::AdmitSegmentForTest(Ptr<UbWqeSegment> segment,
                                           const UbCtpEntityKey& key,
                                           uint32_t taSsn)
{
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    Ptr<UbCtpTransactionContext> context = state.Tx().context;
    if (!context->TryAdmit(taSsn))
    {
        return false;
    }
    state.Tx().outstandingSegments[taSsn] = segment;
    ++state.Tx().segmentOutstandingPackets[segment];
    return true;
}

bool
UbCtpTransportService::SendResponseSegmentForTest(Ptr<UbWqeSegment> response,
                                                  const UbCtpEntityKey& key)
{
    UbCtpEntityState& state = GetOrCreateEntityState(key);
    return SendResponseSegment(response, state);
}

void
UbCtpTransportService::DrainJettys(UbCtpEntityState& state)
{
    const UbCtpEntityKey& key = state.GetKey();
    auto jettysIt = m_keyJettys.find(key);
    if (jettysIt == m_keyJettys.end() || jettysIt->second.empty())
    {
        return;
    }

    auto& jettys = jettysIt->second;
    auto& pendingSegments = state.Tx().pendingSegments;
    uint32_t& rrIndex = m_keyJettyRrIndex[key];
    while (!jettys.empty())
    {
        if (pendingSegments.empty())
        {
            Ptr<UbWqeSegment> segment = nullptr;
            const uint32_t count = static_cast<uint32_t>(jettys.size());
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t idx = (rrIndex + i) % count;
                if (jettys[idx] == nullptr)
                {
                    continue;
                }
                segment = jettys[idx]->GetNextWqeSegment();
                if (segment != nullptr)
                {
                    rrIndex = (idx + 1) % count;
                    break;
                }
            }

            if (segment == nullptr)
            {
                return;
            }
            pendingSegments.push_back(segment);
        }

        Ptr<UbWqeSegment> segment = pendingSegments.front();
        if (segment == nullptr || segment->IsSentCompleted())
        {
            pendingSegments.pop_front();
            continue;
        }

        const UbCtpEntityKey packetKey = MakeTxKeyForSegment(key, segment);
        Ptr<UbCtpTransactionContext> context = state.Tx().context;
        const uint32_t taSsn = segment->GetTaSsn();
        if (!context->TryAdmit(taSsn))
        {
            TraceWindowEvent(packetKey, "drain-blocked", taSsn, *context);
            return;
        }
        TraceWindowEvent(packetKey, "admit", taSsn, *context);
        const uint32_t progressBytes = static_cast<uint32_t>(segment->GetBytesLeft());
        SendAdmittedFragment(segment, state, packetKey, taSsn, progressBytes);
        if (segment->IsSentCompleted())
        {
            pendingSegments.pop_front();
        }
    }
}

bool
UbCtpTransportService::HandleReceivedPacket(Ptr<Packet> packet)
{
    return HandleReceivedPacket(packet, std::numeric_limits<uint32_t>::max());
}

bool
UbCtpTransportService::HandleReceivedPacket(Ptr<Packet> packet, uint32_t receivePortHint)
{
    if (packet == nullptr)
    {
        return false;
    }

    Ptr<Packet> copy = packet->Copy();

    UbDatalinkPacketHeader linkHeader;
    if (copy->RemoveHeader(linkHeader) == 0 ||
        linkHeader.GetConfig() != static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16))
    {
        return false;
    }

    UbCna16NetworkHeader cnaHeader;
    if (copy->RemoveHeader(cnaHeader) == 0 || cnaHeader.GetNlp() != UB_CNA_NLP_CTPH)
    {
        return false;
    }

    UbCtpHeader ctpHeader;
    if (copy->RemoveHeader(ctpHeader) == 0)
    {
        return false;
    }

    uint32_t srcEntityId = 0;
    uint32_t dstEntityId = 0;
    if (ctpHeader.GetNlp() == UB_CTPH_NLP_UPI16_EID40_TAH)
    {
        UbCompactUpiHeader upiHeader;
        UbCompactEidHeader eidHeader;
        if (copy->RemoveHeader(upiHeader) == 0 || copy->RemoveHeader(eidHeader) == 0)
        {
            return false;
        }
        srcEntityId = eidHeader.GetSourceEid();
        dstEntityId = eidHeader.GetDestinationEid();
    }
    else if (ctpHeader.GetNlp() != UB_CTPH_NLP_COMPACT_TAH)
    {
        return false;
    }

    UbCtpEntityKey forwardKey{.srcNodeId = utils::Cna16ToNodeId(cnaHeader.GetScna()),
                              .srcEntityId = srcEntityId,
                              .dstNodeId = utils::Cna16ToNodeId(cnaHeader.GetDcna()),
                              .dstEntityId = dstEntityId,
                              .vl = cnaHeader.GetServiceLevel()};
    UbCtpEntityState& forwardState = GetOrCreateEntityState(forwardKey);
    ++forwardState.Rx().receivedPackets;

    if (ctpHeader.GetTPOpcode() == static_cast<uint8_t>(CtpOpcode::CTP_CNP))
    {
        RecordCnpForTest(forwardKey);
        return true;
    }

    if (ctpHeader.GetTPOpcode() != static_cast<uint8_t>(CtpOpcode::CTP_DATA))
    {
        return false;
    }

    if (copy->GetSize() >= UbCompactAckTransactionHeader().GetSerializedSize())
    {
        Ptr<Packet> ackCandidate = copy->Copy();
        UbCompactAckTransactionHeader ackHeader;
        if (ackCandidate->RemoveHeader(ackHeader) != 0 &&
            (ackHeader.GetTaOpcode() ==
                 static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK) ||
             ackHeader.GetTaOpcode() == static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ_RESPONSE) ||
             ackHeader.GetTaOpcode() == static_cast<uint8_t>(TaOpcode::TA_OPCODE_ATOMIC_RESPONSE)))
        {
            UbFlowTag flowTag;
            copy->PeekPacketTag(flowTag);
            UbPacketTraceTag traceTag;
            copy->PeekPacketTag(traceTag);
            CtpRecvNotify(packet->GetUid(),
                          forwardKey,
                          ackHeader.GetIniTaSsn(),
                          PacketType::ACK,
                          copy->GetSize(),
                          flowTag.GetFlowId(),
                          traceTag);

            UbCtpEntityKey reverseKey{.srcNodeId = forwardKey.dstNodeId,
                                      .srcEntityId = forwardKey.dstEntityId,
                                      .dstNodeId = forwardKey.srcNodeId,
                                      .dstEntityId = forwardKey.srcEntityId,
                                      .vl = forwardKey.vl};
            Ptr<UbWqeSegment> response = CreateObject<UbWqeSegment>();
            response->SetSrc(forwardKey.srcNodeId);
            response->SetDest(forwardKey.dstNodeId);
            response->SetPriority(forwardKey.vl);
            response->SetTaskId(flowTag.GetFlowId());
            response->SetWqeSize(flowTag.GetFlowSize());
            response->SetJettyNum(ackHeader.GetIniTaSsn());
            response->SetTaMsn(0);
            response->SetTaSsn(ackHeader.GetIniTaSsn());
            response->SetSegmentKind(UbTransactionSegmentKind::RESPONSE);
            response->SetOriginJettyNum(ackHeader.GetIniTaSsn());
            response->SetRequestTassn(ackHeader.GetIniTaSsn());
            response->SetType(static_cast<TaOpcode>(ackHeader.GetTaOpcode()));
            response->SetRequestOpcode(ackHeader.GetTaOpcode() ==
                                               static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK)
                                           ? TaOpcode::TA_OPCODE_WRITE
                                           : TaOpcode::TA_OPCODE_READ);
            response->SetResponseBytes(copy->GetSize());
            response->SetNeedsTransactionResponse(false);
            response->SetSize(ackHeader.GetTaOpcode() ==
                                      static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK)
                                  ? 1
                                  : copy->GetSize());
            response->SetLogicalBytes(copy->GetSize());
            response->SetPayloadBytes(copy->GetSize());
            response->SetCarrierBytes(ackHeader.GetTaOpcode() ==
                                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK)
                                          ? 1
                                          : copy->GetSize());
            CompleteFromTaAck(reverseKey, ackHeader.GetIniTaSsn(), response);
            return true;
        }
    }

    UbCompactTransactionHeader taHeader;
    if (copy->RemoveHeader(taHeader) == 0)
    {
        return false;
    }
    UbCompactMAExtTah maHeader;
    if (copy->RemoveHeader(maHeader) == 0)
    {
        return false;
    }

    const auto taOpcode = static_cast<TaOpcode>(taHeader.GetTaOpcode());
    if (taOpcode != TaOpcode::TA_OPCODE_WRITE && taOpcode != TaOpcode::TA_OPCODE_READ)
    {
        return false;
    }

    UbFlowTag flowTag;
    packet->PeekPacketTag(flowTag);
    UbPacketTraceTag traceTag;
    packet->PeekPacketTag(traceTag);
    CtpRecvNotify(packet->GetUid(),
                  forwardKey,
                  taHeader.GetIniTaSsn(),
                  PacketType::PACKET,
                  copy->GetSize(),
                  flowTag.GetFlowId(),
                  traceTag);

    uint32_t receivePort = receivePortHint;
    if (receivePort == std::numeric_limits<uint32_t>::max())
    {
        receivePort = utils::Cna16ToPortId(cnaHeader.GetDcna());
    }

    Ptr<UbWqeSegment> request = TrackInboundTaPacket(forwardState,
                                                     taHeader,
                                                     maHeader,
                                                     taOpcode,
                                                     forwardKey.srcNodeId,
                                                     forwardKey.dstNodeId,
                                                     forwardKey.vl,
                                                     copy->GetSize(),
                                                     flowTag.GetFlowId(),
                                                     flowTag.GetFlowSize());
    if (request == nullptr)
    {
        return true;
    }

    Ptr<UbWqeSegment> response = ProcessInboundTaRequest(request);
    if (response == nullptr)
    {
        return true;
    }
    ++forwardState.Rx().generatedResponses;

    UbCtpEntityKey responseKey{.srcNodeId = forwardKey.dstNodeId,
                               .srcEntityId = forwardKey.dstEntityId,
                               .dstNodeId = forwardKey.srcNodeId,
                               .dstEntityId = forwardKey.srcEntityId,
                               .vl = forwardKey.vl};
    if (!linkHeader.GetLoadBalanceMode() && receivePort < 15)
    {
        SetSourcePortHint(responseKey, receivePort);
    }
    UbCtpEntityState& responseState = GetOrCreateEntityState(responseKey);
    responseState.Tx().routing.useShortestPaths = linkHeader.GetRoutingPolicy();
    responseState.Tx().routing.usePacketSpray = linkHeader.GetLoadBalanceMode();
    return SendResponseSegment(response, responseState);
}

Ptr<UbWqeSegment>
UbCtpTransportService::ProcessInboundTaRequest(Ptr<UbWqeSegment> request) const
{
    if (request == nullptr)
    {
        return nullptr;
    }

    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    Ptr<UbTransaction> transaction = controller == nullptr ? nullptr : controller->GetUbTransaction();
    if (transaction == nullptr)
    {
        return nullptr;
    }

    return transaction->ProcessInboundTaRequest(request);
}

Ptr<UbWqeSegment>
UbCtpTransportService::TrackInboundTaPacket(UbCtpEntityState& state,
                                            const UbCompactTransactionHeader& taHeader,
                                            const UbCompactMAExtTah& maHeader,
                                            TaOpcode taOpcode,
                                            uint32_t srcNodeId,
                                            uint32_t dstNodeId,
                                            uint32_t priority,
                                            uint32_t payloadBytes,
                                            uint32_t flowId,
                                            uint32_t flowSize)
{
    auto& unit = state.Rx().inboundTaUnits[taHeader.GetIniTaSsn()];
    const bool isReadRequest = taOpcode == TaOpcode::TA_OPCODE_READ;
    const uint32_t logicalBytes =
        static_cast<uint32_t>(64u << static_cast<uint32_t>(maHeader.GetLength()));
    const uint32_t expectedPayloadBytes = isReadRequest ? 0 : payloadBytes;

    if (unit.segment == nullptr)
    {
        unit.segment = CreateObject<UbWqeSegment>();
        unit.segment->SetSrc(srcNodeId);
        unit.segment->SetDest(dstNodeId);
        unit.segment->SetPriority(priority);
        unit.segment->SetTaskId(flowId);
        unit.segment->SetWqeSize(flowSize);
        unit.segment->SetJettyNum(taHeader.GetIniTaSsn());
        unit.segment->SetTaMsn(0);
        unit.segment->SetTaSsn(taHeader.GetIniTaSsn());
        unit.segment->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
        unit.segment->SetOriginJettyNum(taHeader.GetIniTaSsn());
        unit.segment->SetRequestTassn(taHeader.GetIniTaSsn());
        unit.segment->SetType(taOpcode);
        unit.segment->SetRequestOpcode(taOpcode);
        unit.segment->SetResponseBytes(isReadRequest ? logicalBytes : 0);
        unit.segment->SetNeedsTransactionResponse(taOpcode == TaOpcode::TA_OPCODE_WRITE ||
                                                  isReadRequest);
        unit.segment->SetSize(isReadRequest ? logicalBytes : payloadBytes);
        unit.segment->SetLogicalBytes(isReadRequest ? logicalBytes : payloadBytes);
        unit.segment->SetPayloadBytes(0);
        unit.segment->SetCarrierBytes(isReadRequest ? 1 : payloadBytes);
        unit.expectedPayloadBytes = expectedPayloadBytes;
    }

    unit.bytesReceived += payloadBytes;
    unit.segment->SetPayloadBytes(unit.bytesReceived);
    if (unit.bytesReceived < unit.expectedPayloadBytes)
    {
        return nullptr;
    }

    Ptr<UbWqeSegment> completed = unit.segment;
    state.Rx().inboundTaUnits.erase(taHeader.GetIniTaSsn());
    return completed;
}

void
UbCtpTransportService::RegisterQueueForTest(Ptr<UbCompactTransportChannel> queue,
                                            uint32_t outPort,
                                            uint32_t priority)
{
    RegisterQueueIfNeeded(queue, outPort, priority);
}

void
UbCtpTransportService::CompleteFromTaAckForTest(const UbCtpEntityKey& key, uint32_t taSsn)
{
    Ptr<UbWqeSegment> response = CreateObject<UbWqeSegment>();
    response->SetSegmentKind(UbTransactionSegmentKind::RESPONSE);
    response->SetType(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    response->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
    response->SetRequestTassn(taSsn);
    response->SetTaSsn(static_cast<uint16_t>(taSsn));
    CompleteFromTaAck(key, taSsn, response);
}

void
UbCtpTransportService::RecordCnpForTest(const UbCtpEntityKey& key)
{
    UbCtpCongestionKey congestionKey{.srcNodeId = key.srcNodeId,
                                     .srcEntityId = key.srcEntityId,
                                     .dstNodeId = key.dstNodeId,
                                     .dstEntityId = key.dstEntityId,
                                     .vl = key.vl};
    ++m_congestionSignals[congestionKey];
}

uint32_t
UbCtpTransportService::GetQueueCount() const
{
    return static_cast<uint32_t>(m_queues.size());
}

uint32_t
UbCtpTransportService::GetCongestionStateCountForTest() const
{
    return static_cast<uint32_t>(m_congestionSignals.size());
}

uint32_t
UbCtpTransportService::SelectOutPort(UbCtpEntityState& state,
                                     const UbCtpEntityKey& routeKey,
                                     uint16_t loadBalanceSalt)
{
    NS_ABORT_MSG_IF(m_node == nullptr, "CTP send requires a source node");

    auto sourceHint = m_srcPortHints.find(routeKey);
    if (!state.Tx().routing.usePacketSpray && sourceHint != m_srcPortHints.end())
    {
        const uint32_t outPort = sourceHint->second;
        NS_ABORT_MSG_IF(outPort >= m_node->GetNDevices(),
                        "CTP source port hint is outside the node device range");
        NS_ABORT_MSG_IF(DynamicCast<UbPort>(m_node->GetDevice(outPort)) == nullptr,
                        "CTP source port hint must reference a UB port");
        return outPort;
    }

    Ptr<UbSwitch> sw = m_node->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr,
                    "CTP send requires a source port hint or a route to destination node");
    Ptr<UbRoutingProcess> routing = sw->GetRoutingProcess();
    NS_ABORT_MSG_IF(routing == nullptr,
                    "CTP send requires a source port hint or a route to destination node");

    const uint32_t dstPort = ResolveDestinationPortHint(routeKey);
    RoutingKey rtKey;
    rtKey.sip = utils::NodeIdToIp(routeKey.srcNodeId).Get();
    rtKey.dip = dstPort == UINT32_MAX ? utils::NodeIdToIp(routeKey.dstNodeId).Get()
                                      : utils::NodeIdToIp(routeKey.dstNodeId, dstPort).Get();
    rtKey.sport = loadBalanceSalt;
    rtKey.dport = dstPort == UINT32_MAX ? 0 : static_cast<uint16_t>(dstPort);
    rtKey.priority = routeKey.vl;
    rtKey.useShortestPath = state.Tx().routing.useShortestPaths;
    rtKey.usePacketSpray = state.Tx().routing.usePacketSpray;
    rtKey.hashIncludesTransportPorts = true;

    bool selectedShortestPath = false;
    const int outPort = routing->GetOutPort(rtKey, selectedShortestPath);
    NS_ABORT_MSG_IF(outPort < 0,
                    "CTP send requires a source port hint or a route to destination node");
    NS_ABORT_MSG_IF(static_cast<uint32_t>(outPort) >= m_node->GetNDevices(),
                    "CTP route selected an outPort outside the node device range");
    NS_ABORT_MSG_IF(DynamicCast<UbPort>(m_node->GetDevice(static_cast<uint32_t>(outPort))) ==
                        nullptr,
                    "CTP route selected a non-UB source port");
    return static_cast<uint32_t>(outPort);
}

uint16_t
UbCtpTransportService::NextLoadBalanceSalt(UbCtpEntityState& state, const UbCtpEntityKey& routeKey)
{
    if (!state.Tx().routing.usePacketSpray)
    {
        return EntityLoadBalanceSalt(routeKey);
    }
    return static_cast<uint16_t>(state.Tx().routing.nextSprayIndex++);
}

uint32_t
UbCtpTransportService::ResolveDestinationPortHint(const UbCtpEntityKey& key) const
{
    auto it = m_dstPortHints.find(key);
    if (it != m_dstPortHints.end())
    {
        return it->second;
    }

    UbCtpEntityKey sourceGroupKey = MakeSourceGroupKeyForRequest(key);
    it = m_dstPortHints.find(sourceGroupKey);
    if (it != m_dstPortHints.end())
    {
        return it->second;
    }
    return UINT32_MAX;
}

void
UbCtpTransportService::RegisterQueueIfNeeded(Ptr<UbCompactTransportChannel> queue,
                                             uint32_t outPort,
                                             uint32_t priority)
{
    NS_ABORT_MSG_IF(queue == nullptr, "CTP queue registration requires a queue");
    NS_ABORT_MSG_IF(m_node == nullptr, "CTP queue registration requires a source node");

    if (m_registeredQueues.find(queue) != m_registeredQueues.end())
    {
        return;
    }

    Ptr<UbSwitch> sw = m_node->GetObject<UbSwitch>();
    NS_ABORT_MSG_IF(sw == nullptr, "CTP queue registration requires UbSwitch");
    sw->RegisterTpWithAllocator(queue, outPort, priority);
    m_registeredQueues.insert(queue);
}

bool
UbCtpTransportService::SendResponseSegment(Ptr<UbWqeSegment> response, UbCtpEntityState& state)
{
    NS_ABORT_MSG_IF(response == nullptr, "CTP response send requires WQE segment");

    const UbCtpEntityKey& key = state.GetKey();
    uint32_t taSsn = response->GetRequestTassn();
    if (response->GetType() == TaOpcode::TA_OPCODE_TRANSACTION_ACK)
    {
        const uint16_t lbSalt = NextLoadBalanceSalt(state, key);
        const uint32_t outPort = SelectOutPort(state, key, lbSalt);
        Ptr<UbCompactTransportChannel> queue = GetOrCreateQueue(key, outPort, key.vl);
        RegisterQueueIfNeeded(queue, outPort, key.vl);
        queue->EnqueueResponsePacket(response, taSsn, 0, static_cast<uint8_t>(lbSalt & 0xff));

        Ptr<UbPort> port = DynamicCast<UbPort>(m_node->GetDevice(outPort));
        NS_ABORT_MSG_IF(port == nullptr, "CTP response send requires valid out port");
        port->TriggerTransmit();
        return true;
    }

    while (!response->IsSentCompleted())
    {
        const uint32_t progressBytes = static_cast<uint32_t>(response->GetBytesLeft());
        const uint16_t lbSalt = NextLoadBalanceSalt(state, key);
        const uint32_t outPort = SelectOutPort(state, key, lbSalt);
        Ptr<UbCompactTransportChannel> queue = GetOrCreateQueue(key, outPort, key.vl);
        RegisterQueueIfNeeded(queue, outPort, key.vl);
        queue->EnqueueResponsePacket(response,
                                     taSsn,
                                     progressBytes,
                                     static_cast<uint8_t>(lbSalt & 0xff));
        response->UpdateSentBytes(progressBytes);

        Ptr<UbPort> port = DynamicCast<UbPort>(m_node->GetDevice(outPort));
        NS_ABORT_MSG_IF(port == nullptr, "CTP response send requires valid out port");
        port->TriggerTransmit();
        ++taSsn;
    }
    return true;
}

void
UbCtpTransportService::CompleteFromTaAck(const UbCtpEntityKey& key,
                                         uint32_t taSsn,
                                         Ptr<UbWqeSegment> response)
{
    if (ShouldDelayTaAck(key, taSsn))
    {
        UbCtpEntityState& state = GetOrCreateEntityState(key);
        TraceWindowEvent(key, "taack-delay", taSsn, *state.Tx().context);
        Simulator::Schedule(m_delayTaAckTime,
                            &UbCtpTransportService::CompleteFromTaAckNow,
                            this,
                            key,
                            taSsn,
                            response);
        return;
    }
    CompleteFromTaAckNow(key, taSsn, response);
}

void
UbCtpTransportService::CompleteFromTaAckNow(const UbCtpEntityKey& key,
                                            uint32_t taSsn,
                                            Ptr<UbWqeSegment> response)
{
    UbCtpEntityState* state = &GetOrCreateEntityState(key);
    taSsn = ResolveWireTaSsn(*state, static_cast<uint16_t>(taSsn));
    auto outstandingIt = FindOutstandingSegment(*state, taSsn);
    const UbCtpEntityKey sourceGroupKey = MakeSourceGroupKeyForRequest(key);
    const bool keyIsSourceGroup = sourceGroupKey == key;
    if (outstandingIt == state->Tx().outstandingSegments.end() && !keyIsSourceGroup)
    {
        auto sourceGroupIt = m_entityStates.find(sourceGroupKey);
        if (sourceGroupIt != m_entityStates.end())
        {
            const uint32_t sourceGroupTaSsn =
                ResolveWireTaSsn(sourceGroupIt->second, static_cast<uint16_t>(taSsn));
            auto sourceGroupOutstandingIt =
                FindOutstandingSegment(sourceGroupIt->second, sourceGroupTaSsn);
            if (sourceGroupOutstandingIt != sourceGroupIt->second.Tx().outstandingSegments.end())
            {
                state = &sourceGroupIt->second;
                taSsn = sourceGroupTaSsn;
                outstandingIt = sourceGroupOutstandingIt;
            }
        }
    }

    Ptr<UbCtpTransactionContext> context = state->Tx().context;
    const uint32_t oldCompleteUna = context->GetCompleteUna();
    if (outstandingIt == state->Tx().outstandingSegments.end())
    {
        context->MarkTaAck(taSsn);
        return;
    }

    context->MarkTaAck(taSsn);
    Ptr<UbWqeSegment> completedSegment = outstandingIt->second;
    LastPacketACKsNotify(completedSegment->GetTaskId(),
                         state->GetKey(),
                         taSsn,
                         completedSegment->GetSize(),
                         completedSegment->GetType());
    state->Tx().outstandingSegments.erase(outstandingIt);
    auto segmentCountIt = state->Tx().segmentOutstandingPackets.find(completedSegment);
    if (segmentCountIt != state->Tx().segmentOutstandingPackets.end())
    {
        if (segmentCountIt->second > 1)
        {
            --segmentCountIt->second;
        }
        else
        {
            state->Tx().segmentOutstandingPackets.erase(segmentCountIt);
        }
    }
    Ptr<UbController> controller = m_node == nullptr ? nullptr : m_node->GetObject<UbController>();
    Ptr<UbTransaction> transaction = controller == nullptr ? nullptr : controller->GetUbTransaction();
    const bool segmentComplete = completedSegment->IsSentCompleted() &&
                                 state->Tx().segmentOutstandingPackets.find(completedSegment) ==
                                     state->Tx().segmentOutstandingPackets.end();
    if (transaction != nullptr && segmentComplete)
    {
        Ptr<UbWqeSegment> completion = response == nullptr ? completedSegment : response;
        completion->SetOriginJettyNum(completedSegment->GetOriginJettyNum());
        completion->SetTaSsn(completedSegment->GetTaSsn());
        completion->SetRequestTassn(completedSegment->GetTaSsn());
        completion->SetRequestOpcode(completedSegment->GetType());
        transaction->ProcessInboundTaResponse(completion);
    }
    if (context->GetCompleteUna() != oldCompleteUna)
    {
        DrainJettys(*state);
    }
}

std::map<uint32_t, Ptr<UbWqeSegment>>::iterator
UbCtpTransportService::FindOutstandingSegment(UbCtpEntityState& state, uint32_t taSsn)
{
    return state.Tx().outstandingSegments.find(taSsn);
}

uint32_t
UbCtpTransportService::ResolveWireTaSsn(UbCtpEntityState& state, uint16_t wireTaSsn) const
{
    const auto& outstandingSegments = state.Tx().outstandingSegments;
    const uint32_t completeUna = state.Tx().context->GetCompleteUna();
    const uint32_t sendNext = state.Tx().context->GetSendNext();
    const uint32_t base = completeUna & ~0xffffu;
    std::vector<uint32_t> candidates;
    candidates.push_back(base | wireTaSsn);
    if (base >= 0x10000u)
    {
        candidates.push_back((base - 0x10000u) | wireTaSsn);
    }
    if (base <= std::numeric_limits<uint32_t>::max() - 0x10000u)
    {
        candidates.push_back((base + 0x10000u) | wireTaSsn);
    }

    for (uint32_t candidate : candidates)
    {
        if (candidate >= completeUna && candidate < sendNext &&
            outstandingSegments.find(candidate) != outstandingSegments.end())
        {
            return candidate;
        }
    }
    for (uint32_t candidate : candidates)
    {
        if (candidate >= completeUna && candidate < sendNext)
        {
            return candidate;
        }
    }
    return wireTaSsn;
}

uint32_t
UbCtpTransportService::GetQueuedPacketCountForTest(const UbCtpEntityKey& key) const
{
    uint32_t count = 0;
    for (const auto& [queueKey, queue] : m_queues)
    {
        if (std::get<0>(queueKey) == key && queue != nullptr)
        {
            count += queue->GetQueuedPacketCountForTest();
        }
    }
    return count;
}

uint32_t
UbCtpTransportService::GetQueuedPacketCountForTest(const UbCtpEntityKey& key,
                                                   uint32_t outPort,
                                                   uint32_t priority) const
{
    QueueKey queueKey = std::make_tuple(key, outPort, priority);
    auto it = m_queues.find(queueKey);
    if (it == m_queues.end() || it->second == nullptr)
    {
        return 0;
    }
    return it->second->GetQueuedPacketCountForTest();
}

uint32_t
UbCtpTransportService::GetRegisteredQueueCountForTest() const
{
    return static_cast<uint32_t>(m_registeredQueues.size());
}

bool
UbCtpTransportService::IsPacketTraceEnabledForTest() const
{
    return m_pktTraceEnabled;
}

void
UbCtpTransportService::FirstPacketSendsNotify(uint32_t taskId,
                                              const UbCtpEntityKey& key,
                                              uint32_t taSsn,
                                              uint32_t outPort,
                                              uint32_t payloadBytes,
                                              TaOpcode opcode)
{
    m_traceFirstPacketSendsNotify(m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                                  taskId,
                                  key.srcNodeId,
                                  key.dstNodeId,
                                  key.srcEntityId,
                                  key.dstEntityId,
                                  static_cast<uint32_t>(key.vl),
                                  taSsn,
                                  outPort,
                                  payloadBytes,
                                  static_cast<uint32_t>(opcode));
}

void
UbCtpTransportService::LastPacketACKsNotify(uint32_t taskId,
                                            const UbCtpEntityKey& key,
                                            uint32_t taSsn,
                                            uint32_t payloadBytes,
                                            TaOpcode opcode)
{
    m_traceLastPacketACKsNotify(m_node == nullptr ? UINT32_MAX : m_node->GetId(),
                                taskId,
                                key.srcNodeId,
                                key.dstNodeId,
                                key.srcEntityId,
                                key.dstEntityId,
                                static_cast<uint32_t>(key.vl),
                                taSsn,
                                UINT32_MAX,
                                payloadBytes,
                                static_cast<uint32_t>(opcode));
}

void
UbCtpTransportService::CtpRecvNotify(uint32_t packetUid,
                                     const UbCtpEntityKey& key,
                                     uint32_t taSsn,
                                     PacketType type,
                                     uint32_t size,
                                     uint32_t taskId,
                                     UbPacketTraceTag traceTag)
{
    m_ctpRecvNotify(packetUid,
                    key.srcNodeId,
                    key.dstNodeId,
                    key.srcEntityId,
                    key.dstEntityId,
                    static_cast<uint32_t>(key.vl),
                    taSsn,
                    type,
                    size,
                    taskId,
                    traceTag);
}

bool
UbCtpTransportService::ShouldTraceWindow(const UbCtpEntityKey& key) const
{
    if (!m_windowTraceEnabled)
    {
        return false;
    }
    if (m_windowTraceSrcNode != std::numeric_limits<uint32_t>::max() &&
        key.srcNodeId != m_windowTraceSrcNode)
    {
        return false;
    }
    if (m_windowTraceDstNode != std::numeric_limits<uint32_t>::max() &&
        key.dstNodeId != m_windowTraceDstNode)
    {
        return false;
    }
    return m_node != nullptr;
}

bool
UbCtpTransportService::ShouldDelayTaAck(const UbCtpEntityKey& key, uint32_t taSsn) const
{
    if (m_delayTaAckSrcNode == std::numeric_limits<uint32_t>::max() ||
        m_delayTaAckTime <= NanoSeconds(0))
    {
        return false;
    }
    const bool exactMatch =
        m_delayTaAckSequence != std::numeric_limits<uint32_t>::max() &&
        taSsn == m_delayTaAckSequence;
    const bool moduloMatch =
        m_delayTaAckModulo != 0 &&
        taSsn % m_delayTaAckModulo == m_delayTaAckRemainder % m_delayTaAckModulo;
    if (!exactMatch && !moduloMatch)
    {
        return false;
    }
    if (key.srcNodeId != m_delayTaAckSrcNode)
    {
        return false;
    }
    if (m_delayTaAckDstNode != std::numeric_limits<uint32_t>::max() &&
        key.dstNodeId != m_delayTaAckDstNode)
    {
        return false;
    }
    return true;
}

void
UbCtpTransportService::TraceWindowEvent(const UbCtpEntityKey& key,
                                        const std::string& event,
                                        uint32_t taSsn,
                                        const UbCtpTransactionContext& context,
                                        uint32_t outPort) const
{
    if (!ShouldTraceWindow(key))
    {
        return;
    }

    std::ostringstream oss;
    oss << "CTP WINDOW"
        << " event: " << event
        << " src: " << key.srcNodeId
        << " srcEntity: " << key.srcEntityId
        << " dst: " << key.dstNodeId
        << " dstEntity: " << key.dstEntityId
        << " vl: " << static_cast<uint32_t>(key.vl)
        << " taSsn: " << taSsn
        << " sendNext: " << context.GetSendNext()
        << " completeUna: " << context.GetCompleteUna()
        << " outstanding: " << context.GetOutstandingCount();
    if (outPort != std::numeric_limits<uint32_t>::max())
    {
        oss << " outPort: " << outPort;
    }
    NS_LOG_INFO(oss.str());
}

void
UbCtpTransportService::TraceDetailEvent(const UbCtpEntityKey& key,
                                        const std::string& event,
                                        uint32_t taSsn,
                                        uint32_t bytes,
                                        uint32_t expectedBytes,
                                        uint32_t flowBytes,
                                        uint32_t flowSize,
                                        uint32_t outPort) const
{
    if (!ShouldTraceWindow(key))
    {
        return;
    }

    std::ostringstream oss;
    oss << "CTP DETAIL"
        << " event: " << event
        << " src: " << key.srcNodeId
        << " srcEntity: " << key.srcEntityId
        << " dst: " << key.dstNodeId
        << " dstEntity: " << key.dstEntityId
        << " vl: " << static_cast<uint32_t>(key.vl)
        << " taSsn: " << taSsn;
    if (bytes != UINT32_MAX)
    {
        oss << " bytes: " << bytes;
    }
    if (expectedBytes != UINT32_MAX)
    {
        oss << " expectedBytes: " << expectedBytes;
    }
    if (flowBytes != UINT32_MAX)
    {
        oss << " flowBytes: " << flowBytes;
    }
    if (flowSize != UINT32_MAX)
    {
        oss << " flowSize: " << flowSize;
    }
    if (outPort != UINT32_MAX)
    {
        oss << " outPort: " << outPort;
    }
    NS_LOG_INFO(oss.str());
}

} // namespace ns3
