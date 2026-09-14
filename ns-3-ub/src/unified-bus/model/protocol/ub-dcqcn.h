// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_DCQCN_H
#define UB_DCQCN_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/data-rate.h"
#include "ns3/ub-congestion-control.h"
#include <cstdint>

namespace ns3 {

class Packet;
class UbSwitch;
class UbTransportChannel;
class UbPort;
class UniformRandomVariable;

class UbDcqcn : public UbCongestionControl
{
public:
    static TypeId GetTypeId(void);

    UbDcqcn();
    ~UbDcqcn() override = default;
};

class UbHostDcqcn : public UbDcqcn
{
public:
    static TypeId GetTypeId(void);

    UbHostDcqcn();
    ~UbHostDcqcn() override = default;

    void OnTpAttached(Ptr<UbTransportChannel> tp) override;
    bool IsCcLimited(uint32_t bytes) override;
    void OnReceiverDataPacketReceived(uint64_t psn,
                                      uint32_t size,
                                      UbIpBasedNetworkHeader header) override;
    UbCongestionExtTph OnReceiverPrepareAckCongestionHeader(uint64_t psnStart,
                                                            uint64_t psnEnd) override;
    TpOpcode GetAckOpcode() const override;
    void OnSenderDataPacketSent(uint32_t psn, uint32_t size) override;
    void OnSenderRetransmissionPacketSent(uint32_t psn, uint32_t size) override;
    using UbCongestionControl::OnSenderCongestionNotification;
    void OnSenderCongestionNotification(TpOpcode opcode,
                                        uint32_t psn,
                                        UbCongestionExtTph header) override;
    void OnSenderTransportIdle() override;

    DataRate GetCurrentRateForTest() const { return m_currentRate; }
    DataRate GetTargetRateForTest() const { return m_targetRate; }
    Time GetNextAvailableSendTimeForTest() const { return m_nextAvailableSendTime; }
    void ApplySyntheticCnpForTest();

private:
    void DoDispose() override;
    void EnsureSenderStartState();
    void TraceSenderState(const char* reason) const;
    uint64_t ClampBitRate(uint64_t bitRate) const;
    void ApplyRecoveryIncrease(bool byteDriven);
    void EmitPendingCnp();
    void ResetRecoveryState();
    void HandleRateIncreaseTimer();
    void HandleByteCounterEvent();
    void HandleAlphaTimer();
    void ReleaseHostActiveFlow();
    void SchedulePacingWakeup();
    void HandlePacingWakeup();

    Ptr<UbTransportChannel> m_tp;
    Ptr<UbPort> m_port;
    uint32_t m_hostNodeId{UINT32_MAX};
    Time m_cnpInterval;
    Time m_lastCnpSent;
    EventId m_pendingCnpEvent;
    bool m_hasSentCnp{false};
    bool m_pendingMarked{false};
    uint8_t m_pendingEcn{0};
    bool m_pendingLocation{false};
    double m_alpha{1.0};
    double m_g{1.0 / 256.0};
    DataRate m_lineRate;
    DataRate m_initialRate;
    DataRate m_currentRate;
    DataRate m_targetRate;
    Time m_nextAvailableSendTime;
    Time m_rateIncreaseTimer;
    Time m_alphaUpdateInterval;
    uint64_t m_byteCounterThreshold{10 * 1024 * 1024};
    uint64_t m_bytesSinceLastIncrease{0};
    DataRate m_rateAi;
    DataRate m_rateHai;
    uint32_t m_timeRecoveryEvents{0};
    uint32_t m_byteRecoveryEvents{0};
    uint32_t m_fastRecoveryLimit{5};
    EventId m_rateIncreaseEvent;
    EventId m_alphaEvent;
    EventId m_pacingWakeupEvent;
    bool m_senderStarted{false};
    bool m_registeredHostFlow{false};
};

class UbSwitchDcqcn : public UbDcqcn
{
public:
    static TypeId GetTypeId(void);

    UbSwitchDcqcn();
    ~UbSwitchDcqcn() override = default;

    void OnSwitchAttached(Ptr<UbSwitch> sw) override;
    void OnSwitchPostEnqueue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) override;
    void OnSwitchPostDequeue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) override;

private:
    uint64_t GetOutPortBacklogBytes(uint32_t outPort) const;
    void MaybeMarkPacket(uint32_t outPort, Ptr<Packet> p);

    Ptr<UbSwitch> m_switch;
    Ptr<UniformRandomVariable> m_random;
    uint32_t m_kminBytes{0};
    uint32_t m_kmaxBytes{0};
    double m_pmax{1.0};
};

} // namespace ns3

#endif
