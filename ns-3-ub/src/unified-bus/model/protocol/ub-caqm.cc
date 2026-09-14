// SPDX-License-Identifier: GPL-2.0-only
#include <climits>
#include "ns3/log.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transport.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/data-rate.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/ub-port.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-utils.h"

namespace ns3 {

static uint32_t GetRealHint(const uint32_t hint, const uint32_t ccUnit)
{
    // hint的值是mtu/ccunit的整数倍
    if (hint % (UB_MTU_BYTE / ccUnit) == 0) {
        return hint * ccUnit;
    } else { // 非整数倍
        uint32_t num = hint / (UB_MTU_BYTE / ccUnit);
        uint32_t realHint = num * UB_MTU_BYTE + (hint - num * (UB_MTU_BYTE / ccUnit));
        return realHint;
    }
}

NS_LOG_COMPONENT_DEFINE("UbCaqm");
NS_OBJECT_ENSURE_REGISTERED(UbCaqm);

// Caqm父类
TypeId UbCaqm::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbCaqm")
            .SetParent<ns3::UbCongestionControl>()
            .AddConstructor<UbCaqm>()
            .AddAttribute("UbCaqmAlpha",
                          "CAQM alpha: additive window increase coefficient (cwnd += alpha/cwnd * MTU per ACK).",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbCaqm::m_alpha),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbCaqmBeta",
                          "CAQM beta: multiplicative window decrease factor (cwnd -= CE * beta * MTU).",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbCaqm::m_beta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbCaqmGamma",
                          "CAQM gamma: minimum congestion window floor coefficient (cwnd >= gamma * MTU).",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbCaqm::m_gamma),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbCaqmLambda",
                          "CAQM lambda: switch-side credit counter EWMA smoothing coefficient.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&UbCaqm::m_lambda),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbCaqmTheta",
                          "CAQM theta: idle RTT multiplier before resetting to slow-start (reset after theta * RTT).",
                          UintegerValue(10),
                          MakeUintegerAccessor(&UbCaqm::m_theta),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UbCaqmQt",
                          "CAQM Qt: target queue depth in bytes at which the switch begins marking packets.",
                          UintegerValue(10 * UB_MTU_BYTE),
                          MakeUintegerAccessor(&UbCaqm::m_idealQueueSize),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UbCaqmCcUint",
                          "CAQM CC unit: number of bytes represented by one credit-counter unit.",
                          UintegerValue(32),
                          MakeUintegerAccessor(&UbCaqm::m_ccUnit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("UbMarkProbability",
                          "CAQM marking probability for setting the congestion bit when CC > 0 but insufficient.",
                          DoubleValue(0.1),
                          MakeDoubleAccessor(&UbCaqm::m_markProbability),
                          MakeDoubleChecker<double>(0.0, 1.0));
    return tid;
}

UbCaqm::UbCaqm()
{
}

UbCaqm::~UbCaqm()
{
}

// host

static const double DATA_BYTE_RECVD_RESET_THREASHOLD = 0.9;
static const uint32_t DATA_BYTE_RECVD_RESET_NUM = 0x80000000; // 2 ^ 31

NS_OBJECT_ENSURE_REGISTERED(UbHostCaqm);

TypeId UbHostCaqm::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbHostCaqm")
            .SetParent<ns3::UbCaqm>()
            .AddConstructor<UbHostCaqm>()
            .AddAttribute("RttEwmaGain",
                          "EWMA gain used by host-side CAQM runtime RTT estimate.",
                          DoubleValue(0.125),
                          MakeDoubleAccessor(&UbHostCaqm::m_rttEwmaGain),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("UbCaqmCwnd",
                          "Initial congestion window size in bytes for host-side CAQM.",
                          UintegerValue(10 * UB_MTU_BYTE),
                          MakeUintegerAccessor(&UbHostCaqm::m_cwnd),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

UbHostCaqm::UbHostCaqm()
{
    m_nodeType = UB_DEVICE;
}

UbHostCaqm::~UbHostCaqm()
{
    NS_LOG_FUNCTION(this);
}

void UbHostCaqm::OnTpAttached(Ptr<UbTransportChannel> tp)
{
    m_src = tp->GetSrc();
    m_dst = tp->GetDest();
    m_tpn = tp->GetTpn();
}

void UbHostCaqm::UpdateRttEstimate(Time sample)
{
    if (sample <= NanoSeconds(0)) {
        return;
    }
    if (m_rtt == NanoSeconds(0)) {
        m_rtt = sample;
        return;
    }
    const double currentNs = static_cast<double>(m_rtt.GetNanoSeconds());
    const double sampleNs = static_cast<double>(sample.GetNanoSeconds());
    const double ewmaNs = (1.0 - m_rttEwmaGain) * currentNs + m_rttEwmaGain * sampleNs;
    m_rtt = NanoSeconds(static_cast<int64_t>(ewmaNs + 0.5));
}

void UbHostCaqm::ApplyRttSampleForTest(Time sample)
{
    UpdateRttEstimate(sample);
}

// 获取剩余窗口
uint32_t UbHostCaqm::GetRestCwnd()
{
    if (m_congestionCtrlEnabled) {
        // 可能存在的特殊情况：拥塞状态下减窗，导致cwnd < inflight，则此情况返回0
        if (m_cwnd >= m_inFlight) {
            return m_cwnd - m_inFlight;
        } else {
            return 0;
        }
    } else {
        return UINT_MAX;
    }
}

bool UbHostCaqm::IsCcLimited(uint32_t bytes)
{
    if (!m_congestionCtrlEnabled) {
        return false;
    }
    return GetRestCwnd() < bytes;
}

// 发送端生成拥塞控制算法需要的header字段
void UbHostCaqm::OnSenderPrepareIpBasedNetworkHeader(UbIpBasedNetworkHeader& networkHeader)
{
    if (m_congestionCtrlEnabled) {
        networkHeader.SetI(1);
        networkHeader.SetC(0);
        // 慢启动阶段或者cw < mtu，hint = MTU
        uint8_t hint;
        if (m_congestionState == SLOW_START || m_cwnd < UB_MTU_BYTE) {
            hint = uint8_t(UB_MTU_BYTE / m_ccUnit);
            NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " Congestion state:" << m_congestionState
                      << " Cwnd:" << m_cwnd
                      << " Set hint:" << (int)hint);
            networkHeader.SetHint(hint);
        } else if (m_alpha / m_cwnd * UB_MTU_BYTE < 1.0) {
            // α / cw * mtu 小于1，则累计，直到大于1在下一帧中一同发出
            m_accumulateHint += m_alpha / m_cwnd * UB_MTU_BYTE;
            if (m_accumulateHint >= 1.0) {
                hint = uint8_t(m_accumulateHint);
                m_accumulateHint -= double(uint8_t(m_accumulateHint));
            } else {
                hint = 0;
            }
            networkHeader.SetHint(hint);
            NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " Congestion state:" << m_congestionState
                      << " AccumulateHint:" << m_accumulateHint
                      << " Cwnd:" << m_cwnd
                      << " Set hint:" << (int)hint);
        }
    } else {
        networkHeader.SetI(0);
        networkHeader.SetC(0);
        networkHeader.SetHint(0);
    }
}

// 发送端发包，更新数据
void UbHostCaqm::OnSenderDataPacketSent(uint32_t psn, uint32_t size)
{
    if (m_congestionCtrlEnabled) {
        // 记录包号对应的发送时间
        m_psnSendTimeMap[psn] = Simulator::Now();
        m_dataByteSent += size;
        m_inFlight += size;
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " Send pkt. Local:" << m_src
                      << " Send to:" << m_dst
                      << " Tpn:" << m_tpn
                      << " Psn:" << psn
                      << " Size:" << size
                      << " Send byte:" << m_dataByteSent
                      << " Inflight:" << m_inFlight);
    }
}

// 接收端接到数据包后记录数据
void UbHostCaqm::OnReceiverDataPacketReceived(uint64_t psn,
                                              uint32_t size,
                                              UbIpBasedNetworkHeader header)
{
    if (m_congestionCtrlEnabled) {
        m_recvdPsnPacketSizeMap[psn] = size; // 记录包号对应包的size, C, I, Hint
        m_recvdPsnCMap[psn] = header.GetC();
        m_recvdPsnIMap[psn] = header.GetI();
        uint16_t hint = header.GetHint();
        hint = GetRealHint(hint, m_ccUnit);
        m_recvdPsnHintMap[psn] = hint;
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Local:" << m_src
                  << " recv from:" << m_dst
                  << " tpn:" << m_tpn
                  << " psn:" << psn
                  << " size:" << size
                  << " C:" << (int)header.GetC()
                  << " I:" << (int)header.GetI()
                  << " Hint:" << (int)header.GetHint());
    }
}

// 接收端生成拥塞控制算法需要的ack header
UbCongestionExtTph UbHostCaqm::OnReceiverPrepareAckCongestionHeader(uint64_t psnStart,
                                                                    uint64_t psnEnd)
{
    UbCongestionExtTph cetph;
    if (m_congestionCtrlEnabled) {
        for (uint64_t i = psnStart; i < psnEnd; i++) {
            m_dataByteRecvd += m_recvdPsnPacketSizeMap[i];
            m_recvdPsnPacketSizeMap.erase(i);
            uint8_t C = m_recvdPsnCMap[i];
            uint8_t I = m_recvdPsnIMap[i];
            uint16_t Hint = m_recvdPsnHintMap[i];
            m_recvdPsnCMap.erase(i);
            m_recvdPsnIMap.erase(i);
            m_recvdPsnHintMap.erase(i);
            if (C == 0 && I == 1) {
                m_HintE += Hint;
                m_IE = 1;
            } else if (C == 1) {
                m_CE++;
            }
        }
        // 聚合ack，ceTph设置为c_e i_e hint_t
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Gen ack, Local:" << m_src
                  << " send back to:" << m_dst
                  << " tpn:" << m_tpn
                  << " C_E:" << (int)m_CE
                  << " I_E:" << (int)m_IE
                  << " Hint_e:" << (int)m_HintE);
        // 由于ack的sequence字段只有4字节，最多4G，因此在大于3.6G的时候将该数字减去2G以免越界
        // 发送端接收到骤降的ack sequence后进行相应操作
        if (m_dataByteRecvd > uint32_t(UINT_MAX * DATA_BYTE_RECVD_RESET_THREASHOLD)) {
            m_dataByteRecvd -= DATA_BYTE_RECVD_RESET_NUM;
        }
        cetph.SetAckSequence(m_dataByteRecvd);
        cetph.SetC(m_CE);
        cetph.SetI(m_IE);
        cetph.SetHint(m_HintE);
        utils::UbUtils::CaqmAckNotify(m_src,
                                      m_tpn,
                                      static_cast<uint32_t>(psnStart & 0xFFFFFFull),
                                      static_cast<uint32_t>(psnEnd & 0xFFFFFFull),
                                      m_CE,
                                      m_IE,
                                      m_HintE);
        m_CE = 0;
        m_IE = 0;
        m_HintE = 0;
        return cetph;
    } else {
        cetph.SetAckSequence(0);
        cetph.SetC(0);
        cetph.SetI(0);
        cetph.SetHint(0);
        return cetph;
    }
}

void
UbHostCaqm::ApplySenderCetphFeedback(uint32_t psn, const UbCongestionExtTph& header)
{
    const auto sentIt = m_psnSendTimeMap.find(psn);
    if (sentIt != m_psnSendTimeMap.end()) {
        UpdateRttEstimate(Simulator::Now() - sentIt->second);
    }

    uint32_t sequence = header.GetAckSequence();
    if (sequence < m_lastSequence && m_lastSequence > DATA_BYTE_RECVD_RESET_NUM) {
        m_dataByteSent = (m_dataByteSent >= DATA_BYTE_RECVD_RESET_NUM)
                             ? (m_dataByteSent - DATA_BYTE_RECVD_RESET_NUM)
                             : 0;
    }
    m_lastSequence = sequence;
    m_inFlight = (m_dataByteSent >= sequence) ? (m_dataByteSent - sequence) : 0;
    uint8_t c_e = header.GetC();
    bool i_e = header.GetI();
    uint16_t hint = header.GetHint();
    NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
              << "[Debug]"
              << "[" << __FUNCTION__ << "]"
              << " Recv ack."
              << " Local:"<< m_src
              << " Recv from:" << m_dst
              << " Psn:" << psn
              << " Tpn:" << m_tpn
              << " Sent byte:" << m_dataByteSent
              << " Sequence:" << sequence
              << " Inflght:" << m_inFlight
              << " C_E:" << (int)c_e
              << " I_E:" << (int)i_e
              << " Hint_e:" << (int)hint);
    utils::UbUtils::CaqmSenderStateNotify(m_src, m_tpn, psn, sequence, m_inFlight, m_cwnd, c_e, i_e, hint);
    // 收到拥塞或者拒绝增窗反馈，切换至拥塞避免阶段。
    // 同时重启定时器，一段时间后若仍没有收到含有阻塞或拒绝增窗的ack，则切换到慢启动阶段，
    if (c_e > 0 || i_e == 0) {
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Congesiton or refuse.");
        m_congestionState = CONGESTION_AVOIDANCE;
        m_congestionStateResetEvent.Cancel();
        m_congestionStateResetEvent =
            Simulator::Schedule(m_rtt * m_theta,
                                &UbHostCaqm::StateReset,
                                this);
    }
    uint32_t oldCwnd;
    // i为1，增窗
    if (i_e == 1) {
        oldCwnd = m_cwnd;
        m_cwnd += hint;
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Congestion state:" << m_congestionState
                  << " Cwnd increase:" << oldCwnd
                  << "->"
                  << m_cwnd
                  << " Rest cwnd:" << m_cwnd - m_inFlight);
    }
    // 存在阻塞情况
    if (c_e >= 1 && m_cwnd > UB_MTU_BYTE) {
        oldCwnd = m_cwnd;
        // cwnd = max(cwnd - c_e * β * MTU, MTU / 2)
        m_cwnd = m_cwnd - c_e * m_beta * UB_MTU_BYTE >= UB_MTU_BYTE / 2
                ? m_cwnd - c_e * m_beta * UB_MTU_BYTE : UB_MTU_BYTE / 2;
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Congestion state:" << m_congestionState
                  << " Cwnd > mtu, decrease from:" << oldCwnd
                  << "->"
                  << m_cwnd
                  << " Rest cwnd:" << m_cwnd - m_inFlight);
    } else if (c_e >= 1 && m_cwnd <= UB_MTU_BYTE) {
        oldCwnd = m_cwnd;
        // cwnd = max(cwnd / 2, γ * MTU)
        m_cwnd = m_cwnd / 2 > m_gamma * UB_MTU_BYTE ? m_cwnd / 2 : m_gamma * UB_MTU_BYTE;
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Congestion state:" << m_congestionState
                  << " Cwnd <= mtu, decrease from:" << oldCwnd
                  << "->"
                  << m_cwnd
                  << " Rest cwnd:" << m_cwnd - m_inFlight);
    }
    if (m_cwnd < UB_MTU_BYTE) {
        m_cwnd = UB_MTU_BYTE;
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Cwnd < mtu. Reset to UB_MTU_BYTE.");
    }
}

// 发送端收到ack，调整窗口、速率等数据
void UbHostCaqm::OnSenderCongestionNotification(TpOpcode opcode,
                                                uint32_t psn,
                                                UbCongestionExtTph header)
{
    if (opcode != TpOpcode::TP_OPCODE_ACK_WITH_CETPH) {
        return;
    }
    if (m_congestionCtrlEnabled) {
        ApplySenderCetphFeedback(psn, header);
    }
}

void
UbHostCaqm::OnSenderCongestionNotification(TpOpcode opcode,
                                           uint32_t psn,
                                           UbCongestionExtTph header,
                                           uint32_t retransmitBytes)
{
    if (opcode != TpOpcode::TP_OPCODE_ACK_WITH_CETPH)
    {
        return;
    }
    if (!m_congestionCtrlEnabled)
    {
        return;
    }

    if (m_dataByteSent >= retransmitBytes)
    {
        m_dataByteSent -= retransmitBytes;
    }
    else
    {
        m_dataByteSent = 0;
    }

    m_inFlight = std::min(m_inFlight, m_dataByteSent);
    ApplySenderCetphFeedback(psn, header);
}

void
UbHostCaqm::OnSenderRetransmissionPacketSent(uint32_t psn, uint32_t size)
{
    OnSenderDataPacketSent(psn, size);
}

void UbHostCaqm::StateReset()
{
    m_congestionState = SLOW_START;
}

void UbHostCaqm::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_recvdPsnPacketSizeMap.clear();
    m_recvdPsnHintMap.clear();
    m_recvdPsnCMap.clear();
    m_recvdPsnIMap.clear();
    m_psnSendTimeMap.clear();
    Object::DoDispose();
}

// switch

NS_OBJECT_ENSURE_REGISTERED(UbSwitchCaqm);

UbSwitchCaqm::UbSwitchCaqm()
{
    m_nodeType = UB_SWITCH;
    m_random = CreateObject<UniformRandomVariable>();
    m_random->SetAttribute("Min", DoubleValue(0.0));
    m_random->SetAttribute("Max", DoubleValue(1.0));
}

UbSwitchCaqm::~UbSwitchCaqm()
{
    NS_LOG_FUNCTION(this);
}

TypeId UbSwitchCaqm::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbSwitchCaqm")
            .SetParent<ns3::UbCaqm>()
            .AddConstructor<UbSwitchCaqm>()
            .AddAttribute("UbCcUpdatePeriod",
                          "Switch-side credit counter periodic update interval.",
                          TimeValue(NanoSeconds(500)),
                          MakeTimeAccessor(&UbSwitchCaqm::m_ccUpdatePeriod),
                          MakeTimeChecker());

    return tid;
}

void UbSwitchCaqm::OnSwitchAttached(Ptr<UbSwitch> sw)
{
    UbCongestionControl::OnSwitchAttached(sw);
    auto node = sw->GetObject<Node>();
    m_nodeId = node->GetId();
    if (m_congestionCtrlEnabled) {
        uint32_t ndevice = node->GetNDevices();
        for (uint32_t i = 0; i < ndevice; i++) {
            m_txSize.push_back(0);
            m_cc.push_back(0);
            m_DC.push_back(0);
            m_creditAllocated.push_back(0);
        }
    }
}

void UbSwitchCaqm::ResetLocalCc()
{
    if (m_congestionCtrlEnabled) {
        auto node = NodeList::GetNode(m_nodeId);
        auto sw = node->GetObject<UbSwitch>();
        uint32_t ndevice = node->GetNDevices();
        for (uint32_t portId = 0; portId < ndevice; portId++) {
            // 使用OutPort视图统计VOQ占用
            uint64_t voqUsed = sw->GetQueueManager()->GetTotalOutPortBufferUsed(portId);
            
            // 加上EgressQueue的字节占用
            Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
            uint64_t egressUsed = port->GetUbQueue()->GetCurrentBytes();
            const DataRate portRate = port->GetDataRate();
            
            // 总队列占用 = VOQ + EgressQueue
            uint64_t totalQueueSize = voqUsed + egressUsed;
            
            uint64_t cc = uint64_t(m_lambda *
                                (m_ccUpdatePeriod.GetSeconds()
                                * portRate.GetBitRate() / 8
                                - m_txSize[portId]
                                + m_idealQueueSize
                                - totalQueueSize
                                - m_creditAllocated[portId]));
            m_cc[portId] = cc;
            m_txSize[portId] = 0;
            m_DC[portId] = 0;
            m_creditAllocated[portId] = 0;
        }
        Simulator::Schedule(m_ccUpdatePeriod, &UbSwitchCaqm::ResetLocalCc, this);
    }
}

void UbSwitchCaqm::OnSwitchPostDequeue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p)
{
    if (m_congestionCtrlEnabled) {
        UbDatalinkHeader dlHeader;
        p->PeekHeader(dlHeader);
        // 只处理config = 3的包，其余忽略
        if (!dlHeader.IsPacketIpv4Header()) {
            NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " This is not ipv4 packet.");
            return;
        }
        m_txSize[outPort] += p->GetSize();
        auto node = NodeList::GetNode(m_nodeId);
        auto sw = node->GetObject<UbSwitch>();
        
        // 计算总队列占用 = VOQ + EgressQueue
        uint64_t voqUsed = sw->GetQueueManager()->GetTotalOutPortBufferUsed(outPort);
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
        uint64_t egressUsed = port->GetUbQueue()->GetCurrentBytes();
        uint64_t totalQueueSize = voqUsed + egressUsed;
        
        NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                  << "[Debug]"
                  << "[" << __FUNCTION__ << "]"
                  << " Node:" << m_nodeId
                  << " Inport:" << inPort
                  << " OutPort:" << outPort
                  << " VOQ:" << voqUsed
                  << " Egress:" << egressUsed
                  << " Total queue:" << totalQueueSize
                  << " Txsize:" << m_txSize[outPort]);
        UbDatalinkPacketHeader dlPktHeader;
        UbIpBasedNetworkHeader netHeader;
        p->RemoveHeader(dlPktHeader);
        p->RemoveHeader(netHeader);
        uint8_t c = netHeader.GetC();
        uint8_t i = netHeader.GetI();
        uint16_t hint = netHeader.GetHint();
        hint = GetRealHint(hint, m_ccUnit);
        if (c == 1) { // 前面已经被判断拥塞的包，无需再处理，直接增加cc
            NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " Already congestion. Only record.");
            m_cc[outPort] += m_beta * UB_MTU_BYTE;
            m_creditAllocated[outPort] -= m_beta * UB_MTU_BYTE;
        } else if (m_cc[outPort] >= hint * i) { // cc足够，允许增窗
            NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " CC enough."
                      << " Hint * i:" << hint * i
                      << " CC:" << m_cc[outPort]
                      << "->" << m_cc[outPort] - hint * i
                      << " CreditAllocated:" << m_creditAllocated[outPort]
                      << "->" << m_creditAllocated[outPort] + hint * i);
            m_cc[outPort] -= hint * i;
            m_creditAllocated[outPort] += hint * i;
        } else if (m_cc[outPort] >= 0) {
            // cc不足，随机给流减窗，生成一个0~1范围的数字，小于p即可以认为触发了概率事件
            double res = m_random->GetValue();
            if (res < m_markProbability) {
                NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                          << "[Debug]"
                          << "[" << __FUNCTION__ << "]"
                          << " CC not enough. Random result:" << res
                          << " MK. DC:" << m_DC[outPort]
                          << "->" << m_DC[outPort] + m_beta * UB_MTU_BYTE);
                netHeader.SetC(1);
                netHeader.SetI(0);
                m_DC[outPort] += m_beta * UB_MTU_BYTE;
            } else if (res >= m_markProbability && m_DC[outPort] >= hint * i) {
                NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                          << "[Debug]"
                          << "[" << __FUNCTION__ << "]"
                          << " CC not enough. Random result:" << res
                          << " Not MK. DC >= hint * i, DC from:" << m_DC[outPort]
                          << "->" << m_DC[outPort] - hint * i);
                m_DC[outPort] -= hint * i;
            } else {
                NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                          << "[Debug]"
                          << "[" << __FUNCTION__ << "]"
                          << " CC not enough. Random result:" << res
                          << " Not MK. DC < hint * i, set i = 0");
                netHeader.SetI(0);
            }
        } else { // cc < 0， 阻塞
            NS_LOG_DEBUG("[" << GetTypeId().GetName() << "]"
                      << "[Debug]"
                      << "[" << __FUNCTION__ << "]"
                      << " Congestion. CC from:" << m_cc[outPort]
                      << "->" << m_cc[outPort] + m_beta * UB_MTU_BYTE);
            netHeader.SetC(1);
            netHeader.SetI(0);
            m_cc[outPort] += m_beta * UB_MTU_BYTE;
        }
        p->AddHeader(netHeader);
        p->AddHeader(dlPktHeader);
    }
}

void UbSwitchCaqm::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_cc.clear();
    m_txSize.clear();
    m_DC.clear();
    m_creditAllocated.clear();
    m_random = nullptr;
    Object::DoDispose();
}

}
