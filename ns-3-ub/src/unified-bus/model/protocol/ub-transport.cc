// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-dcqcn.h"
#include "../ub-network-address.h"
#include "ns3/node.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-utils.h"
#include "ns3/ub-modulo-sequence.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

using namespace utils;
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbTransportChannel");

NS_OBJECT_ENSURE_REGISTERED(UbTransportChannel);

namespace
{

using UbTpPsnSequence = UbModuloSequence<24, uint64_t>;
using UbTpMsnSequence = UbModuloSequence<24, uint64_t>;
using UbTaSsnSequence = UbModuloSequence<16, uint32_t>;

constexpr uint32_t kShallowPipelineActiveSendDepth = 2;

bool
IsZeroPayloadReadRequest(const Ptr<UbWqeSegment>& segment)
{
    return segment != nullptr && segment->GetType() == TaOpcode::TA_OPCODE_READ &&
           segment->GetSegmentKind() == UbTransactionSegmentKind::REQUEST;
}

uint32_t
GetProgressBytesThisPacket(const Ptr<UbWqeSegment>& segment)
{
    return std::min<uint64_t>(segment->GetBytesLeft(), UB_MTU_BYTE);
}

uint32_t
GetPayloadBytesThisPacket(const Ptr<UbWqeSegment>& segment, uint32_t progressBytes)
{
    return IsZeroPayloadReadRequest(segment) ? 0 : progressBytes;
}

uint32_t
GetWireLengthBytes(const Ptr<UbWqeSegment>& segment, uint32_t payloadBytes)
{
    return IsZeroPayloadReadRequest(segment) ? segment->GetResLenBytes() : payloadBytes;
}

uint32_t
GetTotalProgressBytes(const Ptr<UbWqeSegment>& segment)
{
    return segment->GetCarrierBytes() == 0 ? segment->GetSize() : segment->GetCarrierBytes();
}

void
TraceTpDebugState(uint32_t nodeId,
                  uint32_t tpn,
                  const std::string& reason,
                  uint64_t psnSndNxt,
                  uint64_t psnSndUna,
                  uint64_t maxInflightPackets,
                  bool ccLimited,
                  bool sendWindowLimited,
                  uint32_t activeSegments,
                  uint32_t totalSegments,
                  uint32_t ackQueueLen,
                  uint32_t cnpQueueLen)
{
    const uint64_t inflightPackets = psnSndNxt - psnSndUna;
    utils::UbUtils::TpDebugStateNotify(nodeId,
                                       tpn,
                                       reason,
                                       psnSndNxt,
                                       psnSndUna,
                                       inflightPackets,
                                       maxInflightPackets,
                                       inflightPackets >= maxInflightPackets,
                                       ccLimited,
                                       sendWindowLimited,
                                       activeSegments,
                                       totalSegments,
                                       ackQueueLen,
                                       cnpQueueLen);
}

Ptr<const AttributeChecker>
MakeSelectiveAckBitmapBitsChecker()
{
    struct Checker : public AttributeChecker
    {
        bool Check(const AttributeValue& value) const override
        {
            const auto* v = dynamic_cast<const UintegerValue*>(&value);
            if (v == nullptr)
            {
                return false;
            }

            switch (v->Get())
            {
            case 0:
            case 64:
            case 128:
            case 256:
            case 512:
            case 1024:
                return true;
            default:
                return false;
            }
        }

        std::string GetValueTypeName() const override
        {
            return "ns3::UintegerValue";
        }

        bool HasUnderlyingTypeInformation() const override
        {
            return true;
        }

        std::string GetUnderlyingTypeInformation() const override
        {
            return "uint32_t 0|64|128|256|512|1024";
        }

        Ptr<AttributeValue> Create() const override
        {
            return ns3::Create<UintegerValue>();
        }

        bool Copy(const AttributeValue& source, AttributeValue& destination) const override
        {
            const auto* src = dynamic_cast<const UintegerValue*>(&source);
            auto* dst = dynamic_cast<UintegerValue*>(&destination);
            if (src == nullptr || dst == nullptr)
            {
                return false;
            }
            *dst = *src;
            return true;
        }
    }* checker = new Checker();

    return Ptr<const AttributeChecker>(checker, false);
}

std::string
FormatSimpleAckInfo(const char* ackType, uint64_t psn)
{
    std::ostringstream oss;
    oss << ackType << "(PSN=" << psn << ")";
    return oss.str();
}

std::string
FormatSelectiveAckInfo(const UbTransportHeader& tpHeader, const UbSelectiveAckExtTph& saetph)
{
    const uint64_t ackBase = tpHeader.GetPsn();
    const uint64_t maxRcvPsn = saetph.GetMaxRcvPsn();
    const uint32_t bitmapBits = saetph.GetBitmapBitCount();
    uint32_t visibleBits = bitmapBits;
    if (maxRcvPsn >= ackBase) {
        visibleBits = static_cast<uint32_t>(std::min<uint64_t>(bitmapBits, maxRcvPsn - ackBase + 1));
    }

    std::ostringstream bitmap;
    bitmap << '[';
    for (uint32_t i = 0; i < visibleBits; ++i) {
        if (i > 0) {
            bitmap << ',';
        }
        bitmap << (saetph.GetBitmapBit(i) ? '1' : '0');
    }
    bitmap << ']';

    std::ostringstream oss;
    oss << "TPSACK(PSN=" << ackBase
        << ",MaxRcvPSN=" << maxRcvPsn
        << ",BitmapSize=" << bitmapBits
        << ",BitmapRange=[" << ackBase << ','
        << (visibleBits == 0 ? ackBase : ackBase + visibleBits - 1) << ']'
        << ",Bitmap=" << bitmap.str() << ")";
    return oss.str();
}

} // namespace

TypeId UbTransportChannel::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransportChannel")
        .SetParent<UbIngressQueue>()
        .SetGroupName("UnifiedBus")
        .AddAttribute("EnableRetrans",
                      "Enable transport-layer retransmission.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::SetRetransEnable,
                                          &UbTransportChannel::GetRetransEnable),
                      MakeBooleanChecker())
        .AddAttribute("BaseRTO",
                      "Base retransmission timeout. In DYNAMIC mode it is Base_time; in STATIC mode it is the fixed timeout.",
                      TimeValue(NanoSeconds(25600)),
                      MakeTimeAccessor(&UbTransportChannel::SetBaseRto,
                                       &UbTransportChannel::GetBaseRto),
                      MakeTimeChecker())
        .AddAttribute("RetransTimeoutMode",
                      "Retransmission timeout mode.",
                      EnumValue(UbRetransTimeoutMode::DYNAMIC),
                      MakeEnumAccessor<UbRetransTimeoutMode>(
                          &UbTransportChannel::SetRetransTimeoutMode,
                          &UbTransportChannel::GetRetransTimeoutMode),
                      MakeEnumChecker(UbRetransTimeoutMode::STATIC,
                                      "STATIC",
                                      UbRetransTimeoutMode::DYNAMIC,
                                      "DYNAMIC"))
        .AddAttribute("MaxRetransAttempts",
                      "Maximum retransmission attempts before aborting.",
                      UintegerValue(7),
                      MakeUintegerAccessor(&UbTransportChannel::SetMaxRetransAttempts,
                                           &UbTransportChannel::GetMaxRetransAttempts),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("RetransExponentFactor",
                      "Dynamic timeout interval coefficient N: RTO = BaseRTO * 2^(N * Times). Ignored in STATIC mode.",
                      UintegerValue(1),
                      MakeUintegerAccessor(&UbTransportChannel::SetRetransExponentFactor,
                                           &UbTransportChannel::GetRetransExponentFactor),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("DefaultMaxWqeSegNum",
                      "Default limit on outstanding WQE segments per TP.",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&UbTransportChannel::m_defaultMaxWqeSegNum),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("DefaultMaxInflightPacketSize",
                      "Maximum number of in-flight packets allowed per transport channel.",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&UbTransportChannel::m_defaultMaxInflightPacketSize),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("TpOooThreshold",
                      "Receiver out-of-order PSN window size tracked in bitmap.",
                      UintegerValue(2048),
                      MakeUintegerAccessor(&UbTransportChannel::m_psnOooThreshold),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("RetransmissionMode",
                      "Transport retransmission algorithm.",
                      EnumValue(UbRetransmissionMode::GBN),
                      MakeEnumAccessor<UbRetransmissionMode>(
                          &UbTransportChannel::SetRetransmissionMode,
                          &UbTransportChannel::GetRetransmissionMode),
                      MakeEnumChecker(UbRetransmissionMode::GBN,
                                      "GBN",
                                      UbRetransmissionMode::SELECTIVE,
                                      "SELECTIVE"))
        .AddAttribute("SelectiveAckBitmapBits",
                      "TPSACK/SAETPH bitmap width in bits; 0 selects an automatic width from TpOooThreshold.",
                      UintegerValue(0),
                      MakeUintegerAccessor(&UbTransportChannel::SetSelectiveAckBitmapBits,
                                           &UbTransportChannel::GetSelectiveAckBitmapBitsConfig),
                      MakeSelectiveAckBitmapBitsChecker())
        .AddAttribute("EnableFastRetrans",
                      "Trigger fast retransmission for the selected mode: TPNAK for GBN, TPSACK for SELECTIVE.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::SetFastRetransEnable,
                                          &UbTransportChannel::GetFastRetransEnable),
                      MakeBooleanChecker())
        .AddAttribute("EnableSelectiveMarkPsn",
                      "Enable MarkPSN phase control for selective retransmission with fast retransmit.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::SetSelectiveMarkPsnEnable,
                                          &UbTransportChannel::GetSelectiveMarkPsnEnable),
                      MakeBooleanChecker())
        .AddAttribute("UsePacketSpray",
                      "Enable per-packet ECMP/packet spray across multiple paths.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&UbTransportChannel::m_usePacketSpray),
                      MakeBooleanChecker())
        .AddAttribute("UseShortestPaths",
                      "Sets a packet header flag that instructs switches to restrict forwarding to shortest paths (true) or allow non-shortest paths (false).",
                      BooleanValue(true),
                      MakeBooleanAccessor(&UbTransportChannel::m_useShortestPaths),
                      MakeBooleanChecker())
        .AddTraceSource("FirstPacketSendsNotify",
                        "Fires when the first packet of a WQE segment is sent.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceFirstPacketSendsNotify),
                        "ns3::UbTransportChannel::FirstPacketSendsNotify")
        .AddTraceSource("LastPacketSendsNotify",
                        "Fires when the last packet of a WQE segment is sent.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketSendsNotify),
                        "ns3::UbTransportChannel::LastPacketSendsNotify")
        .AddTraceSource("LastPacketACKsNotify",
                        "Fires when the last packet of a WQE segment is ACKed.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketACKsNotify),
                        "ns3::UbTransportChannel::LastPacketACKsNotify")
        .AddTraceSource("LastPacketReceivesNotify",
                        "Fires when the last packet of a WQE segment is received.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceLastPacketReceivesNotify),
                        "ns3::UbTransportChannel::LastPacketReceivesNotify")
        .AddTraceSource("WqeSegmentSendsNotify",
                        "Fires when a WQE segment is scheduled for transmission.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceWqeSegmentSendsNotify),
                        "ns3::UbTransportChannel::WqeSegmentSendsNotify")
        .AddTraceSource("WqeSegmentCompletesNotify",
                        "Fires when a WQE segment completes at the receiver.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceWqeSegmentCompletesNotify),
                        "ns3::UbTransportChannel::WqeSegmentCompletesNotify")
        .AddTraceSource("TpRecvNotify",
                        "Fires on TP data or ACK reception (provides info and trace tags).",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_tpRecvNotify),
                        "ns3::UbTransportChannel::TpRecvNotify")
        .AddTraceSource("SelectiveRetransmitNotify",
                        "Selective retransmission event: node id, TPN, PSN, payload bytes.",
                        MakeTraceSourceAccessor(&UbTransportChannel::m_traceSelectiveRetransmit),
                        "ns3::UbTransportChannel::SelectiveRetransmitNotify");
    return tid;
}

bool
UbTransportChannel::IsTransportResponseOpcode(TpOpcode opcode)
{
    return IsTransportResponseOpcode(static_cast<uint8_t>(opcode));
}

bool
UbTransportChannel::IsTransportResponseOpcode(uint8_t opcode)
{
    return opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH) ||
           opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH) ||
           opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH) ||
           opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITH_CETPH) ||
           opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_CNP);
}

void
UbTransportChannel::SetBaseRto(Time rto)
{
    m_retrans->SetBaseRto(rto);
}

Time
UbTransportChannel::GetBaseRto() const
{
    return m_retrans->GetBaseRto();
}

void
UbTransportChannel::SetMaxRetransAttempts(uint16_t attempts)
{
    m_retrans->SetMaxRetransAttempts(attempts);
}

uint16_t
UbTransportChannel::GetMaxRetransAttempts() const
{
    return m_retrans->GetMaxRetransAttempts();
}

void
UbTransportChannel::SetRetransExponentFactor(uint16_t factor)
{
    m_retrans->SetRetransExponentFactor(factor);
}

uint16_t
UbTransportChannel::GetRetransExponentFactor() const
{
    return m_retrans->GetRetransExponentFactor();
}

void
UbTransportChannel::SetRetransTimeoutMode(UbRetransTimeoutMode mode)
{
    m_retrans->SetRetransTimeoutMode(mode);
}

UbRetransTimeoutMode
UbTransportChannel::GetRetransTimeoutMode() const
{
    return m_retrans->GetRetransTimeoutMode();
}

void
UbTransportChannel::SetRetransmissionMode(UbRetransmissionMode mode)
{
    m_retrans->SetRetransmissionMode(mode);
}

UbRetransmissionMode
UbTransportChannel::GetRetransmissionMode() const
{
    return m_retrans->GetRetransmissionMode();
}

void
UbTransportChannel::SetRetransEnable(bool enable)
{
    m_retrans->SetRetransEnable(enable);
}

bool
UbTransportChannel::GetRetransEnable() const
{
    return m_retrans->GetRetransEnable();
}

void
UbTransportChannel::SetSelectiveAckBitmapBits(uint32_t bits)
{
    m_retrans->SetSelectiveAckBitmapBits(bits);
}

uint32_t
UbTransportChannel::GetSelectiveAckBitmapBitsConfig() const
{
    return m_retrans->GetSelectiveAckBitmapBitsConfig();
}

void
UbTransportChannel::SetFastRetransEnable(bool enable)
{
    m_retrans->SetFastRetransEnable(enable);
}

bool
UbTransportChannel::GetFastRetransEnable() const
{
    return m_retrans->GetFastRetransEnable();
}

void
UbTransportChannel::SetSelectiveMarkPsnEnable(bool enable)
{
    m_retrans->SetSelectiveMarkPsnEnable(enable);
}

bool
UbTransportChannel::GetSelectiveMarkPsnEnable() const
{
    return m_retrans->GetSelectiveMarkPsnEnable();
}

uint64_t
UbTransportChannel::GetPsnSndUna() const
{
    return m_psnSndUna;
}

void
UbTransportChannel::SetPsnSndUna(uint64_t psn)
{
    m_psnSndUna = psn;
}

uint64_t
UbTransportChannel::GetPsnSndNxt() const
{
    return m_psnSndNxt;
}

void
UbTransportChannel::SetPsnSndNxt(uint64_t psn)
{
    m_psnSndNxt = psn;
}

uint64_t
UbTransportChannel::GetPsnRecvNxt() const
{
    return m_psnRecvNxt;
}

void
UbTransportChannel::TriggerTransportTransmit()
{
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit();
}

bool
UbTransportChannel::IsCcLimitedForRetransmission(uint32_t payloadBytes) const
{
    return m_congestionCtrl->IsCcLimited(payloadBytes);
}

void
UbTransportChannel::SetSendWindowLimited(bool limited)
{
    m_sendWindowLimited = limited;
}

void
UbTransportChannel::OnSelectiveRetransmissionPacketSent(uint64_t psn, uint32_t payloadBytes)
{
    m_congestionCtrl->OnSenderRetransmissionPacketSent(UbTpPsnSequence::ToWire(psn), payloadBytes);
    m_traceSelectiveRetransmit(m_nodeId, m_tpn, psn, payloadBytes);
}

void
UbTransportChannel::OnSenderReceivesTpsackCongestionFeedback(uint64_t psn,
                                                             const UbCongestionExtTph& cetph,
                                                             uint32_t retransmitBytes)
{
    m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                                     UbTpPsnSequence::ToWire(psn),
                                                     cetph,
                                                     retransmitBytes);
}

/**
 * @brief Constructor for UbTransportChannel
 */
UbTransportChannel::UbTransportChannel()
{
    m_retrans = std::make_unique<UbRetransController>(*this);
    BooleanValue val;
    if (GlobalValue::GetValueByNameFailSafe("UB_RECORD_PKT_TRACE", val)) {
        GlobalValue::GetValueByName("UB_RECORD_PKT_TRACE", val);
        m_pktTraceEnabled = val.Get();
    } else {
        m_pktTraceEnabled = false;
    }
    NS_LOG_FUNCTION(this);
}

UbTransportChannel::~UbTransportChannel()
{
    // Clear WQE queues and release resources
    NS_LOG_INFO("tp release, node:" << m_src << " tpn:" << m_tpn);
    NS_LOG_FUNCTION(this);
}


void UbTransportChannel::DoDispose()
{
    NS_LOG_FUNCTION(this);
    if (m_retrans != nullptr) {
        m_retrans->CancelTimer();
        m_retrans->ClearRetainedState();
    }
    m_ackQ = queue<Ptr<Packet>>();
    m_cnpQ = queue<Ptr<Packet>>();
    m_wqeSegmentVector.clear();
    m_inboundTaUnits.clear();
    m_inboundTpMsnReferences.clear();
    m_inboundTaSsnReferences.clear();
    m_bufferedInboundPackets.clear();
    m_congestionCtrl = nullptr;
    m_recvPsnWindow.Resize(0);
}

bool
UbTransportChannel::IsRetransEnabled() const
{
    return m_retrans != nullptr && m_retrans->GetRetransEnable();
}

bool
UbTransportChannel::HasPendingSelectiveRetransmissionWork() const
{
    return IsRetransEnabled() && m_retrans->CanSendSelectiveRetransmission();
}

/**
 * @brief Get next packet from transport channel queue
 * Called by Switch Allocator during scheduling to retrieve the next packet for transmission
 */
Ptr<Packet>
UbTransportChannel::PopQueuedPacket(std::queue<Ptr<Packet>>& packetQ)
{
    if (packetQ.empty()) {
        return nullptr;
    }

    Ptr<Packet> packet = packetQ.front();
    packetQ.pop();
    if (HasPendingTransmitWork()) {
        m_headArrivalTime = Simulator::Now();
    }
    return packet;
}

bool
UbTransportChannel::CanTrySendNewDataPacket()
{
    if (m_wqeSegmentVector.empty()) {
        NS_LOG_DEBUG("No WQE segments available to send");
        TraceTpDebugState(m_nodeId,
                          m_tpn,
                          "GET_NEXT_EMPTY_SEGMENTS",
                          m_psnSndNxt,
                          m_psnSndUna,
                          m_maxInflightPacketSize,
                          false,
                          m_sendWindowLimited,
                          GetActiveSendSegmentCount(),
                          static_cast<uint32_t>(m_wqeSegmentVector.size()),
                          static_cast<uint32_t>(m_ackQ.size()),
                          static_cast<uint32_t>(m_cnpQ.size()));
        return false;
    }

    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        TraceTpDebugState(m_nodeId,
                          m_tpn,
                          "GET_NEXT_INFLIGHT_LIMITED",
                          m_psnSndNxt,
                          m_psnSndUna,
                          m_maxInflightPacketSize,
                          false,
                          m_sendWindowLimited,
                          GetActiveSendSegmentCount(),
                          static_cast<uint32_t>(m_wqeSegmentVector.size()),
                          static_cast<uint32_t>(m_ackQ.size()),
                          static_cast<uint32_t>(m_cnpQ.size()));
        return false;
    }

    return true;
}

bool
UbTransportChannel::BuildNextDataSendContext(NewDataSendContext& ctx)
{
    // This helper selects only a new data packet. Retransmission packets are handled earlier.
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }

        ctx.segment = currentSegment;
        ctx.progressBytes = GetProgressBytesThisPacket(currentSegment);
        ctx.payloadBytes = GetPayloadBytesThisPacket(currentSegment, ctx.progressBytes);
        ctx.wireLengthBytes = GetWireLengthBytes(currentSegment, ctx.payloadBytes);
        ctx.totalProgressBytes = GetTotalProgressBytes(currentSegment);

        if (m_congestionCtrl->IsCcLimited(ctx.progressBytes)) {
            m_sendWindowLimited = true;
            TraceTpDebugState(m_nodeId,
                              m_tpn,
                              "GET_NEXT_CC_LIMITED",
                              m_psnSndNxt,
                              m_psnSndUna,
                              m_maxInflightPacketSize,
                              true,
                              m_sendWindowLimited,
                              GetActiveSendSegmentCount(),
                              static_cast<uint32_t>(m_wqeSegmentVector.size()),
                              static_cast<uint32_t>(m_ackQ.size()),
                              static_cast<uint32_t>(m_cnpQ.size()));
            return false;
        }

        return true;
    }

    return false;
}

void
UbTransportChannel::NotifyNewDataPacketSent(const NewDataSendContext& ctx,
                                            Ptr<Packet> packet)
{
    // TODO: Keep Notify* helpers as logging/trace notifications only. Retransmission
    // and congestion-control state updates should live at the actual send trigger.
    if (IsRetransEnabled()) {
        m_retrans->OnNewDataPacketSent(m_psnSndNxt,
                                       packet,
                                       ctx.payloadBytes);
    }

    m_congestionCtrl->OnSenderDataPacketSent(UbTpPsnSequence::ToWire(m_psnSndNxt),
                                             ctx.progressBytes);

    if (ctx.segment->GetBytesLeft() == ctx.totalProgressBytes) {
        FirstPacketSendsNotify(m_nodeId, ctx.segment->GetTaskId(), m_tpn, m_dstTpn,
                               UbTpMsnSequence::ToWire(ctx.segment->GetTpMsn()),
                               UbTpPsnSequence::ToWire(m_psnSndNxt),
                               m_sport);
    }
    if (ctx.segment->GetBytesLeft() == ctx.progressBytes) {
        LastPacketSendsNotify(m_nodeId, ctx.segment->GetTaskId(), m_tpn, m_dstTpn,
                              UbTpMsnSequence::ToWire(ctx.segment->GetTpMsn()),
                              UbTpPsnSequence::ToWire(m_psnSndNxt),
                              m_sport);
    }

    NS_LOG_DEBUG("[Transport channel] Send packet."
              << " PacketUid: " << packet->GetUid()
              << " Tpn: " << m_tpn
              << " DstTpn: " << m_dstTpn
              << " Psn: " << m_psnSndNxt
              << " PacketType: Packet"
              << " Src: " << m_src
              << " Dst: " << m_dest
              << " PacketSize: " << packet->GetSize()
              << " TaskId: " << ctx.segment->GetTaskId());
}

void
UbTransportChannel::AdvanceNewDataSendState(const NewDataSendContext& ctx,
                                            Ptr<Packet>)
{
    ctx.segment->UpdateSentBytes(ctx.progressBytes);
    m_psnSndNxt++;
    TraceTpDebugState(m_nodeId,
                      m_tpn,
                      "SEND_PACKET",
                      m_psnSndNxt,
                      m_psnSndUna,
                      m_maxInflightPacketSize,
                      false,
                      m_sendWindowLimited,
                      GetActiveSendSegmentCount(),
                      static_cast<uint32_t>(m_wqeSegmentVector.size()),
                      static_cast<uint32_t>(m_ackQ.size()),
                      static_cast<uint32_t>(m_cnpQ.size()));
    if (IsRetransEnabled()) {
        m_retrans->StartTimerIfNeeded();
    }

    // Shallow pipeline keeps a small active and unacked segment budget.
    if (ctx.segment->IsSentCompleted() && CanScheduleAnotherSegment()) {
        ApplyNextWqeSegment();
    }
    if (HasPendingTransmitWork()) {
        m_headArrivalTime = Simulator::Now();
    }
}

Ptr<Packet>
UbTransportChannel::SendNewDataPacket(const NewDataSendContext& ctx)
{
    Ptr<Packet> packet = GenDataPacket(ctx.segment,
                                       ctx.payloadBytes,
                                       ctx.wireLengthBytes,
                                       ctx.progressBytes);
    NotifyNewDataPacketSent(ctx, packet);
    AdvanceNewDataSendState(ctx, packet);
    return packet;
}

Ptr<Packet>
UbTransportChannel::TryGetNextNewDataPacket()
{
    if (!CanTrySendNewDataPacket()) {
        return nullptr;
    }

    NewDataSendContext ctx;
    if (!BuildNextDataSendContext(ctx)) {
        return nullptr;
    }

    return SendNewDataPacket(ctx);
}

Ptr<Packet> UbTransportChannel::GetNextPacket()
{
    Ptr<Packet> packet = PopQueuedPacket(m_cnpQ);
    if (packet != nullptr) {
        return packet;
    }

    packet = PopQueuedPacket(m_ackQ);
    if (packet != nullptr) {
        return packet;
    }

    if (IsRetransEnabled()) {
        packet = m_retrans->TryGetNextRetransmissionPacket();
        if (packet != nullptr) {
            if (HasPendingTransmitWork()) {
                m_headArrivalTime = Simulator::Now();
            }
            return packet;
        }
        if (HasPendingSelectiveRetransmissionWork()) {
            return nullptr;
        }
    }

    return TryGetNextNewDataPacket();
}

void
UbTransportChannel::EnqueueDcqcnCnp(uint8_t ecn, bool location)
{
    Ptr<Packet> cnp = BuildDcqcnCnp(ecn, location);
    const uint8_t packetVl = std::max<uint8_t>(m_priority, 1);

    UbPort::AddUdpHeader(cnp, this);
    UbPort::AddIpv4Header(cnp, this);

    UbIpBasedNetworkHeader networkHeader;
    cnp->AddHeader(networkHeader);

    UbDataLink::GenPacketHeader(cnp,
                                false,
                                false,
                                packetVl,
                                packetVl,
                                m_usePacketSpray,
                                m_useShortestPaths,
                                UbDatalinkHeaderConfig::PACKET_IPV4);

    if (m_cnpQ.empty() && m_ackQ.empty() && m_wqeSegmentVector.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    m_cnpQ.push(cnp);

    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    if (port != nullptr) {
        port->TriggerTransmit();
    }
}

Ptr<Packet>
UbTransportChannel::BuildDcqcnCnp(uint8_t ecn, bool location) const
{
    Ptr<Packet> cnp = Create<Packet>(0);

    UbCnpExtTph cnpHeader;
    cnpHeader.SetEcn(ecn);
    cnpHeader.SetLocation(location);
    cnp->AddHeader(cnpHeader);

    UbTransportHeader tpHeader;
    tpHeader.SetLastPacket(false);
    tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_CNP);
    tpHeader.SetNLP(0x0);
    tpHeader.SetSrcTpn(m_tpn);
    tpHeader.SetDestTpn(m_dstTpn);
    tpHeader.SetAckRequest(0);
    tpHeader.SetErrorFlag(0);
    tpHeader.SetPsn(0);
    tpHeader.SetTpMsn(0);
    cnp->AddHeader(tpHeader);

    return cnp;
}

uint32_t UbTransportChannel::GetNextPacketSize()
{
    uint32_t pktSize = 0;
    UbMAExtTah MAExtTaHeader;
    UbTransactionHeader TransactionHeader;
    UbTransportHeader  TransportHeader;
    UdpHeader   UHeader;
    Ipv4Header  I4Header;
    UbDatalinkPacketHeader  DataLinkPacketHeader;

    uint32_t MAExtTaHeaderSize = MAExtTaHeader.GetSerializedSize();
    uint32_t UbTransactionHeaderSize = TransactionHeader.GetSerializedSize();
    uint32_t UbTransportHeaderSize = TransportHeader.GetSerializedSize();
    uint32_t UdpHeaderSize = UHeader.GetSerializedSize();
    uint32_t Ipv4HeaderSize = I4Header.GetSerializedSize();
    uint32_t UbDataLinkPktSize = DataLinkPacketHeader.GetSerializedSize();

    uint32_t headerSize = MAExtTaHeaderSize + UbTransactionHeaderSize + UbTransportHeaderSize
                          + UdpHeaderSize + Ipv4HeaderSize + UbDataLinkPktSize;

    if (!m_cnpQ.empty()) {
        return m_cnpQ.front()->GetSize();
    }
    if (!m_ackQ.empty()) {
        return m_ackQ.front()->GetSize();
    }
    const uint32_t selectiveRetransmissionSize =
        HasPendingSelectiveRetransmissionWork()
            ? m_retrans->GetNextSelectiveRetransmissionSize()
            : 0;
    if (selectiveRetransmissionSize > 0) {
        return selectiveRetransmissionSize;
    }
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        if (currentSegment == nullptr || currentSegment->IsSentCompleted()) {
            continue;
        }
        const uint32_t progressBytes = GetProgressBytesThisPacket(currentSegment);
        const uint32_t payloadSize = GetPayloadBytesThisPacket(currentSegment, progressBytes);
        pktSize = payloadSize + headerSize;
        return pktSize;
    }
    return pktSize;
}
Ptr<Packet> UbTransportChannel::GenDataPacket(Ptr<UbWqeSegment> wqeSegment,
                                              uint32_t payloadSize,
                                              uint32_t wireLengthBytes,
                                              uint32_t progressBytes)
{
    Ptr<Packet> p = Create<Packet>(payloadSize);
    UbFlowTag flowTag(wqeSegment->GetTaskId(), wqeSegment->GetWqeSize());
    p->AddPacketTag(flowTag);
    // add UbMAExtTah
    UbMAExtTah MAExtTaHeader;
    MAExtTaHeader.SetLength(wireLengthBytes);
    p->AddHeader(MAExtTaHeader);
    // add TaHeader
    UbTransactionHeader TaHeader;
    TaHeader.SetTaOpcode(wqeSegment->GetType());
    const uint16_t wireIniTaSsn =
        wqeSegment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE
            ? UbTaSsnSequence::ToWire(wqeSegment->GetRequestTassn())
            : UbTaSsnSequence::ToWire(wqeSegment->GetTaSsn());
    TaHeader.SetIniTaSsn(wireIniTaSsn);
    TaHeader.SetOrder(wqeSegment->GetOrderType());
    TaHeader.SetIniRcType(0x01);
    TaHeader.SetIniRcId(wqeSegment->GetOriginJettyNum());
    p->AddHeader(TaHeader);
    // add TpHeader
    UbTransportHeader TpHeader;
    if (wqeSegment->GetBytesLeft() == progressBytes) {
        TpHeader.SetLastPacket(true); // last packet
    } else {
        TpHeader.SetLastPacket(false); // not last packet
    }
    TpHeader.SetTPOpcode(0x1);
    TpHeader.SetNLP(0x0);
    TpHeader.SetSrcTpn(m_tpn);
    TpHeader.SetDestTpn(m_dstTpn);
    TpHeader.SetAckRequest(1);
    TpHeader.SetErrorFlag(0);
    TpHeader.SetPsn(UbTpPsnSequence::ToWire(m_psnSndNxt));
    TpHeader.SetTpMsn(UbTpMsnSequence::ToWire(wqeSegment->GetTpMsn()));
    p->AddHeader(TpHeader);
    // Packet-spray hashing must be derived from packet identity, not from the order in which
    // worker threads happen to call this function.
    m_lbHashSalt = m_usePacketSpray ? UbTpPsnSequence::ToWire(m_psnSndNxt) : 0;
    UbPort::AddUdpHeader(p, this);
    // add ipv4 header
    UbPort::AddIpv4Header(p, this);
    // add network header
    UbIpBasedNetworkHeader networkHeader;
    m_congestionCtrl->OnSenderPrepareIpBasedNetworkHeader(networkHeader);
    p->AddHeader(networkHeader);
    // add dl header
    UbDataLink::GenPacketHeader(p, false, false, m_priority, m_priority, m_usePacketSpray,
                                m_useShortestPaths, UbDatalinkHeaderConfig::PACKET_IPV4);
    return p;
}

Ptr<UbWqeSegment> UbTransportChannel::TrackInboundTaPacket(const UbTransportHeader& tpHeader,
                                                           const UbTransactionHeader& taHeader,
                                                           uint32_t resLenBytes,
                                                           uint32_t payloadBytes,
                                                           uint32_t taskId)
{
    const uint32_t srcTpn = tpHeader.GetSrcTpn();
    uint64_t& tpMsnReference = m_inboundTpMsnReferences[srcTpn];
    const uint64_t logicalTpMsn = UbTpMsnSequence::Unwrap(tpHeader.GetTpMsn(), tpMsnReference);
    const auto taSsnKey = std::make_pair(srcTpn, taHeader.GetIniRcId());
    uint32_t& taSsnReference = m_inboundTaSsnReferences[taSsnKey];
    const uint32_t logicalTaSsn =
        UbTaSsnSequence::Unwrap(taHeader.GetIniTaSsn(), taSsnReference);
    const auto key = std::make_pair(srcTpn, logicalTpMsn);
    auto& state = m_inboundTaUnits[key];
    tpMsnReference = std::max<uint64_t>(tpMsnReference, logicalTpMsn + 1);
    taSsnReference = std::max<uint32_t>(taSsnReference, logicalTaSsn + 1);
    if (state.segment == nullptr)
    {
        state.segment = CreateObject<UbWqeSegment>();
        state.segment->SetSrc(m_dest);
        state.segment->SetDest(m_src);
        state.segment->SetSport(m_dport);
        state.segment->SetDport(m_sport);
        state.segment->SetPriority(m_priority);
        state.segment->SetTaskId(taskId);
        state.segment->SetWqeSize(0);
        state.segment->SetJettyNum(taHeader.GetIniRcId());
        state.segment->SetTaMsn(logicalTpMsn);
        state.segment->SetTaSsn(logicalTaSsn);
        state.segment->SetOriginJettyNum(taHeader.GetIniRcId());
        state.segment->SetRequestTassn(logicalTaSsn);
        state.segment->SetTpMsn(logicalTpMsn);
        state.segment->SetTpn(m_tpn);
    }

    const auto taOpcode = static_cast<TaOpcode>(taHeader.GetTaOpcode());
    const bool isReadRequest = taOpcode == TaOpcode::TA_OPCODE_READ;
    const bool isReadResponse = taOpcode == TaOpcode::TA_OPCODE_READ_RESPONSE;
    const bool isTransactionAck = taOpcode == TaOpcode::TA_OPCODE_TRANSACTION_ACK;
    state.segment->SetType(taOpcode);
    state.segment->SetOrderType(static_cast<OrderType>(taHeader.GetOrder()));
    state.segment->SetSegmentKind(
        isTransactionAck || isReadResponse
            ? UbTransactionSegmentKind::RESPONSE
            : UbTransactionSegmentKind::REQUEST);
    if (isTransactionAck)
    {
        state.segment->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
    }
    else if (isReadResponse)
    {
        state.segment->SetRequestOpcode(TaOpcode::TA_OPCODE_READ);
    }
    else
    {
        state.segment->SetRequestOpcode(taOpcode);
    }
    state.segment->SetResponseBytes(isReadRequest ? resLenBytes : (isReadResponse ? payloadBytes : 0));
    state.segment->SetNeedsTransactionResponse(
        taOpcode == TaOpcode::TA_OPCODE_WRITE || isReadRequest);

    state.bytesReceived += payloadBytes;
    if (isReadRequest)
    {
        state.segment->SetSize(resLenBytes);
        state.segment->SetWqeSize(resLenBytes);
        state.segment->SetResLenBytes(resLenBytes);
        state.segment->SetPayloadBytes(0);
        state.segment->SetCarrierBytes(1);
    }
    else
    {
        state.segment->SetSize(state.bytesReceived);
        state.segment->SetWqeSize(state.bytesReceived);
        state.segment->SetResLenBytes(state.bytesReceived);
        state.segment->SetPayloadBytes(state.bytesReceived);
        state.segment->SetCarrierBytes(state.bytesReceived);
    }

    if (!tpHeader.GetLastPacket())
    {
        return nullptr;
    }

    Ptr<UbWqeSegment> completed = state.segment;
    m_inboundTaUnits.erase(key);
    return completed;
}

bool UbTransportChannel::ShouldCompleteOnTpAck(const Ptr<UbWqeSegment>& segment) const
{
    if (segment == nullptr)
    {
        return false;
    }
    if (segment->GetSegmentKind() == UbTransactionSegmentKind::RESPONSE)
    {
        return false;
    }
    return !segment->NeedsTransactionResponse();
}

bool
UbTransportChannel::ResolveSelectiveAckBitmapBitsForTest(uint32_t& bits) const
{
    return m_retrans->ResolveSelectiveAckBitmapBits(bits);
}

uint64_t
UbTransportChannel::GetCumulativeAckPsn() const
{
    if (m_psnRecvNxt == 0)
    {
        return 0;
    }
    return m_psnRecvNxt - 1;
}

TpOpcode
UbTransportChannel::GetResponseOpcode(bool selectiveAck) const
{
    TpOpcode ackOpcode = m_congestionCtrl->GetAckOpcode();
    if (!selectiveAck)
    {
        return ackOpcode;
    }

    if (ackOpcode == TpOpcode::TP_OPCODE_ACK_WITH_CETPH)
    {
        return TpOpcode::TP_OPCODE_SACK_WITH_CETPH;
    }
    return TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH;
}

uint32_t
UbTransportChannel::GetPsnOooThresholdForRetrans() const
{
    return m_psnOooThreshold;
}

bool
UbTransportChannel::HasReceiveGapForRetrans() const
{
    return m_hasReceivedAnyPsn && m_maxRcvPsn >= m_psnRecvNxt;
}

uint64_t
UbTransportChannel::GetCumulativeAckPsnForRetrans() const
{
    return GetCumulativeAckPsn();
}

bool
UbTransportChannel::ReceiveWindowContainsForRetrans(uint64_t psn) const
{
    return m_recvPsnWindow.Contains(psn);
}

uint64_t
UbTransportChannel::GetMaxRcvPsnForRetrans() const
{
    return m_maxRcvPsn;
}

TpOpcode
UbTransportChannel::GetResponseOpcodeForRetrans(bool selectiveAck) const
{
    return GetResponseOpcode(selectiveAck);
}

void
UbTransportChannel::RetainSentPsnForTest(uint64_t psn, uint32_t payloadBytes)
{
    Ptr<Packet> packet = Create<Packet>(payloadBytes);
    UbTransportHeader tpHeader;
    tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RELIABLE_TA);
    tpHeader.SetSrcTpn(m_tpn);
    tpHeader.SetDestTpn(m_dstTpn);
    tpHeader.SetPsn(UbTpPsnSequence::ToWire(psn));
    packet->AddHeader(tpHeader);
    m_retrans->OnNewDataPacketSent(psn, packet, payloadBytes);
    m_psnSndNxt = std::max(m_psnSndNxt, psn + 1);
}

uint32_t
UbTransportChannel::GetPendingSelectiveRetransmissionCountForTest() const
{
    return m_retrans->GetPendingSelectiveRetransmissionCountForTest();
}

uint32_t
UbTransportChannel::GetRawSelectiveRetransmissionQueueCountForTest() const
{
    return m_retrans->GetRawSelectiveRetransmissionQueueCountForTest();
}

bool
UbTransportChannel::WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const
{
    return m_retrans->WasPsnSelectivelyReportedMissingForTest(psn);
}

bool
UbTransportChannel::HasRetainedPsnForTest(uint64_t psn) const
{
    return m_retrans->HasRetainedPsnForTest(psn);
}

uint32_t
UbTransportChannel::GetPsnRetransmitCountForTest(uint64_t psn) const
{
    return m_retrans->GetPsnRetransmitCountForTest(psn);
}

bool
UbTransportChannel::ParseTransportResponsePacket(Ptr<Packet> packet,
                                                 TransportResponseContext& ctx)
{
    if (packet == nullptr) {
        NS_LOG_ERROR("Null ack packet received");
        return false;
    }

    ctx.packet = packet;
    packet->RemoveHeader(ctx.transportHeader);
    ctx.opcode = static_cast<TpOpcode>(ctx.transportHeader.GetTPOpcode());
    ctx.isCnp = ctx.opcode == TpOpcode::TP_OPCODE_CNP;
    ctx.hasCetph = ctx.opcode == TpOpcode::TP_OPCODE_ACK_WITH_CETPH ||
                   ctx.opcode == TpOpcode::TP_OPCODE_SACK_WITH_CETPH;
    ctx.hasSaetph = ctx.opcode == TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH ||
                    ctx.opcode == TpOpcode::TP_OPCODE_SACK_WITH_CETPH;
    ctx.isTpnak = (ctx.opcode == TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH ||
                   ctx.opcode == TpOpcode::TP_OPCODE_ACK_WITH_CETPH) &&
                  ctx.transportHeader.GetRspSt() == 3;

    if (ctx.isCnp) {
        packet->RemoveHeader(ctx.cnpHeader);
        return true;
    }

    if (ctx.hasCetph) {
        packet->RemoveHeader(ctx.congestionHeader);
    }
    if (ctx.hasSaetph) {
        try
        {
            packet->RemoveHeader(ctx.selectiveAckHeader);
        }
        catch (const std::invalid_argument& e)
        {
            NS_LOG_WARN("Dropping malformed TPSACK: " << e.what());
            return false;
        }
    }
    packet->RemoveHeader(ctx.ackTransactionHeader);
    return true;
}

bool
UbTransportChannel::HandleReceivedCnp(const TransportResponseContext& ctx)
{
    if (!ctx.isCnp) {
        return false;
    }

    // CNP is a congestion-control path; it does not advance ACK state.
    const uint64_t cnpPsn = UbTpPsnSequence::Unwrap(ctx.transportHeader.GetPsn(), m_psnSndUna);
    UbCongestionExtTph notification;
    notification.SetAckSequence(0);
    notification.SetRawBytes4to7(
        (static_cast<uint32_t>(ctx.cnpHeader.GetEcn() & 0x3U) << 30) |
        (static_cast<uint32_t>(ctx.cnpHeader.GetLocation() ? 1U : 0U) << 29));
    m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_CNP,
                                                     UbTpPsnSequence::ToWire(cnpPsn),
                                                     notification);
    NS_LOG_DEBUG("Recv TP CNP");
    return true;
}

bool
UbTransportChannel::HandleReceivedTpNak(const TransportResponseContext& ctx)
{
    if (!ctx.isTpnak) {
        return false;
    }

    // TPNAK is negative feedback for retransmission; it does not enter ACK progress finalization.
    const uint64_t nakPsn =
        UbTpPsnSequence::Unwrap(ctx.transportHeader.GetPsn(), m_psnSndUna);
    NS_LOG_DEBUG("[Transport channel] Recv tpnak."
              << " PacketUid: " << ctx.packet->GetUid()
              << " Tpn: " << m_tpn
              << " Psn: " << nakPsn
              << " PacketType: Nak"
              << " Src: " << m_src
              << " Dst: " << m_dest
              << " PacketSize: " << ctx.packet->GetSize());
    if (m_pktTraceEnabled) {
        UbFlowTag flowTag;
        ctx.packet->PeekPacketTag(flowTag);
        UbPacketTraceTag traceTag;
        ctx.packet->PeekPacketTag(traceTag);
        TpRecvNotify(ctx.packet->GetUid(), UbTpPsnSequence::ToWire(nakPsn), m_dest, m_src, m_dstTpn, m_tpn,
                     PacketType::NAK, ctx.packet->GetSize(), flowTag.GetFlowId(),
                     FormatSimpleAckInfo("TPNAK", nakPsn), traceTag);
    }

    if (!IsRetransEnabled()) {
        if (ctx.hasCetph) {
            m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                                             UbTpPsnSequence::ToWire(nakPsn),
                                                             ctx.congestionHeader);
        }
        return true;
    }

    const UbRetransAckResult ackResult = m_retrans->OnTransportNak(ctx.transportHeader);
    if (ctx.hasCetph) {
        if (ackResult.triggerTransmit) {
            m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                                             UbTpPsnSequence::ToWire(nakPsn),
                                                             ctx.congestionHeader,
                                                             ackResult.retransmitBytes);
        } else {
            m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                                             UbTpPsnSequence::ToWire(nakPsn),
                                                             ctx.congestionHeader);
        }
    }
    if (ackResult.triggerTransmit) {
        TriggerTransportTransmit();
    }
    return true;
}

bool
UbTransportChannel::HandleReceivedAckOrSack(const TransportResponseContext& ctx,
                                            uint64_t previousSndUna,
                                            UbRetransAckResult& ackResult)
{
    if (ctx.hasCetph && !ctx.hasSaetph) {
        const uint64_t ackPsn =
            UbTpPsnSequence::Unwrap(ctx.transportHeader.GetPsn(), m_psnSndUna);
        m_congestionCtrl->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH,
                                                         UbTpPsnSequence::ToWire(ackPsn),
                                                         ctx.congestionHeader);
    }
    if (ctx.hasSaetph && m_pktTraceEnabled) {
        UbFlowTag flowTag;
        ctx.packet->PeekPacketTag(flowTag);
        UbPacketTraceTag traceTag;
        ctx.packet->PeekPacketTag(traceTag);
        TpRecvNotify(ctx.packet->GetUid(), ctx.transportHeader.GetPsn(),
                     m_dest, m_src, m_dstTpn, m_tpn,
                     PacketType::SACK, ctx.packet->GetSize(), flowTag.GetFlowId(),
                     FormatSelectiveAckInfo(ctx.transportHeader, ctx.selectiveAckHeader),
                     traceTag);
    }

    if (!ctx.hasSaetph) {
        ackResult = AdvanceSenderAckFromPlainTpack(ctx, previousSndUna);
        return true;
    }

    if (!IsRetransEnabled()) {
        ackResult.ignoreResponse = true;
        return false;
    }

    ackResult = m_retrans->OnTransportSelectiveAck(
        ctx.transportHeader,
        ctx.opcode,
        ctx.selectiveAckHeader,
        ctx.hasCetph ? &ctx.congestionHeader : nullptr);
    if (ackResult.ignoreResponse) {
        return false;
    }
    if (ackResult.triggerTransmit) {
        TriggerTransportTransmit();
    }
    return true;
}

UbRetransAckResult
UbTransportChannel::AdvanceSenderAckFromPlainTpack(const TransportResponseContext& ctx,
                                                   uint64_t previousSndUna)
{
    UbRetransAckResult result;
    result.previousSndUna = previousSndUna;

    const auto logicalAck =
        UbTpPsnSequence::UnwrapInWindow(ctx.transportHeader.GetPsn(),
                                        m_psnSndUna,
                                        m_psnSndNxt);
    if (!logicalAck.has_value()) {
        result.newSndUna = m_psnSndUna;
        return result;
    }

    const uint64_t ackedThrough = *logicalAck;
    if (ackedThrough + 1 > m_psnSndUna) {
        m_psnSndUna = ackedThrough + 1;
    }

    result.newSndUna = m_psnSndUna;
    result.ackAdvanced = result.newSndUna > result.previousSndUna;
    if (result.ackAdvanced && IsRetransEnabled()) {
        m_retrans->OnSenderCumulativeAckProgress(result.previousSndUna, result.newSndUna);
    }
    return result;
}

UbRetransReceiveDecision
UbTransportChannel::BuildCurrentAckDecision() const
{
    if (IsRetransEnabled()) {
        return m_retrans->BuildReceiveDecisionForCurrentState();
    }

    UbRetransReceiveDecision decision;
    decision.shouldAck = true;
    decision.responsePsn = GetCumulativeAckPsnForRetrans();
    decision.responseOpcode = GetResponseOpcodeForRetrans(false);
    return decision;
}

void
UbTransportChannel::CompleteAckedWqeSegments(const TransportResponseContext& ctx)
{
    for (size_t i = 0; i < m_wqeSegmentVector.size();) {
        Ptr<UbWqeSegment> segment = m_wqeSegmentVector[i];
        if (m_psnSndUna < segment->GetPsnStart() + segment->GetPsnSize()) {
            ++i;
            continue;
        }

        if (ctx.transportHeader.GetLastPacket()) {
            LastPacketACKsNotify(m_nodeId, segment->GetTaskId(), m_tpn, m_dstTpn,
                                 ctx.transportHeader.GetTpMsn(),
                                 ctx.transportHeader.GetPsn(),
                                 m_sport);
        }
        if (ShouldCompleteOnTpAck(segment)) {
            auto ubTa = GetTransaction();
            if (!ubTa->ProcessWqeSegmentComplete(segment)) {
                ++i;
                continue;
            }
            WqeSegmentCompletesNotify(m_nodeId,
                                      segment->GetTaskId(),
                                      UbTaSsnSequence::ToWire(segment->GetTaSsn()));
        }

        m_wqeSegmentVector.erase(m_wqeSegmentVector.begin() + i);
        if (CanScheduleAnotherSegment()) {
            ApplyNextWqeSegment();
        }
    }
}

void
UbTransportChannel::UpdateSenderAfterTransportAck(const TransportResponseContext&,
                                                  uint64_t)
{
    if (m_tpFullFlag && IsWqeSegmentLimited() == false) {
        m_tpFullFlag = false;
        ApplyNextWqeSegment();
    }
    if (IsRetransEnabled() && m_wqeSegmentVector.size() == 0) {
        m_retrans->CancelTimer();
    }

    const bool transportIdle = !HasPendingTransmitWork();
    if (transportIdle) {
        m_congestionCtrl->OnSenderTransportIdle();
    }
    if (!transportIdle && !m_congestionCtrl->IsCcLimited(UB_MTU_BYTE)) {
        Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
        port->TriggerTransmit();
    }
    NS_LOG_DEBUG("Recv TP(data packet) acknowledgment");
}

void
UbTransportChannel::FinalizeTransportAckProgress(const TransportResponseContext& ctx,
                                                 uint64_t previousSndUna)
{
    if (m_psnSndUna > previousSndUna) {
        if (m_sendWindowLimited && IsInflightLimited() == false) {
            if (!m_congestionCtrl->IsCcLimited(UB_MTU_BYTE)) {
                m_sendWindowLimited = false;
                Ptr<UbPort> port =
                    DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
                port->TriggerTransmit();
            }
        }
        NS_LOG_DEBUG("[Transport channel] Recv ack."
                  << " PacketUid: " << ctx.packet->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << m_psnSndUna - 1
                  << " PacketType: Ack"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ctx.packet->GetSize());
        if (m_pktTraceEnabled && !ctx.hasSaetph) {
            UbFlowTag flowTag;
            ctx.packet->PeekPacketTag(flowTag);
            UbPacketTraceTag traceTag;
            ctx.packet->PeekPacketTag(traceTag);
            TpRecvNotify(ctx.packet->GetUid(), UbTpPsnSequence::ToWire(m_psnSndUna - 1),
                         m_dest, m_src, m_dstTpn, m_tpn,
                         PacketType::ACK, ctx.packet->GetSize(), flowTag.GetFlowId(),
                         FormatSimpleAckInfo("TPACK", ctx.transportHeader.GetPsn()),
                         traceTag);
        }

        // Only real ACK progress resets the RTO state and reschedules timeout.
        if (IsRetransEnabled()) {
            m_retrans->RestartTimerAfterAckProgress();
        }
    }

    CompleteAckedWqeSegments(ctx);
    TraceTpDebugState(m_nodeId,
                      m_tpn,
                      "RECV_ACK",
                      m_psnSndNxt,
                      m_psnSndUna,
                      m_maxInflightPacketSize,
                      m_congestionCtrl->IsCcLimited(UB_MTU_BYTE),
                      m_sendWindowLimited,
                      GetActiveSendSegmentCount(),
                      static_cast<uint32_t>(m_wqeSegmentVector.size()),
                      static_cast<uint32_t>(m_ackQ.size()),
                      static_cast<uint32_t>(m_cnpQ.size()));
    UpdateSenderAfterTransportAck(ctx, previousSndUna);
}

/**
 * @brief Receive Transport Acknowledgment message
 * @param tpack Transport acknowledgment message to process
 * TP完成一个WQE后，产生TA ACK. 调用此函数将TA ACK传到TA
 */
void UbTransportChannel::RecvTpAck(Ptr<Packet> p)
{
    TransportResponseContext ctx;
    if (!ParseTransportResponsePacket(p, ctx)) {
        return;
    }

    if (HandleReceivedCnp(ctx)) {
        return;
    }

    if (HandleReceivedTpNak(ctx)) {
        return;
    }

    const uint64_t previousSndUna = m_psnSndUna;
    UbRetransAckResult ackResult;
    if (!HandleReceivedAckOrSack(ctx, previousSndUna, ackResult)) {
        return;
    }

    FinalizeTransportAckProgress(ctx, previousSndUna);
}


void UbTransportChannel::SetUbTransport(uint32_t nodeId,
                                        uint32_t src,
                                        uint32_t dest,
                                        uint32_t srcTpn,        // TP Number
                                        uint32_t dstTpn,
                                        uint64_t size,          // Size parameter
                                        uint16_t priority,      // Process group identifier
                                        uint16_t sport,
                                        uint16_t dport,
                                        Ipv4Address sip,         // Source IP address
                                        Ipv4Address dip,         // Dest IP address
                                        Ptr<UbCongestionControl> congestionCtrl)
{
    m_nodeId = nodeId;
    m_src = src;
    m_dest = dest;
    m_tpn = srcTpn;
    m_dstTpn = dstTpn;
    m_size = size;
    m_priority = priority;
    m_sport = sport;
    m_dport = dport;
    m_sip = sip;
    m_dip = dip;
    m_congestionCtrl = congestionCtrl;
    m_congestionCtrl->OnTpAttached(this);
    m_maxQueueSize = m_defaultMaxWqeSegNum;
    m_maxInflightPacketSize = m_defaultMaxInflightPacketSize;
    m_recvPsnWindow.Resize(m_psnOooThreshold);
    m_recvPsnWindow.Reset(m_psnRecvNxt);
}

bool
UbTransportChannel::ParseReceivedDataPacket(Ptr<Packet> packet,
                                            ReceivedDataPacketContext& ctx)
{
    if (packet == nullptr) {
        NS_LOG_ERROR("Null packet received");
        return false;
    }

    ctx.packet = packet;
    packet->RemoveHeader(ctx.dataLinkHeader);
    packet->RemoveHeader(ctx.networkHeader);
    packet->RemoveHeader(ctx.ipv4Header);
    packet->RemoveHeader(ctx.udpHeader);
    packet->RemoveHeader(ctx.transportHeader);
    packet->RemoveHeader(ctx.transactionHeader);
    packet->RemoveHeader(ctx.maExtHeader);
    ctx.payloadBytes = packet->GetSize();
    ctx.resLenBytes = ctx.maExtHeader.GetLength();
    ctx.psn = UbTpPsnSequence::Unwrap(ctx.transportHeader.GetPsn(), m_psnRecvNxt);
    packet->PeekPacketTag(ctx.flowTag);
    return true;
}

void
UbTransportChannel::TraceReceivedDataPacket(const ReceivedDataPacketContext& ctx)
{
    NS_LOG_DEBUG("[Transport channel] Recv packet."
                  << " PacketUid: "  << ctx.packet->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << ctx.psn
                  << " PacketType: Packet"
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << ctx.packet->GetSize());
    if (m_pktTraceEnabled) {
        UbPacketTraceTag traceTag;
        UbFlowTag flowTag = ctx.flowTag;
        ctx.packet->PeekPacketTag(traceTag);
        TpRecvNotify(ctx.packet->GetUid(), UbTpPsnSequence::ToWire(ctx.psn), m_dest, m_src, m_dstTpn, m_tpn,
                     PacketType::PACKET, ctx.packet->GetSize(), flowTag.GetFlowId(), "", traceTag);
    }
}

Ptr<Packet>
UbTransportChannel::BuildTransportResponsePacket(const ReceivedDataPacketContext& ctx,
                                                 const AckResponseContext& response)
{
    Ptr<Packet> responsePacket = Create<Packet>(0);
    responsePacket->AddPacketTag(ctx.flowTag);

    UbAckTransactionHeader ackTaHeader;
    ackTaHeader.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    ackTaHeader.SetIniTaSsn(ctx.transactionHeader.GetIniTaSsn());
    ackTaHeader.SetIniRcId(ctx.transactionHeader.GetIniRcId());

    UbTransportHeader tpHeader = ctx.transportHeader;
    tpHeader.SetTPOpcode(response.opcode);
    tpHeader.SetRspSt(response.rspSt);
    tpHeader.SetRspInfo(0);
    tpHeader.SetPsn(UbTpPsnSequence::ToWire(response.psn));
    tpHeader.SetSrcTpn(m_tpn);
    tpHeader.SetDestTpn(m_dstTpn);

    responsePacket->AddHeader(ackTaHeader);
    if (response.selectiveAck) {
        NS_ASSERT_MSG(response.selectiveAckHeader.has_value(),
                      "Selective response requires SAETPH.");
        responsePacket->AddHeader(*response.selectiveAckHeader);
    }
    if (response.congestionHeader.has_value()) {
        responsePacket->AddHeader(*response.congestionHeader);
    }
    responsePacket->AddHeader(tpHeader);
    responsePacket->AddHeader(ctx.udpHeader);
    UbPort::AddIpv4Header(responsePacket,
                          ctx.ipv4Header.GetDestination(),
                          ctx.ipv4Header.GetSource());
    responsePacket->AddHeader(ctx.networkHeader);
    UbDataLink::GenPacketHeader(responsePacket,
                                false,
                                true,
                                ctx.dataLinkHeader.GetCreditTargetVL(),
                                ctx.dataLinkHeader.GetPacketVL(),
                                0,
                                1,
                                UbDatalinkHeaderConfig::PACKET_IPV4);
    return responsePacket;
}

void
UbTransportChannel::EnqueueTransportResponse(Ptr<Packet> response,
                                             const char* logType,
                                             uint64_t psn)
{
    if (m_ackQ.empty()) {
        m_headArrivalTime = Simulator::Now();
    }
    m_ackQ.push(response);

    std::string packetType = logType;
    if (packetType == "tpnak") {
        packetType = "Nak";
    } else if (packetType == "ack") {
        packetType = "Ack";
    }
    NS_LOG_DEBUG("[Transport channel] Send " << logType << ". "
                  << " PacketUid: "  << response->GetUid()
                  << " Tpn: " << m_tpn
                  << " Psn: " << psn
                  << " PacketType: " << packetType
                  << " Src: " << m_src
                  << " Dst: " << m_dest
                  << " PacketSize: " << response->GetSize());
    TriggerTransportTransmit();
}

bool
UbTransportChannel::HandleImmediateRetransReceiveDecision(
    const ReceivedDataPacketContext& ctx,
    const UbRetransReceiveDecision& decision)
{
    if (decision.suppressResponse) {
        NS_LOG_DEBUG("Suppress repeated GBN TPNAK,tpn:{" << m_tpn << "} psn:{"
                     << decision.responsePsn << "}");
        return true;
    }
    if (!decision.shouldNak) {
        return false;
    }

    AckResponseContext response;
    response.opcode = decision.responseOpcode;
    response.rspSt = 3;
    response.psn = decision.responsePsn;
    if (response.opcode == TpOpcode::TP_OPCODE_ACK_WITH_CETPH) {
        response.congestionHeader =
            m_congestionCtrl->OnReceiverPrepareAckCongestionHeader(0, 0);
    }
    Ptr<Packet> responsePacket = BuildTransportResponsePacket(ctx, response);
    EnqueueTransportResponse(responsePacket, "tpnak", response.psn);
    return true;
}

bool
UbTransportChannel::HandleRepeatedDataPacket(const ReceivedDataPacketContext& ctx)
{
    if (!IsRepeatPacket(ctx.psn)) {
        return false;
    }

    const UbRetransReceiveDecision decision = BuildCurrentAckDecision();
    if (decision.suppressResponse) {
        if (decision.selectiveAck) {
            NS_LOG_WARN("Suppressing duplicate-packet TPSACK because SelectiveAckBitmapBits cannot be resolved");
        }
        return true;
    }

    AckResponseContext response;
    response.opcode = decision.selectiveAck ? decision.responseOpcode
                                            : TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH;
    response.psn = decision.responsePsn;
    response.selectiveAck = decision.selectiveAck;
    if (decision.selectiveAck) {
        NS_ASSERT_MSG(decision.selectiveAckHeader.has_value(),
                      "SELECTIVE receive decision requires SAETPH.");
        response.selectiveAckHeader = *decision.selectiveAckHeader;
    }
    if (response.opcode == TpOpcode::TP_OPCODE_SACK_WITH_CETPH) {
        response.congestionHeader =
            m_congestionCtrl->OnReceiverPrepareAckCongestionHeader(0, 0);
    }
    Ptr<Packet> responsePacket = BuildTransportResponsePacket(ctx, response);
    EnqueueTransportResponse(responsePacket, "ack", response.psn);
    return true;
}

void
UbTransportChannel::NotifyLastPacketReceived(const ReceivedDataPacketContext& ctx)
{
    if (!ctx.transportHeader.GetLastPacket()) {
        return;
    }

    LastPacketReceivesNotify(m_nodeId,
                             ctx.transportHeader.GetSrcTpn(),
                             ctx.transportHeader.GetDestTpn(),
                             ctx.transportHeader.GetTpMsn(),
                             UbTpPsnSequence::ToWire(ctx.psn),
                             m_dport);
}

bool
UbTransportChannel::UpdateReceiveWindowAndCollectCompletedTa(
    const ReceivedDataPacketContext& ctx,
    const UbRetransReceiveDecision& decision,
    uint64_t& psnStart,
    uint64_t& psnEnd,
    std::vector<Ptr<UbWqeSegment>>& completedTaUnits)
{
    if (ctx.psn < m_psnRecvNxt) {
        return true;
    }

    const bool outOfOrderPacket = ctx.psn > m_psnRecvNxt;
    if (!SetBitmap(ctx.psn)) {
        NS_LOG_WARN("Over Out-of-Order! Max Out-of-Order :" << m_psnOooThreshold);
        return false;
    }

    m_hasReceivedAnyPsn = true;
    m_maxRcvPsn = std::max(m_maxRcvPsn, ctx.psn);
    m_congestionCtrl->OnReceiverDataPacketReceived(ctx.psn,
                                                   ctx.payloadBytes,
                                                   ctx.networkHeader);
    UbFlowTag flowTag = ctx.flowTag;
    m_bufferedInboundPackets[ctx.psn] = {ctx.transportHeader,
                                         ctx.transactionHeader,
                                         ctx.resLenBytes,
                                         ctx.payloadBytes,
                                         flowTag.GetFlowId()};
    if (outOfOrderPacket) {
        NS_LOG_DEBUG("Out-of-Order Packet,tpn:{" << m_tpn << "} psn:{"
                     << ctx.psn << "} expectedPsn:{" << m_psnRecvNxt << "}");
        if (!IsRetransEnabled()) {
            return false;
        }
        return !decision.dropPacket;
    }

    uint64_t oldRecvNxt = m_psnRecvNxt;
    while (m_psnRecvNxt < oldRecvNxt + m_psnOooThreshold) {
        uint32_t currentBitIndex = static_cast<uint32_t>(m_psnRecvNxt - oldRecvNxt);
        if (currentBitIndex >= m_recvPsnWindow.GetWindowSize() ||
            !m_recvPsnWindow.Contains(m_psnRecvNxt)) {
            break;
        }

        auto bufferedIt = m_bufferedInboundPackets.find(m_psnRecvNxt);
        if (bufferedIt == m_bufferedInboundPackets.end()) {
            NS_LOG_WARN("Missing buffered inbound packet for contiguous psn " << m_psnRecvNxt
                        << " on tpn " << m_tpn);
            break;
        }

        Ptr<UbWqeSegment> completedTaUnit =
            TrackInboundTaPacket(bufferedIt->second.tpHeader,
                                 bufferedIt->second.taHeader,
                                 bufferedIt->second.resLenBytes,
                                 bufferedIt->second.payloadBytes,
                                 bufferedIt->second.taskId);
        if (completedTaUnit != nullptr) {
            completedTaUnits.push_back(completedTaUnit);
        }
        m_bufferedInboundPackets.erase(bufferedIt);
        m_psnRecvNxt++;
    }

    if (m_psnRecvNxt > oldRecvNxt) {
        NS_LOG_DEBUG("Updated m_psnRecvNxt from " << oldRecvNxt
                     << " to " << m_psnRecvNxt);
        if (IsRetransEnabled()) {
            m_retrans->ClearNakSuppressionIfGapClosed(m_psnRecvNxt);
        }
        uint32_t shiftCount = static_cast<uint32_t>(m_psnRecvNxt - oldRecvNxt);
        RightShiftBitset(shiftCount);
        psnStart = oldRecvNxt;
        psnEnd = m_psnRecvNxt;
    }
    return true;
}

bool
UbTransportChannel::BuildAckResponseFromDecision(
    const UbRetransReceiveDecision& decision,
    uint64_t psnStart,
    uint64_t psnEnd,
    AckResponseContext& response)
{
    if (decision.suppressResponse) {
        if (decision.selectiveAck) {
            NS_LOG_WARN("Suppressing TPSACK because SelectiveAckBitmapBits cannot be resolved");
        }
        return false;
    }

    response.opcode = decision.responseOpcode;
    response.psn = decision.responsePsn;
    response.selectiveAck = decision.selectiveAck;
    if (decision.selectiveAck) {
        NS_ASSERT_MSG(decision.selectiveAckHeader.has_value(),
                      "SELECTIVE receive decision requires SAETPH.");
        response.selectiveAckHeader = *decision.selectiveAckHeader;
    }

    UbCongestionExtTph congestionHeader =
        m_congestionCtrl->OnReceiverPrepareAckCongestionHeader(psnStart, psnEnd);
    if (response.opcode == TpOpcode::TP_OPCODE_ACK_WITH_CETPH ||
        response.opcode == TpOpcode::TP_OPCODE_SACK_WITH_CETPH) {
        response.congestionHeader = congestionHeader;
    }
    return true;
}

void
UbTransportChannel::CompleteInboundTaUnits(
    const std::vector<Ptr<UbWqeSegment>>& completedTaUnits)
{
    for (const Ptr<UbWqeSegment>& completedTaUnit : completedTaUnits) {
        if (completedTaUnit == nullptr) {
            continue;
        }
        GetTransaction()->HandleInboundTaUnit(m_tpn, completedTaUnit);
        WqeSegmentCompletesNotify(m_nodeId,
                                  completedTaUnit->GetTaskId(),
                                  UbTaSsnSequence::ToWire(completedTaUnit->GetTaSsn()));
    }
}

/**
 * @brief Receive Data Packets
 * @param tpack Transport acknowledgment message to process
 * TP接收到一个数据包的时候，调用此函数处理，产生tpack
 */
void UbTransportChannel::RecvDataPacket(Ptr<Packet> p)
{
    ReceivedDataPacketContext ctx;
    if (!ParseReceivedDataPacket(p, ctx)) {
        return;
    }

    TraceReceivedDataPacket(ctx);
    UbRetransReceiveDecision receiveDecision;
    if (IsRetransEnabled()) {
        receiveDecision = m_retrans->OnDataPacketReceived(ctx.psn);
    }
    if (HandleImmediateRetransReceiveDecision(ctx, receiveDecision)) {
        return;
    }
    NotifyLastPacketReceived(ctx);
    if (HandleRepeatedDataPacket(ctx)) {
        return;
    }
    uint64_t psnStart = 0;
    uint64_t psnEnd = 0;
    std::vector<Ptr<UbWqeSegment>> completedTaUnits;
    if (!UpdateReceiveWindowAndCollectCompletedTa(ctx,
                                                  receiveDecision,
                                                  psnStart,
                                                  psnEnd,
                                                  completedTaUnits)) {
        return;
    }
    AckResponseContext response;
    const UbRetransReceiveDecision ackDecision = BuildCurrentAckDecision();
    if (!BuildAckResponseFromDecision(ackDecision, psnStart, psnEnd, response)) {
        return;
    }

    NS_LOG_DEBUG("RecvDataPacket ready to send ack psn: " << response.psn << " node: " << m_src);
    Ptr<Packet> responsePacket = BuildTransportResponsePacket(ctx, response);
    EnqueueTransportResponse(responsePacket, "ack", response.psn);
    CompleteInboundTaUnits(completedTaUnits);
}

uint32_t
UbTransportChannel::GetGbnRetransmissionProgressBytesFromPsn(uint64_t psn) const
{
    uint64_t retransmitBytes = 0;
    for (const Ptr<UbWqeSegment>& segment : m_wqeSegmentVector) {
        if (segment == nullptr) {
            continue;
        }
        const uint64_t segmentStart = segment->GetPsnStart();
        const uint64_t segmentEnd = segmentStart + segment->GetPsnSize();
        const uint64_t overlapStart = std::max(psn, segmentStart);
        const uint64_t overlapEnd = std::min(m_psnSndNxt, segmentEnd);
        if (overlapStart >= overlapEnd) {
            continue;
        }
        const uint64_t segmentBytes = GetTotalProgressBytes(segment);
        const uint64_t startOffset =
            std::min(segmentBytes, (overlapStart - segmentStart) * UB_MTU_BYTE);
        const uint64_t endOffset =
            std::min(segmentBytes, (overlapEnd - segmentStart) * UB_MTU_BYTE);
        retransmitBytes += endOffset - startOffset;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(retransmitBytes, UINT32_MAX));
}

void
UbTransportChannel::ResetSegmentSendProgressFromPsn(uint64_t psn)
{
    for (size_t i = 0; i < m_wqeSegmentVector.size(); ++i) {
        Ptr<UbWqeSegment> currentSegment = m_wqeSegmentVector[i];
        const uint64_t segmentStart = currentSegment->GetPsnStart();
        const uint64_t segmentEnd = segmentStart + currentSegment->GetPsnSize();
        if (segmentEnd <= psn) {
            continue;
        }
        if (segmentStart <= psn) {
            const uint32_t resetSentBytes = (psn - segmentStart) * UB_MTU_BYTE;
            currentSegment->ResetSentBytes(resetSentBytes);
        } else {
            currentSegment->ResetSentBytes();
        }
        NS_LOG_INFO("GBN fast retransmit,taskId: " << currentSegment->GetTaskId()
                    << " psn: " << m_psnSndNxt);
    }
}

void UbTransportChannel::ReTxTimeout()
{
    const UbRetransTimeoutResult timeoutResult = m_retrans->OnTimeout();
    if (timeoutResult.triggerTransmit) {
        TriggerTransportTransmit();
    }
}

/**
 * @brief Get current queue size
 * @return Current number of WQEs in queue
 */
uint32_t UbTransportChannel::GetCurrentSqSize() const
{
    return m_wqeSegmentVector.size();
}

uint32_t UbTransportChannel::GetActiveSendSegmentCount() const
{
    uint32_t activeCount = 0;
    for (const Ptr<UbWqeSegment>& segment : m_wqeSegmentVector) {
        if (segment != nullptr && !segment->IsSentCompleted()) {
            ++activeCount;
        }
    }
    return activeCount;
}

uint32_t UbTransportChannel::GetOutstandingUnackedSegmentCount() const
{
    uint32_t outstandingCount = 0;
    for (const Ptr<UbWqeSegment>& segment : m_wqeSegmentVector) {
        if (segment != nullptr) {
            ++outstandingCount;
        }
    }
    return outstandingCount;
}

bool UbTransportChannel::CanScheduleAnotherSegment() const
{
    return GetActiveSendSegmentCount() < kShallowPipelineActiveSendDepth;
}

bool
UbTransportChannel::IsWqeSegmentLimited() const
{
    if (GetCurrentSqSize() >= m_maxQueueSize) {
        return true;
    }
    return false;
}

// 相当于发送窗口，应该与拥塞窗口取小值。目前尚未使用。
bool UbTransportChannel::IsInflightLimited() const
{
    if (m_psnSndNxt - m_psnSndUna >= m_maxInflightPacketSize) {
        return true;
    }
    return false;
}

/**
 * @brief Move right Bitset
 * @return
*/
void UbTransportChannel::RightShiftBitset(uint32_t shiftCount)
{
    (void)shiftCount;
    m_recvPsnWindow.AdvanceContiguous();
}

/**
  * @brief Set bitmap
  * @return Set the PSN position to 1
*/
bool UbTransportChannel::SetBitmap(uint64_t psn)
{
    return m_recvPsnWindow.Mark(psn);
}

/**
  * @brief IsRepeatPacket
  * @return
*/
bool UbTransportChannel::IsRepeatPacket(uint64_t psn)
{
    if (psn < m_psnRecvNxt) {
        return true;
    }
    if (psn >= m_recvPsnWindow.GetBase() + m_recvPsnWindow.GetWindowSize()) {
        return false;
    }
    return m_recvPsnWindow.Contains(psn);
}

void UbTransportChannel::WqeSegmentTriggerPortTransmit(Ptr<UbWqeSegment> segment)
{
    WqeSegmentSendsNotify(m_nodeId, segment->GetTaskId(), UbTaSsnSequence::ToWire(segment->GetTaSsn()));
    Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(m_nodeId)->GetDevice(m_sport));
    port->TriggerTransmit(); // 触发发送
}

Ptr<UbTransaction> UbTransportChannel::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

void UbTransportChannel::ApplyNextWqeSegment()
{
    GetTransaction()->ApplyScheduleWqeSegment(this);
}

bool UbTransportChannel::IsEmpty()
{
    return !HasPendingTransmitWork();
}

bool UbTransportChannel::HasPendingTransmitWork()
{
    if (!m_cnpQ.empty()) {
        return true;
    }
    if (!m_ackQ.empty()) {
        return true;
    }
    if (HasPendingSelectiveRetransmissionWork()) {
        return true;
    }
    if (m_wqeSegmentVector.empty()) {
        return false;
    }
    return m_psnSndNxt < m_tpPsnCnt;
}

bool UbTransportChannel::IsLimited()
{
    if (!m_cnpQ.empty()) {
        return false;
    }
    if (!m_ackQ.empty()) {
        return false;
    }
    if (HasPendingSelectiveRetransmissionWork()) {
        const uint32_t payloadBytes = m_retrans->GetNextSelectiveRetransmissionPayloadBytes();
        if (m_congestionCtrl->IsCcLimited(payloadBytes)) {
            m_sendWindowLimited = true;
            return true;
        }
        return false;
    }
    if (IsInflightLimited()) {
        m_sendWindowLimited = true;
        NS_LOG_DEBUG("Full Send Window");
        return true;
    }
    if (m_congestionCtrl->IsCcLimited(UB_MTU_BYTE)) {
        m_sendWindowLimited = true;
        return true;
    }
    return false;
}

IngressQueueType UbTransportChannel::GetIngressQueueType()
{
    return m_ingressQueueType;
}

void UbTransportChannel::FirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                                uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport)
{
    m_traceFirstPacketSendsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, mPsnSndNxt, mSport);
}

void UbTransportChannel::LastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                               uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport)
{
    m_traceLastPacketSendsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, mPsnSndNxt, mSport);
}

void UbTransportChannel::LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                              uint32_t mDstTpn, uint32_t tpMsn, uint32_t psn, uint32_t mSport)
{
    m_traceLastPacketACKsNotify(nodeId, taskId, mTpn, mDstTpn, tpMsn, psn, mSport);
}

void UbTransportChannel::LastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                                  uint32_t tpMsn, uint32_t psn, uint32_t mDport)
{
    m_traceLastPacketReceivesNotify(nodeId, srcTpn, dstTpn, tpMsn, psn, mDport);
}

void UbTransportChannel::WqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    m_traceWqeSegmentSendsNotify(nodeId, taskId, taSsn);
}

void UbTransportChannel::WqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    m_traceWqeSegmentCompletesNotify(nodeId, taskId, taSsn);
}

void UbTransportChannel::TpRecvNotify(uint32_t packetUid, uint32_t psn, uint32_t src, uint32_t dst,
                                      uint32_t srcTpn, uint32_t dstTpn, PacketType type,
                                      uint32_t size, uint32_t taskId, std::string ackInfo,
                                      UbPacketTraceTag traceTag)
{
    m_tpRecvNotify(packetUid, psn, src, dst, srcTpn, dstTpn, type, size, taskId, ackInfo, traceTag);
}

// ==========================================================================
// UbTransportGroup Implementation
// ==========================================================================

TypeId UbTransportGroup::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransportGroup")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbTransportGroup>();
    return tid;
}

UbTransportGroup::UbTransportGroup()
{
}

UbTransportGroup::~UbTransportGroup()
{
}

} // namespace ns3
