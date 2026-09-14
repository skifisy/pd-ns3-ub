// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_RETRANS_H
#define UB_RETRANS_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-header.h"
#include "ns3/packet.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ns3 {

class UbTransportChannel;
class UbRetransController;

struct UbRetransAckResult
{
    bool ackAdvanced{false};
    bool ignoreResponse{false};
    uint64_t previousSndUna{0};
    uint64_t newSndUna{0};
    bool triggerTransmit{false};
    uint32_t retransmitBytes{0};
};

struct UbRetransReceiveDecision
{
    bool dropPacket{false};
    bool shouldAck{false};
    bool shouldNak{false};
    bool selectiveAck{false};
    bool suppressResponse{false};
    uint64_t responsePsn{0};
    TpOpcode responseOpcode{TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH};
    std::optional<UbSelectiveAckExtTph> selectiveAckHeader;
};

struct UbRetransTimeoutResult
{
    bool triggerTransmit{false};
};

class UbRetransStrategy
{
public:
    virtual ~UbRetransStrategy() = default;
};

class UbGbnRetransStrategy : public UbRetransStrategy
{
public:
    explicit UbGbnRetransStrategy(UbRetransController& controller);

    void PrepareRetransmissionFromPsn(uint64_t psn);
    bool HandleTpNak(uint64_t nakPsn);
    UbRetransReceiveDecision OnDataPacketReceived(uint64_t psn);
    UbRetransTimeoutResult OnTimeout();
    void ClearNakSuppressionIfGapClosed(uint64_t recvNext);

private:
    UbRetransController& m_controller;
    uint64_t m_lastNakPsn{std::numeric_limits<uint64_t>::max()};
};

class UbSelectiveRetransStrategy : public UbRetransStrategy
{
public:
    explicit UbSelectiveRetransStrategy(UbRetransController& controller);

    void RetainSentPsn(uint64_t psn,
                       Ptr<Packet> packet,
                       uint32_t payloadBytes);
    void MarkPsnAcked(uint64_t psn);
    void AcknowledgeCumulativePsn(uint64_t ackPsn);
    void OnCumulativeAckProgress(uint64_t previousSndUna, uint64_t newSndUna);
    void AdvanceSendUnaFromAckState();
    std::vector<uint64_t> CollectMissingPsnsFromSelectiveAck(uint64_t ackBase,
                                                             const UbSelectiveAckExtTph& saetph);
    std::vector<uint64_t> GetMissingPsnsFromSelectiveAck(uint64_t ackBase,
                                                         const UbSelectiveAckExtTph& saetph) const;
    bool QueueRetransmission(uint64_t psn);
    void CompactRetransmissionQueue();
    Ptr<Packet> TryGetNextRetransmissionPacket();
    UbRetransTimeoutResult OnTimeout();
    UbRetransAckResult OnTransportResponse(const UbTransportHeader& tpHeader,
                                           TpOpcode opcode,
                                           const UbSelectiveAckExtTph& saetph,
                                           const UbCongestionExtTph* cetph);
    UbRetransAckResult OnCumulativeAck(const UbTransportHeader& tpHeader);
    bool ResolveSelectiveAckBitmapBits(uint32_t& bits) const;
    uint32_t GetSelectiveAckBitmapBits() const;
    uint64_t GetSelectiveAckBase() const;
    UbSelectiveAckExtTph BuildSelectiveAckHeader(uint64_t ackBase) const;
    UbRetransReceiveDecision BuildReceiveDecisionForCurrentState() const;
    void OnNewDataPacketSent(uint64_t psn,
                             Ptr<Packet> packet,
                             uint32_t payloadBytes);
    bool HasPendingRetransmission() const;
    bool CanSendRetransmission() const;
    uint32_t GetNextRetransmissionSize() const;
    uint32_t GetNextRetransmissionPayloadBytes() const;
    uint32_t GetRetainedPayloadBytes(uint64_t psn) const;

    uint32_t GetPendingRetransmissionCountForTest() const;
    uint32_t GetRawRetransmissionQueueCountForTest() const;
    bool WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const;
    bool HasRetainedPsnForTest(uint64_t psn) const;
    uint32_t GetPsnRetransmitCountForTest(uint64_t psn) const;
    void Clear();

private:
    struct SentPsnState
    {
        Ptr<Packet> packet;
        uint32_t payloadBytes{0};
        bool acknowledged{false};
        bool selectivelyReportedMissing{false};
        bool retransmitPending{false};
        uint32_t retransmitCount{0};
    };

    void RetireAckedStateBeforeSendUna();
    bool IsMarkPsnEnabled() const;
    bool SelectiveAckReportsReceivedAtOrAboveMarkPsn(uint64_t ackBase,
                                                     const UbSelectiveAckExtTph& saetph) const;
    void EnterMarkPsnRetransPhase();
    void FinishMarkPsnRetransPhaseIfDone();
    void MaybeMarkFirstNewSelectivePacket(uint64_t psn);

    UbRetransController& m_controller;
    std::map<uint64_t, SentPsnState> m_sentPsnState;
    std::deque<uint64_t> m_selectiveRetransmitQ;
    bool m_markPsnRetransPhase{false};
    bool m_markPsnAwaitingFirstNew{true};
    bool m_markPsnValid{false};
    uint64_t m_markPsn{0};
    bool m_lastFirstRtxPsnValid{false};
    uint64_t m_lastFirstRtxPsn{0};
};

class UbRetransController
{
public:
    explicit UbRetransController(UbTransportChannel& transport);
    ~UbRetransController();

    void SetBaseRto(Time rto);
    Time GetBaseRto() const;

    void SetCurrentRto(Time rto);
    Time GetCurrentRto() const;

    void SetMaxRetransAttempts(uint16_t attempts);
    uint16_t GetMaxRetransAttempts() const;

    void SetRetransAttemptsLeft(uint16_t attempts);
    uint16_t GetRetransAttemptsLeft() const;

    void SetRetransExponentFactor(uint16_t factor);
    uint16_t GetRetransExponentFactor() const;

    void SetRetransTimeoutMode(UbRetransTimeoutMode mode);
    UbRetransTimeoutMode GetRetransTimeoutMode() const;

    void SetRetransmissionMode(UbRetransmissionMode mode);
    UbRetransmissionMode GetRetransmissionMode() const;

    void SetSelectiveAckBitmapBits(uint32_t bits);
    uint32_t GetSelectiveAckBitmapBitsConfig() const;

    void SetFastRetransEnable(bool enable);
    bool GetFastRetransEnable() const;

    void SetSelectiveMarkPsnEnable(bool enable);
    bool GetSelectiveMarkPsnEnable() const;

    void SetRetransEnable(bool enable);
    bool GetRetransEnable() const;
    void StartTimerIfNeeded();
    void RestartTimerAfterAckProgress();
    void CancelTimer();
    UbRetransTimeoutResult OnTimeout();
    bool HasTimerRunning() const;
    UbRetransAckResult OnTransportNak(const UbTransportHeader& tph);
    UbRetransAckResult OnTransportSelectiveAck(const UbTransportHeader& tph,
                                               TpOpcode opcode,
                                               const UbSelectiveAckExtTph& saetph,
                                               const UbCongestionExtTph* cetph);
    void OnSenderCumulativeAckProgress(uint64_t previousSndUna, uint64_t newSndUna);
    UbRetransReceiveDecision OnDataPacketReceived(uint64_t psn);
    bool ResolveSelectiveAckBitmapBits(uint32_t& bits) const;
    UbRetransReceiveDecision BuildReceiveDecisionForCurrentState() const;
    void OnNewDataPacketSent(uint64_t psn,
                             Ptr<Packet> packet,
                             uint32_t payloadBytes);
    Ptr<Packet> TryGetNextRetransmissionPacket();
    bool HasPendingSelectiveRetransmission() const;
    bool CanSendSelectiveRetransmission() const;
    uint32_t GetNextSelectiveRetransmissionSize() const;
    uint32_t GetNextSelectiveRetransmissionPayloadBytes() const;
    uint32_t GetPendingSelectiveRetransmissionCountForTest() const;
    uint32_t GetRawSelectiveRetransmissionQueueCountForTest() const;
    bool WasPsnSelectivelyReportedMissingForTest(uint64_t psn) const;
    bool HasRetainedPsnForTest(uint64_t psn) const;
    uint32_t GetPsnRetransmitCountForTest(uint64_t psn) const;
    void ClearRetainedState();
    void ClearNakSuppressionIfGapClosed(uint64_t recvNext);
    UbTransportChannel& GetTransport();
    const UbTransportChannel& GetTransport() const;

private:
    void ScheduleTimeout();

    UbTransportChannel& m_transport;
    std::unique_ptr<UbGbnRetransStrategy> m_gbn;
    std::unique_ptr<UbSelectiveRetransStrategy> m_selective;
    EventId m_retransEvent{};
    Time m_baseRto{NanoSeconds(25600)};
    Time m_rto{NanoSeconds(25600)};
    uint16_t m_maxRetransAttempts{7};
    uint16_t m_retransAttemptsLeft{7};
    uint16_t m_retransExponentFactor{1};
    UbRetransTimeoutMode m_retransTimeoutMode{UbRetransTimeoutMode::DYNAMIC};
    UbRetransmissionMode m_retransmissionMode{UbRetransmissionMode::GBN};
    uint32_t m_selectiveAckBitmapBits{0};
    bool m_enableRetrans{false};
    bool m_enableFastRetrans{false};
    bool m_enableSelectiveMarkPsn{false};
};

} // namespace ns3

#endif // UB_RETRANS_H
