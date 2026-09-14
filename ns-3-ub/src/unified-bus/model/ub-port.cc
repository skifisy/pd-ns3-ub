// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-port.h"
#include "ns3/ub-link.h"
#include "ns3/log.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-utils.h"
#ifdef NS3_MPI
#include "ns3/mpi-receiver.h"
#endif
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbPort);
NS_LOG_COMPONENT_DEFINE("UbPort");

constexpr long DEFAULT_PFC_UP_THLD = 819200;   // 800KB ≈ 78% of 1MB per-queue reserve
constexpr long DEFAULT_PFC_LOW_THLD = 655360;  // 640KB ≈ 80% of XOFF threshold

/*********************
 * UbEgressQueue
 ********************/

// UbEgressQueue
TypeId UbEgressQueue::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbEgressQueue")
            .SetParent<Object>()
            .AddTraceSource(
            "UbEnqueue",
            "Enqueue a packet in the UbEgressQueue.",
            MakeTraceSourceAccessor(&UbEgressQueue::m_traceUbEnqueue),
            "ns3::UbEgressQueue::UbEnqueue")
            .AddTraceSource(
            "UbDequeue",
            "Dequeue a packet in the UbEgressQueue.",
            MakeTraceSourceAccessor(&UbEgressQueue::m_traceUbDequeue),
            "ns3::UbEgressQueue::UbDequeue")
            .AddAttribute(
            "MaxEgressBytes",
            "The maximum number of bytes accepted by this egress queue.",
            UintegerValue(512 * 1024),  // 512KB default (about 128 max-size packets of 4096 bytes)
            MakeUintegerAccessor(&UbEgressQueue::m_maxEgressBytes),
            MakeUintegerChecker<uint64_t>());
    return tid;
}

UbEgressQueue::UbEgressQueue()
{
}

bool UbEgressQueue::DoEnqueue(PacketEntry packetEntry)
{
    NS_LOG_FUNCTION (this);
    
    auto [inPortId, priority, pkt] = packetEntry;
    uint32_t pktSize = pkt->GetSize();

    // Check byte limit
    if (m_currentBytes + pktSize > m_maxEgressBytes) {
        UbUtils::RecordRuntimePacketDrop("egress queue buffer full");
        NS_LOG_WARN ("Buffer full, packet dropped: " 
                     << (m_currentBytes + pktSize) << " bytes > limit " << m_maxEgressBytes 
                     << " bytes. Packet dropped (inPort=" << inPortId 
                     << " priority=" << (uint32_t)priority << ")");
        return false;
    }
    
    m_egressQ.push(packetEntry);
    m_currentBytes += pktSize;  // 更新字节统计
    m_traceUbEnqueue(pkt, static_cast<uint32_t>(m_currentBytes));

    NS_LOG_LOGIC ("[UbEgressQueue DoEnqueue] Egress Queue size: " << m_egressQ.size () 
                  << " bytes: " << m_currentBytes << " (+" << pktSize << ")");

    return true;
}

bool UbEgressQueue::CanEnqueue(uint32_t packetBytes) const
{
    return m_currentBytes + packetBytes <= m_maxEgressBytes;
}

bool
UbPort::EnqueueToEgress(PacketEntry packetEntry)
{
    auto [inPortId, priority, packet] = packetEntry;
    (void)inPortId;
    (void)priority;
    const uint32_t pktSize = packet->GetSize();

    const bool enqueueOk = m_ubEQ->DoEnqueue(packetEntry);
    if (!enqueueOk)
    {
        return false;
    }

    Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
    if (ubSwitch != nullptr)
    {
        ubSwitch->GetQueueManager()->AddEgressBufferedBytes(pktSize);
    }
    return true;
}

PacketEntry UbEgressQueue::Peekqueue()
{
    NS_LOG_FUNCTION (this);
    if (m_egressQ.empty()) {
        NS_LOG_LOGIC ("Queue empty");
        return std::make_tuple(0, 0, nullptr);
    }
    auto [inPortId, priority, pkt] = m_egressQ.front();
    return std::make_tuple(inPortId, priority, pkt);
}

PacketEntry UbEgressQueue::DoDequeue()
{
    NS_LOG_FUNCTION (this);

    if (m_egressQ.empty()) {
        NS_LOG_LOGIC ("Queue empty");
        return std::make_tuple(0, 0, nullptr);
    }

    auto packetEntry = m_egressQ.front ();
    m_egressQ.pop();
    
    auto [inPortId, priority, pkt] = packetEntry;
    uint32_t pktSize = pkt->GetSize();
    m_currentBytes -= pktSize;  // 更新字节统计
    m_traceUbDequeue(pkt, static_cast<uint32_t>(m_currentBytes));

    NS_LOG_LOGIC ("[UbEgressQueue DoDequeue] Egress Queue size: " << m_egressQ.size ()
                  << " bytes: " << m_currentBytes << " (-" << pktSize << ")");

    return packetEntry;
}

bool UbEgressQueue::IsEmpty()
{
    return m_egressQ.empty();
}

/******************
 * UbPort
 *****************/
std::unordered_map<UbNodeType_t, std::string>
UbPort::g_node_type_map = {{UB_SWITCH, "SWITCH"},
                           {UB_DEVICE, "HOST"}};

TypeId UbPort::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbPort")
            .SetParent<PointToPointNetDevice>()
            .AddConstructor<UbPort>()
            .AddAttribute("UbDataRate",
                          "Port data rate for Unified Bus link transmission.",
                          DataRateValue(DataRate ("400Gbps")),
                          MakeDataRateAccessor(&UbPort::m_bps),
                          MakeDataRateChecker())
            .AddAttribute("UbInterframeGap",
                          "Minimum idle time inserted between consecutive packet transmissions.",
                          TimeValue(Seconds (0.0)),
                          MakeTimeAccessor(&UbPort::m_tInterframeGap),
                          MakeTimeChecker())
            .AddAttribute("CbfcFlitLenByte",
                          "Flit size in bytes; the fixed-size data link layer transfer unit (Spec §1.6: 20 bytes).",
                          UintegerValue(20),
                          MakeUintegerAccessor(&UbPort::m_cbfcFlitLen),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcFlitsPerCell",
                          "Number of flits per credit cell (Spec FLOW_CTRL_SIZE). "
                          "Valid: {1,2,4,8,16,32,64,128}; spec default 8 = 160 bytes/cell.",
                          UintegerValue(8),
                          MakeUintegerAccessor(&UbPort::m_cbfcFlitsPerCell),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcRetCellGrainDataPacket",
                          "Credit return granularity in DLLDP for data packets (Spec DATA_CREDIT_GRAIN_SIZE). "
                          "Valid: {1,2,4,8,16,32,64,128} cells; spec default 4.",
                          UintegerValue(4),
                          MakeUintegerAccessor(&UbPort::m_cbfcRetCellGrainDataPacket),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcRetCellGrainControlPacket",
                          "Credit return granularity in Crd_Ack Block for control packets "
                          "(Spec CTRL_CREDIT_GRAIN_SIZE). Valid: {1,2,4,8,16,32,64,128} cells; "
                          "repo default 32, spec default 1.",
                          UintegerValue(32),
                          MakeUintegerAccessor(&UbPort::m_cbfcRetCellGrainControlPacket),
                          MakeUintegerChecker<uint8_t>())
            .AddAttribute("CbfcInitCreditCell",
                          "Initial CBFC transmit credit in cells. In exclusive mode this equals "
                          "floor(receive_buffer / cell_size); spec example: 1MB / 160B = 6553. "
                          "In shared mode this is the per-VL reserved minimum and should be reduced.",
                          IntegerValue(6553),
                          MakeIntegerAccessor(&UbPort::m_cbfcPortTxfree),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("CbfcSharedInitCreditCell",
                          "Shared credit pool size in cells for CBFC shared-credit mode. "
                          "reserved + shared must not exceed per-VL buffer capacity in cells.",
                          IntegerValue(4096),
                          MakeIntegerAccessor(&UbPort::m_cbfcSharedInitCells),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("CbfcCtrlCrdRtrThldCell",
                          "Per-VL pending returned-credit threshold in cells. Once a VL reaches this threshold, "
                          "CBFC switches to control-frame credit return instead of waiting for piggyback opportunities. "
                          "Repo default 1024 cells.",
                          IntegerValue(1024),
                          MakeIntegerAccessor(&UbPort::m_cbfcCtrlCrdRtrThldCells),
                          MakeIntegerChecker<int32_t>(1))
            .AddAttribute("PfcUpThld",
                          "PFC XOFF threshold in bytes; a PAUSE frame is sent when the receive queue exceeds this level.",
                          IntegerValue(DEFAULT_PFC_UP_THLD),
                          MakeIntegerAccessor(&UbPort::m_pfcUpThld),
                          MakeIntegerChecker<int32_t>())
            .AddAttribute("PfcLowThld",
                          "PFC XON threshold in bytes; the PAUSE is released when the receive queue drains below this level.",
                          IntegerValue(DEFAULT_PFC_LOW_THLD),
                          MakeIntegerAccessor(&UbPort::m_pfcLowThld),
                          MakeIntegerChecker<int32_t>())
            .AddTraceSource("PortTxNotify",
                            "Port Tx",
                            MakeTraceSourceAccessor(&UbPort::m_tracePortTxNotify),
                            "ns3::UbPort::PortTxNotify")
            .AddTraceSource("PortRxNotify",
                            "Port Rx",
                            MakeTraceSourceAccessor(&UbPort::m_tracePortRxNotify),
                            "ns3::UbPort::PortRxNotify")
            .AddTraceSource("PktRcvNotify",
                            "Notify after receiving the data packet.",
                            MakeTraceSourceAccessor(&UbPort::m_tracePktRcvNotify),
                            "ns3::UbPort::PktRcvNotify")
            .AddTraceSource("TraComEventNotify",
                            "Transmit complete event.",
                            MakeTraceSourceAccessor(&UbPort::m_traceTraComEventNotify),
                            "ns3::UbPort::TraComEventNotify");
    return tid;
}

UbPort::UbPort()
{
    NS_LOG_FUNCTION(this);
    m_ubEQ = CreateObject<UbEgressQueue>();
    m_sendState = SendState::READY;
    m_txBytes = 0;
    m_mpiReceiveEnabled = false;
    BooleanValue val;
    if (GlobalValue::GetValueByNameFailSafe("UB_RECORD_PKT_TRACE", val)) {
        GlobalValue::GetValueByName("UB_RECORD_PKT_TRACE", val);
        m_pktTraceEnabled = val.Get();
    } else {
        m_pktTraceEnabled = false;
    }
}

UbPort::~UbPort()
{
    NS_LOG_FUNCTION(this);
}

void
UbPort::EnableMpiReceive()
{
#ifdef NS3_MPI
    if (m_mpiReceiveEnabled)
    {
        return;
    }

    Ptr<MpiReceiver> mpiReceiver = GetObject<MpiReceiver>();
    if (mpiReceiver == nullptr)
    {
        mpiReceiver = CreateObject<MpiReceiver>();
        AggregateObject(mpiReceiver);
    }

    mpiReceiver->SetReceiveCallback(MakeCallback(&UbPort::Receive, this));
    m_mpiReceiveEnabled = true;
#else
    NS_LOG_WARN("EnableMpiReceive called without MPI support");
#endif
}

bool
UbPort::HasMpiReceive() const
{
    return m_mpiReceiveEnabled;
}

void UbPort::SetIfIndex(const uint32_t portId)
{
    m_portId = portId;
}

uint32_t UbPort::GetIfIndex() const
{
    return m_portId;
}

void UbPort::SetSendState(SendState state)
{
    m_sendState = state;
}

void UbPort::CreateAndInitFc(FcType type)
{
    switch (type) {
        case FcType::CBFC:
            m_flowControl = CreateObject<UbCbfc>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbCbfc object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbCbfc>(m_flowControl);
                flowControl->Init(m_cbfcFlitLen, m_cbfcFlitsPerCell, m_cbfcRetCellGrainDataPacket,
                    m_cbfcRetCellGrainControlPacket, m_cbfcPortTxfree, m_cbfcCtrlCrdRtrThldCells,
                    GetNode()->GetId(), m_portId);
                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl Cbfc Init");
            }
            break;
        case FcType::CBFC_SHARED:
            m_flowControl = CreateObject<UbCbfcSharedCredit>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbCbfcSharedCredit object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbCbfcSharedCredit>(m_flowControl);
                const int32_t reservedPerVlCells = m_cbfcPortTxfree;
                const int32_t sharedInitCells = m_cbfcSharedInitCells;

                flowControl->Init(m_cbfcFlitLen, m_cbfcFlitsPerCell,
                                m_cbfcRetCellGrainDataPacket, m_cbfcRetCellGrainControlPacket,
                                reservedPerVlCells, m_cbfcCtrlCrdRtrThldCells, sharedInitCells,
                                GetNode()->GetId(), m_portId);

                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl CbfcSharedMode Init");
            }
            break;
        case FcType::PFC_FIXED:
        case FcType::PFC_DYNAMIC:
        case FcType::PFC_DYNAMIC_PAPER:
            m_flowControl = CreateObject<UbPfc>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbPfc object for port " << m_portId);
            } else {
                auto flowControl = DynamicCast<UbPfc>(m_flowControl);
                flowControl->Init(type, m_pfcUpThld, m_pfcLowThld, GetNode()->GetId(), m_portId);
                IntegerValue val;
                g_ub_vl_num.GetValue(val);
                int ubVlNum = val.Get();
                m_revQueueSize.resize(ubVlNum, 0);
                for (int pri = 0; pri < ubVlNum; ++pri) {
                    // PFC starts from the link-up/resumed state. Keeping the local port-side
                    // credit image explicit avoids empty-queue/xoff==0 cases looking paused.
                    m_credits[pri] = UB_CREDIT_MAX_VALUE;
                }
                NS_LOG_DEBUG("[UbPort CreateAndInitFc] flowControl Pfc Init mode=" << static_cast<int>(type));
            }
            break;
        case FcType::NONE:
            // No flow control: base class with no-op implementation
            m_flowControl = CreateObject<UbFlowControl>();
            if (m_flowControl == nullptr) {
                NS_FATAL_ERROR("Failed to create UbFlowControl object for port " << m_portId);
            }
            NS_LOG_DEBUG("[UbPort CreateAndInitFc] Default flow control (no-op) Init");
            break;
        default:
            NS_FATAL_ERROR("Unknown flow control type for port " << m_portId);
    }
}

void UbPort::TransmitComplete()
{
    NS_LOG_FUNCTION(this);
    
    NS_ASSERT_MSG(m_currentPkt, "UbPort::TransmitComplete(): m_currentPkt zero");

    NS_LOG_DEBUG("[UbPort TransmitComplete] complete at: "
        << " NodeId: " << GetNode()->GetId()
        << " PortId: " << GetIfIndex()
        << " PacketUid: " << m_currentPkt->GetUid());

    m_sendState = SendState::READY;
    m_currentPkt = nullptr;
    m_currentInPortId = 0;
    m_currentPriority = 0;
    Simulator::ScheduleNow(&UbPort::TriggerTransmit, this);
}

void UbPort::DequeuePacket(void)
{
    NS_ASSERT_MSG(!m_ubEQ->IsEmpty(), "No packets can be sent! NodeId: "<< GetNode()->GetId()
        << " PortId: " << m_portId);
    m_sendState = SendState::BUSY;

    auto [inPortId, priority, packet] = m_ubEQ->DoDequeue();
    Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
    if (ubSwitch != nullptr)
    {
        ubSwitch->GetQueueManager()->RemoveEgressBufferedBytes(packet->GetSize());
    }

    m_currentPkt = packet;
    m_currentInPortId = inPortId;
    m_currentPriority = priority;
    if (m_ubEQ->IsEmpty()) {
        // Switch allocation when port sendding packet.
        if (ubSwitch != nullptr && ubSwitch->GetAllocator() != nullptr) {
            Simulator::ScheduleNow(&UbSwitchAllocator::TriggerAllocator, ubSwitch->GetAllocator(), this);
        }
    }

    if (!m_faultCallBack.IsNull()) {
        m_faultCallBack(packet, GetNode()->GetId(), m_portId, this);
        return;
    }
    TransmitPacket(packet, Time(0));
    return;
}

void UbPort::TransmitPacket(Ptr<Packet> packet, Time delay)
{
    PortTxNotify(GetNode()->GetId(), m_portId, packet->GetSize());
    NS_LOG_DEBUG("[UbPort send] nodetype: " << g_node_type_map[GetNode()->GetObject<UbSwitch>()->GetNodeType()]
        << " NodeId: " << GetNode()->GetId() << " PortId: " << m_portId << " send to:"
        << " NodeId: " << m_channel->GetDestination(this)->GetNode()->GetId()
        << " PortId: " << m_channel->GetDestination(this)->GetIfIndex()
        << " PacketUid: " << packet->GetUid());
    if (m_pktTraceEnabled) {
        UbPacketTraceTag tag;
        packet->RemovePacketTag(tag);
        tag.AddPortSendTrace(GetNode()->GetId(), m_portId, Simulator::Now().GetNanoSeconds());
        packet->AddPacketTag(tag);
    }

    Time txTime = m_bps.CalculateBytesTxTime(packet->GetSize()) + delay;
    Time txCompleteTime = txTime + m_tInterframeGap;

    // Try to start transmission on the channel first. Only if it succeeds
    // should we schedule the TransmitComplete event and notify traces.
    bool result = m_channel->TransmitStart(packet, this, txTime);
    if (result == false) {
        NS_LOG_WARN("Channel transmission failed! Packet dropped at Node " << GetNode()->GetId() << " Port " << m_portId);
        // Reset port state so it won't remain BUSY and try next packet
        m_sendState = SendState::READY;
        m_currentPkt = nullptr;
        m_currentInPortId = 0;
        m_currentPriority = 0;
        Simulator::ScheduleNow(&UbPort::TriggerTransmit, this);
        return;
    }

    // Transmission started successfully: notify and schedule completion
    TraComEventNotify(packet, txCompleteTime);
    Simulator::Schedule(txCompleteTime, &UbPort::TransmitComplete, this);
    NS_LOG_DEBUG("[UbFc DequeueAndTransmit] will send pkt size: " << packet->GetSize());
    UpdateTxBytes(packet->GetSize());

    return;
}

void
UbPort::TransmitPacketDetached(Ptr<Packet> packet)
{
    PortTxNotify(GetNode()->GetId(), m_portId, packet->GetSize());
    NS_LOG_DEBUG("[UbPort detached send] nodetype: " << g_node_type_map[GetNode()->GetObject<UbSwitch>()->GetNodeType()]
        << " NodeId: " << GetNode()->GetId() << " PortId: " << m_portId << " send to:"
        << " NodeId: " << m_channel->GetDestination(this)->GetNode()->GetId()
        << " PortId: " << m_channel->GetDestination(this)->GetIfIndex()
        << " PacketUid: " << packet->GetUid());
    if (m_pktTraceEnabled) {
        UbPacketTraceTag tag;
        packet->RemovePacketTag(tag);
        tag.AddPortSendTrace(GetNode()->GetId(), m_portId, Simulator::Now().GetNanoSeconds());
        packet->AddPacketTag(tag);
    }

    Time txTime = m_bps.CalculateBytesTxTime(packet->GetSize());
    bool result = m_channel->TransmitStart(packet, this, txTime);
    if (result == false) {
        NS_LOG_WARN("Detached channel transmission failed! Packet dropped at Node "
                    << GetNode()->GetId() << " Port " << m_portId);
        return;
    }

    TraComEventNotify(packet, txTime);
    UpdateTxBytes(packet->GetSize());
}

void UbPort::Receive(Ptr<Packet> packet)
{
    NS_LOG_DEBUG("[UbPort recv] nodetype: " << g_node_type_map[GetNode()->GetObject<UbSwitch>()->GetNodeType()]
        << " NodeId: " << GetNode()->GetId()
        << " PortId: " << GetIfIndex()
        << " recv from:"
        << " NodeId: " << m_channel->GetDestination(this)->GetNode()->GetId()
        << " PortId: " << m_channel->GetDestination(this)->GetIfIndex()
        << " PacketUid: "<< packet->GetUid());
    if (m_pktTraceEnabled) {
        UbPacketTraceTag tag;
        packet->RemovePacketTag(tag);
        tag.AddPortRecvTrace(GetNode()->GetId(), m_portId, Simulator::Now().GetNanoSeconds());
        packet->AddPacketTag(tag);
    }
    PortRxNotify(GetNode()->GetId(), m_portId, packet->GetSize());
    PktRcvNotify(packet);
    GetNode()->GetObject<UbSwitch>()->SwitchHandlePacket(this, packet);

    return;
}

bool UbPort::Attach(Ptr<UbLink> ch)
{
    NS_LOG_FUNCTION(this << &ch);
    m_channel = ch;
    m_channel->Attach(this);
    NotifyLinkUp();
    return true;
}

void UbPort::NotifyLinkUp (void)
{
    NS_LOG_FUNCTION(this);
    m_linkUp = true;
}


uint32_t UbPort::ParseHeader(Ptr<Packet> p, Header& h)
{
    return p->RemoveHeader(h);
}

void UbPort::AddUdpHeader(Ptr<Packet> p, Ptr<UbTransportChannel> tp)
{
    UdpHeader udpHeader;
    udpHeader.SetDestinationPort(tp->GetDport());
    udpHeader.SetSourcePort(tp->GetUdpSport());
    p->AddHeader(udpHeader);
}

void UbPort::AddUdpHeader(Ptr<Packet> p, uint16_t sPort, uint16_t dPort)
{
    UdpHeader udpHeader;
    udpHeader.SetDestinationPort(dPort);
    udpHeader.SetSourcePort(sPort);
    p->AddHeader(udpHeader);
}

void UbPort::AddIpv4Header(Ptr<Packet> p, Ptr<UbTransportChannel> tp)
{
    Ipv4Header ipHeader;
    ipHeader.SetSource(tp->GetSip());
    ipHeader.SetDestination(tp->GetDip());
    ipHeader.SetProtocol(0x11);
    ipHeader.SetPayloadSize(p->GetSize());
    ipHeader.SetTtl(TIME_TO_LIVE);
    ipHeader.SetTos(0);
    p->AddHeader(ipHeader);
}

void UbPort::AddIpv4Header(Ptr<Packet> p, Ipv4Address sIp, Ipv4Address dIp)
{
    Ipv4Header ipHeader;
    ipHeader.SetSource(sIp);
    ipHeader.SetDestination(dIp);
    ipHeader.SetProtocol(0x11);
    ipHeader.SetPayloadSize(p->GetSize());
    ipHeader.SetTtl(TIME_TO_LIVE);
    ipHeader.SetTos(0);
    p->AddHeader(ipHeader);
}

void UbPort::AddNetHeader(Ptr<Packet> p)
{
    UbIpBasedNetworkHeader netHeader;
    p->AddHeader(netHeader);
}

Ptr<Channel> UbPort::GetChannel(void) const
{
    return m_channel;
}

bool UbPort::IsUb(void) const
{
    return true;
}

void UbPort::TriggerTransmit()
{
    NS_LOG_DEBUG("[UbPort TriggerTransmit] nodeId: " << GetNode()->GetId()
        << " portId: " << GetIfIndex() <<" TriggerTransmit...");
    if (!m_linkUp) {
        NS_LOG_DEBUG("[UbPort TriggerTransmit] m_linkUp");
        return;
    } // if link is down, return
    if (IsBusy()) {
        if (m_ubEQ->IsEmpty()) {
            Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
            if (ubSwitch != nullptr && ubSwitch->GetAllocator() != nullptr) {
                // Keep allocator prefetch alive while the current packet is still sending.
                // This lets a late-arriving next packet hide AllocationTime behind the
                // current serialization delay instead of always bubbling after tx-complete.
                Simulator::ScheduleNow(&UbSwitchAllocator::TriggerAllocator, ubSwitch->GetAllocator(), this);
            }
        }
        NS_LOG_DEBUG("[UbPort TriggerTransmit] SendState::BUSY");
        return; // Quit if channel busy
    }
    if (m_ubEQ->IsEmpty()) {
        NS_LOG_DEBUG("[UbPort TriggerTransmit] trigger Allocator");
        Ptr<UbSwitch> ubSwitch = GetNode()->GetObject<UbSwitch>();
        if (ubSwitch != nullptr && ubSwitch->GetAllocator() != nullptr) {
            Simulator::ScheduleNow(&UbSwitchAllocator::TriggerAllocator, ubSwitch->GetAllocator(), this);
        }
        return;
    }
    DequeuePacket();
}

void UbPort::NotifyAllocationFinish()
{
    if (IsBusy()) {
        return;
    }
    if (m_ubEQ->IsEmpty()) {
        // No Packet to send
        return;
    }
    DequeuePacket();
}

Ptr<UbEgressQueue> UbPort::GetUbQueue()
{
    return m_ubEQ;
}

void UbPort::PktRcvNotify(Ptr<Packet> p)
{
    m_tracePktRcvNotify(p);
}

void UbPort::TraComEventNotify(Ptr<Packet> p, Time t)
{
    m_traceTraComEventNotify(p, t);
}

DataRate UbPort::GetDataRate()
{
    return m_bps;
}

Time UbPort::GetInterframeGap()
{
    return m_tInterframeGap;
}

void UbPort::SetCredits(int index, uint8_t value)
{ // 设置用于恢复的信用证值
    m_credits[index] = value;
}

void UbPort::ResetCredits()
{ // 设置用于恢复的信用证值
    for (uint8_t index = 0; index < qCnt; index++) {
        m_credits[index] = 0;
    }
}

uint8_t UbPort::GetCredits(int index)
{ // 获取用于恢复的信用证值
    return m_credits[index];
}

void UbPort::UpdateTxBytes(uint64_t bytes)
{
    m_txBytes += bytes;
}

uint64_t UbPort::GetTxBytes()
{
    return m_txBytes;
}

bool UbPort::IsReady()
{
    return m_sendState == SendState::READY;
}

bool UbPort::IsBusy()
{
    return m_sendState == SendState::BUSY;
}

void UbPort::SetDataRate(DataRate bps)
{
    NS_LOG_DEBUG("port set data rate");
    m_bps = bps;
}

void UbPort::IncreaseRcvQueueSize(Ptr<Packet> p, Ptr<UbPort> port)
{
    uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("[UbFc IncreaseRcvQueueSize] pktSize: " << pktSize << " PortId: " << port->GetIfIndex());
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    NS_LOG_DEBUG("[UbFc IncreaseRcvQueueSize] before m_revQueueSize[ "
        << (uint32_t)vlId << " ]: " << (uint32_t)m_revQueueSize[vlId]);
    m_revQueueSize[vlId] += pktSize;
    NS_LOG_DEBUG("[UbFc IncreaseRcvQueueSize] after m_revQueueSize[ "
        << (uint32_t)vlId << " ]: " << (uint32_t)m_revQueueSize[vlId]);
}

void UbPort::DecreaseRcvQueueSize(Ptr<Packet> p, uint32_t portId)
{
    Ptr<UbPort> port = DynamicCast<UbPort>(GetNode()->GetDevice(portId));
    uint32_t pktSize = p->GetSize();
    NS_LOG_DEBUG("[UbFc DecreaseRcvQueueSize] pktSize: " << pktSize << " PortId: " << port->GetIfIndex());
    UbDatalinkPacketHeader pktHeader;
    p->PeekHeader(pktHeader);
    uint8_t vlId = pktHeader.GetPacketVL();
    NS_LOG_DEBUG("[UbFc DecreaseRcvQueueSize] before m_revQueueSize[ " << (uint32_t)vlId << " ]: "
        << (uint32_t)m_revQueueSize[vlId]);
    port->m_revQueueSize[vlId] -= pktSize;
    NS_LOG_DEBUG("[UbFc DecreaseRcvQueueSize] after m_revQueueSize[ " << (uint32_t)vlId << " ]: "
        << (uint32_t)m_revQueueSize[vlId]);
}

uint32_t UbPort::GetRcvVlQueueSize(uint8_t vlId)
{
    return m_revQueueSize[vlId];
}

std::vector<uint32_t> UbPort::GetRcvQueueSize()
{
    return m_revQueueSize;
}

void UbPort::PortTxNotify(uint32_t nodeId, uint32_t mPortId, uint32_t size)
{
    m_tracePortTxNotify(nodeId, mPortId, size);
}

void UbPort::PortRxNotify(uint32_t nodeId, uint32_t mPortId, uint32_t size)
{
    m_tracePortRxNotify(nodeId, mPortId, size);
}

void UbPort::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ubEQ = nullptr;
    m_channel = nullptr;
    m_currentPkt = nullptr;
    m_datalink = nullptr;
    m_flowControl = nullptr;
    m_mpiReceiveEnabled = false;
    m_revQueueSize.clear();
    NetDevice::DoDispose();
}

} // namespace ns3
