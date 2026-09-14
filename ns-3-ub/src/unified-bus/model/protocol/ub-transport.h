// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRANSPORT_H
#define UB_TRANSPORT_H

#include <memory>
#include <map>
#include <optional>
#include <queue>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ipv4-address.h"
#include "ns3/callback.h"
#include "ns3/ub-sliding-bitmap-window.h"
#include "ub-header.h"
#include "ns3/timer.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-retrans.h"
#include "ns3/ub-tag.h"
namespace ns3 {

class UbFunction;
class UbTransaction;
class UbController;
class UbPort;
class UbWqe;
class UbWqeSegment;
class UbJetty;

const uint32_t UB_TP_PSN_OOO_THRESHOLD = 2048;   // Jetty分段乱序阈值（用于乱序缓存等）

/**
 * @class UbTransportChannel, TP in short
 * @brief Transport layer class for managing communication between endpoints
 */
class UbTransportChannel : public UbIngressQueue {
public:
    /**
    * @brief Get TypeId for this class
     * @return TypeId
     */
    static TypeId GetTypeId(void);
    static bool IsTransportResponseOpcode(TpOpcode opcode);
    static bool IsTransportResponseOpcode(uint8_t opcode);

    /**
     * @brief Constructor for UbTransportChannel
     * @param node Node that owns this transport
     * @param tag Tag identifier for simulation

     * @param src Source identifier
     * @param dest Destination identifier
     * @param sport Source port
     * @param dport Destination port
     * @param tpn TP Number
     * @param size Size parameter
     * @param priority

     * @param sip Source IP address
     * @param dip Destination IP address

     * @param win Window size
     * @param baseRtt Base round trip time
     * @param notifyAppFinish Callback for application finish notification
     * @param notifyAppSent Callback for application sent notification
     */

    UbTransportChannel();

    /**
     * @brief Destructor for UbTransportChannel
     */
    virtual ~UbTransportChannel();

    /**
     * @brief Get Wqe Segment from Jetty related to this TP, do round robin

     * @return UbWqeSegment if succeed, nullptr otherwise
     */
    void GetNextWqeSegment();

    /**
     * @brief Send packet through specified port
     * @param pkt Packet to send
     * @param port Port to send through
     */
    Ptr<Packet> GetNextPacket() override;
    uint32_t GetNextPacketSize() override;
    bool IsEmpty() override;
    bool IsLimited() override;

    Ptr<Packet> GenDataPacket(Ptr<UbWqeSegment> wqeSegment,
                              uint32_t payloadSize,
                              uint32_t wireLengthBytes,
                              uint32_t progressBytes);

    void EnqueueDcqcnCnp(uint8_t ecn, bool location);
    Ptr<Packet> BuildDcqcnCnp(uint8_t ecn, bool location) const;

    /**
     * @brief Process Transport Acknowledgment message
     * @param tpack Transport acknowledgment message to process
     */
    void RecvTpAck(Ptr<Packet> p);

    void SetUbTransport(uint32_t nodeId,
                        uint32_t src,
                        uint32_t dest,
                        uint32_t srcTpn,        // TP Number
                        uint32_t dstTpn,
                        uint64_t size,          // Size parameter
                        uint16_t priority,      // Process group identifier
                        uint16_t sport,
                        uint16_t dport,
                        Ipv4Address sip,        // Source IP address
                        Ipv4Address dip,
                        Ptr<UbCongestionControl> congestionCtrl);

    /**
     * @brief Receive Data Packets
     * @param tpack Transport acknowledgment message to process
     */
    void RecvDataPacket(Ptr<Packet> p);

    /**
     * @brief apply ta for next wqesegment
     */
    void ApplyNextWqeSegment();

    void WqeSegmentTriggerPortTransmit(Ptr<UbWqeSegment> segment);

    void ReTxTimeout(); // Retransmit timeout

    /**
     * @brief Get current queue size
     * @return Current number of WQEs in queue
     */
    uint32_t GetCurrentSqSize() const;

    /**
    * @brief Check if queue of WQE Segment is full
    * @return True if queue is at maximum capacity
    */
    bool IsWqeSegmentLimited() const;

    void SetTpFullStatus(bool status) { m_tpFullFlag = status; }
    /**
    * @brief Check if inflight packets exceed maximum limit
    * @return True if inflight packets exceed maximum limit
    */
    bool IsInflightLimited() const;

    /**
     * @brief Create jetty and tp relationship
     * @return
     */
    void CreateTpJettyRelationship(Ptr<UbJetty> ubJetty);

    void DeleteTpJettyRelationship(uint32_t jettyNum);

    /**
     * @brief Get TP Number
     * @return TP Number
     */
    uint32_t GetTpn() const { return m_tpn; }

    /**
     * @brief Get the destination transport path number.
     * @return Destination TPN carried by this transport endpoint.
     */
    uint32_t GetDestTpn() const
    {
        return m_dstTpn;
    }

    /**
     * @brief Get size parameter
     * @return Size parameter
     */
    uint64_t GetSize() const { return m_size; }

    /**
     * @brief Get process group identifier
     * @return Process group identifier
     */
    uint16_t GetPriority() const { return m_priority; }

    /**
     * @brief Get source IP address
     * @return Source IP address
     */
    Ipv4Address GetSip() const { return m_sip; }

    /**
     * @brief Get destination IP address
     * @return Destination IP address
     */
    Ipv4Address GetDip() const { return m_dip; }

    /**
     * @brief Get source port
     * @return Source port
     */
    uint16_t GetSport() const { return m_sport; }

    /**
     * @brief Get udp source port
     * @return Udp Source port
     */
    uint16_t GetUdpSport() const { return m_lbHashSalt; }

    /**
     * @brief Get destination port
     * @return Destination port
     */
    uint16_t GetDport() const { return m_dport; }

    /**
    * @brief Move right Bitset
    * @return
    */
    void RightShiftBitset(uint32_t shiftCount);

    /**
     * @brief Set bitmap
     * @return Set the PSN position to 1
     */
    bool SetBitmap(uint64_t psn);

    /**
     * @brief IsRepeatPacket
     * @return
     */
    bool IsRepeatPacket(uint64_t psn);
    std::queue<Ptr<Packet>> m_ackQ; // ack queue high pg
    std::queue<Ptr<Packet>> m_cnpQ; // higher-priority control queue for standalone CNP

    virtual IngressQueueType GetIngressQueueType() override;

    /**
     * @brief Get hash salt for packet-spray or ECMP
     * @return hash salt
     */
    uint16_t GetLbHashSalt() const { return m_lbHashSalt; }

    uint32_t GetSrc() { return m_src; }
    uint32_t GetDest() { return m_dest; }

    uint64_t GetMsnCnt() { return m_tpMsnCnt; }
    uint32_t GetDstTpn() { return m_dstTpn; }
    void UpDateMsnCnt(uint32_t num) { m_tpMsnCnt += num; }

    uint64_t GetPsnCnt() { return m_tpPsnCnt; }

    void UpdatePsnCnt(uint32_t num) { m_tpPsnCnt += num; }
    void PushWqeSegment(Ptr<UbWqeSegment> segment) {
        if (m_wqeSegmentVector.empty() && m_ackQ.empty() && m_cnpQ.empty()) {
            m_headArrivalTime = Simulator::Now();
        }
        m_wqeSegmentVector.push_back(segment);
    }

    uint32_t GetWqeSegmentVecSize() { return m_wqeSegmentVector.size(); }
    uint32_t GetActiveSendSegmentCount() const;
    uint32_t GetOutstandingUnackedSegmentCount() const;
    bool CanScheduleAnotherSegment() const;

    Ptr<UbCongestionControl> GetCongestionCtrlForTest() const { return m_congestionCtrl; }
    void SetCongestionControlForTest(Ptr<UbCongestionControl> cc) { m_congestionCtrl = cc; }
    void EnqueueAckForTest(Ptr<Packet> p) { m_ackQ.push(p); }
    void EnqueueCnpForTest(Ptr<Packet> p) { m_cnpQ.push(p); }
    Ptr<Packet> GetNextPacketForTest() { return GetNextPacket(); }
    uint32_t GetPendingAckCountForTest() const { return static_cast<uint32_t>(m_ackQ.size()); }
    Ptr<Packet> PopAckForTest() { return GetNextPacket(); }
    uint32_t GetPendingCnpCountForTest() const { return static_cast<uint32_t>(m_cnpQ.size()); }
    uint64_t GetPsnSndUnaForTest() const { return m_psnSndUna; }
    void SetPsnSndUnaForTest(uint64_t psn) { m_psnSndUna = psn; }
    uint64_t GetPsnSndNxtForTest() const { return m_psnSndNxt; }
    void SetPsnSndNxtForTest(uint64_t psn) { m_psnSndNxt = psn; }
    uint64_t GetPsnRecvNxtForTest() const { return m_psnRecvNxt; }
    void SetPsnRecvNxtForTest(uint64_t psn)
    {
        m_psnRecvNxt = psn;
        m_maxRcvPsn = psn == 0 ? 0 : psn - 1;
        m_recvPsnWindow.Reset(m_psnRecvNxt);
    }
    void SetInboundTpMsnReferenceForTest(uint32_t srcTpn, uint64_t reference)
    {
        m_inboundTpMsnReferences[srcTpn] = reference;
    }
    void SetInboundTaSsnReferenceForTest(uint32_t srcTpn, uint32_t iniRcId, uint32_t reference)
    {
        m_inboundTaSsnReferences[std::make_pair(srcTpn, iniRcId)] = reference;
    }
    uint32_t GetInboundTaUnitCountForTest() const
    {
        return static_cast<uint32_t>(m_inboundTaUnits.size());
    }
    bool ResolveSelectiveAckBitmapBitsForTest(uint32_t& bits) const;
    void RetainSentPsnForTest(uint64_t psn, uint32_t payloadBytes);
    uint32_t GetPendingSelectiveRetransmissionCountForTest() const;
    uint32_t GetRawSelectiveRetransmissionQueueCountForTest() const;
    bool WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const;
    bool HasRetainedPsnForTest(uint64_t psn) const;
    uint32_t GetPsnRetransmitCountForTest(uint64_t psn) const;

    void SetBaseRto(Time rto);
    Time GetBaseRto() const;
    void SetMaxRetransAttempts(uint16_t attempts);
    uint16_t GetMaxRetransAttempts() const;
    void SetRetransExponentFactor(uint16_t factor);
    uint16_t GetRetransExponentFactor() const;
    void SetRetransTimeoutMode(UbRetransTimeoutMode mode);
    UbRetransTimeoutMode GetRetransTimeoutMode() const;
    void SetRetransmissionMode(UbRetransmissionMode mode);
    UbRetransmissionMode GetRetransmissionMode() const;
    void SetRetransEnable(bool enable);
    bool GetRetransEnable() const;
    void SetSelectiveAckBitmapBits(uint32_t bits);
    uint32_t GetSelectiveAckBitmapBitsConfig() const;
    void SetFastRetransEnable(bool enable);
    bool GetFastRetransEnable() const;
    void SetSelectiveMarkPsnEnable(bool enable);
    bool GetSelectiveMarkPsnEnable() const;
    uint64_t GetPsnSndUna() const;
    void SetPsnSndUna(uint64_t psn);
    uint64_t GetPsnSndNxt() const;
    void SetPsnSndNxt(uint64_t psn);
    uint64_t GetPsnRecvNxt() const;
    uint32_t GetGbnRetransmissionProgressBytesFromPsn(uint64_t psn) const;
    void ResetSegmentSendProgressFromPsn(uint64_t psn);
    void TriggerTransportTransmit();
    bool IsCcLimitedForRetransmission(uint32_t payloadBytes) const;
    void SetSendWindowLimited(bool limited);
    void OnSelectiveRetransmissionPacketSent(uint64_t psn, uint32_t payloadBytes);
    void OnSenderReceivesTpsackCongestionFeedback(uint64_t psn,
                                                  const UbCongestionExtTph& cetph,
                                                  uint32_t retransmitBytes);
    uint32_t GetPsnOooThresholdForRetrans() const;
    bool HasReceiveGapForRetrans() const;
    uint64_t GetCumulativeAckPsnForRetrans() const;
    bool ReceiveWindowContainsForRetrans(uint64_t psn) const;
    uint64_t GetMaxRcvPsnForRetrans() const;
    TpOpcode GetResponseOpcodeForRetrans(bool selectiveAck) const;
private:
    struct InboundTaUnitState
    {
        Ptr<UbWqeSegment> segment;
        uint32_t bytesReceived{0};
    };

    struct BufferedInboundPacket
    {
        UbTransportHeader tpHeader;
        UbTransactionHeader taHeader;
        uint32_t resLenBytes{0};
        uint32_t payloadBytes{0};
        uint32_t taskId{0};
    };

    struct ReceivedDataPacketContext
    {
        Ptr<Packet> packet;
        UbDatalinkPacketHeader dataLinkHeader;
        UbIpBasedNetworkHeader networkHeader;
        Ipv4Header ipv4Header;
        UdpHeader udpHeader;
        UbTransportHeader transportHeader;
        UbTransactionHeader transactionHeader;
        UbMAExtTah maExtHeader;
        UbFlowTag flowTag;
        uint64_t psn{0};
        uint32_t payloadBytes{0};
        uint32_t resLenBytes{0};
    };

    struct AckResponseContext
    {
        TpOpcode opcode{TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH};
        uint8_t rspSt{0};
        uint64_t psn{0};
        bool selectiveAck{false};
        std::optional<UbSelectiveAckExtTph> selectiveAckHeader;
        std::optional<UbCongestionExtTph> congestionHeader;
    };

    struct TransportResponseContext
    {
        Ptr<Packet> packet;
        UbTransportHeader transportHeader;
        UbAckTransactionHeader ackTransactionHeader;
        UbCongestionExtTph congestionHeader;
        UbSelectiveAckExtTph selectiveAckHeader;
        UbCnpExtTph cnpHeader;
        TpOpcode opcode{TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH};
        bool hasCetph{false};
        bool hasSaetph{false};
        bool isTpnak{false};
        bool isCnp{false};
    };

    struct NewDataSendContext
    {
        Ptr<UbWqeSegment> segment;
        uint32_t progressBytes{0};
        uint32_t payloadBytes{0};
        uint32_t wireLengthBytes{0};
        uint32_t totalProgressBytes{0};
    };

    void DoDispose() override;

    bool IsRetransEnabled() const;
    bool HasPendingSelectiveRetransmissionWork() const;
    bool HasPendingTransmitWork();
    Ptr<Packet> PopQueuedPacket(std::queue<Ptr<Packet>>& packetQ);
    Ptr<Packet> TryGetNextNewDataPacket();
    bool CanTrySendNewDataPacket();
    bool BuildNextDataSendContext(NewDataSendContext& ctx);
    Ptr<Packet> SendNewDataPacket(const NewDataSendContext& ctx);
    void NotifyNewDataPacketSent(const NewDataSendContext& ctx, Ptr<Packet> packet);
    void AdvanceNewDataSendState(const NewDataSendContext& ctx, Ptr<Packet> packet);
    Ptr<UbTransaction> GetTransaction();
    Ptr<UbWqeSegment> TrackInboundTaPacket(const UbTransportHeader& tpHeader,
                                           const UbTransactionHeader& taHeader,
                                           uint32_t resLenBytes,
                                           uint32_t payloadBytes,
                                           uint32_t taskId);
    bool ShouldCompleteOnTpAck(const Ptr<UbWqeSegment>& segment) const;
    uint64_t GetCumulativeAckPsn() const;
    TpOpcode GetResponseOpcode(bool selectiveAck) const;
    bool ParseReceivedDataPacket(Ptr<Packet> packet, ReceivedDataPacketContext& ctx);
    bool ParseTransportResponsePacket(Ptr<Packet> packet, TransportResponseContext& ctx);
    bool HandleReceivedCnp(const TransportResponseContext& ctx);
    bool HandleReceivedTpNak(const TransportResponseContext& ctx);
    bool HandleReceivedAckOrSack(const TransportResponseContext& ctx,
                                 uint64_t previousSndUna,
                                 UbRetransAckResult& ackResult);
    UbRetransAckResult AdvanceSenderAckFromPlainTpack(const TransportResponseContext& ctx,
                                                      uint64_t previousSndUna);
    UbRetransReceiveDecision BuildCurrentAckDecision() const;
    void FinalizeTransportAckProgress(const TransportResponseContext& ctx,
                                      uint64_t previousSndUna);
    void CompleteAckedWqeSegments(const TransportResponseContext& ctx);
    void UpdateSenderAfterTransportAck(const TransportResponseContext& ctx,
                                       uint64_t previousSndUna);
    void TraceReceivedDataPacket(const ReceivedDataPacketContext& ctx);
    Ptr<Packet> BuildTransportResponsePacket(const ReceivedDataPacketContext& ctx,
                                             const AckResponseContext& response);
    void EnqueueTransportResponse(Ptr<Packet> response, const char* logType, uint64_t psn);
    bool HandleImmediateRetransReceiveDecision(const ReceivedDataPacketContext& ctx,
                                               const UbRetransReceiveDecision& decision);
    bool HandleRepeatedDataPacket(const ReceivedDataPacketContext& ctx);
    void NotifyLastPacketReceived(const ReceivedDataPacketContext& ctx);
    bool UpdateReceiveWindowAndCollectCompletedTa(
        const ReceivedDataPacketContext& ctx,
        const UbRetransReceiveDecision& decision,
        uint64_t& psnStart,
        uint64_t& psnEnd,
        std::vector<Ptr<UbWqeSegment>>& completedTaUnits);
    bool BuildAckResponseFromDecision(const UbRetransReceiveDecision& decision,
                                      uint64_t psnStart,
                                      uint64_t psnEnd,
                                      AckResponseContext& response);
    void CompleteInboundTaUnits(const std::vector<Ptr<UbWqeSegment>>& completedTaUnits);

    TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> m_traceFirstPacketSendsNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> m_traceLastPacketSendsNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> m_traceLastPacketACKsNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> m_traceLastPacketReceivesNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_traceWqeSegmentSendsNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_traceWqeSegmentCompletesNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                   uint32_t, PacketType, uint32_t, uint32_t, std::string, UbPacketTraceTag> m_tpRecvNotify;
    TracedCallback<uint32_t, uint32_t, uint64_t, uint32_t> m_traceSelectiveRetransmit;

    void FirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                                uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport);
    void LastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                               uint32_t mDstTpn, uint32_t tpMsn, uint32_t mPsnSndNxt, uint32_t mSport);
    void LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t mTpn,
                              uint32_t mDstTpn, uint32_t tpMsn, uint32_t psn, uint32_t mSport);
    void LastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                  uint32_t tpMsn, uint32_t psn, uint32_t mDport);
    void WqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);
    void WqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);
    void TpRecvNotify(uint32_t packetUid, uint32_t psn, uint32_t src, uint32_t dst, uint32_t srcTpn, uint32_t dstTpn,
                      PacketType type, uint32_t size, uint32_t taskId, std::string ackInfo, UbPacketTraceTag traceTag);
    // Node and controller references
    uint32_t m_nodeId;

    // Network identification parameters
    uint32_t m_src;
    uint32_t m_dest;
    uint32_t m_tpn;           // TP Number
    uint32_t m_dstTpn;
    uint64_t m_size;          // Size parameter
    uint16_t m_priority;      // Process group identifier
    uint16_t m_sport;
    uint16_t m_dport;

    // IP addresses
    Ipv4Address m_sip;        // Source IP address
    Ipv4Address m_dip;        // Destination IP address

    std::vector<Ptr<UbWqeSegment>> m_remoteRequest; // FIFO
    std::map<std::pair<uint32_t, uint64_t>, InboundTaUnitState> m_inboundTaUnits;
    std::map<uint32_t, uint64_t> m_inboundTpMsnReferences;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> m_inboundTaSsnReferences;
    std::map<uint64_t, BufferedInboundPacket> m_bufferedInboundPackets;

    // Queue parameters
    uint32_t m_maxQueueSize;

    uint32_t m_maxInflightPacketSize;
    std::vector<Ptr<UbWqeSegment>> m_wqeSegmentVector; // FIFO
    /// TP1: ->port1 0 1 2 (3 4) 5 6
    ///     |->port2        3 4
    Ptr<UbCongestionControl> m_congestionCtrl;
    // ========== TP层动态信息 (发送过程中更新) ==========
    uint64_t        m_psnSndNxt = 0;      // TP层下一个待发送的包序号
    uint64_t        m_psnSndUna = 0;      // TP层未确认的最小包序号
    uint64_t        m_psnRecvNxt { 0 };   // TP层记录最大顺序包序号
    uint64_t        m_maxRcvPsn { 0 };
    bool            m_hasReceivedAnyPsn { false };
    uint64_t        m_tpMsnCnt {0};       // TP层总计获取的消息(WQE Segment)计数
    uint64_t        m_tpPsnCnt {0};       // TP层总计获取的数据包个数计数
    static constexpr uint32_t DEFAULT_OOO_THRESHOLD = 2048;
    uint32_t m_psnOooThreshold = DEFAULT_OOO_THRESHOLD;
    UbSlidingBitmapWindow m_recvPsnWindow{DEFAULT_OOO_THRESHOLD};

    // Status flags
    bool m_isActive = true;
    bool m_tpFullFlag = false; // 记录tp队列状态是否满
    bool m_sendWindowLimited = false; // 记录发送窗口是否满
    uint64_t m_defaultMaxWqeSegNum;
    uint64_t m_defaultMaxInflightPacketSize;
    bool m_usePacketSpray;
    bool m_useShortestPaths;
    uint16_t m_lbHashSalt = 0; // load balance salt for ECMP/packet-spray hashing, increases per packet

    std::unique_ptr<UbRetransController> m_retrans;

    // Callback functions
    IngressQueueType m_ingressQueueType = IngressQueueType::TP; // Transport channel queue type (TP)

    bool m_pktTraceEnabled = false;
};

/**
 * @class UbTransportGroup
 * @brief Group of UbTransportChannel objects
 */
class UbTransportGroup : public Object {
public:
    /**
     * @enum
     * @brief Scheduling algorithms for Transport selection
     */
    static TypeId GetTypeId(void);

    UbTransportGroup();
    virtual ~UbTransportGroup();
};

} // namespace ns3

#endif /* UB_TRANSPORT_H */
