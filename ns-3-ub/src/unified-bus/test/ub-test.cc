// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file ub-test.cc
 * @brief Test suite for the unified-bus module
 * 
 * This file contains unit tests for the unified-bus functionality,
 * including basic object creation, configuration, and core features.
 */

#include "ns3/config.h"
#include "ns3/data-rate.h"
#include "ns3/log.h"
#include "ns3/node-container.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/simulator.h"
#include "ns3/test.h"
#include "ns3/ub-app.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-dcqcn.h"
#include "ns3/ub-flow-control.h"
#include "ns3/ub-function.h"
#include "ns3/ub-link.h"
#include "ns3/ub-modulo-sequence.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-sliding-bitmap-window.h"
#include "ns3/ub-small-fifo-queue.h"
#include "ns3/ub-switch-allocator.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-traffic-gen.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UbTest");

namespace {

constexpr uint32_t kUrmaWriteRegressionJettyNum = 0;
constexpr uint32_t kUrmaWriteRegressionTaskId = 9001;
constexpr uint32_t kUrmaReadRegressionJettyNum = 1;
constexpr uint32_t kUrmaReadRegressionTaskId = 9002;
constexpr uint32_t kUrmaReadMultiPacketTaskId = 9003;
constexpr uint32_t kUrmaWriteRegressionSenderTpn = 101;
constexpr uint32_t kUrmaWriteRegressionReceiverTpn = 202;
constexpr auto kUrmaWriteRegressionPriority = UB_PRIORITY_DEFAULT;

Ptr<UbWqeSegment>
CreateCtpWriteSegment(uint32_t taSsn = 0)
{
    Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
    segment->SetSrc(0);
    segment->SetDest(1);
    segment->SetPriority(7);
    segment->SetType(TaOpcode::TA_OPCODE_WRITE);
    segment->SetSize(64);
    segment->SetTaSsn(taSsn);
    segment->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    segment->SetPayloadBytes(64);
    segment->SetResLenBytes(64);
    segment->SetRemoteAddress(0);
    return segment;
}

Ptr<UbWqeSegment>
CreateCtpTaAckResponseSegment(uint32_t requestTaSsn)
{
    Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
    segment->SetSrc(1);
    segment->SetDest(0);
    segment->SetPriority(7);
    segment->SetType(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    segment->SetSize(0);
    segment->SetTaSsn(requestTaSsn);
    segment->SetSegmentKind(UbTransactionSegmentKind::RESPONSE);
    segment->SetRequestTassn(requestTaSsn);
    segment->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
    segment->SetPayloadBytes(0);
    segment->SetResLenBytes(0);
    segment->SetRemoteAddress(0);
    return segment;
}

struct DecodedCtpCompactDataPacket
{
    uint16_t taSsn{0};
    uint32_t payloadBytes{0};
    uint32_t logicalBytes{0};
};

DecodedCtpCompactDataPacket
DecodeCtpCompactDataPacket(Ptr<Packet> packet)
{
    Ptr<Packet> copy = packet->Copy();
    UbDatalinkPacketHeader datalinkHeader;
    UbCna16NetworkHeader cnaHeader;
    UbCtpHeader ctpHeader;
    UbCompactTransactionHeader compactTah;

    copy->RemoveHeader(datalinkHeader);
    copy->RemoveHeader(cnaHeader);
    copy->RemoveHeader(ctpHeader);
    if (ctpHeader.GetNlp() == UB_CTPH_NLP_UPI16_EID40_TAH)
    {
        UbCompactUpiHeader upiHeader;
        UbCompactEidHeader eidHeader;
        copy->RemoveHeader(upiHeader);
        copy->RemoveHeader(eidHeader);
    }
    copy->RemoveHeader(compactTah);
    UbCompactMAExtTah maHeader;
    copy->RemoveHeader(maHeader);

    return {.taSsn = compactTah.GetIniTaSsn(),
            .payloadBytes = copy->GetSize(),
            .logicalBytes = static_cast<uint32_t>(64u << maHeader.GetLength())};
}

Ptr<Packet>
BuildCtpCnpPacket(const UbCtpEntityKey& key)
{
    Ptr<Packet> packet = Create<Packet>();

    UbCtpHeader ctpHeader;
    ctpHeader.SetTPOpcode(CtpOpcode::CTP_CNP);
    ctpHeader.SetNlp(UB_CTPH_NLP_COMPACT_TAH);
    packet->AddHeader(ctpHeader);

    UbCna16NetworkHeader cnaHeader;
    cnaHeader.SetScna(static_cast<uint16_t>(NodeIdToCna16(key.srcNodeId)));
    cnaHeader.SetDcna(static_cast<uint16_t>(NodeIdToCna16(key.dstNodeId)));
    cnaHeader.SetServiceLevel(key.vl);
    cnaHeader.SetNlp(UB_CNA_NLP_CTPH);
    packet->AddHeader(cnaHeader);

    UbDataLink::GenPacketHeader(packet,
                                false,
                                false,
                                key.vl,
                                key.vl,
                                false,
                                true,
                                UbDatalinkHeaderConfig::PACKET_CNA16);
    return packet;
}

struct LocalTpTopology
{
    Ptr<Node> sender;
    Ptr<Node> switch0;
    Ptr<Node> switch1;
    Ptr<Node> receiver;
    Ptr<UbPort> senderPort;
    Ptr<UbPort> switch0DevicePort;
    Ptr<UbPort> switch0CorePort;
    Ptr<UbPort> switch1CorePort;
    Ptr<UbPort> switch1DevicePort;
    Ptr<UbPort> receiverPort;
};

Ptr<UbPort>
CreatePort(Ptr<Node> node)
{
    Ptr<UbPort> port = CreateObject<UbPort>();
    port->SetAddress(Mac48Address::Allocate());
    node->AddDevice(port);
    return port;
}

void
InitNode(Ptr<Node> node, UbNodeType_t nodeType, uint32_t portCount)
{
    Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
    node->AggregateObject(sw);
    sw->SetNodeType(nodeType);

    if (nodeType == UB_DEVICE)
    {
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();
    }

    for (uint32_t index = 0; index < portCount; ++index)
    {
        CreatePort(node);
    }

    sw->Init();
    Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
    congestionCtrl->OnSwitchAttached(sw);
}

class UbTestIngressQueue : public UbIngressQueue
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbTestIngressQueue")
                                .SetParent<UbIngressQueue>()
                                .SetGroupName("UnifiedBus")
                                .AddConstructor<UbTestIngressQueue>();
        return tid;
    }

    void PushPacket(uint32_t size)
    {
        if (m_packets.empty())
        {
            m_headArrivalTime = Simulator::Now();
        }
        m_packets.push_back(Create<Packet>(size));
    }

    bool IsEmpty() override
    {
        return m_packets.empty();
    }

    bool IsLimited() override
    {
        return m_limited;
    }

    Ptr<Packet> GetNextPacket() override
    {
        if (m_packets.empty())
        {
            return nullptr;
        }
        Ptr<Packet> packet = m_packets.front();
        m_packets.erase(m_packets.begin());
        if (!m_packets.empty())
        {
            m_headArrivalTime = Simulator::Now();
        }
        return packet;
    }

    uint32_t GetNextPacketSize() override
    {
        if (m_packets.empty())
        {
            return 0;
        }
        return m_packets.front()->GetSize();
    }

    IngressQueueType GetIngressQueueType() override
    {
        return m_queueType;
    }

    void SetLimited(bool limited)
    {
        m_limited = limited;
    }

  private:
    std::vector<Ptr<Packet>> m_packets;
    bool m_limited{false};
    IngressQueueType m_queueType{IngressQueueType::TP};
};

void
AddShortestRoute(Ptr<Node> node, uint32_t destNodeId, uint32_t destPortId, uint16_t outPort)
{
    std::vector<uint16_t> outPorts = {outPort};
    Ptr<UbRoutingProcess> routing = node->GetObject<UbSwitch>()->GetRoutingProcess();
    routing->AddShortestRoute(NodeIdToIp(destNodeId).Get(), outPorts);
    routing->AddShortestRoute(NodeIdToIp(destNodeId, destPortId).Get(), outPorts);
}

Ptr<Node>
BuildSinglePortCtpNode()
{
    Ptr<Node> node = CreateObject<Node>(0);
    InitNode(node, UB_DEVICE, 1);
    return node;
}

LocalTpTopology
BuildLocalTpTopology(bool addReverseRoutes = true)
{
    LocalTpTopology topo;
    topo.sender = CreateObject<Node>(0);
    topo.switch0 = CreateObject<Node>(0);
    topo.switch1 = CreateObject<Node>(0);
    topo.receiver = CreateObject<Node>(0);

    InitNode(topo.sender, UB_DEVICE, 1);
    InitNode(topo.switch0, UB_SWITCH, 2);
    InitNode(topo.switch1, UB_SWITCH, 2);
    InitNode(topo.receiver, UB_DEVICE, 1);

    topo.senderPort = DynamicCast<UbPort>(topo.sender->GetDevice(0));
    topo.switch0DevicePort = DynamicCast<UbPort>(topo.switch0->GetDevice(0));
    topo.switch0CorePort = DynamicCast<UbPort>(topo.switch0->GetDevice(1));
    topo.switch1CorePort = DynamicCast<UbPort>(topo.switch1->GetDevice(0));
    topo.switch1DevicePort = DynamicCast<UbPort>(topo.switch1->GetDevice(1));
    topo.receiverPort = DynamicCast<UbPort>(topo.receiver->GetDevice(0));

    Ptr<UbLink> leftLink = CreateObject<UbLink>();
    topo.senderPort->Attach(leftLink);
    topo.switch0DevicePort->Attach(leftLink);

    Ptr<UbLink> coreLink = CreateObject<UbLink>();
    topo.switch0CorePort->Attach(coreLink);
    topo.switch1CorePort->Attach(coreLink);

    Ptr<UbLink> rightLink = CreateObject<UbLink>();
    topo.switch1DevicePort->Attach(rightLink);
    topo.receiverPort->Attach(rightLink);

    AddShortestRoute(topo.sender,
                     topo.receiver->GetId(),
                     topo.receiverPort->GetIfIndex(),
                     topo.senderPort->GetIfIndex());
    AddShortestRoute(topo.switch0,
                     topo.receiver->GetId(),
                     topo.receiverPort->GetIfIndex(),
                     topo.switch0CorePort->GetIfIndex());
    AddShortestRoute(topo.switch1,
                     topo.receiver->GetId(),
                     topo.receiverPort->GetIfIndex(),
                     topo.switch1DevicePort->GetIfIndex());

    if (addReverseRoutes)
    {
        AddShortestRoute(topo.receiver,
                         topo.sender->GetId(),
                         topo.senderPort->GetIfIndex(),
                         topo.receiverPort->GetIfIndex());
        AddShortestRoute(topo.switch1,
                         topo.sender->GetId(),
                         topo.senderPort->GetIfIndex(),
                         topo.switch1CorePort->GetIfIndex());
        AddShortestRoute(topo.switch0,
                         topo.sender->GetId(),
                         topo.senderPort->GetIfIndex(),
                         topo.switch0DevicePort->GetIfIndex());
    }

    return topo;
}

void
InstallStaticTpPair(const LocalTpTopology& topo)
{
    Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
    Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
    Connection connection{topo.sender->GetId(),
                          topo.senderPort->GetIfIndex(),
                          kUrmaWriteRegressionSenderTpn,
                          topo.receiver->GetId(),
                          topo.receiverPort->GetIfIndex(),
                          kUrmaWriteRegressionReceiverTpn,
                          kUrmaWriteRegressionPriority,
                          0};

    Connection existingConnection;
    if (!senderCtrl->GetTpConnManager()->TryGetLocalConnection(topo.sender->GetId(),
                                                               kUrmaWriteRegressionSenderTpn,
                                                               existingConnection))
    {
        senderCtrl->GetTpConnManager()->AddUnilateralConnection(connection, topo.sender->GetId());
    }
    if (!receiverCtrl->GetTpConnManager()->TryGetLocalConnection(topo.receiver->GetId(),
                                                                 kUrmaWriteRegressionReceiverTpn,
                                                                 existingConnection))
    {
        receiverCtrl->GetTpConnManager()->AddUnilateralConnection(connection,
                                                                  topo.receiver->GetId());
    }

    if (!senderCtrl->IsTPExists(kUrmaWriteRegressionSenderTpn))
    {
        Ptr<UbCongestionControl> senderCc = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             kUrmaWriteRegressionSenderTpn,
                             kUrmaWriteRegressionReceiverTpn,
                             senderCc);
    }

    if (!receiverCtrl->IsTPExists(kUrmaWriteRegressionReceiverTpn))
    {
        Ptr<UbCongestionControl> receiverCc = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               kUrmaWriteRegressionReceiverTpn,
                               kUrmaWriteRegressionSenderTpn,
                               receiverCc);
    }
}

void
UseSelectiveRetransmissionForTest()
{
    Config::SetDefault("ns3::UbTransportChannel::EnableRetrans", BooleanValue(true));
    Config::SetDefault("ns3::UbTransportChannel::RetransmissionMode",
                       EnumValue(UbRetransmissionMode::SELECTIVE));
}

Ptr<Packet>
BuildReceiverDataPacketWithSequences(const LocalTpTopology& topo,
                                     uint32_t psn,
                                     bool lastPacket,
                                     uint32_t tpMsn,
                                     uint16_t iniTaSsn,
                                     uint32_t payloadBytes = 16,
                                     uint32_t resLenBytes = 16,
                                     uint32_t taskId = kUrmaWriteRegressionTaskId)
{
    Ptr<Packet> data = Create<Packet>(payloadBytes);
    UbFlowTag flowTag(taskId, resLenBytes);
    data->AddPacketTag(flowTag);

    UbMAExtTah maHeader;
    maHeader.SetLength(resLenBytes);
    data->AddHeader(maHeader);

    UbTransactionHeader taHeader;
    taHeader.SetTaOpcode(TaOpcode::TA_OPCODE_WRITE);
    taHeader.SetIniTaSsn(iniTaSsn);
    taHeader.SetIniRcId(0);
    data->AddHeader(taHeader);

    UbTransportHeader tpHeader;
    tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RELIABLE_TA);
    tpHeader.SetSrcTpn(kUrmaWriteRegressionSenderTpn);
    tpHeader.SetDestTpn(kUrmaWriteRegressionReceiverTpn);
    tpHeader.SetPsn(psn);
    tpHeader.SetTpMsn(tpMsn);
    tpHeader.SetLastPacket(lastPacket);
    data->AddHeader(tpHeader);

    UdpHeader udpHeader;
    data->AddHeader(udpHeader);
    UbPort::AddIpv4Header(data, NodeIdToIp(topo.sender->GetId()), NodeIdToIp(topo.receiver->GetId()));

    UbIpBasedNetworkHeader networkHeader;
    data->AddHeader(networkHeader);
    UbDataLink::GenPacketHeader(data,
                                false,
                                false,
                                kUrmaWriteRegressionPriority,
                                kUrmaWriteRegressionPriority,
                                false,
                                true,
                                UbDatalinkHeaderConfig::PACKET_IPV4);
    return data;
}

Ptr<Packet>
BuildReceiverDataPacket(const LocalTpTopology& topo, uint32_t psn, bool lastPacket)
{
    return BuildReceiverDataPacketWithSequences(topo, psn, lastPacket, 0, 7);
}

Ptr<Packet>
BuildInboundTpPacket(const LocalTpTopology& topo,
                     uint32_t srcTpn,
                     uint32_t dstTpn,
                     Ipv4Address sourceIp,
                     Ipv4Address destinationIp,
                     uint8_t priority)
{
    Ptr<Packet> packet = Create<Packet>(16);
    packet->AddPacketTag(UbFlowTag(kUrmaWriteRegressionTaskId, 16));

    UbMAExtTah maHeader;
    maHeader.SetLength(16);
    packet->AddHeader(maHeader);

    UbTransactionHeader transactionHeader;
    transactionHeader.SetTaOpcode(TaOpcode::TA_OPCODE_WRITE);
    transactionHeader.SetIniTaSsn(0);
    transactionHeader.SetIniRcId(0);
    packet->AddHeader(transactionHeader);

    UbTransportHeader transportHeader;
    transportHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RELIABLE_TA);
    transportHeader.SetSrcTpn(srcTpn);
    transportHeader.SetDestTpn(dstTpn);
    transportHeader.SetPsn(0);
    transportHeader.SetTpMsn(0);
    transportHeader.SetLastPacket(true);
    packet->AddHeader(transportHeader);

    UbPort::AddUdpHeader(packet, 0, topo.receiverPort->GetIfIndex());
    UbPort::AddIpv4Header(packet, sourceIp, destinationIp);

    UbIpBasedNetworkHeader networkHeader;
    packet->AddHeader(networkHeader);
    UbDataLink::GenPacketHeader(packet,
                                false,
                                false,
                                priority,
                                priority,
                                false,
                                true,
                                UbDatalinkHeaderConfig::PACKET_IPV4);
    return packet;
}

struct DecodedReceiverAck
{
    UbTransportHeader tpHeader;
    UbCongestionExtTph congestionHeader;
    UbSelectiveAckExtTph selectiveAckHeader;
    UbAckTransactionHeader taAckHeader;
    bool hasCongestionHeader{false};
    bool hasSelectiveAck{false};
};

DecodedReceiverAck
DecodeReceiverAck(Ptr<Packet> ack)
{
    DecodedReceiverAck decoded;
    UbDatalinkPacketHeader dlHeader;
    UbIpBasedNetworkHeader networkHeader;
    Ipv4Header ipv4Header;
    UdpHeader udpHeader;

    ack->RemoveHeader(dlHeader);
    ack->RemoveHeader(networkHeader);
    ack->RemoveHeader(ipv4Header);
    ack->RemoveHeader(udpHeader);
    ack->RemoveHeader(decoded.tpHeader);

    const uint8_t opcode = decoded.tpHeader.GetTPOpcode();
    if (opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH))
    {
        decoded.hasCongestionHeader = true;
        ack->RemoveHeader(decoded.congestionHeader);
    }
    else if (opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH))
    {
        decoded.hasSelectiveAck = true;
        ack->RemoveHeader(decoded.selectiveAckHeader);
    }
    else if (opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITH_CETPH))
    {
        decoded.hasCongestionHeader = true;
        ack->RemoveHeader(decoded.congestionHeader);
        decoded.hasSelectiveAck = true;
        ack->RemoveHeader(decoded.selectiveAckHeader);
    }

    ack->RemoveHeader(decoded.taAckHeader);
    return decoded;
}

Ptr<Packet>
BuildTpsackForSender(uint32_t ackPsn, bool ackBaseReceived, bool withCetph)
{
    Ptr<Packet> packet = Create<Packet>(0);

    UbAckTransactionHeader taAck;
    taAck.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    packet->AddHeader(taAck);

    UbSelectiveAckExtTph saetph;
    saetph.SetBitmapBitCount(64);
    saetph.SetMaxRcvPsn(ackPsn);
    saetph.SetBitmapBit(0, ackBaseReceived);
    packet->AddHeader(saetph);

    if (withCetph)
    {
        UbCongestionExtTph cetph;
        cetph.SetAckSequence(0);
        packet->AddHeader(cetph);
    }

    UbTransportHeader tpHeader;
    tpHeader.SetTPOpcode(withCetph ? TpOpcode::TP_OPCODE_SACK_WITH_CETPH
                                   : TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH);
    tpHeader.SetSrcTpn(kUrmaWriteRegressionReceiverTpn);
    tpHeader.SetDestTpn(kUrmaWriteRegressionSenderTpn);
    tpHeader.SetPsn(ackPsn);
    packet->AddHeader(tpHeader);
    return packet;
}

Ptr<Packet>
BuildTpsackBitmapForSender(uint32_t ackPsn,
                           uint32_t maxRcvPsn,
                           const std::vector<uint32_t>& receivedOffsets,
                           uint32_t bitmapBits = 64,
                           bool withCetph = false,
                           uint32_t ackSequence = 0)
{
    Ptr<Packet> packet = Create<Packet>(0);

    UbAckTransactionHeader taAck;
    taAck.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    packet->AddHeader(taAck);

    UbSelectiveAckExtTph saetph;
    saetph.SetBitmapBitCount(bitmapBits);
    saetph.SetMaxRcvPsn(maxRcvPsn);
    for (uint32_t offset : receivedOffsets)
    {
        saetph.SetBitmapBit(offset, true);
    }
    packet->AddHeader(saetph);

    if (withCetph)
    {
        UbCongestionExtTph cetph;
        cetph.SetAckSequence(ackSequence);
        packet->AddHeader(cetph);
    }

    UbTransportHeader tpHeader;
    tpHeader.SetTPOpcode(withCetph ? TpOpcode::TP_OPCODE_SACK_WITH_CETPH
                                   : TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH);
    tpHeader.SetSrcTpn(kUrmaWriteRegressionReceiverTpn);
    tpHeader.SetDestTpn(kUrmaWriteRegressionSenderTpn);
    tpHeader.SetPsn(ackPsn);
    packet->AddHeader(tpHeader);
    return packet;
}

Ptr<Packet>
BuildTpackForSender(uint32_t ackPsn)
{
    Ptr<Packet> packet = Create<Packet>(0);

    UbAckTransactionHeader taAck;
    taAck.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    packet->AddHeader(taAck);

    UbTransportHeader tpHeader;
    tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH);
    tpHeader.SetSrcTpn(kUrmaWriteRegressionReceiverTpn);
    tpHeader.SetDestTpn(kUrmaWriteRegressionSenderTpn);
    tpHeader.SetPsn(ackPsn);
    packet->AddHeader(tpHeader);
    return packet;
}

Ptr<Packet>
BuildTpnakForSender(uint32_t nakPsn, bool withCetph = false, uint32_t ackSequence = 0)
{
    Ptr<Packet> packet = Create<Packet>(0);

    UbAckTransactionHeader taAck;
    taAck.SetTaOpcode(TaOpcode::TA_OPCODE_TRANSACTION_ACK);
    packet->AddHeader(taAck);

    if (withCetph)
    {
        UbCongestionExtTph cetph;
        cetph.SetAckSequence(ackSequence);
        packet->AddHeader(cetph);
    }

    UbTransportHeader tpHeader;
    tpHeader.SetTPOpcode(withCetph ? TpOpcode::TP_OPCODE_ACK_WITH_CETPH
                                   : TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH);
    tpHeader.SetRspSt(3);
    tpHeader.SetSrcTpn(kUrmaWriteRegressionReceiverTpn);
    tpHeader.SetDestTpn(kUrmaWriteRegressionSenderTpn);
    tpHeader.SetPsn(nakPsn);
    packet->AddHeader(tpHeader);
    return packet;
}

struct SelectiveRetransmitTraceRecord
{
    uint32_t nodeId;
    uint32_t tpn;
    uint64_t psn;
    uint32_t payloadBytes;
};

static std::vector<SelectiveRetransmitTraceRecord> g_selectiveRetransmitRecordsForTest;

void
RecordSelectiveRetransmitForTest(uint32_t nodeId, uint32_t tpn, uint64_t psn, uint32_t payloadBytes)
{
    g_selectiveRetransmitRecordsForTest.push_back({nodeId, tpn, psn, payloadBytes});
}

class RecordingRetransmissionCc : public UbCongestionControl
{
public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::RecordingRetransmissionCc")
                .SetParent<UbCongestionControl>()
                .SetGroupName("UnifiedBus")
                .AddConstructor<RecordingRetransmissionCc>();
        return tid;
    }

    bool IsCcLimited(uint32_t bytes) override
    {
        lastLimitedCheckBytes = bytes;
        return bytes > 0;
    }

    void OnSenderRetransmissionPacketSent(uint32_t psn, uint32_t size) override
    {
        lastRetransmissionPsn = psn;
        lastRetransmissionBytes = size;
        retransmissionNotifyCount++;
    }

    using UbCongestionControl::OnSenderCongestionNotification;
    void OnSenderCongestionNotification(TpOpcode opcode,
                                        uint32_t psn,
                                        UbCongestionExtTph header,
                                        uint32_t retransmitBytes) override
    {
        (void)opcode;
        (void)psn;
        (void)header;
        lastCongestionNotificationRetransmitBytes = retransmitBytes;
        congestionNotificationCount++;
    }

    uint32_t lastLimitedCheckBytes{0};
    uint32_t lastRetransmissionPsn{0};
    uint32_t lastRetransmissionBytes{0};
    uint32_t retransmissionNotifyCount{0};
    uint32_t lastCongestionNotificationRetransmitBytes{0};
    uint32_t congestionNotificationCount{0};
};

class RecordingTpnakCc : public UbCongestionControl
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::RecordingTpnakCc")
                                .SetParent<UbCongestionControl>()
                                .SetGroupName("UnifiedBus")
                                .AddConstructor<RecordingTpnakCc>();
        return tid;
    }

    TpOpcode GetAckOpcode() const override
    {
        return TpOpcode::TP_OPCODE_ACK_WITH_CETPH;
    }

    UbCongestionExtTph OnReceiverPrepareAckCongestionHeader(uint64_t, uint64_t) override
    {
        ++prepareCount;
        UbCongestionExtTph header;
        header.SetAckSequence(ackSequence);
        return header;
    }

    void OnSenderCongestionNotification(TpOpcode opcode,
                                        uint32_t psn,
                                        UbCongestionExtTph header) override
    {
        ++notificationCount;
        lastOpcode = opcode;
        lastPsn = psn;
        lastAckSequence = header.GetAckSequence();
    }

    void OnSenderCongestionNotification(TpOpcode opcode,
                                        uint32_t psn,
                                        UbCongestionExtTph header,
                                        uint32_t retransmitBytes) override
    {
        OnSenderCongestionNotification(opcode, psn, header);
        lastRetransmitBytes = retransmitBytes;
    }

    uint32_t ackSequence{17};
    uint32_t prepareCount{0};
    uint32_t notificationCount{0};
    TpOpcode lastOpcode{TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH};
    uint32_t lastPsn{0};
    uint32_t lastAckSequence{0};
    uint32_t lastRetransmitBytes{0};
};

Ptr<UbTransportChannel>
CreateSelectiveReceiverTp(const LocalTpTopology& topo)
{
    InstallStaticTpPair(topo);
    return topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
}

#ifndef _WIN32
int
RunInChildProcess(const std::function<void()>& fn);
#endif

} // namespace

/**
 * @brief Unified-bus functionality test
 * 
 * Tests basic unified-bus module functionality including:
 * - Object creation and initialization
 * - Singleton pattern verification
 * - Configuration system integration
 * - Basic API functionality
 */
class UbFunctionalityTest : public TestCase
{
public:
    UbFunctionalityTest();
    void DoRun() override;
    
private:
    void DoSetup() override;
    void DoTeardown() override;
};

UbFunctionalityTest::UbFunctionalityTest()
    : TestCase("UnifiedBus - Core functionality test")
{
}

void UbFunctionalityTest::DoSetup()
{
    Config::Reset();
    RngSeedManager::SetSeed(12345);
}

void UbFunctionalityTest::DoTeardown()
{
    // Minimal cleanup
    if (!Simulator::IsFinished()) {
        Simulator::Destroy();
    }
}

void UbFunctionalityTest::DoRun()
{
    NS_LOG_FUNCTION(this);
    
    // Test 1: UbTrafficGen singleton
    UbTrafficGen& gen1 = UbTrafficGen::GetInstance();
    UbTrafficGen& gen2 = UbTrafficGen::GetInstance();
    NS_TEST_ASSERT_MSG_EQ(&gen1, &gen2, "UbTrafficGen should be singleton");
    
    // Test 2: Initial state
    NS_TEST_ASSERT_MSG_EQ(gen1.IsCompleted(), true, "UbTrafficGen should be completed initially");
    
    // Test 3: UbApp creation
    Ptr<UbApp> app = CreateObject<UbApp>();
    NS_TEST_ASSERT_MSG_NE(app, nullptr, "UbApp creation should succeed");
    
    // Test 4: Node creation
    NodeContainer nodes;
    nodes.Create(2);
    NS_TEST_ASSERT_MSG_EQ(nodes.GetN(), 2, "Should create 2 nodes");
    
    // Test 5: Configuration setting (without getting)
    Config::SetDefault("ns3::UbApp::EnableMultiPath", BooleanValue(false));
    Config::SetDefault("ns3::UbPort::UbDataRate", StringValue("400Gbps"));
    
    NS_LOG_INFO("All basic tests completed successfully");
}

class UbControllerCtpLazyServiceTest : public TestCase
{
  public:
    UbControllerCtpLazyServiceTest()
        : TestCase("UnifiedBus - controller lazily creates CTP transport service")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);

        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        NS_TEST_ASSERT_MSG_NE(service, nullptr, "CTP transport service should be created lazily");
        NS_TEST_ASSERT_MSG_EQ(service->GetNode(), node, "CTP transport service should bind the node");
        NS_TEST_ASSERT_MSG_EQ(controller->GetCtpTransportService(),
                              service,
                              "Repeated accessor calls should return the same service");

        Simulator::Destroy();
    }
};

class UbTrafficConfigEntityFieldsTest : public TestCase
{
  public:
    UbTrafficConfigEntityFieldsTest()
        : TestCase("UnifiedBus - traffic parser accepts optional source and destination entity ids")
    {
    }

  private:
    void DoRun() override
    {
        const auto trafficPath =
            std::filesystem::temp_directory_path() / "ub-traffic-config-entity-fields.csv";
        {
            std::ofstream trafficFile(trafficPath);
            NS_TEST_ASSERT_MSG_EQ(trafficFile.good(), true, "temporary traffic file should open");
            trafficFile << "taskId,sourceNode,destNode,dataSize,opType,priority,delay,phaseId,"
                           "dependOnPhases,srcEntityId,dstEntityId\r\n";
            trafficFile << "1,2,3,64,URMA_WRITE,1,0ns,10,\r\n";
            trafficFile << "2,4,5,128,URMA_READ,2,7ns,20,10,11,12\r\n";
            trafficFile << "3,6,7,256,URMA_WRITE,3,0ns,30,,,\r\n";
        }

        auto records = UbUtils::Get()->LoadTrafficConfig(trafficPath.string());
        NS_TEST_ASSERT_MSG_EQ(records.size(), 3u, "three traffic records should load");
        NS_TEST_ASSERT_MSG_EQ(records[0].srcEntityId, 0u, "9-field rows should default source entity to 0");
        NS_TEST_ASSERT_MSG_EQ(records[0].dstEntityId, 0u, "9-field rows should default destination entity to 0");
        NS_TEST_ASSERT_MSG_EQ(records[0].hasSrcEntityId, false, "9-field rows should not mark source entity present");
        NS_TEST_ASSERT_MSG_EQ(records[0].hasDstEntityId, false, "9-field rows should not mark destination entity present");
        NS_TEST_ASSERT_MSG_EQ(records[1].srcEntityId, 11u, "11-field rows should parse source entity");
        NS_TEST_ASSERT_MSG_EQ(records[1].dstEntityId, 12u, "11-field rows should parse destination entity");
        NS_TEST_ASSERT_MSG_EQ(records[1].hasSrcEntityId, true, "11-field rows should mark source entity present");
        NS_TEST_ASSERT_MSG_EQ(records[1].hasDstEntityId, true, "11-field rows should mark destination entity present");
        NS_TEST_ASSERT_MSG_EQ(records[2].srcEntityId, 0u, "empty source entity field should parse as 0");
        NS_TEST_ASSERT_MSG_EQ(records[2].dstEntityId, 0u, "empty destination entity field should parse as 0");
        NS_TEST_ASSERT_MSG_EQ(records[2].hasSrcEntityId, true, "empty 11-field source entity should still be present");
        NS_TEST_ASSERT_MSG_EQ(records[2].hasDstEntityId, true, "empty 11-field destination entity should still be present");

        uint32_t viewIndex = 0;
        UbUtils::Get()->ForEachTrafficRecordView(
            trafficPath.string(),
            [this, &viewIndex](const TrafficRecordView& record) {
                if (viewIndex == 0)
                {
                    NS_TEST_ASSERT_MSG_EQ(record.srcEntityId, 0u, "9-field views should default source entity to 0");
                    NS_TEST_ASSERT_MSG_EQ(record.dstEntityId, 0u, "9-field views should default destination entity to 0");
                    NS_TEST_ASSERT_MSG_EQ(record.hasSrcEntityId,
                                          false,
                                          "9-field views should not mark source entity present");
                    NS_TEST_ASSERT_MSG_EQ(record.hasDstEntityId,
                                          false,
                                          "9-field views should not mark destination entity present");
                }
                else if (viewIndex == 1)
                {
                    NS_TEST_ASSERT_MSG_EQ(record.srcEntityId, 11u, "11-field views should parse source entity");
                    NS_TEST_ASSERT_MSG_EQ(record.dstEntityId, 12u, "11-field views should parse destination entity");
                    NS_TEST_ASSERT_MSG_EQ(record.hasSrcEntityId,
                                          true,
                                          "11-field views should mark source entity present");
                    NS_TEST_ASSERT_MSG_EQ(record.hasDstEntityId,
                                          true,
                                          "11-field views should mark destination entity present");
                }
                else if (viewIndex == 2)
                {
                    NS_TEST_ASSERT_MSG_EQ(record.srcEntityId, 0u, "empty source entity view field should parse as 0");
                    NS_TEST_ASSERT_MSG_EQ(record.dstEntityId, 0u, "empty destination entity view field should parse as 0");
                    NS_TEST_ASSERT_MSG_EQ(record.hasSrcEntityId,
                                          true,
                                          "empty 11-field view source entity should still be present");
                    NS_TEST_ASSERT_MSG_EQ(record.hasDstEntityId,
                                          true,
                                          "empty 11-field view destination entity should still be present");
                }
                ++viewIndex;
            });
        NS_TEST_ASSERT_MSG_EQ(viewIndex, 3u, "three traffic record views should stream");

        std::filesystem::remove(trafficPath);
        Simulator::Destroy();
    }
};

class UbAppCtpTransportModeEntryTest : public TestCase
{
  public:
    UbAppCtpTransportModeEntryTest()
        : TestCase("UnifiedBus - app CTP transport mode enters CTP service without RTP TP setup")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();

        Ptr<UbApp> app = CreateObject<UbApp>();
        node->AddApplication(app);
        app->SetTransportMode(TransportMode::CTP);
        app->SetLocalEntityId(21);
        app->SetPeerEntityId(22);

        TrafficRecord record;
        record.taskId = 1;
        record.sourceNode = 3;
        record.destNode = 4;
        record.dataSize = 64;
        record.opType = "URMA_WRITE";
        record.priority = 5;
        record.delay = "0ns";
        record.phaseId = 1;

        const uint32_t initialTransportCount = controller->GetTransportCountForTest();
        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        service->SetSourcePortHint(UbCtpEntityKey{.srcNodeId = 3,
                                                  .srcEntityId = 21,
                                                  .dstNodeId = 4,
                                                  .dstEntityId = 22,
                                                  .vl = 5},
                                   0);
        app->SendTraffic(record);

        UbCtpEntityKey key{.srcNodeId = 3,
                           .srcEntityId = 21,
                           .dstNodeId = 4,
                           .dstEntityId = 22,
                           .vl = 5};
        NS_TEST_ASSERT_MSG_NE(service, nullptr, "CTP mode should initialize the CTP service");
        NS_TEST_ASSERT_MSG_EQ(controller->GetTransportCountForTest(),
                              initialTransportCount,
                              "CTP mode should not create RTP transport channels");
        NS_TEST_ASSERT_MSG_EQ(service->HasTransactionContextForTesting(key),
                              true,
                              "CTP mode should create a transaction context for the app key");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              1u,
                              "CTP mode should drain the single WQE segment into the CTP service queue");

        Simulator::Destroy();
    }
};

class UbTrafficGenRuntimeTaskEntityFieldsTest : public TestCase
{
  public:
    UbTrafficGenRuntimeTaskEntityFieldsTest()
        : TestCase("UnifiedBus - runtime task conversion preserves CTP entity fields and presence")
    {
    }

  private:
    void DoRun() override
    {
        UbTrafficGen gen;

        TrafficRecordView record;
        record.taskId = 9;
        record.sourceNode = 1;
        record.destNode = 2;
        record.dataSize = 64;
        record.opType = "URMA_WRITE";
        record.priority = 3;
        record.delay = "0ns";
        record.phaseId = 4;
        record.srcEntityId = 0;
        record.dstEntityId = 12;
        record.hasSrcEntityId = true;
        record.hasDstEntityId = true;

        UbTrafficGen::RuntimeTask task = gen.ConvertToRuntimeTask(record);

        NS_TEST_ASSERT_MSG_EQ(task.srcEntityId,
                              0u,
                              "runtime task should preserve explicit source entity 0");
        NS_TEST_ASSERT_MSG_EQ(task.dstEntityId,
                              12u,
                              "runtime task should preserve destination entity");
        NS_TEST_ASSERT_MSG_EQ(task.hasSrcEntityId,
                              true,
                              "runtime task should preserve source entity presence");
        NS_TEST_ASSERT_MSG_EQ(task.hasDstEntityId,
                              true,
                              "runtime task should preserve destination entity presence");
        NS_TEST_ASSERT_MSG_EQ(task.hasPhaseDependencies,
                              false,
                              "an empty dependency field should mark an initial task");

        record.taskId = 10;
        record.dependOnPhases = "3";
        task = gen.ConvertToRuntimeTask(record);
        NS_TEST_ASSERT_MSG_EQ(task.hasPhaseDependencies,
                              true,
                              "a non-empty dependency field should mark a dependent task");
    }
};

class UbAppCtpRuntimeTaskEntityFallbackTest : public TestCase
{
  public:
    UbAppCtpRuntimeTaskEntityFallbackTest()
        : TestCase("UnifiedBus - app CTP runtime task path honors explicit entity zero")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();

        Ptr<UbApp> app = CreateObject<UbApp>();
        node->AddApplication(app);
        app->SetTransportMode(TransportMode::CTP);
        app->SetLocalEntityId(21);
        app->SetPeerEntityId(22);

        UbTrafficGen::RuntimeTask task;
        task.taskId = 2;
        task.sourceNode = 3;
        task.destNode = 4;
        task.dataSize = 64;
        task.phaseId = 1;
        task.priority = 5;
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_WRITE;
        task.srcEntityId = 0;
        task.dstEntityId = 12;
        task.hasSrcEntityId = true;
        task.hasDstEntityId = true;

        const uint32_t initialTransportCount = controller->GetTransportCountForTest();
        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        service->SetSourcePortHint(UbCtpEntityKey{.srcNodeId = 3,
                                                  .srcEntityId = 0,
                                                  .dstNodeId = 4,
                                                  .dstEntityId = 12,
                                                  .vl = 5},
                                   0);
        app->SendTraffic(task);

        UbCtpEntityKey explicitKey{.srcNodeId = 3,
                                   .srcEntityId = 0,
                                   .dstNodeId = 4,
                                   .dstEntityId = 12,
                                   .vl = 5};
        UbCtpEntityKey fallbackKey{.srcNodeId = 3,
                                   .srcEntityId = 21,
                                   .dstNodeId = 4,
                                   .dstEntityId = 12,
                                   .vl = 5};

        NS_TEST_ASSERT_MSG_EQ(service->HasTransactionContextForTesting(explicitKey),
                              true,
                              "runtime CTP branch should create the explicit entity-0 context");
        NS_TEST_ASSERT_MSG_EQ(service->HasTransactionContextForTesting(fallbackKey),
                              false,
                              "explicit entity 0 must not fall back to the app default entity");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(explicitKey),
                              1u,
                              "runtime CTP branch should drain the single WQE segment into the explicit key queue");
        NS_TEST_ASSERT_MSG_EQ(controller->GetTransportCountForTest(),
                              initialTransportCount,
                              "CTP runtime task path should not create RTP transport channels");

        Simulator::Destroy();
    }
};

class UbAppCtpAdmissionBackpressurePreservesJettyTest : public TestCase
{
  public:
    UbAppCtpAdmissionBackpressurePreservesJettyTest()
        : TestCase("UnifiedBus - app CTP path does not consume Jetty segments under admission backpressure")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();

        Ptr<UbApp> app = CreateObject<UbApp>();
        node->AddApplication(app);
        app->SetTransportMode(TransportMode::CTP);
        app->SetLocalEntityId(21);
        app->SetPeerEntityId(22);

        UbCtpEntityKey key{.srcNodeId = 3,
                           .srcEntityId = 21,
                           .dstNodeId = 4,
                           .dstEntityId = 22,
                           .vl = 5};
        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        service->SetSourcePortHint(key, 0);
        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(key);
        for (uint32_t sequence = 0; sequence < UB_JETTY_TASSN_OOO_THRESHOLD; ++sequence)
        {
            NS_TEST_ASSERT_MSG_EQ(context->TryAdmit(sequence),
                                  true,
                                  "test setup should fill the CTP admission window");
        }

        UbTrafficGen::RuntimeTask task;
        task.taskId = 33;
        task.sourceNode = 3;
        task.destNode = 4;
        task.dataSize = 2 * UB_WQE_TA_SEGMENT_BYTE;
        task.phaseId = 1;
        task.priority = 5;
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_WRITE;

        app->SendCtpUrmaTraffic(task);

        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              0u,
                              "full CTP admission window should prevent app from queueing packets");
        NS_TEST_ASSERT_MSG_EQ(context->GetSendNext(),
                              UB_JETTY_TASSN_OOO_THRESHOLD,
                              "blocked app send must not advance the transaction context");

        Ptr<UbJetty> jetty = controller->GetUbFunction()->GetJetty(0);
        NS_TEST_ASSERT_MSG_NE(jetty,
                              nullptr,
                              "blocked app send should still leave the created Jetty available");
        NS_TEST_ASSERT_MSG_EQ(jetty->GetTaSsnSndNxtForTest(),
                              0u,
                              "blocked app send must not consume a Jetty segment");

        Simulator::Destroy();
    }
};

class UbAppCtpForwardsRoutingFlagsToServiceTest : public TestCase
{
  public:
    UbAppCtpForwardsRoutingFlagsToServiceTest()
        : TestCase("UnifiedBus - app CTP path forwards routing flags to CTP service")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_DEVICE, 2);

        Ptr<UbApp> app = CreateObject<UbApp>();
        node->AddApplication(app);
        app->SetTransportMode(TransportMode::CTP);
        app->SetAttribute("UseShortestPaths", BooleanValue(true));
        app->SetAttribute("UsePacketSpray", BooleanValue(true));

        Ptr<UbRoutingProcess> routing = node->GetObject<UbSwitch>()->GetRoutingProcess();
        routing->AddShortestRoute(NodeIdToIp(4).Get(), {0, 1});

        UbTrafficGen::RuntimeTask task;
        task.taskId = 44;
        task.sourceNode = node->GetId();
        task.destNode = 4;
        task.dataSize = 2 * UB_WQE_TA_SEGMENT_BYTE;
        task.phaseId = 1;
        task.priority = 5;
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_WRITE;
        task.srcEntityId = 0;
        task.dstEntityId = 0;
        task.hasSrcEntityId = true;
        task.hasDstEntityId = true;

        app->SendCtpUrmaTraffic(task);

        Ptr<UbCtpTransportService> service = node->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = node->GetId(),
                           .srcEntityId = 0,
                           .dstNodeId = 4,
                           .dstEntityId = 0,
                           .vl = 5};

        const uint32_t queuedOnPort0 = service->GetQueuedPacketCountForTest(key, 0, key.vl);
        const uint32_t queuedOnPort1 = service->GetQueuedPacketCountForTest(key, 1, key.vl);
        NS_TEST_ASSERT_MSG_NE(queuedOnPort0,
                              0u,
                              "packet-spray CTP app path should use port 0");
        NS_TEST_ASSERT_MSG_NE(queuedOnPort1,
                              0u,
                              "packet-spray CTP app path should use port 1");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              task.dataSize / UB_MTU_BYTE,
                              "packet-spray CTP app path should queue one MTU packet per CTP TA unit");

        Simulator::Destroy();
    }
};

#ifndef _WIN32
class UbAppCtpTrafficRecordRejectsInvalidPriorityTest : public TestCase
{
  public:
    UbAppCtpTrafficRecordRejectsInvalidPriorityTest()
        : TestCase("UnifiedBus - direct app CTP traffic record rejects out-of-range priority")
    {
    }

  private:
    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Ptr<UbApp> app = CreateObject<UbApp>();

            TrafficRecord record;
            record.taskId = 1;
            record.sourceNode = 3;
            record.destNode = 4;
            record.dataSize = 64;
            record.opType = "URMA_WRITE";
            record.priority = static_cast<int>(UB_PRIORITY_MAX) + 1;
            record.delay = "0ns";
            record.phaseId = 1;

            app->SendCtpUrmaTraffic(record);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "direct CTP TrafficRecord with invalid priority should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "invalid direct CTP TrafficRecord priority should fail with SIGABRT");
    }
};
#endif

class UbTrafficGenPhaseDependencyMemoryTest : public TestCase
{
  public:
    UbTrafficGenPhaseDependencyMemoryTest()
        : TestCase("UnifiedBus - traffic generator keeps phase dependency memory linear")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;
        constexpr uint32_t kTasksPerPhase = 256;
        constexpr uint32_t kDependencyPhase = 10;
        constexpr uint32_t kDependentPhase = 20;

        for (uint32_t taskId = 0; taskId < kTasksPerPhase; ++taskId)
        {
            TrafficRecord record;
            record.taskId = taskId;
            record.sourceNode = 0;
            record.destNode = 0;
            record.dataSize = 64;
            record.opType = "URMA_WRITE";
            record.priority = 1;
            record.delay = "0ns";
            record.phaseId = kDependencyPhase;
            gen.SetPhaseDepend(record.phaseId, record.taskId);
            gen.AddTask(record);
        }

        for (uint32_t taskId = kTasksPerPhase; taskId < 2 * kTasksPerPhase; ++taskId)
        {
            TrafficRecord record;
            record.taskId = taskId;
            record.sourceNode = 0;
            record.destNode = 0;
            record.dataSize = 64;
            record.opType = "URMA_WRITE";
            record.priority = 1;
            record.delay = "0ns";
            record.phaseId = kDependentPhase;
            record.dependOnPhases.push_back(kDependencyPhase);
            gen.SetPhaseDepend(record.phaseId, record.taskId);
            gen.AddTask(record);
        }

        const uint64_t materializedTaskReferences =
            static_cast<uint64_t>(kTasksPerPhase) * kTasksPerPhase;

        NS_TEST_ASSERT_MSG_EQ(gen.GetDependencyReferenceCountForTesting(),
                              kTasksPerPhase,
                              "phase DAG should store one reverse reference per dependent task");
        NS_TEST_ASSERT_MSG_LT(gen.GetDependencyReferenceCountForTesting(),
                              materializedTaskReferences,
                              "phase DAG must not expand one phase dependency into all task edges");

        gen.ScheduleNextTasks();
        gen.ApplyTaskCompletion(0);
        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(kTasksPerPhase),
                              1u,
                              "dependent phase should wait until every task in the previous phase "
                              "completes");

        for (uint32_t taskId = 1; taskId < kTasksPerPhase; ++taskId)
        {
            gen.ApplyTaskCompletion(taskId);
        }
        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(kTasksPerPhase),
                              0u,
                              "dependent phase should become ready only after the whole previous "
                              "phase completes");
        Simulator::Destroy();
    }
};

class UbTrafficGenSparseTaskIdFallbackTest : public TestCase
{
  public:
    UbTrafficGenSparseTaskIdFallbackTest()
        : TestCase("UnifiedBus - traffic generator preserves dependencies for sparse task ids")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;

        TrafficRecord dependency;
        dependency.taskId = 0;
        dependency.sourceNode = 0;
        dependency.destNode = 0;
        dependency.dataSize = 64;
        dependency.opType = "URMA_WRITE";
        dependency.priority = 1;
        dependency.delay = "0ns";
        dependency.phaseId = 10;
        gen.SetPhaseDepend(dependency.phaseId, dependency.taskId);
        gen.AddTask(dependency);

        TrafficRecord dependent;
        dependent.taskId = 10;
        dependent.sourceNode = 0;
        dependent.destNode = 0;
        dependent.dataSize = 64;
        dependent.opType = "URMA_WRITE";
        dependent.priority = 1;
        dependent.delay = "0ns";
        dependent.phaseId = 20;
        dependent.dependOnPhases.push_back(10);
        gen.SetPhaseDepend(dependent.phaseId, dependent.taskId);
        gen.AddTask(dependent);

        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(dependent.taskId),
                              1u,
                              "sparse dependent task should wait for its dependency phase");

        gen.ScheduleNextTasks();
        gen.ApplyTaskCompletion(dependency.taskId);

        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(dependent.taskId),
                              0u,
                              "sparse dependent task should be released after dependency phase "
                              "completes");
        Simulator::Destroy();
    }
};

class UbTrafficGenReserveHintTest : public TestCase
{
  public:
    UbTrafficGenReserveHintTest()
        : TestCase("UnifiedBus - traffic generator accepts reserve hints without behavior change")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;
        gen.ReserveTasksForTraffic(3, 2);

        TrafficRecord first;
        first.taskId = 0;
        first.sourceNode = 0;
        first.destNode = 0;
        first.dataSize = 64;
        first.opType = "URMA_WRITE";
        first.priority = 1;
        first.delay = "0ns";
        first.phaseId = 1;
        gen.SetPhaseDepend(first.phaseId, first.taskId);
        gen.AddTask(first);

        TrafficRecord second;
        second.taskId = 1;
        second.sourceNode = 0;
        second.destNode = 0;
        second.dataSize = 64;
        second.opType = "URMA_WRITE";
        second.priority = 1;
        second.delay = "0ns";
        second.phaseId = 2;
        second.dependOnPhases.push_back(1);
        gen.SetPhaseDepend(second.phaseId, second.taskId);
        gen.AddTask(second);

        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(1),
                              1u,
                              "reserve hints must not change dependency activation");
        Simulator::Destroy();
    }
};

class UbTrafficGenDenseTaskStoreTest : public TestCase
{
  public:
    UbTrafficGenDenseTaskStoreTest()
        : TestCase("UnifiedBus - traffic generator stores contiguous tasks densely")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;
        gen.ReserveTasksForTraffic(2, 1);

        for (uint32_t taskId = 0; taskId < 2; ++taskId)
        {
            TrafficRecord record;
            record.taskId = taskId;
            record.sourceNode = 0;
            record.destNode = 0;
            record.dataSize = 64;
            record.opType = "URMA_WRITE";
            record.priority = 1;
            record.delay = "0ns";
            record.phaseId = taskId + 1;
            gen.SetPhaseDepend(record.phaseId, record.taskId);
            gen.AddTask(record);
        }

        NS_TEST_ASSERT_MSG_EQ(gen.GetStoredTaskCountForTesting(),
                              2u,
                              "two tasks should be stored");
        NS_TEST_ASSERT_MSG_EQ(gen.IsUsingDenseTaskStoreForTesting(),
                              true,
                              "contiguous task ids should use dense task store");
        Simulator::Destroy();
    }
};

class UbTrafficGenDuplicateDependencyPhaseTest : public TestCase
{
  public:
    UbTrafficGenDuplicateDependencyPhaseTest()
        : TestCase("UnifiedBus - duplicate dependency phases count once")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;

        TrafficRecord dependency;
        dependency.taskId = 0;
        dependency.sourceNode = 0;
        dependency.destNode = 0;
        dependency.dataSize = 64;
        dependency.opType = "URMA_WRITE";
        dependency.priority = 1;
        dependency.delay = "0ns";
        dependency.phaseId = 10;
        gen.SetPhaseDepend(dependency.phaseId, dependency.taskId);
        gen.AddTask(dependency);

        TrafficRecord dependent;
        dependent.taskId = 1;
        dependent.sourceNode = 0;
        dependent.destNode = 0;
        dependent.dataSize = 64;
        dependent.opType = "URMA_WRITE";
        dependent.priority = 1;
        dependent.delay = "0ns";
        dependent.phaseId = 20;
        dependent.dependOnPhases = {10, 10, 10};
        gen.SetPhaseDepend(dependent.phaseId, dependent.taskId);
        gen.AddTask(dependent);

        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(1),
                              1u,
                              "duplicate references to one phase should count as one dependency");
        Simulator::Destroy();
    }
};

class UbTrafficGenRecordViewDependencyTest : public TestCase
{
  public:
    UbTrafficGenRecordViewDependencyTest()
        : TestCase("UnifiedBus - traffic generator record view matches dependency semantics")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;
        gen.ReserveTasksForTraffic(3, 2);
        gen.RegisterPhaseTaskDuringInitialLoad(10);
        gen.RegisterPhaseTaskDuringInitialLoad(20);
        gen.RegisterPhaseTaskDuringInitialLoad(30);

        TrafficRecordView first;
        first.taskId = 0;
        first.sourceNode = 0;
        first.destNode = 0;
        first.dataSize = 64;
        first.opType = "URMA_WRITE";
        first.priority = 1;
        first.delay = "0ns";
        first.phaseId = 10;
        gen.AddTaskDuringInitialLoad(first);

        TrafficRecordView second;
        second.taskId = 1;
        second.sourceNode = 0;
        second.destNode = 0;
        second.dataSize = 64;
        second.opType = "URMA_WRITE";
        second.priority = 1;
        second.delay = "0ns";
        second.phaseId = 20;
        gen.AddTaskDuringInitialLoad(second);

        TrafficRecordView dependent;
        dependent.taskId = 2;
        dependent.sourceNode = 0;
        dependent.destNode = 0;
        dependent.dataSize = 64;
        dependent.opType = "URMA_WRITE";
        dependent.priority = 1;
        dependent.delay = "0ns";
        dependent.phaseId = 30;
        dependent.dependOnPhases = "10  10\t20";
        gen.AddTaskDuringInitialLoad(dependent);

        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(2),
                              2u,
                              "record view should deduplicate repeated dependency phases");

        gen.ScheduleNextTasks();
        gen.ApplyTaskCompletion(0);
        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(2),
                              1u,
                              "record view dependent task should still wait for the second phase");

        gen.ApplyTaskCompletion(1);
        NS_TEST_ASSERT_MSG_EQ(gen.GetPendingPhaseCountForTesting(2),
                              0u,
                              "record view dependent task should release after all phases complete");
        Simulator::Destroy();
    }
};

class UbTrafficGenReadyOrderTest : public TestCase
{
  public:
    UbTrafficGenReadyOrderTest()
        : TestCase("UnifiedBus - ready task collection preserves ascending task order")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        node->AddApplication(CreateObject<UbApp>());

        UbTrafficGen gen;
        gen.ReserveTasksForTraffic(3, 2);

        for (uint32_t taskId = 0; taskId < 3; ++taskId)
        {
            TrafficRecord record;
            record.taskId = taskId;
            record.sourceNode = 0;
            record.destNode = 0;
            record.dataSize = 64;
            record.opType = "URMA_WRITE";
            record.priority = 1;
            record.delay = "0ns";
            record.phaseId = taskId + 1;
            gen.SetPhaseDepend(record.phaseId, record.taskId);
            gen.AddTask(record);
        }

        const auto readyIds = gen.CollectReadyTaskIdsForTesting();
        NS_TEST_ASSERT_MSG_EQ(readyIds.size(), 3u, "three ready tasks should be collected");
        NS_TEST_ASSERT_MSG_EQ(readyIds[0], 0u, "first ready task should be task 0");
        NS_TEST_ASSERT_MSG_EQ(readyIds[1], 1u, "second ready task should be task 1");
        NS_TEST_ASSERT_MSG_EQ(readyIds[2], 2u, "third ready task should be task 2");
        Simulator::Destroy();
    }
};

class UbTrafficGenInitialTaskStartOffsetTest : public TestCase
{
  public:
    UbTrafficGenInitialTaskStartOffsetTest()
        : TestCase("UnifiedBus - initial task start offsets are deterministic by source")
    {
    }

    void DoRun() override
    {
        const auto makeTask =
            [](uint32_t taskId, uint32_t sourceNode, bool hasDependencies = false) {
                UbTrafficGen::RuntimeTask task;
                task.taskId = taskId;
                task.sourceNode = sourceNode;
                task.phaseId = 1;
                task.hasPhaseDependencies = hasDependencies;
                return task;
            };

        UbTrafficGen gen;
        UbTrafficGen::ReadyTaskBatch batch;
        batch.tasks = {
            makeTask(100, 2),
            makeTask(101, 7),
            makeTask(102, 11),
            makeTask(103, 2),
            makeTask(104, 13, true),
        };

        gen.SetInitialTaskStartOffsetWindow(Time(0), 1);
        const auto disabled = gen.GetInitialTaskStartOffsets(batch);
        for (const auto& offset : disabled)
        {
            NS_TEST_ASSERT_MSG_EQ(offset,
                                  Time(0),
                                  "disabled initial task offset should preserve task time");
        }

        gen.SetInitialTaskStartOffsetWindow(NanoSeconds(32), 1);
        const auto seedOne = gen.GetInitialTaskStartOffsets(batch);
        const auto seedOneRepeat = gen.GetInitialTaskStartOffsets(batch);
        NS_TEST_ASSERT_MSG_EQ((seedOne == seedOneRepeat),
                              true,
                              "initial task offsets should repeat exactly for the same seed");
        NS_TEST_ASSERT_MSG_EQ(seedOne[0],
                              seedOne[3],
                              "initial tasks from one source should share one offset");
        NS_TEST_ASSERT_MSG_EQ(seedOne[4],
                              Time(0),
                              "dependent tasks should not receive an initial task offset");
        for (size_t index = 0; index < 4; ++index)
        {
            NS_TEST_ASSERT_MSG_EQ(seedOne[index].IsStrictlyNegative(),
                                  false,
                                  "initial task offsets should be non-negative");
            NS_TEST_ASSERT_MSG_EQ((seedOne[index] < NanoSeconds(32)),
                                  true,
                                  "initial task offsets should stay inside the configured window");
        }

        gen.SetInitialTaskStartOffsetWindow(NanoSeconds(32), 2);
        const auto seedTwo = gen.GetInitialTaskStartOffsets(batch);
        bool seedChangesOffsets = false;
        for (size_t index = 0; index < 4; ++index)
        {
            seedChangesOffsets |= seedOne[index] != seedTwo[index];
        }
        NS_TEST_ASSERT_MSG_EQ(seedChangesOffsets,
                              true,
                              "changing the seed should change at least one source offset");

        UbTrafficGen::ReadyTaskBatch collidingTasks;
        collidingTasks.tasks = {makeTask(200, 21), makeTask(201, 22)};
        gen.SetInitialTaskStartOffsetWindow(TimeStep(1), 1);
        std::ostringstream warning;
        std::streambuf* previousStderr = std::cerr.rdbuf(warning.rdbuf());
        (void)gen.GetInitialTaskStartOffsets(collidingTasks);
        std::cerr.rdbuf(previousStderr);
        NS_TEST_ASSERT_MSG_NE(warning.str().find("offset-reuses=1"),
                              std::string::npos,
                              "reused initial task offsets should be visible in release builds");
    }
};

class UbLinkDelayOffsetTest : public TestCase
{
  public:
    UbLinkDelayOffsetTest()
        : TestCase("UnifiedBus - link delay offsets are deterministic by stable endpoint identity")
    {
    }

    void DoRun() override
    {
        const Time window = NanoSeconds(8);
        const auto getDelay = [&](uint32_t seed,
                                  uint32_t node1,
                                  uint32_t port1,
                                  uint32_t node2,
                                  uint32_t port2,
                                  Time baseDelay = NanoSeconds(20)) {
            return utils::UbUtils::ResolveLinkDelayWithOffset(baseDelay,
                                                              window,
                                                              seed,
                                                              node1,
                                                              port1,
                                                              node2,
                                                              port2);
        };

        const Time forward = getDelay(1, 2, 3, 7, 5);
        const Time reverse = getDelay(1, 7, 5, 2, 3);
        NS_TEST_ASSERT_MSG_EQ(forward,
                              reverse,
                              "reversing one physical link should preserve its delay offset");
        NS_TEST_ASSERT_MSG_EQ((forward >= NanoSeconds(20)),
                              true,
                              "link offset should never reduce the configured base delay");
        NS_TEST_ASSERT_MSG_EQ((forward < NanoSeconds(28)),
                              true,
                              "link offset should stay inside the configured half-open window");
        NS_TEST_ASSERT_MSG_EQ(getDelay(1, 2, 3, 7, 5),
                              forward,
                              "the same seed and link identity should repeat exactly");

        const std::array<std::array<uint32_t, 4>, 4> links{{
            {{2, 3, 7, 5}},
            {{2, 4, 9, 1}},
            {{11, 0, 20, 6}},
            {{13, 7, 17, 2}},
        }};
        bool seedChangesOffset = false;
        for (const auto& link : links)
        {
            seedChangesOffset |= getDelay(1, link[0], link[1], link[2], link[3]) !=
                                 getDelay(2, link[0], link[1], link[2], link[3]);
        }
        NS_TEST_ASSERT_MSG_EQ(seedChangesOffset,
                              true,
                              "changing the seed should change at least one link offset");
        NS_TEST_ASSERT_MSG_EQ(
            utils::UbUtils::ResolveLinkDelayWithOffset(Time(0), window, 1, 2, 3, 7, 5),
            Time(0),
            "zero-delay links should retain the MTP co-location boundary");
        NS_TEST_ASSERT_MSG_EQ(
            utils::UbUtils::ResolveLinkDelayWithOffset(NanoSeconds(20), Time(0), 1, 2, 3, 7, 5),
            NanoSeconds(20),
            "a zero offset window should preserve the configured delay");
    }
};

class UbTrafficGenIntegerDelayParserTest : public TestCase
{
  public:
    UbTrafficGenIntegerDelayParserTest()
        : TestCase("UnifiedBus - traffic generator integer delay fast parser matches Time")
    {
    }

    void DoRun() override
    {
        for (const std::string& delay : {"0ns", "10ns", "7us", "3ms", "2s", "1min"})
        {
            Time parsed;
            NS_TEST_ASSERT_MSG_EQ(UbTrafficGen::TryParseIntegerDelay(delay, &parsed),
                                  true,
                                  "integer delay should use fast parser");
            NS_TEST_ASSERT_MSG_EQ(parsed.GetTimeStep(),
                                  Time(delay).GetTimeStep(),
                                  "fast parsed delay should match Time(string)");
        }

        Time parsed;
        NS_TEST_ASSERT_MSG_EQ(UbTrafficGen::TryParseIntegerDelay(std::string_view("1.5ms"),
                                                                 &parsed),
                              false,
                              "fractional delay should fall back to Time(string)");
    }
};

class UbDcqcnFactoryCreatesHostAndSwitchTest : public TestCase
{
public:
    UbDcqcnFactoryCreatesHostAndSwitchTest()
        : TestCase("UnifiedBus - DCQCN factory creates host and switch implementations")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<UbCongestionControl> host = UbCongestionControl::Create(UB_DEVICE);
        Ptr<UbCongestionControl> sw = UbCongestionControl::Create(UB_SWITCH);

        NS_TEST_ASSERT_MSG_NE(host, nullptr, "Host DCQCN factory should return an object");
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Switch DCQCN factory should return an object");
        NS_TEST_ASSERT_MSG_EQ(host->GetCongestionAlgo(),
                              DCQCN,
                              "Host object should report DCQCN");
        NS_TEST_ASSERT_MSG_EQ(sw->GetCongestionAlgo(),
                              DCQCN,
                              "Switch object should report DCQCN");
    }
};

class UbDcqcnHostAckCeTphDefaultTest : public TestCase
{
public:
    UbDcqcnHostAckCeTphDefaultTest()
        : TestCase("UnifiedBus - DCQCN host default ACK CETPH generation does not throw")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<UbCongestionControl> host = UbCongestionControl::Create(UB_DEVICE);
        NS_TEST_ASSERT_MSG_NE(host, nullptr, "Host DCQCN factory should return an object");

        bool exceptionThrown = false;
        UbCongestionExtTph cetph;
        try
        {
            cetph = host->OnReceiverPrepareAckCongestionHeader(10, 12);
        }
        catch (const std::runtime_error&)
        {
            exceptionThrown = true;
        }

        NS_TEST_ASSERT_MSG_EQ(exceptionThrown,
                              false,
                              "DCQCN host should not throw when generating a default ACK CETPH");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetAckSequence(), 0u, "Default ACK sequence should be zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetC(), 0u, "Default C field should be zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetI(), false, "Default I field should be false");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetHint(), 0u, "Default hint should be zero");
    }
};

class UbCongestionControlDisabledFactoryStillReturnsObjectTest : public TestCase
{
public:
    UbCongestionControlDisabledFactoryStillReturnsObjectTest()
        : TestCase("UnifiedBus - disabled congestion control factory still returns usable objects")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<UbCongestionControl> host = UbCongestionControl::Create(UB_DEVICE);
        Ptr<UbCongestionControl> sw = UbCongestionControl::Create(UB_SWITCH);

        NS_TEST_ASSERT_MSG_NE(host, nullptr, "Disabled host congestion control should still return an object");
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Disabled switch congestion control should still return an object");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(host->GetAckOpcode()),
                              static_cast<uint32_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "Disabled congestion control should use ACK without CETPH");
        NS_TEST_ASSERT_MSG_EQ(host->IsCcLimited(4096),
                              false,
                              "Disabled congestion control should never block transport progress");

        bool exceptionThrown = false;
        UbCongestionExtTph cetph;
        try
        {
            cetph = host->OnReceiverPrepareAckCongestionHeader(1, 2);
        }
        catch (const std::runtime_error&)
        {
            exceptionThrown = true;
        }

        NS_TEST_ASSERT_MSG_EQ(exceptionThrown,
                              false,
                              "Disabled congestion control should still provide a no-op ACK congestion header");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetAckSequence(), 0u, "No-op ACK sequence should stay zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetC(), 0u, "No-op ACK C field should stay zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetI(), false, "No-op ACK I field should stay zero");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetHint(), 0u, "No-op ACK hint should stay zero");
    }
};

class UbCaqmHostRttUsesSmoothedEstimateTest : public TestCase
{
public:
    UbCaqmHostRttUsesSmoothedEstimateTest()
        : TestCase("UnifiedBus - CAQM host RTT estimate uses EWMA instead of latching min RTT")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("CAQM"));
        Config::SetDefault("ns3::UbHostCaqm::RttEwmaGain", DoubleValue(0.125));

        Ptr<UbHostCaqm> host = DynamicCast<UbHostCaqm>(UbCongestionControl::Create(UB_DEVICE));
        NS_TEST_ASSERT_MSG_NE(host, nullptr, "CAQM host factory should return a concrete host object");

        host->ApplyRttSampleForTest(NanoSeconds(100));
        NS_TEST_ASSERT_MSG_EQ(host->GetRttForTest().GetNanoSeconds(),
                              100,
                              "First RTT sample should seed the smoothed RTT estimate");

        host->ApplyRttSampleForTest(NanoSeconds(500));
        NS_TEST_ASSERT_MSG_GT(host->GetRttForTest().GetNanoSeconds(),
                              100,
                              "A larger second RTT sample should increase the smoothed RTT estimate");
        NS_TEST_ASSERT_MSG_LT(host->GetRttForTest().GetNanoSeconds(),
                              500,
                              "EWMA RTT estimate should smooth toward the sample rather than jump to it");

        Config::Reset();
    }
};

class UbSwitchAttachDisabledCongestionControlTest : public TestCase
{
public:
    UbSwitchAttachDisabledCongestionControlTest()
        : TestCase("UnifiedBus - disabled switch congestion control still attaches uniformly")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        node->AddDevice(CreateObject<UbPort>());
        sw->Init();

        Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
        NS_TEST_ASSERT_MSG_NE(congestionCtrl,
                              nullptr,
                              "Disabled switch congestion control factory should still return an object");

        congestionCtrl->OnSwitchAttached(sw);

        NS_TEST_ASSERT_MSG_EQ(sw->GetCongestionCtrl(),
                              congestionCtrl,
                              "Switch should always keep a congestion-control object after attach");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchAttachEnabledCongestionControlTest : public TestCase
{
public:
    UbSwitchAttachEnabledCongestionControlTest()
        : TestCase("UnifiedBus - enabled switch congestion control attaches via shared base semantics")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        node->AddDevice(CreateObject<UbPort>());
        sw->Init();

        Ptr<UbCongestionControl> congestionCtrl = UbCongestionControl::Create(UB_SWITCH);
        NS_TEST_ASSERT_MSG_NE(congestionCtrl,
                              nullptr,
                              "Enabled switch congestion control factory should return an object");

        congestionCtrl->OnSwitchAttached(sw);

        NS_TEST_ASSERT_MSG_EQ(sw->GetCongestionCtrl(),
                              congestionCtrl,
                              "Shared base attach semantics should bind the switch to the concrete congestion-control object");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbRoutingProcessRangeRouteTest : public TestCase
{
public:
    UbRoutingProcessRangeRouteTest()
        : TestCase("UnifiedBus - routing process resolves compressed destination ranges")
    {
    }

    void DoRun() override
    {
        Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
        std::vector<uint16_t> rangePorts = {2, 3};
        std::vector<uint16_t> exactPorts = {7};

        routing->AddShortestRouteRange(100, 199, rangePorts);
        routing->AddShortestRoute(NodeIdToIp(123).Get(), exactPorts);

        std::vector<uint16_t> outPorts;
        routing->GetShortestOutPorts(NodeIdToIp(101).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 2u, "range route should resolve inner host");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 2u, "range route should preserve first output port");
        NS_TEST_ASSERT_MSG_EQ(outPorts[1], 3u, "range route should preserve second output port");

        routing->GetShortestOutPorts(NodeIdToIp(101, 0).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 2u, "range route should resolve port-scoped host IP");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 2u, "port-scoped range route should preserve first output port");
        NS_TEST_ASSERT_MSG_EQ(outPorts[1], 3u, "port-scoped range route should preserve second output port");

        outPorts = routing->GetAllOutPorts(NodeIdToIp(101, 0).Get());
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 2u, "all route lookup should include range ports");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 2u, "all route lookup should preserve first range port");
        NS_TEST_ASSERT_MSG_EQ(outPorts[1], 3u, "all route lookup should preserve second range port");

        routing->GetShortestOutPorts(NodeIdToIp(123).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 1u, "exact route should override range route");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 7u, "exact route should keep configured output port");

        routing->GetShortestOutPorts(NodeIdToIp(250).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.empty(), true, "outside range should not resolve");

        Ptr<UbRoutingProcess> overlapRouting = CreateObject<UbRoutingProcess>();
        overlapRouting->AddShortestRouteRange(100, 199, std::vector<uint16_t>{2});
        overlapRouting->AddShortestRouteRange(150, 160, std::vector<uint16_t>{3});

        overlapRouting->GetShortestOutPorts(NodeIdToIp(149).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 1u, "overlap route should keep prefix range");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 2u, "overlap route should keep original prefix port");

        overlapRouting->GetShortestOutPorts(NodeIdToIp(150).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 2u, "overlap route should merge inner range ports");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 2u, "overlap route should keep existing inner port");
        NS_TEST_ASSERT_MSG_EQ(outPorts[1], 3u, "overlap route should add new inner port");

        overlapRouting->GetShortestOutPorts(NodeIdToIp(161).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 1u, "overlap route should keep suffix range");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 2u, "overlap route should keep original suffix port");

        Ptr<UbRoutingProcess> portRouting = CreateObject<UbRoutingProcess>();
        portRouting->AddShortestRouteRange(100, 199, 2, std::vector<uint16_t>{9});
        portRouting->AddShortestRouteRange(100, 199, 0, std::vector<uint16_t>{4});

        portRouting->GetShortestOutPorts(NodeIdToIp(101, 2).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 1u, "port-specific range route should resolve matching port");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 9u, "port-specific range route should keep matching port route");

        portRouting->GetShortestOutPorts(NodeIdToIp(101, 0).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(), 1u, "port 0 range should not use port 2 route");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 4u, "port 0 range should use default node route");

        portRouting->GetShortestOutPorts(NodeIdToIp(101).Get(), outPorts);
        NS_TEST_ASSERT_MSG_EQ(outPorts.size(),
                              2u,
                              "primary node IP should aggregate all destination-port range routes");
        NS_TEST_ASSERT_MSG_EQ(outPorts[0], 4u, "primary node IP should include port 0 route");
        NS_TEST_ASSERT_MSG_EQ(outPorts[1], 9u, "primary node IP should include port 2 route");
    }
};

class UbDcqcnCnpHeaderRoundTripTest : public TestCase
{
public:
    UbDcqcnCnpHeaderRoundTripTest()
        : TestCase("UnifiedBus - DCQCN CNP header round-trips ECN and location in spec layout")
    {
    }

    void DoRun() override
    {
        UbCnpExtTph cnpOut;
        cnpOut.SetEcn(0x3);
        cnpOut.SetLocation(true);

        Ptr<Packet> packet = Create<Packet>();
        packet->AddHeader(cnpOut);

        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              16u,
                              "Spec-aligned CNP CETPH should occupy 16 bytes");

        uint8_t serialized[16] = {};
        packet->CopyData(serialized, sizeof(serialized));
        const uint32_t word0 = (static_cast<uint32_t>(serialized[0]) << 24) |
                               (static_cast<uint32_t>(serialized[1]) << 16) |
                               (static_cast<uint32_t>(serialized[2]) << 8) |
                               static_cast<uint32_t>(serialized[3]);
        NS_TEST_ASSERT_MSG_EQ(word0,
                              0xE0000000u,
                              "CNP ECN/location bits should live in the first serialized word");
        for (uint32_t i = 4; i < sizeof(serialized); ++i)
        {
            NS_TEST_ASSERT_MSG_EQ(serialized[i],
                                  0u,
                                  "Remaining spec-reserved CNP CETPH bytes should serialize as zero");
        }

        UbCnpExtTph cnpIn;
        packet->RemoveHeader(cnpIn);
        NS_TEST_ASSERT_MSG_EQ(cnpIn.GetEcn(), 0x3u, "ECN bits should survive CNP header serialization");
        NS_TEST_ASSERT_MSG_EQ(cnpIn.GetLocation(),
                              true,
                              "Location bit should survive CNP header serialization");
    }
};

class UbDcqcnSwitchMarksFecnTest : public TestCase
{
public:
    UbDcqcnSwitchMarksFecnTest()
        : TestCase("UnifiedBus - DCQCN switch marks FECN when packet enters congested outport backlog")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(1024));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(2048));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(1.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);
        UbIpBasedNetworkHeader nth;
        nth.SetMode(0);
        nth.SetC(0);
        nth.SetI(0);
        nth.SetHint(0);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(1);
        packet->AddHeader(dl);

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       1);

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(), 0b100, "DCQCN should switch NTH to FECN mode");
        NS_TEST_ASSERT_MSG_NE(nth.GetFecn(),
                              0u,
                              "DCQCN should mark packet as soon as it enters congested outport backlog");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSwitchNoMarkPreservesPacketHeadersTest : public TestCase
{
public:
    UbDcqcnSwitchNoMarkPreservesPacketHeadersTest()
        : TestCase("UnifiedBus - DCQCN switch preserves headers when enqueue does not mark")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(8192));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(16384));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(1.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);

        UbIpBasedNetworkHeader nth;
        nth.SetMode(0);
        nth.SetLocation(true);
        nth.SetC(0x2a);
        nth.SetI(1);
        nth.SetHint(0x5b);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(3);
        dl.SetLoadBalanceMode(true);
        dl.SetRoutingPolicy(true);
        packet->AddHeader(dl);

        const uint32_t originalSize = packet->GetSize();
        std::vector<uint8_t> originalBytes(originalSize);
        packet->CopyData(originalBytes.data(), originalBytes.size());

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       3);

        std::vector<uint8_t> restoredBytes(packet->GetSize());
        packet->CopyData(restoredBytes.data(), restoredBytes.size());
        const bool sameBytes = restoredBytes == originalBytes;
        NS_TEST_ASSERT_MSG_EQ(sameBytes,
                              true,
                              "DCQCN no-mark path should restore packet bytes exactly");

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(dl.GetConfig(),
                              static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4),
                              "DCQCN no-mark path should preserve datalink config");
        NS_TEST_ASSERT_MSG_EQ(dl.GetPacketVL(), 3u, "DCQCN no-mark path should preserve packet VL");
        NS_TEST_ASSERT_MSG_EQ(dl.GetLoadBalanceMode(),
                              true,
                              "DCQCN no-mark path should preserve load-balance mode");
        NS_TEST_ASSERT_MSG_EQ(dl.GetRoutingPolicy(),
                              true,
                              "DCQCN no-mark path should preserve routing policy");
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(), 0u, "DCQCN no-mark path should preserve NTH mode");
        NS_TEST_ASSERT_MSG_EQ(nth.GetLocation(),
                              true,
                              "DCQCN no-mark path should preserve NTH location");
        NS_TEST_ASSERT_MSG_EQ(nth.GetC(), 1u, "DCQCN no-mark path should preserve NTH C field");
        NS_TEST_ASSERT_MSG_EQ(nth.GetI(), 1u, "DCQCN no-mark path should preserve NTH I field");
        NS_TEST_ASSERT_MSG_EQ(nth.GetHint(), 0x5bu, "DCQCN no-mark path should preserve NTH hint");
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              4096u,
                              "DCQCN no-mark path should preserve payload size");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSwitchMarksAboveKmaxEvenWhenPmaxIsZeroTest : public TestCase
{
public:
    UbDcqcnSwitchMarksAboveKmaxEvenWhenPmaxIsZeroTest()
        : TestCase("UnifiedBus - DCQCN switch marks with probability one above Kmax")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(1024));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(2048));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(0.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);

        UbIpBasedNetworkHeader nth;
        nth.SetMode(0);
        nth.SetFecn(0);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(1);
        packet->AddHeader(dl);

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       1);

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(),
                              0b100,
                              "DCQCN should mark every packet once backlog exceeds Kmax");
        NS_TEST_ASSERT_MSG_NE(nth.GetFecn(),
                              0u,
                              "DCQCN should not cap the above-Kmax marking probability at Pmax");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSwitchDoesNotRemarkMarkedFecnTest : public TestCase
{
public:
    UbDcqcnSwitchDoesNotRemarkMarkedFecnTest()
        : TestCase("UnifiedBus - DCQCN switch leaves already-marked FECN packets unchanged")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbSwitchDcqcn::KminBytes", UintegerValue(0));
        Config::SetDefault("ns3::UbSwitchDcqcn::KmaxBytes", UintegerValue(1));
        Config::SetDefault("ns3::UbSwitchDcqcn::Pmax", DoubleValue(1.0));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbSwitch> sw = topo.switch0->GetObject<UbSwitch>();
        Ptr<Packet> packet = Create<Packet>(4096);

        UbIpBasedNetworkHeader nth;
        nth.SetMode(0b100);
        nth.SetLocation(true);
        nth.SetFecn(0x2);
        packet->AddHeader(nth);

        UbDatalinkPacketHeader dl;
        dl.SetConfig(static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4));
        dl.SetPacketVL(1);
        packet->AddHeader(dl);

        sw->SendPacket(packet,
                       topo.switch0DevicePort->GetIfIndex(),
                       topo.switch0CorePort->GetIfIndex(),
                       1);

        packet->RemoveHeader(dl);
        packet->RemoveHeader(nth);
        NS_TEST_ASSERT_MSG_EQ(nth.GetMode(), 0b100, "Marked packet should stay in FECN mode");
        NS_TEST_ASSERT_MSG_EQ(nth.GetLocation(), true, "Already-marked location bit should not be overwritten");
        NS_TEST_ASSERT_MSG_EQ(nth.GetFecn(), 0x2u, "Already-marked FECN level should not be overwritten");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnReceiverSuppressesBurstCnpTest : public TestCase
{
public:
    UbDcqcnReceiverSuppressesBurstCnpTest()
        : TestCase("UnifiedBus - DCQCN receiver emits at most one CNP per suppression window")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::CnpInterval", TimeValue(MicroSeconds(50)));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(rxTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Receiver TP should bind a host DCQCN instance");

        UbIpBasedNetworkHeader marked;
        marked.SetMode(0b100);
        marked.SetFecn(0x1);
        marked.SetLocation(false);

        cc->OnReceiverDataPacketReceived(1, 256, marked);
        cc->OnReceiverDataPacketReceived(2, 256, marked);

        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingCnpCountForTest(),
                              1u,
                              "Receiver should queue only one immediate CNP inside suppression window");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnControlPriorityPrefersCnpTest : public TestCase
{
public:
    UbDcqcnControlPriorityPrefersCnpTest()
        : TestCase("UnifiedBus - transport dequeues CNP before ACK")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> tp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);

        Ptr<Packet> ackPacket = Create<Packet>(0);
        UbTransportHeader ackHeader;
        ackHeader.SetTPOpcode(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH);
        ackPacket->AddHeader(ackHeader);
        tp->EnqueueAckForTest(ackPacket);

        Ptr<Packet> cnpPacket = Create<Packet>(0);
        UbTransportHeader cnpHeader;
        cnpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_CNP);
        cnpPacket->AddHeader(cnpHeader);
        tp->EnqueueCnpForTest(cnpPacket);

        Ptr<Packet> first = tp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(first, nullptr, "Transport should return queued control work");

        UbTransportHeader tph;
        first->RemoveHeader(tph);
        NS_TEST_ASSERT_MSG_EQ(tph.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_CNP),
                              "CNP should be sent before ACK");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCnpOpcodeIsValidTransportOpcodeTest : public TestCase
{
public:
    UbDcqcnCnpOpcodeIsValidTransportOpcodeTest()
        : TestCase("UnifiedBus - DCQCN CNP is a valid transport opcode")
    {
    }

    void DoRun() override
    {
        UbTransportHeader header;
        header.SetTPOpcode(TpOpcode::TP_OPCODE_CNP);
        NS_TEST_ASSERT_MSG_EQ(header.IsValidOpcode(), true, "CNP opcode must pass transport header validation");
    }
};

class UbCaqmReceiverAggregatesLogicalPsnAcrossWrapTest : public TestCase
{
public:
    UbCaqmReceiverAggregatesLogicalPsnAcrossWrapTest()
        : TestCase("UnifiedBus - CAQM receiver aggregates logical PSN ranges across wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));

        Ptr<UbHostCaqm> caqm = CreateObject<UbHostCaqm>();

        UbIpBasedNetworkHeader header;
        header.SetC(1);
        header.SetI(0);
        header.SetHint(0);

        caqm->OnReceiverDataPacketReceived(0xFFFFFEull, 16, header);
        caqm->OnReceiverDataPacketReceived(0xFFFFFFull, 16, header);
        caqm->OnReceiverDataPacketReceived(0x1000000ull, 16, header);
        caqm->OnReceiverDataPacketReceived(0x1000001ull, 16, header);

        UbCongestionExtTph cetph =
            caqm->OnReceiverPrepareAckCongestionHeader(0xFFFFFEull, 0x1000002ull);

        NS_TEST_ASSERT_MSG_EQ(cetph.GetAckSequence(),
                              64u,
                              "CAQM should count all packets in the logical PSN range");
        NS_TEST_ASSERT_MSG_EQ(cetph.GetC(),
                              4u,
                              "CAQM should aggregate CE evidence across PSN wire wrap");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbModuloSequenceUnwrapTest : public TestCase
{
public:
    UbModuloSequenceUnwrapTest()
        : TestCase("UnifiedBus - modulo sequence maps wire ids to logical ids")
    {
    }

    void DoRun() override
    {
        using UbTaSsnSequenceForTest = UbModuloSequence<16, uint32_t>;

        NS_TEST_ASSERT_MSG_EQ(UbModuloSequence<24>::ToWire(0x1000001ull),
                              1u,
                              "24-bit wire value should keep low bits");
        NS_TEST_ASSERT_MSG_EQ(UbModuloSequence<24>::Unwrap(1, 0x1000000ull),
                              0x1000001ull,
                              "wire PSN 1 should unwrap after 24-bit wrap");
        NS_TEST_ASSERT_MSG_EQ(UbTaSsnSequenceForTest::Unwrap(1, 0x10000u),
                              0x10001u,
                              "wire TASSN 1 should unwrap after 16-bit wrap");
        auto inWindow = UbModuloSequence<24>::UnwrapInWindow(1, 0x1000000ull, 0x1000004ull);
        NS_TEST_ASSERT_MSG_EQ(inWindow.has_value(),
                              true,
                              "wire PSN 1 should unwrap inside the sender window");
        NS_TEST_ASSERT_MSG_EQ(*inWindow,
                              0x1000001ull,
                              "window unwrap should return the logical PSN");
        auto outsideWindow = UbModuloSequence<24>::UnwrapInWindow(10, 0x1000000ull, 0x1000004ull);
        NS_TEST_ASSERT_MSG_EQ(outsideWindow.has_value(),
                              false,
                              "wire PSN outside the sender window should be rejected");
    }
};

class UbSelectiveAckExtTphRoundTripTest : public TestCase
{
public:
    UbSelectiveAckExtTphRoundTripTest()
        : TestCase("UnifiedBus - SAETPH round-trips bitmap sizes")
    {
    }

    void DoRun() override
    {
        const std::array<uint32_t, 5> bitCounts = {64, 128, 256, 512, 1024};
        for (uint32_t bits : bitCounts)
        {
            UbSelectiveAckExtTph out;
            out.SetBitmapBitCount(bits);
            out.SetMaxRcvPsn(0x123456);
            out.SetBitmapBit(0, true);
            out.SetBitmapBit(bits - 1, true);

            Ptr<Packet> packet = Create<Packet>(0);
            packet->AddHeader(out);

            UbSelectiveAckExtTph in;
            packet->RemoveHeader(in);

            NS_TEST_ASSERT_MSG_EQ(in.GetBitmapBitCount(), bits, "bitmap size should survive round trip");
            NS_TEST_ASSERT_MSG_EQ(in.GetMaxRcvPsn(), 0x123456u, "MaxRcvPSN should survive round trip");
            NS_TEST_ASSERT_MSG_EQ(in.GetBitmapBit(0), true, "first bit should survive round trip");
            NS_TEST_ASSERT_MSG_EQ(in.GetBitmapBit(bits - 1), true, "last bit should survive round trip");
        }
    }
};

class UbSelectiveAckExtTphBitmapBoundaryTest : public TestCase
{
public:
    UbSelectiveAckExtTphBitmapBoundaryTest()
        : TestCase("UnifiedBus - SAETPH bitmap has no offset equal to bit count")
    {
    }

    void DoRun() override
    {
        UbSelectiveAckExtTph header;
        header.SetBitmapBitCount(64);
        header.SetBitmapBit(63, true);

        NS_TEST_ASSERT_MSG_EQ(header.GetBitmapBit(63), true, "offset 63 should be valid");
        NS_TEST_ASSERT_MSG_EQ(header.GetBitmapBit(64), false, "offset 64 must be outside a 64-bit bitmap");
    }
};

class UbSelectiveAckExtTphEncodingTest : public TestCase
{
public:
    UbSelectiveAckExtTphEncodingTest()
        : TestCase("UnifiedBus - SAETPH bitmap-size encoding is spec visible")
    {
    }

    void DoRun() override
    {
        const std::array<std::pair<uint32_t, uint8_t>, 5> cases = {{
            {64, 0},
            {128, 1},
            {256, 2},
            {512, 3},
            {1024, 4},
        }};

        for (auto [bits, encoded] : cases)
        {
            UbSelectiveAckExtTph header;
            header.SetBitmapBitCount(bits);
            NS_TEST_ASSERT_MSG_EQ(header.GetEncodedBitmapSize(),
                                  encoded,
                                  "SAETPH size encoding must match the UB spec table");
        }

        NS_TEST_ASSERT_MSG_EQ(UbSelectiveAckExtTph::IsSupportedEncodedBitmapSize(5),
                              false,
                              "encoding 5 is reserved");
        NS_TEST_ASSERT_MSG_EQ(UbSelectiveAckExtTph::IsSupportedEncodedBitmapSize(6),
                              false,
                              "encoding 6 is reserved");
        NS_TEST_ASSERT_MSG_EQ(UbSelectiveAckExtTph::IsSupportedEncodedBitmapSize(7),
                              false,
                              "encoding 7 is reserved");

        NS_TEST_ASSERT_MSG_EQ(UbSelectiveAckExtTph::IsSupportedBitmapBitCount(65),
                              false,
                              "65 bits is not an encodable SAETPH bitmap width");

        bool invalidSetterThrew = false;
        try
        {
            UbSelectiveAckExtTph header;
            header.SetBitmapBitCount(65);
        }
        catch (const std::invalid_argument&)
        {
            invalidSetterThrew = true;
        }

        NS_TEST_ASSERT_MSG_EQ(invalidSetterThrew,
                              true,
                              "unsupported explicit width must fail instead of serializing any valid size");
    }
};

class UbSelectiveAckExtTphReservedEncodingRejectTest : public TestCase
{
public:
    UbSelectiveAckExtTphReservedEncodingRejectTest()
        : TestCase("UnifiedBus - SAETPH reserved bitmap-size encodings are malformed")
    {
    }

    void DoRun() override
    {
        const std::array<uint8_t, 2> malformedSizeBytes = {5, 0x80};

        for (uint8_t malformedSizeByte : malformedSizeBytes)
        {
            Ptr<Packet> packet = Create<Packet>(0);
            const uint8_t malformed[] = {malformedSizeByte, 0, 0, 0};
            packet->AddAtEnd(Create<Packet>(malformed, sizeof(malformed)));

            bool deserializeThrew = false;
            try
            {
                UbSelectiveAckExtTph decoded;
                packet->RemoveHeader(decoded);
            }
            catch (const std::invalid_argument&)
            {
                deserializeThrew = true;
            }

            NS_TEST_ASSERT_MSG_EQ(deserializeThrew,
                                  true,
                                  "reserved SAETPH bitmap-size bits must be rejected");
        }
    }
};

class UbSelectiveAckExtTphWireBitOrderTest : public TestCase
{
public:
    UbSelectiveAckExtTphWireBitOrderTest()
        : TestCase("UnifiedBus - SAETPH bitmap serialization is LSB-first within each byte")
    {
    }

    void DoRun() override
    {
        UbSelectiveAckExtTph header;
        header.SetBitmapBitCount(64);
        header.SetMaxRcvPsn(0);
        header.SetBitmapBit(0, true);
        header.SetBitmapBit(1, true);
        header.SetBitmapBit(7, true);

        Ptr<Packet> packet = Create<Packet>(0);
        packet->AddHeader(header);

        std::array<uint8_t, 12> bytes{};
        const uint32_t copied = packet->CopyData(bytes.data(), bytes.size());

        NS_TEST_ASSERT_MSG_EQ(copied, bytes.size(), "serialized 64-bit SAETPH should be 12 bytes");
        NS_TEST_ASSERT_MSG_EQ(bytes[0], 0u, "64-bit SAETPH should use encoded bitmap size 0");
        NS_TEST_ASSERT_MSG_EQ(bytes[4],
                              0x83u,
                              "bitmap offsets 0, 1, and 7 should serialize as byte 0x83");
    }
};

class UbTransportResponseStatusFieldsTest : public TestCase
{
public:
    UbTransportResponseStatusFieldsTest()
        : TestCase("UnifiedBus - transport response status fields are explicit")
    {
    }

    void DoRun() override
    {
        const auto verifyResponse = [this](TpOpcode opcode, uint8_t rspSt, const std::string& name) {
            UbTransportHeader header;
            header.SetTPOpcode(opcode);
            header.SetRspSt(rspSt);
            header.SetRspInfo(0);
            Ptr<Packet> packet = Create<Packet>(0);
            packet->AddHeader(header);
            UbTransportHeader decoded;
            packet->RemoveHeader(decoded);
            NS_TEST_ASSERT_MSG_EQ(decoded.GetTPOpcode(), static_cast<uint8_t>(opcode), name + " opcode");
            NS_TEST_ASSERT_MSG_EQ(decoded.GetRspSt(), rspSt, name + " RSPST");
            NS_TEST_ASSERT_MSG_EQ(decoded.GetRspInfo(), 0u, name + " RSPINFO");
        };

        verifyResponse(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH, 0, "TPACK");
        verifyResponse(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH, 3, "TPNAK");
        verifyResponse(TpOpcode::TP_OPCODE_ACK_WITH_CETPH, 0, "TPACK-CC");
        verifyResponse(TpOpcode::TP_OPCODE_ACK_WITH_CETPH, 3, "TPNAK-CC");
        verifyResponse(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH, 0, "TPSACK");
        UbTransportHeader reserved;
        reserved.SetTPOpcode(TpOpcode::TP_OPCODE_RESERVED1);
        NS_TEST_ASSERT_MSG_EQ(reserved.IsValidOpcode(), false,
                              "TPOpcode 0x4 should fail transport header validation");
    }
};

class UbFaultClassifiesTransportResponseStatusTest : public TestCase
{
public:
    UbFaultClassifiesTransportResponseStatusTest()
        : TestCase("UnifiedBus - fault classification uses transport response RSPST")
    {
    }

    void DoRun() override
    {
        const auto buildPacket = [](TpOpcode opcode, uint8_t rspSt) {
            Ptr<Packet> packet = Create<Packet>(0);
            UbTransportHeader transportHeader;
            transportHeader.SetTPOpcode(opcode);
            transportHeader.SetRspSt(rspSt);
            packet->AddHeader(transportHeader);
            packet->AddHeader(UdpHeader());
            UbPort::AddIpv4Header(packet, NodeIdToIp(0), NodeIdToIp(1));
            packet->AddHeader(UbIpBasedNetworkHeader());
            UbDataLink::GenPacketHeader(packet, false, true, UB_PRIORITY_DEFAULT,
                                        UB_PRIORITY_DEFAULT, false, true,
                                        UbDatalinkHeaderConfig::PACKET_IPV4);
            return packet;
        };
        const auto verifyType = [this, &buildPacket](TpOpcode opcode, uint8_t rspSt,
                                                     RetransFaultPacketType expected) {
            RetransFaultPacketInfo info;
            NS_TEST_ASSERT_MSG_EQ(CreateObject<UbFault>()->TryGetRetransFaultPacketInfo(
                                      buildPacket(opcode, rspSt), 2, 1, 9001, info),
                                  true, "transport response should decode");
            NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(info.packetType),
                                  static_cast<uint32_t>(expected), "fault classification");
        };
        verifyType(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH, 0, RetransFaultPacketType::TPACK);
        verifyType(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH, 3, RetransFaultPacketType::TPNAK);
        verifyType(TpOpcode::TP_OPCODE_ACK_WITH_CETPH, 3, RetransFaultPacketType::TPNAK);
        RetransFaultPacketInfo info;
        NS_TEST_ASSERT_MSG_EQ(CreateObject<UbFault>()->TryGetRetransFaultPacketInfo(
                                  buildPacket(TpOpcode::TP_OPCODE_RESERVED1, 3), 2, 1, 9001, info),
                              false, "reserved TPOpcode must not classify as TPNAK");
    }
};

class UbTransportRetransmissionModeDefaultsTest : public TestCase
{
public:
    UbTransportRetransmissionModeDefaultsTest()
        : TestCase("UnifiedBus - selective retransmission mode is opt-in")
    {
    }

    void DoRun() override
    {
        Ptr<UbTransportChannel> tp = CreateObject<UbTransportChannel>();

        EnumValue<UbRetransmissionMode> mode;
        tp->GetAttribute("RetransmissionMode", mode);
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint32_t>(mode.Get()),
                              static_cast<uint32_t>(UbRetransmissionMode::GBN),
                              "default should preserve GBN behavior");

        UintegerValue bitmapBits;
        tp->GetAttribute("SelectiveAckBitmapBits", bitmapBits);
        NS_TEST_ASSERT_MSG_EQ(bitmapBits.Get(),
                              0u,
                              "default selective ACK bitmap width should be AUTO");

        tp->SetAttribute("SelectiveAckBitmapBits", UintegerValue(64));
        tp->GetAttribute("SelectiveAckBitmapBits", bitmapBits);
        NS_TEST_ASSERT_MSG_EQ(bitmapBits.Get(),
                              64u,
                              "explicit selective ACK bitmap width should be settable");
        NS_TEST_ASSERT_MSG_EQ(tp->SetAttributeFailSafe("SelectiveAckBitmapBits", UintegerValue(65)),
                              false,
                              "invalid explicit selective ACK bitmap width should be rejected");

        BooleanValue fastRetrans;
        tp->GetAttribute("EnableFastRetrans", fastRetrans);
        NS_TEST_ASSERT_MSG_EQ(fastRetrans.Get(),
                              false,
                              "fast retransmission should be opt-in");

        tp->SetAttribute("EnableFastRetrans", BooleanValue(true));
        tp->GetAttribute("EnableFastRetrans", fastRetrans);
        NS_TEST_ASSERT_MSG_EQ(fastRetrans.Get(),
                              true,
                              "fast retransmission should be settable");

        BooleanValue retrans;
        tp->GetAttribute("EnableRetrans", retrans);
        NS_TEST_ASSERT_MSG_EQ(retrans.Get(),
                              false,
                              "transport retransmission should preserve the legacy opt-in default");

        tp->SetAttribute("EnableRetrans", BooleanValue(true));
        tp->GetAttribute("EnableRetrans", retrans);
        NS_TEST_ASSERT_MSG_EQ(retrans.Get(),
                              true,
                              "EnableRetrans should preserve the legacy TypeId attribute");

        NS_TEST_ASSERT_MSG_EQ(
            UbTransportChannel::IsTransportResponseOpcode(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH),
            true,
            "TPSACK should be a transport response");
        NS_TEST_ASSERT_MSG_EQ(
            UbTransportChannel::IsTransportResponseOpcode(TpOpcode::TP_OPCODE_SACK_WITH_CETPH),
            true,
            "TPSACK-CC should be a transport response");
    }
};

class UbRetransDisabledDoesNotRetainSentPacketsTest : public TestCase
{
public:
    UbRetransDisabledDoesNotRetainSentPacketsTest()
        : TestCase("UnifiedBus - disabled retransmission does not retain sent packets")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbTransportChannel::EnableRetrans", BooleanValue(false));

        Ptr<UbTransportChannel> txTp = CreateObject<UbTransportChannel>();
        txTp->RetainSentPsnForTest(10, 64);

        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(10),
                              false,
                              "disabled retransmission must not retain packet state");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbTransportShallowPipelineIgnoresUnackedSegmentsTest : public TestCase
{
public:
    UbTransportShallowPipelineIgnoresUnackedSegmentsTest()
        : TestCase("UnifiedBus - transport shallow pipeline ignores unacked segments")
    {
    }

    void DoRun() override
    {
        Ptr<UbTransportChannel> tp = CreateObject<UbTransportChannel>();

        for (uint32_t i = 0; i < 3; ++i) {
            Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
            segment->SetSize(64);
            segment->UpdateSentBytes(64);
            tp->PushWqeSegment(segment);
        }

        NS_TEST_ASSERT_MSG_EQ(tp->GetActiveSendSegmentCount(),
                              0u,
                              "fully sent segments should not consume active send slots");
        NS_TEST_ASSERT_MSG_EQ(tp->GetOutstandingUnackedSegmentCount(),
                              3u,
                              "fully sent but unacked segments should remain outstanding");
        NS_TEST_ASSERT_MSG_EQ(tp->CanScheduleAnotherSegment(),
                              true,
                              "shallow pipeline should not stop on sent-completed unacked segments");

        Ptr<UbWqeSegment> activeA = CreateObject<UbWqeSegment>();
        activeA->SetSize(64);
        tp->PushWqeSegment(activeA);
        Ptr<UbWqeSegment> activeB = CreateObject<UbWqeSegment>();
        activeB->SetSize(64);
        tp->PushWqeSegment(activeB);

        NS_TEST_ASSERT_MSG_EQ(tp->GetActiveSendSegmentCount(),
                              2u,
                              "two unsent segments should consume the active-send budget");
        NS_TEST_ASSERT_MSG_EQ(tp->CanScheduleAnotherSegment(),
                              false,
                              "shallow pipeline should still enforce active-send depth");

        Simulator::Destroy();
    }
};

class UbAckWithoutCetphCarriesNoCetphHeaderTest : public TestCase
{
public:
    UbAckWithoutCetphCarriesNoCetphHeaderTest()
        : TestCase("UnifiedBus - ACK_WITHOUT_CETPH wire packet carries no CETPH header")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);

        Ptr<Packet> data = Create<Packet>(16);
        UbFlowTag flowTag(kUrmaWriteRegressionTaskId, 16);
        data->AddPacketTag(flowTag);

        UbMAExtTah maHeader;
        maHeader.SetLength(16);
        data->AddHeader(maHeader);

        UbTransactionHeader taHeader;
        taHeader.SetTaOpcode(TaOpcode::TA_OPCODE_WRITE);
        taHeader.SetIniTaSsn(7);
        taHeader.SetIniRcId(0);
        data->AddHeader(taHeader);

        UbTransportHeader tpHeader;
        tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RELIABLE_TA);
        tpHeader.SetSrcTpn(kUrmaWriteRegressionSenderTpn);
        tpHeader.SetDestTpn(kUrmaWriteRegressionReceiverTpn);
        tpHeader.SetPsn(0);
        data->AddHeader(tpHeader);

        UdpHeader udpHeader;
        data->AddHeader(udpHeader);
        UbPort::AddIpv4Header(data, NodeIdToIp(topo.sender->GetId()), NodeIdToIp(topo.receiver->GetId()));

        UbIpBasedNetworkHeader networkHeader;
        data->AddHeader(networkHeader);
        UbDataLink::GenPacketHeader(data,
                                    false,
                                    false,
                                    kUrmaWriteRegressionPriority,
                                    kUrmaWriteRegressionPriority,
                                    false,
                                    true,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);

        rxTp->RecvDataPacket(data);
        Ptr<Packet> ack = rxTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(ack, nullptr, "Receiver should enqueue an ACK packet");

        UbDatalinkPacketHeader ackDlHeader;
        UbIpBasedNetworkHeader ackNetworkHeader;
        Ipv4Header ackIpv4Header;
        UdpHeader ackUdpHeader;
        UbTransportHeader ackTpHeader;
        UbAckTransactionHeader ackTaHeader;

        ack->RemoveHeader(ackDlHeader);
        ack->RemoveHeader(ackNetworkHeader);
        ack->RemoveHeader(ackIpv4Header);
        ack->RemoveHeader(ackUdpHeader);
        ack->RemoveHeader(ackTpHeader);
        NS_TEST_ASSERT_MSG_EQ(ackTpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "Disabled congestion control should emit ACK without CETPH");

        ack->RemoveHeader(ackTaHeader);
        NS_TEST_ASSERT_MSG_EQ(ackTaHeader.GetTaOpcode(),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK),
                              "ACK_WITHOUT_CETPH should expose TA ACK immediately after TP header");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbRetransDisabledReceiveGapDoesNotEmitSackTest : public TestCase
{
public:
    UbRetransDisabledReceiveGapDoesNotEmitSackTest()
        : TestCase("UnifiedBus - disabled retransmission receive gap does not emit SACK")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        Config::SetDefault("ns3::UbTransportChannel::EnableRetrans", BooleanValue(false));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        NS_TEST_ASSERT_MSG_EQ(
            rxTp->GetPendingAckCountForTest(),
            0u,
            "disabled retransmission should match main: out-of-order gap records state without ACK/SACK");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveReceiverTpsackGapTest : public TestCase
{
public:
    UbSelectiveReceiverTpsackGapTest()
        : TestCase("UnifiedBus - selective receiver emits TPSACK for receive gap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(), 1u, "PSN 0 should produce TPACK");
        rxTp->PopAckForTest();

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(), 1u, "PSN 2 should produce TPSACK while PSN 1 is missing");

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH),
                              "selective gap response should use TPSACK without CETPH when CC is disabled");
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetPsn(), 0u, "TPSACK RTPH.PSN should name the contiguous ACK base");
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetRspSt(), 0u, "TPSACK RSPST should be zero");
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetRspInfo(), 0u, "TPSACK RSPINFO should be zero");
        NS_TEST_ASSERT_MSG_EQ(ack.hasSelectiveAck, true, "TPSACK should carry SAETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBitCount(),
                              1024u,
                              "AUTO should cap the default 2048 receive window to a 1024-bit SAETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetMaxRcvPsn(), 2u, "MaxRcvPSN should track the highest observed PSN");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(0), true, "PSN 0 should be marked received");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(1), false, "PSN 1 should remain missing");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(2), true, "PSN 2 should be marked received");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbReceiverUnwrapsDataPsnAcrossWireWrapTest : public TestCase
{
public:
    UbReceiverUnwrapsDataPsnAcrossWireWrapTest()
        : TestCase("UnifiedBus - receiver unwraps RTPH PSN across 24-bit wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);
        rxTp->SetPsnRecvNxtForTest(0x1000000ull);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 1, true));

        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPsnRecvNxtForTest(),
                              0x1000002ull,
                              "wire PSN 1 should advance logical recv next after 24-bit wrap");
        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetPsn(),
                              1u,
                              "receiver ACK should serialize low 24 bits of the logical PSN");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbReceiverRejectsOutOfWindowFuturePsnWithoutPollutingMaxRcvPsnTest : public TestCase
{
public:
    UbReceiverRejectsOutOfWindowFuturePsnWithoutPollutingMaxRcvPsnTest()
        : TestCase("UnifiedBus - receiver rejected future PSN does not pollute MaxRcvPSN")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::TpOooThreshold", UintegerValue(4));
        Config::SetDefault("ns3::UbTransportChannel::SelectiveAckBitmapBits",
                           UintegerValue(64));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);
        rxTp->SetPsnRecvNxtForTest(0x1000000ull);

        rxTp->RecvDataPacket(BuildReceiverDataPacketWithSequences(topo, 0x10, false, 0, 0));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(),
                              0u,
                              "future PSN outside receive window should be rejected without ACK");

        rxTp->RecvDataPacket(BuildReceiverDataPacketWithSequences(topo, 1, false, 0, 1));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(),
                              1u,
                              "valid in-window gap should still produce TPSACK");

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH),
                              "valid in-window gap should use TPSACK");
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetPsn(),
                              0xFFFFFFu,
                              "TPSACK RTPH.PSN should serialize logical recvNxt - 1");
        NS_TEST_ASSERT_MSG_EQ(ack.hasSelectiveAck, true, "TPSACK should carry SAETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetMaxRcvPsn(),
                              1u,
                              "rejected future PSN must not pollute MaxRcvPSN");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveReceiverAckAfterGapClosesTest : public TestCase
{
public:
    UbSelectiveReceiverAckAfterGapClosesTest()
        : TestCase("UnifiedBus - selective receiver returns to TPACK after receive gap closes")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        rxTp->PopAckForTest();

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 1, false));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(), 1u, "Filling the receive gap should produce an ACK");

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "closed gap should use ordinary TPACK without CETPH when CC is disabled");
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetPsn(), 2u, "TPACK should acknowledge through the buffered PSN 2");
        NS_TEST_ASSERT_MSG_EQ(ack.hasSelectiveAck, false, "ordinary TPACK should not carry SAETPH");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveReceiverExplicitBitmapWidthTest : public TestCase
{
public:
    UbSelectiveReceiverExplicitBitmapWidthTest()
        : TestCase("UnifiedBus - selective receiver honors explicit SAETPH bitmap width")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::SelectiveAckBitmapBits", UintegerValue(64));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.hasSelectiveAck, true, "selective gap response should carry SAETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBitCount(),
                              64u,
                              "explicit SelectiveAckBitmapBits=64 should select 64-bit SAETPH");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveReceiverFirstPacketLossTest : public TestCase
{
public:
    UbSelectiveReceiverFirstPacketLossTest()
        : TestCase("UnifiedBus - selective receiver represents first-packet loss without ACK underflow")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(),
                              1u,
                              "First observed out-of-order packet should produce TPSACK");

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH),
                              "first-packet loss response should use TPSACK");
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetPsn(),
                              0u,
                              "RTPH.PSN should stay at 0 instead of underflowing when PSN 0 is missing");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetMaxRcvPsn(), 2u, "MaxRcvPSN should report the observed PSN");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(0),
                              false,
                              "BitMap[0] should be negative evidence for missing PSN 0");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(2),
                              true,
                              "BitMap[2] should mark observed PSN 2");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveReceiverDuplicateGapTpsackTest : public TestCase
{
public:
    UbSelectiveReceiverDuplicateGapTpsackTest()
        : TestCase("UnifiedBus - selective receiver repeats TPSACK for duplicate packet while gap remains")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        rxTp->PopAckForTest();

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(),
                              1u,
                              "Duplicate out-of-order packet should still enqueue one response");

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH),
                              "duplicate packet should repeat TPSACK while PSN 1 remains missing");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(1),
                              false,
                              "Repeated TPSACK should preserve the missing PSN evidence");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(2),
                              true,
                              "Repeated TPSACK should preserve received duplicate evidence");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveReceiverTpsackWithCetphOrderTest : public TestCase
{
public:
    UbSelectiveReceiverTpsackWithCetphOrderTest()
        : TestCase("UnifiedBus - selective receiver serializes TPSACK-CC with CETPH before SAETPH")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("CAQM"));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));

        DecodedReceiverAck ack = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(ack.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITH_CETPH),
                              "CAQM-enabled selective gap response should use TPSACK with CETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.hasSelectiveAck,
                              true,
                              "TPSACK-CC should still expose SAETPH after CETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetMaxRcvPsn(), 2u, "SAETPH should parse after CETPH");
        NS_TEST_ASSERT_MSG_EQ(ack.selectiveAckHeader.GetBitmapBit(2),
                              true,
                              "SAETPH bitmap should survive TPSACK-CC header ordering");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbTransportRecvTpsackDoesNotMisparseSaetphTest : public TestCase
{
public:
    UbTransportRecvTpsackDoesNotMisparseSaetphTest()
        : TestCase("UnifiedBus - sender transport safely consumes TPSACK headers")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(0, 16);
        txTp->RetainSentPsnForTest(1, 16);
        txTp->SetPsnSndUnaForTest(0);
        txTp->RecvTpAck(BuildTpsackForSender(0, false, false));
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              0u,
                              "First-packet-loss TPSACK must not move cumulative sender ACK state");

        txTp->RecvTpAck(BuildTpsackForSender(0, true, false));
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              1u,
                              "TPSACK with BitMap[0]=1 may advance cumulative sender ACK state");

        txTp->RecvTpAck(BuildTpsackForSender(1, true, true));
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              2u,
                              "TPSACK-CC should remove CETPH and SAETPH before TAACK");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchDispatchesTpsackAsTransportResponseTest : public TestCase
{
public:
    UbSwitchDispatchesTpsackAsTransportResponseTest()
        : TestCase("UnifiedBus - local sink dispatches TPSACK to transport response path")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        txTp->RetainSentPsnForTest(0, 16);
        txTp->SetPsnSndUnaForTest(0);

        Ptr<Packet> packet = BuildTpsackForSender(0, true, false);
        UdpHeader udpHeader;
        packet->AddHeader(udpHeader);
        UbPort::AddIpv4Header(packet,
                              NodeIdToIp(topo.receiver->GetId(), topo.receiverPort->GetIfIndex()),
                              NodeIdToIp(topo.sender->GetId(), topo.senderPort->GetIfIndex()));
        UbIpBasedNetworkHeader networkHeader;
        packet->AddHeader(networkHeader);
        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    true,
                                    kUrmaWriteRegressionPriority,
                                    kUrmaWriteRegressionPriority,
                                    false,
                                    true,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);

        topo.sender->GetObject<UbSwitch>()->SwitchHandlePacket(topo.senderPort, packet);

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              1u,
                              "TPSACK reaching local sink should advance only through RecvTpAck");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchRejectsReservedTpOpcodeTest : public TestCase
{
public:
    UbSwitchRejectsReservedTpOpcodeTest()
        : TestCase("UnifiedBus - local sink rejects reserved transport opcode")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<Packet> packet = Create<Packet>(0);
        packet->AddHeader(UbAckTransactionHeader());
        UbTransportHeader tpHeader;
        tpHeader.SetTPOpcode(TpOpcode::TP_OPCODE_RESERVED1);
        tpHeader.SetRspSt(3);
        tpHeader.SetSrcTpn(kUrmaWriteRegressionReceiverTpn);
        tpHeader.SetDestTpn(kUrmaWriteRegressionSenderTpn);
        packet->AddHeader(tpHeader);
        packet->AddHeader(UdpHeader());
        UbPort::AddIpv4Header(packet,
                              NodeIdToIp(topo.receiver->GetId(), topo.receiverPort->GetIfIndex()),
                              NodeIdToIp(topo.sender->GetId(), topo.senderPort->GetIfIndex()));
        packet->AddHeader(UbIpBasedNetworkHeader());
        UbDataLink::GenPacketHeader(packet, false, true, kUrmaWriteRegressionPriority,
                                    kUrmaWriteRegressionPriority, false, true,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);
        txTp->SetPsnSndUnaForTest(7);
        txTp->SetPsnRecvNxtForTest(11);
        topo.sender->GetObject<UbSwitch>()->SwitchHandlePacket(topo.senderPort, packet);
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(), 7u,
                              "reserved TPOpcode must not enter the ACK path");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnRecvNxtForTest(), 11u,
                              "reserved TPOpcode must not enter the data path");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbGbnReceiverKeepsOutOfOrderAckSilentTest : public TestCase
{
public:
    UbGbnReceiverKeepsOutOfOrderAckSilentTest()
        : TestCase("UnifiedBus - default GBN receiver keeps out-of-order ACK silent")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(), 1u, "PSN 0 should still produce ordinary TPACK");
        rxTp->PopAckForTest();

        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        NS_TEST_ASSERT_MSG_EQ(rxTp->GetPendingAckCountForTest(),
                              0u,
                              "default GBN mode should keep existing silent out-of-order behavior");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbGbnReceiverEmitsSpecTpnakWireFormatTest : public TestCase
{
public:
    UbGbnReceiverEmitsSpecTpnakWireFormatTest()
        : TestCase("UnifiedBus - GBN receiver emits TPNAK as TPACK opcode with RSPST 3")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        Config::SetDefault("ns3::UbTransportChannel::EnableRetrans", BooleanValue(true));
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans", BooleanValue(true));
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        DecodedReceiverAck response = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(response.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "ordinary TPNAK should use TPOpcode 0x2");
        NS_TEST_ASSERT_MSG_EQ(response.tpHeader.GetRspSt(), 3u,
                              "ordinary TPNAK should carry RSPST 3");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbGbnTpnakCcWireAndConsumptionTest : public TestCase
{
public:
    UbGbnTpnakCcWireAndConsumptionTest()
        : TestCase("UnifiedBus - GBN TPNAK-CC carries and consumes CETPH")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbTransportChannel::EnableRetrans", BooleanValue(true));
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans", BooleanValue(true));
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> rxTp =
            topo.receiver->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        Ptr<RecordingTpnakCc> receiverCc = CreateObject<RecordingTpnakCc>();
        rxTp->SetCongestionControlForTest(receiverCc);
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 0, false));
        rxTp->PopAckForTest();
        rxTp->RecvDataPacket(BuildReceiverDataPacket(topo, 2, true));
        DecodedReceiverAck response = DecodeReceiverAck(rxTp->PopAckForTest());
        NS_TEST_ASSERT_MSG_EQ(response.tpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH),
                              "TPNAK-CC should use TPOpcode 0x3");
        NS_TEST_ASSERT_MSG_EQ(response.tpHeader.GetRspSt(), 3u,
                              "TPNAK-CC should carry RSPST 3");
        NS_TEST_ASSERT_MSG_EQ(response.hasCongestionHeader, true,
                              "TPNAK-CC should carry CETPH before TAACK");
        NS_TEST_ASSERT_MSG_EQ(response.congestionHeader.GetAckSequence(), 17u,
                              "TPNAK-CC should preserve CETPH Ack_seq");

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<RecordingTpnakCc> senderCc = CreateObject<RecordingTpnakCc>();
        txTp->SetCongestionControlForTest(senderCc);
        Ptr<UbWqeSegment> first = CreateObject<UbWqeSegment>();
        first->SetPsnStart(7);
        first->SetCarrierBytes(100);
        first->UpdateSentBytes(100);
        txTp->PushWqeSegment(first);
        Ptr<UbWqeSegment> second = CreateObject<UbWqeSegment>();
        second->SetPsnStart(8);
        second->SetCarrierBytes(200);
        second->UpdateSentBytes(200);
        txTp->PushWqeSegment(second);
        txTp->SetPsnSndUnaForTest(7);
        txTp->SetPsnSndNxtForTest(9);
        txTp->RecvTpAck(BuildTpnakForSender(7, true, 23));
        NS_TEST_ASSERT_MSG_EQ(senderCc->notificationCount, 1u,
                              "sender CC should consume TPNAK-CC feedback");
        NS_TEST_ASSERT_MSG_EQ(senderCc->lastAckSequence, 23u,
                              "sender CC should receive TPNAK-CC Ack_seq");
        NS_TEST_ASSERT_MSG_EQ(senderCc->lastRetransmitBytes, 300u,
                              "sender CC should retire the full GBN retransmission range once");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderRecordsMissingWithoutFastRetransmitTest : public TestCase
{
public:
    UbSelectiveSenderRecordsMissingWithoutFastRetransmitTest()
        : TestCase("UnifiedBus - sender records TPSACK holes without fast selective retransmission")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(false));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        for (uint32_t psn = 10; psn <= 13; ++psn)
        {
            txTp->RetainSentPsnForTest(psn, 16);
        }
        txTp->SetPsnSndUnaForTest(10);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 13, {0, 2, 3}));

        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(11),
                              true,
                              "TPSACK [1,0,1,1] should mark PSN 11 as reported missing");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              0u,
                              "fast selective retransmission disabled should not enqueue immediately");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              11u,
                              "BitMap[0]=1 should cumulatively advance through the ACK base only");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderFastRetransmitQueuesMissingOnceTest : public TestCase
{
public:
    UbSelectiveSenderFastRetransmitQueuesMissingOnceTest()
        : TestCase("UnifiedBus - sender fast selective retransmission queues missing PSNs once")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        for (uint32_t psn = 10; psn <= 13; ++psn)
        {
            txTp->RetainSentPsnForTest(psn, 16);
        }
        txTp->SetPsnSndUnaForTest(10);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 13, {0, 2, 3}));
        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 13, {0, 2, 3}));

        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(11),
                              true,
                              "TPSACK should mark PSN 11 as reported missing");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              1u,
                              "duplicate TPSACK should not enqueue a second retransmission");

        Ptr<Packet> retransmission = txTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(retransmission, nullptr, "queued selective retransmission should produce a packet");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              0u,
                              "dequeueing selective retransmission should drain the queue");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnRetransmitCountForTest(11),
                              1u,
                              "selective retransmission should update retransmit count once");
        txTp->ReTxTimeout();
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              1u,
                              "RTO should requeue an unacknowledged PSN after fast retransmission dequeue");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveRetransmitTraceReportsSparsePsnTest : public TestCase
{
public:
    UbSelectiveRetransmitTraceReportsSparsePsnTest()
        : TestCase("UnifiedBus - selective retransmission trace reports sparse missing PSN")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        for (uint32_t psn = 10; psn <= 12; ++psn)
        {
            txTp->RetainSentPsnForTest(psn, 16);
        }
        txTp->SetPsnSndUnaForTest(10);
        g_selectiveRetransmitRecordsForTest.clear();
        txTp->TraceConnectWithoutContext("SelectiveRetransmitNotify",
                                         MakeCallback(&RecordSelectiveRetransmitForTest));

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 12, {0, 2}));

        while (txTp->GetPendingSelectiveRetransmissionCountForTest() > 0)
        {
            Ptr<Packet> retransmission = txTp->GetNextPacketForTest();
            NS_TEST_ASSERT_MSG_NE(retransmission, nullptr, "pending sparse retransmission should dequeue");
        }

        NS_TEST_ASSERT_MSG_EQ(g_selectiveRetransmitRecordsForTest.size(),
                              1u,
                              "TPSACK [1,0,1] should produce exactly one retransmission trace");
        const SelectiveRetransmitTraceRecord traced =
            g_selectiveRetransmitRecordsForTest.empty()
                ? SelectiveRetransmitTraceRecord{0,
                                                 0,
                                                 std::numeric_limits<uint64_t>::max(),
                                                 0}
                : g_selectiveRetransmitRecordsForTest.front();
        NS_TEST_ASSERT_MSG_EQ(traced.nodeId,
                              topo.sender->GetId(),
                              "trace should report the sender node id");
        NS_TEST_ASSERT_MSG_EQ(traced.tpn,
                              kUrmaWriteRegressionSenderTpn,
                              "trace should report the sender TPN");
        NS_TEST_ASSERT_MSG_EQ(traced.psn,
                              11u,
                              "TPSACK [1,0,1] should retransmit only PSN 11");
        NS_TEST_ASSERT_MSG_EQ(traced.payloadBytes,
                              16u,
                              "trace should report the retained packet payload bytes");

        Simulator::Destroy();
        Config::Reset();
        g_selectiveRetransmitRecordsForTest.clear();
    }
};

class UbSelectiveSenderBitmapBoundaryIgnoresPaddingTest : public TestCase
{
public:
    UbSelectiveSenderBitmapBoundaryIgnoresPaddingTest()
        : TestCase("UnifiedBus - sender ignores TPSACK zero padding beyond MaxRcvPSN")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(63, 16);
        txTp->RetainSentPsnForTest(64, 16);
        txTp->SetPsnSndUnaForTest(0);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(0, 64, {0, 63}, 64));

        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(64),
                              false,
                              "offset equal to bitmap width should be outside the represented TPSACK interval");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              0u,
                              "sender should not enqueue padding bits beyond MaxRcvPSN");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderMaxRcvPsnSuppressesPaddingHolesTest : public TestCase
{
public:
    UbSelectiveSenderMaxRcvPsnSuppressesPaddingHolesTest()
        : TestCase("UnifiedBus - sender ignores TPSACK zero padding beyond MaxRcvPSN")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(11, 16);
        txTp->RetainSentPsnForTest(12, 16);
        txTp->SetPsnSndUnaForTest(10);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}, 64));

        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(11),
                              true,
                              "zero bit at or below MaxRcvPSN should report a missing PSN");
        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(12),
                              false,
                              "zero bit beyond MaxRcvPSN should be padding, not missing evidence");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderUnwrapsTpsackAcrossWirePsnWrapTest : public TestCase
{
public:
    UbSelectiveSenderUnwrapsTpsackAcrossWirePsnWrapTest()
        : TestCase("UnifiedBus - sender unwraps TPSACK wire PSNs across 24-bit wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        constexpr uint64_t ackBase = 0xFFFFFE;
        for (uint64_t psn = ackBase; psn <= ackBase + 3; ++psn)
        {
            txTp->RetainSentPsnForTest(psn, 16);
        }
        txTp->SetPsnSndUnaForTest(ackBase);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(static_cast<uint32_t>(ackBase),
                                                   static_cast<uint32_t>(ackBase + 3),
                                                   {0, 1, 3},
                                                   64));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              ackBase + 2,
                              "contiguous bitmap bits should ACK through the wrapped wire PSN");
        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(ackBase + 2),
                              true,
                              "zero bit after wire wrap should mark the logical PSN missing");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              1u,
                              "wrapped TPSACK should queue one logical missing PSN");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderIgnoresOutOfWindowTpsackAcrossWireWrapTest : public TestCase
{
public:
    UbSelectiveSenderIgnoresOutOfWindowTpsackAcrossWireWrapTest()
        : TestCase("UnifiedBus - sender ignores out-of-window TPSACK across 24-bit wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(0x1000000ull, 16);
        txTp->RetainSentPsnForTest(0x1000001ull, 16);
        txTp->SetPsnSndUnaForTest(0x1000000ull);
        txTp->SetPsnSndNxtForTest(0x1000002ull);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(3, 3, {0}, 64));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              0x1000000ull,
                              "out-of-window TPSACK must not advance sender ACK state");
        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(0x1000000ull),
                              true,
                              "out-of-window TPSACK must not retire retained PSN 0x1000000");
        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(0x1000001ull),
                              true,
                              "out-of-window TPSACK must not retire retained PSN 0x1000001");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderAcceptsTpsackAtCurrentAckBaseTest : public TestCase
{
public:
    UbSelectiveSenderAcceptsTpsackAtCurrentAckBaseTest()
        : TestCase("UnifiedBus - sender accepts TPSACK at current cumulative ACK base")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(10, 16);
        txTp->RetainSentPsnForTest(11, 16);
        txTp->SetPsnSndUnaForTest(11);
        txTp->SetPsnSndNxtForTest(12);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}, 64));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              11u,
                              "TPSACK at sndUna - 1 must not change cumulative ACK state");
        NS_TEST_ASSERT_MSG_EQ(txTp->WasPsnSelectivelyReportedMissingForTest(11),
                              true,
                              "TPSACK at sndUna - 1 should still report the next PSN missing");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              1u,
                              "valid TPSACK at current ACK base should queue the missing PSN");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSenderUnwrapsPlainTpackAcrossWirePsnWrapTest : public TestCase
{
public:
    UbSenderUnwrapsPlainTpackAcrossWirePsnWrapTest()
        : TestCase("UnifiedBus - sender unwraps plain TPACK across 24-bit wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(0x1000000ull, 16);
        txTp->RetainSentPsnForTest(0x1000001ull, 16);
        txTp->SetPsnSndUnaForTest(0x1000000ull);
        txTp->SetPsnSndNxtForTest(0x1000002ull);

        txTp->RecvTpAck(BuildTpackForSender(1));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              0x1000002ull,
                              "wire TPACK PSN 1 should ACK through logical PSN 0x1000001");
        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(0x1000000ull),
                              false,
                              "plain TPACK across wrap should retire retained logical PSN 0x1000000");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbGbnSenderUnwrapsTpnakAcrossWirePsnWrapTest : public TestCase
{
public:
    UbGbnSenderUnwrapsTpnakAcrossWirePsnWrapTest()
        : TestCase("UnifiedBus - GBN sender unwraps TPNAK across 24-bit wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        Config::SetDefault("ns3::UbTransportChannel::EnableRetrans", BooleanValue(true));
        Config::SetDefault("ns3::UbTransportChannel::RetransmissionMode",
                           EnumValue(UbRetransmissionMode::GBN));
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->SetPsnSndUnaForTest(0x1000000ull);
        txTp->SetPsnSndNxtForTest(0x1000004ull);
        txTp->RecvTpAck(BuildTpnakForSender(1));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndNxtForTest(),
                              0x1000001ull,
                              "wire TPNAK PSN 1 should reset GBN send next to logical 0x1000001");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderCumulativeAckClearsRetainedStateTest : public TestCase
{
public:
    UbSelectiveSenderCumulativeAckClearsRetainedStateTest()
        : TestCase("UnifiedBus - sender cumulative ACK clears retained PSN state")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        for (uint32_t psn = 10; psn <= 13; ++psn)
        {
            txTp->RetainSentPsnForTest(psn, 16);
        }
        txTp->SetPsnSndUnaForTest(10);

        txTp->RecvTpAck(BuildTpackForSender(13));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              14u,
                              "TPACK PSN 13 should advance cumulative sender ACK state to 14");
        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(10), false, "acked PSN 10 should be removed");
        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(13), false, "acked PSN 13 should be removed");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveAckedGapStateKeepsStateButSkipsRetransmitTest : public TestCase
{
public:
    UbSelectiveAckedGapStateKeepsStateButSkipsRetransmitTest()
        : TestCase("UnifiedBus - selective ACKed gap state keeps state but skips retransmit")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(10, 64);
        txTp->RetainSentPsnForTest(11, 64);
        txTp->SetPsnSndUnaForTest(10);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {1}));

        NS_TEST_ASSERT_MSG_EQ(txTp->HasRetainedPsnForTest(11),
                              true,
                              "ACKed PSN beyond a gap must keep state until the gap closes");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnRetransmitCountForTest(11),
                              0u,
                              "ACKed PSN must not be retransmitted");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderRetransmitsRetainedMissingPacketTest : public TestCase
{
public:
    UbSelectiveSenderRetransmitsRetainedMissingPacketTest()
        : TestCase("UnifiedBus - sender selective retransmission replays retained missing packet")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        Ptr<UbWqeSegment> request = CreateObject<UbWqeSegment>();
        request->SetSrc(topo.sender->GetId());
        request->SetDest(topo.receiver->GetId());
        request->SetSport(topo.senderPort->GetIfIndex());
        request->SetDport(topo.receiverPort->GetIfIndex());
        request->SetType(TaOpcode::TA_OPCODE_WRITE);
        request->SetSize(UB_MTU_BYTE * 4);
        request->SetPriority(kUrmaWriteRegressionPriority);
        request->SetTaskId(kUrmaWriteRegressionTaskId);
        request->SetWqeSize(UB_MTU_BYTE * 4);
        request->SetJettyNum(kUrmaWriteRegressionJettyNum);
        request->SetTaMsn(0);
        request->SetTaSsn(0);
        request->SetOrderType(OrderType::ORDER_NO);
        request->SetTpn(kUrmaWriteRegressionSenderTpn);
        request->SetTpMsn(txTp->GetMsnCnt());
        request->SetPsnStart(txTp->GetPsnCnt());
        request->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
        request->SetOriginJettyNum(kUrmaWriteRegressionJettyNum);
        request->SetRequestTassn(0);
        request->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
        request->SetResponseBytes(0);
        request->SetNeedsTransactionResponse(true);
        request->SetResLenBytes(UB_MTU_BYTE * 4);
        request->SetPayloadBytes(UB_MTU_BYTE * 4);
        request->SetCarrierBytes(UB_MTU_BYTE * 4);

        txTp->UpdatePsnCnt(request->GetPsnSize());
        txTp->UpDateMsnCnt(1);
        txTp->PushWqeSegment(request);

        for (uint32_t i = 0; i < 4; ++i)
        {
            Ptr<Packet> sent = txTp->GetNextPacketForTest();
            NS_TEST_ASSERT_MSG_NE(sent, nullptr, "initial send should retain each PSN");
        }

        txTp->RecvTpAck(BuildTpsackBitmapForSender(0, 3, {0, 2, 3}));
        NS_TEST_ASSERT_MSG_EQ(txTp->IsEmpty(),
                              false,
                              "pending selective retransmission should make TP non-empty before dequeue");
        Ptr<Packet> retransmission = txTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(retransmission, nullptr, "missing PSN should be retransmitted");

        UbDatalinkPacketHeader dlHeader;
        UbIpBasedNetworkHeader networkHeader;
        Ipv4Header ipv4Header;
        UdpHeader udpHeader;
        UbTransportHeader tpHeader;
        retransmission->RemoveHeader(dlHeader);
        retransmission->RemoveHeader(networkHeader);
        retransmission->RemoveHeader(ipv4Header);
        retransmission->RemoveHeader(udpHeader);
        retransmission->RemoveHeader(tpHeader);

        NS_TEST_ASSERT_MSG_EQ(tpHeader.GetPsn(),
                              1u,
                              "selective retransmission should replay only the missing PSN");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnRetransmitCountForTest(1),
                              1u,
                              "missing retained PSN should record one retransmission");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnRetransmitCountForTest(2),
                              0u,
                              "selectively acknowledged PSN should not be retransmitted");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderQueueContractTest : public TestCase
{
public:
    UbSelectiveSenderQueueContractTest()
        : TestCase("UnifiedBus - sender selective retransmission participates in TP queue contract")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(11, 128);
        txTp->SetPsnSndUnaForTest(11);
        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}));

        NS_TEST_ASSERT_MSG_EQ(txTp->IsEmpty(),
                              false,
                              "pending selective retransmission should make TP non-empty");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetNextPacketSize(),
                              128u + UbTransportHeader().GetSerializedSize(),
                              "next packet size should report the retained retransmission packet");
        NS_TEST_ASSERT_MSG_EQ(txTp->IsLimited(),
                              false,
                              "selective retransmission should bypass new-PSN inflight limit when CC allows");

        Ptr<Packet> retransmission = txTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(retransmission, nullptr, "queue contract should allow dequeuing retransmission");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderDropsAckedStaleRetransmitEntriesTest : public TestCase
{
public:
    UbSelectiveSenderDropsAckedStaleRetransmitEntriesTest()
        : TestCase("UnifiedBus - sender drops stale selective retransmission entries after ACK")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(11, 64);
        txTp->SetPsnSndUnaForTest(11);
        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}));
        NS_TEST_ASSERT_MSG_EQ(txTp->GetRawSelectiveRetransmissionQueueCountForTest(),
                              1u,
                              "TPSACK should create one raw queue entry before ACK cleanup");

        txTp->RecvTpAck(BuildTpackForSender(11));

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              0u,
                              "ACKed retransmission entry should no longer be live");
        NS_TEST_ASSERT_MSG_EQ(txTp->GetRawSelectiveRetransmissionQueueCountForTest(),
                              0u,
                              "ACK cleanup should compact stale selective retransmission queue entries");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderQueuePriorityTest : public TestCase
{
public:
    UbSelectiveSenderQueuePriorityTest()
        : TestCase("UnifiedBus - sender queue priority keeps control responses before selective retransmission")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(11, 64);
        txTp->SetPsnSndUnaForTest(11);
        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}));
        txTp->EnqueueAckForTest(BuildTpackForSender(10));
        txTp->EnqueueCnpForTest(Create<Packet>(1));

        Ptr<Packet> first = txTp->GetNextPacketForTest();
        Ptr<Packet> second = txTp->GetNextPacketForTest();
        Ptr<Packet> third = txTp->GetNextPacketForTest();

        NS_TEST_ASSERT_MSG_EQ(first->GetSize(), 1u, "CNP queue should have highest priority");

        UbTransportHeader secondTpHeader;
        second->RemoveHeader(secondTpHeader);
        NS_TEST_ASSERT_MSG_EQ(secondTpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH),
                              "ACK/TPSACK response queue should precede selective retransmission");

        UbTransportHeader thirdTpHeader;
        third->RemoveHeader(thirdTpHeader);
        NS_TEST_ASSERT_MSG_EQ(thirdTpHeader.GetPsn(),
                              11u,
                              "selective retransmission should run after control responses");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveSenderRtoEnqueuesOutstandingPsnsTest : public TestCase
{
public:
    UbSelectiveSenderRtoEnqueuesOutstandingPsnsTest()
        : TestCase("UnifiedBus - selective RTO enqueues retained outstanding PSNs")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);

        txTp->RetainSentPsnForTest(10, 64);
        txTp->RetainSentPsnForTest(11, 64);
        txTp->SetPsnSndUnaForTest(10);

        txTp->ReTxTimeout();

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPendingSelectiveRetransmissionCountForTest(),
                              2u,
                              "selective RTO should enqueue all unacknowledged retained PSNs");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveRetransmissionAccountsDcqcnSendStateTest : public TestCase
{
public:
    UbSelectiveRetransmissionAccountsDcqcnSendStateTest()
        : TestCase("UnifiedBus - selective retransmission updates DCQCN sender accounting")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        (void)cc->IsCcLimited(1);
        txTp->RetainSentPsnForTest(11, 64);
        txTp->SetPsnSndUnaForTest(11);
        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}));

        const Time beforeRetransmit = cc->GetNextAvailableSendTimeForTest();
        Ptr<Packet> retransmission = txTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(retransmission, nullptr, "selective retransmission should dequeue under DCQCN");
        NS_TEST_ASSERT_MSG_GT(cc->GetNextAvailableSendTimeForTest(),
                              beforeRetransmit,
                              "selective retransmission should advance DCQCN pacing debt");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSelectiveRetransmissionUsesPayloadBytesForCongestionControlTest : public TestCase
{
public:
    UbSelectiveRetransmissionUsesPayloadBytesForCongestionControlTest()
        : TestCase("UnifiedBus - selective retransmission congestion control uses payload bytes")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<RecordingRetransmissionCc> cc = CreateObject<RecordingRetransmissionCc>();
        txTp->SetCongestionControlForTest(cc);

        txTp->RetainSentPsnForTest(10, 16);
        txTp->RetainSentPsnForTest(11, 0);
        txTp->SetPsnSndUnaForTest(10);

        txTp->RecvTpAck(BuildTpsackBitmapForSender(10, 11, {0}, 64, true, 1000));

        NS_TEST_ASSERT_MSG_EQ(cc->congestionNotificationCount,
                              1u,
                              "TPSACK-CC should notify congestion control once");
        NS_TEST_ASSERT_MSG_EQ(cc->lastCongestionNotificationRetransmitBytes,
                              0u,
                              "TPSACK-CC congestion accounting should use missing payload bytes");

        Ptr<Packet> retransmission = txTp->GetNextPacketForTest();
        NS_TEST_ASSERT_MSG_NE(retransmission,
                              nullptr,
                              "zero-payload selective retransmission should not be blocked by payload-byte accounting");
        NS_TEST_ASSERT_MSG_EQ(cc->lastLimitedCheckBytes,
                              0u,
                              "retransmission CC limit check should use payload bytes");
        NS_TEST_ASSERT_MSG_EQ(cc->retransmissionNotifyCount,
                              1u,
                              "dequeued selective retransmission should notify congestion control");
        NS_TEST_ASSERT_MSG_EQ(cc->lastRetransmissionBytes,
                              0u,
                              "retransmission send accounting should use payload bytes");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCaqmSelectiveAckWithCetphAccountingTest : public TestCase
{
public:
    UbCaqmSelectiveAckWithCetphAccountingTest()
        : TestCase("UnifiedBus - CAQM accounts selective feedback without SAETPH dependency")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("CAQM"));

        Ptr<UbHostCaqm> caqm = DynamicCast<UbHostCaqm>(UbCongestionControl::Create(UB_DEVICE));
        NS_TEST_ASSERT_MSG_NE(caqm, nullptr, "CAQM host factory should return a concrete host object");

        caqm->OnSenderDataPacketSent(10, 1000);
        caqm->OnSenderDataPacketSent(11, 1000);
        NS_TEST_ASSERT_MSG_EQ(caqm->GetDataByteSentForTest(),
                              2000u,
                              "two sends should be durable accounting");
        NS_TEST_ASSERT_MSG_EQ(caqm->GetInflightForTest(), 2000u, "two sends should be in flight");

        UbCongestionExtTph cetph;
        cetph.SetAckSequence(1000);
        cetph.SetC(0);
        cetph.SetI(1);
        cetph.SetHint(0);

        caqm->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH, 10, cetph, 1000);

        NS_TEST_ASSERT_MSG_EQ(caqm->GetDataByteSentForTest(),
                              1000u,
                              "missing bytes should be removed from durable accounting once");
        NS_TEST_ASSERT_MSG_EQ(caqm->GetInflightForTest(),
                              0u,
                              "inflight should be recomputed from adjusted bytes and CETPH ack sequence");

        caqm->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH, 10, cetph, 0);

        NS_TEST_ASSERT_MSG_EQ(caqm->GetDataByteSentForTest(),
                              1000u,
                              "duplicate selective feedback should not subtract again");
        NS_TEST_ASSERT_MSG_EQ(caqm->GetInflightForTest(),
                              0u,
                              "duplicate selective feedback should keep inflight stable");

        caqm->OnSenderRetransmissionPacketSent(11, 1000);

        NS_TEST_ASSERT_MSG_EQ(caqm->GetDataByteSentForTest(),
                              2000u,
                              "selective retransmission send should re-enter CAQM sent accounting");
        NS_TEST_ASSERT_MSG_EQ(caqm->GetInflightForTest(),
                              1000u,
                              "selective retransmission send should occupy CAQM inflight");

        UbCongestionExtTph finalCetph;
        finalCetph.SetAckSequence(2000);
        finalCetph.SetC(0);
        finalCetph.SetI(1);
        finalCetph.SetHint(0);
        caqm->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_ACK_WITH_CETPH, 11, finalCetph);

        NS_TEST_ASSERT_MSG_EQ(caqm->GetInflightForTest(),
                              0u,
                              "final CETPH should clear retransmitted inflight bytes");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbTransportTpsackCcReportsRetransmitBytesOnceTest : public TestCase
{
public:
    UbTransportTpsackCcReportsRetransmitBytesOnceTest()
        : TestCase("UnifiedBus - TPSACK-CC reports newly missing selective bytes once")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("CAQM"));
        UseSelectiveRetransmissionForTest();
        Config::SetDefault("ns3::UbTransportChannel::EnableFastRetrans",
                           BooleanValue(false));

        Ptr<UbTransportChannel> senderTp = CreateObject<UbTransportChannel>();
        Ptr<UbHostCaqm> caqm = DynamicCast<UbHostCaqm>(UbCongestionControl::Create(UB_DEVICE));
        NS_TEST_ASSERT_MSG_NE(caqm, nullptr, "CAQM host factory should return a concrete host object");
        senderTp->SetCongestionControlForTest(caqm);

        caqm->OnSenderDataPacketSent(10, 1000);
        caqm->OnSenderDataPacketSent(11, 1000);
        senderTp->RetainSentPsnForTest(10, 1000);
        senderTp->RetainSentPsnForTest(11, 1000);

        Ptr<Packet> sack = BuildTpsackBitmapForSender(10, 11, {0}, 64, true, 1000);
        senderTp->RecvTpAck(sack->Copy());
        NS_TEST_ASSERT_MSG_EQ(caqm->GetDataByteSentForTest(),
                              1000u,
                              "first TPSACK should report newly missing bytes");

        senderTp->RecvTpAck(sack->Copy());
        NS_TEST_ASSERT_MSG_EQ(caqm->GetDataByteSentForTest(),
                              1000u,
                              "duplicate TPSACK should not report the same missing PSN twice");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnSenderCutsRateOnCnpTest : public TestCase
{
public:
    UbDcqcnSenderCutsRateOnCnpTest()
        : TestCase("UnifiedBus - DCQCN sender cuts rate and blocks send after CNP")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        UbCongestionExtTph cnp;
        cnp.SetAckSequence(0);
        cnp.SetRawBytes4to7(static_cast<uint32_t>(0x1U) << 30);

        NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(256), false, "Fresh sender should not be blocked");
        cc->OnSenderCongestionNotification(TpOpcode::TP_OPCODE_CNP, 0, cnp);
        cc->OnSenderDataPacketSent(1, 4096);
        NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(4096),
                              true,
                              "Sender should become pacing-limited immediately after a CNP cut");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCnpDoesNotAdvanceAckStateTest : public TestCase
{
public:
    UbDcqcnCnpDoesNotAdvanceAckStateTest()
        : TestCase("UnifiedBus - DCQCN CNP does not advance reliable ACK state")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        uint32_t before = txTp->GetPsnSndUnaForTest();

        Ptr<Packet> cnpPacket = txTp->BuildDcqcnCnp(0x1, false);
        txTp->RecvTpAck(cnpPacket);

        NS_TEST_ASSERT_MSG_EQ(txTp->GetPsnSndUnaForTest(),
                              before,
                              "CNP must not advance ACK tracking");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnRecoveryTimerIncreasesRateTest : public TestCase
{
public:
    UbDcqcnRecoveryTimerIncreasesRateTest()
        : TestCase("UnifiedBus - DCQCN recovery timer raises sender rate after CNP cut")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(55)));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        const uint64_t before = cc->GetCurrentRateForTest().GetBitRate();
        cc->ApplySyntheticCnpForTest();

        Simulator::Schedule(MicroSeconds(60), [this, cc, before]() {
            NS_TEST_ASSERT_MSG_GT(cc->GetCurrentRateForTest().GetBitRate(),
                                  before / 2,
                                  "Recovery timer should begin increasing sender rate");
        });

        Simulator::Stop(MicroSeconds(61));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnByteCounterIncreasesRateBeforeTimerTest : public TestCase
{
public:
    UbDcqcnByteCounterIncreasesRateBeforeTimerTest()
        : TestCase("UnifiedBus - DCQCN byte counter increases rate before timer expiry")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MilliSeconds(1)));
        Config::SetDefault("ns3::UbHostDcqcn::ByteCounterThreshold", UintegerValue(1024));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        cc->ApplySyntheticCnpForTest();
        const uint64_t cutRate = cc->GetCurrentRateForTest().GetBitRate();
        cc->OnSenderDataPacketSent(1, 2048);

        Simulator::Schedule(MicroSeconds(5), [this, cc, cutRate]() {
            NS_TEST_ASSERT_MSG_GT(cc->GetCurrentRateForTest().GetBitRate(),
                                  cutRate,
                                  "Byte counter should increase rate before timer expiry");
        });

        Simulator::Stop(MicroSeconds(6));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnHyperIncreaseUsesHaiRateTest : public TestCase
{
public:
    UbDcqcnHyperIncreaseUsesHaiRateTest()
        : TestCase("UnifiedBus - DCQCN hyper increase uses the configured HAI step")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("1000Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("250Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(1)));
        Config::SetDefault("ns3::UbHostDcqcn::ByteCounterThreshold", UintegerValue(1));
        Config::SetDefault("ns3::UbHostDcqcn::RateAi", DataRateValue(DataRate("1Mbps")));
        Config::SetDefault("ns3::UbHostDcqcn::HyperAiRate", DataRateValue(DataRate("100Mbps")));
        Config::SetDefault("ns3::UbHostDcqcn::FastRecoveryLimit", UintegerValue(2));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> firstTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> firstCc = DynamicCast<UbHostDcqcn>(firstTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(firstCc, nullptr, "First sender TP should bind host DCQCN");
        (void)firstCc->IsCcLimited(1);

        Ptr<UbCongestionControl> senderCc2 = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             303,
                             404,
                             senderCc2);
        Ptr<UbCongestionControl> receiverCc2 = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               404,
                               303,
                               receiverCc2);

        Ptr<UbTransportChannel> txTp = senderCtrl->GetTpByTpn(303);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");
        (void)cc->IsCcLimited(1);

        DataRate beforeHyper;
        cc->ApplySyntheticCnpForTest();
        cc->OnSenderDataPacketSent(1, 1);
        Simulator::Schedule(MicroSeconds(2), [cc]() { cc->OnSenderDataPacketSent(2, 1); });
        Simulator::Schedule(MicroSeconds(3), [&beforeHyper, cc]() {
            beforeHyper = cc->GetTargetRateForTest();
        });
        Simulator::Schedule(MicroSeconds(4), [cc]() { cc->OnSenderDataPacketSent(3, 1); });
        Simulator::Schedule(MicroSeconds(6), [this, cc, &beforeHyper]() {
            NS_TEST_ASSERT_MSG_GT(cc->GetTargetRateForTest().GetBitRate() - beforeHyper.GetBitRate(),
                                  DataRate("50Mbps").GetBitRate(),
                                  "Hyper increase should use a larger HAI step than additive increase");
        });

        Simulator::Stop(MicroSeconds(7));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnRateNeverExceedsLineRateTest : public TestCase
{
public:
    UbDcqcnRateNeverExceedsLineRateTest()
        : TestCase("UnifiedBus - DCQCN sender rate never exceeds configured line rate")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(1)));
        Config::SetDefault("ns3::UbHostDcqcn::ByteCounterThreshold", UintegerValue(1));
        Config::SetDefault("ns3::UbHostDcqcn::RateAi", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::HyperAiRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::FastRecoveryLimit", UintegerValue(1));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        cc->ApplySyntheticCnpForTest();
        for (uint32_t index = 0; index < 8; ++index)
        {
            Simulator::Schedule(MicroSeconds(index + 1),
                                [cc, index]() { cc->OnSenderDataPacketSent(index + 1, 1); });
        }

        Simulator::Schedule(MicroSeconds(12), [this, cc]() {
            NS_TEST_ASSERT_MSG_LT_OR_EQ(cc->GetCurrentRateForTest().GetBitRate(),
                                        DataRate("100Gbps").GetBitRate(),
                                        "Recovery logic must clamp sender rate to line rate");
        });

        Simulator::Stop(MicroSeconds(13));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest : public TestCase
{
public:
    UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest()
        : TestCase("UnifiedBus - DCQCN busy host starts second flow below line rate")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("25Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> firstTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> firstCc = DynamicCast<UbHostDcqcn>(firstTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(firstCc, nullptr, "First sender TP should bind host DCQCN");

        (void)firstCc->IsCcLimited(1);

        Ptr<UbCongestionControl> senderCc2 = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             303,
                             404,
                             senderCc2);

        Ptr<UbCongestionControl> receiverCc2 = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               404,
                               303,
                               receiverCc2);

        Ptr<UbTransportChannel> secondTp = senderCtrl->GetTpByTpn(303);
        Ptr<UbHostDcqcn> secondCc = DynamicCast<UbHostDcqcn>(secondTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(secondCc, nullptr, "Second sender TP should bind host DCQCN");

        (void)secondCc->IsCcLimited(1);

        NS_TEST_ASSERT_MSG_EQ(firstCc->GetCurrentRateForTest().GetBitRate(),
                              DataRate("100Gbps").GetBitRate(),
                              "First active flow should start at line rate");
        NS_TEST_ASSERT_MSG_EQ(secondCc->GetCurrentRateForTest().GetBitRate(),
                              DataRate("25Gbps").GetBitRate(),
                              "Busy-host second flow should start at configured initial rate");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnPacingWakeupResumesQueuedSendTest : public TestCase
{
public:
    UbDcqcnPacingWakeupResumesQueuedSendTest()
        : TestCase("UnifiedBus - DCQCN pacing wakeup resumes queued sends without external events")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("100Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaWriteRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaWriteRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(senderTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");
        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaWriteRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(MakeCallback(&UbDcqcnPacingWakeupResumesQueuedSendTest::OnTaskCompleted,
                                              this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   64 * 1024,
                                                   kUrmaWriteRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_WRITE);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaWriteRegressionJettyNum);

        Simulator::Schedule(MicroSeconds(2), [cc]() { cc->ApplySyntheticCnpForTest(); });

        uint64_t txBytesAtCut = 0;
        Simulator::Schedule(MicroSeconds(3), [&txBytesAtCut, &topo]() {
            txBytesAtCut = topo.senderPort->GetTxBytes();
        });

        Simulator::Schedule(MicroSeconds(80), [this, &topo, &txBytesAtCut]() {
            NS_TEST_ASSERT_MSG_GT(topo.senderPort->GetTxBytes(),
                                  txBytesAtCut,
                                  "Pacing wakeup should resume sending after a DCQCN cut even without ACK/CNP/PFC follow-up events");
        });

        Simulator::Stop(MicroSeconds(81));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }

private:
    void OnTaskCompleted(uint32_t, uint32_t) {}
};

class UbDcqcnCnpCutRescalesOutstandingPacingDebtTest : public TestCase
{
public:
    UbDcqcnCnpCutRescalesOutstandingPacingDebtTest()
        : TestCase("UnifiedBus - DCQCN CNP rescales outstanding pacing debt to the reduced rate")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("100Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbTransportChannel> txTp =
            topo.sender->GetObject<UbController>()->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        (void)cc->IsCcLimited(1);
        cc->OnSenderDataPacketSent(1, 4096);

        Simulator::Schedule(NanoSeconds(100), [cc]() { cc->ApplySyntheticCnpForTest(); });
        Simulator::Schedule(NanoSeconds(400), [this, cc]() {
            NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(4096),
                                  true,
                                  "CNP cut should stretch outstanding pacing debt to the reduced rate");
        });
        Simulator::Schedule(NanoSeconds(600), [this, cc]() {
            NS_TEST_ASSERT_MSG_EQ(cc->IsCcLimited(4096),
                                  false,
                                  "Sender should become sendable again after the rescaled pacing deadline");
        });

        Simulator::Stop(NanoSeconds(601));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDcqcnCompletedFlowReleasesHostActiveSlotTest : public TestCase
{
public:
    UbDcqcnCompletedFlowReleasesHostActiveSlotTest()
        : TestCase("UnifiedBus - completed DCQCN flow releases host active-flow slot")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("25Gbps")));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaWriteRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaWriteRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaWriteRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(MakeCallback(&UbDcqcnCompletedFlowReleasesHostActiveSlotTest::OnTaskCompleted,
                                              this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   4 * 1024,
                                                   kUrmaWriteRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_WRITE);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaWriteRegressionJettyNum);

        Simulator::Stop(MicroSeconds(200));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_taskCompleted,
                              true,
                              "Baseline write should complete before checking host active-flow accounting");
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                              0u,
                              "Completed flow should release the host active-flow slot");

        Ptr<UbCongestionControl> senderCc2 = UbCongestionControl::Create(UB_DEVICE);
        senderCtrl->CreateTp(topo.sender->GetId(),
                             topo.receiver->GetId(),
                             topo.senderPort->GetIfIndex(),
                             topo.receiverPort->GetIfIndex(),
                             kUrmaWriteRegressionPriority,
                             303,
                             404,
                             senderCc2);
        Ptr<UbCongestionControl> receiverCc2 = UbCongestionControl::Create(UB_DEVICE);
        receiverCtrl->CreateTp(topo.receiver->GetId(),
                               topo.sender->GetId(),
                               topo.receiverPort->GetIfIndex(),
                               topo.senderPort->GetIfIndex(),
                               kUrmaWriteRegressionPriority,
                               404,
                               303,
                               receiverCc2);

        Ptr<UbTransportChannel> secondTp = senderCtrl->GetTpByTpn(303);
        Ptr<UbHostDcqcn> secondCc = DynamicCast<UbHostDcqcn>(secondTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(secondCc, nullptr, "Second sender TP should bind a host DCQCN instance");
        (void)secondCc->IsCcLimited(1);

        NS_TEST_ASSERT_MSG_EQ(secondCc->GetCurrentRateForTest().GetBitRate(),
                              DataRate("100Gbps").GetBitRate(),
                              "A host with no active flows should restart new flows at line rate");

        Simulator::Destroy();
        Config::Reset();
    }

private:
    void OnTaskCompleted(uint32_t, uint32_t)
    {
        m_taskCompleted = true;
    }

    bool m_taskCompleted{false};
};

class UbDcqcnIdleFlowCancelsRecoveryStateTest : public TestCase
{
public:
    UbDcqcnIdleFlowCancelsRecoveryStateTest()
        : TestCase("UnifiedBus - idle DCQCN sender cancels recovery timers and stays inactive")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(true));
        GlobalValue::Bind("UB_CC_ALGO", StringValue("DCQCN"));
        Config::SetDefault("ns3::UbHostDcqcn::LineRate", DataRateValue(DataRate("100Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::InitialRate", DataRateValue(DataRate("25Gbps")));
        Config::SetDefault("ns3::UbHostDcqcn::RateIncreaseTimer", TimeValue(MicroSeconds(55)));
        Config::SetDefault("ns3::UbHostDcqcn::AlphaUpdateInterval", TimeValue(MicroSeconds(55)));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbTransportChannel> txTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbHostDcqcn> cc = DynamicCast<UbHostDcqcn>(txTp->GetCongestionCtrlForTest());
        NS_TEST_ASSERT_MSG_NE(cc, nullptr, "Sender TP should bind a host DCQCN instance");

        (void)cc->IsCcLimited(1);
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                              1u,
                              "Starting sender should register one active host flow");

        cc->ApplySyntheticCnpForTest();
        cc->OnSenderTransportIdle();

        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                              0u,
                              "Idle sender should release host active-flow slot immediately");

        Simulator::Schedule(MicroSeconds(120), [this, senderCtrl, cc]() {
            NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetActiveSenderFlowCount(),
                                  0u,
                                  "Idle sender must stay inactive after old DCQCN recovery timers would have fired");
            NS_TEST_ASSERT_MSG_EQ(cc->GetCurrentRateForTest().GetBitRate(),
                                  DataRate("25Gbps").GetBitRate(),
                                  "Idle sender should keep fresh baseline rate until a new flow starts");
        });

        Simulator::Stop(MicroSeconds(121));
        Simulator::Run();
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbUrmaReadWqeMetadataPropagationTest : public TestCase
{
  public:
    UbUrmaReadWqeMetadataPropagationTest()
        : TestCase("UnifiedBus - URMA_READ WQE metadata propagates through Jetty segmentation")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbController> controller = CreateObject<UbController>();
        node->AggregateObject(controller);
        controller->CreateUbFunction();
        controller->CreateUbTransaction();

        Ptr<UbFunction> function = controller->GetUbFunction();
        const uint32_t jettyNum = 7;
        const uint32_t taskId = 1234;
        const uint32_t payloadBytes = 4096;
        function->CreateJetty(node->GetId(), node->GetId() + 1, jettyNum);

        Ptr<UbWqe> wqe = function->CreateWqe(node->GetId(),
                                             node->GetId() + 1,
                                             payloadBytes,
                                             taskId,
                                             TaOpcode::TA_OPCODE_READ);
        NS_TEST_ASSERT_MSG_NE(wqe, nullptr, "CreateWqe should return a valid WQE");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(wqe->GetType()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "URMA_READ must map to TA_OPCODE_READ");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(wqe->GetSegmentKind()),
                              static_cast<uint8_t>(UbTransactionSegmentKind::REQUEST),
                              "CreateWqe should initialize segment kind as request");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetOriginJettyNum(),
                              UINT32_MAX,
                              "originJettyNum should be invalid before enqueue");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetRequestTassn(),
                              UINT32_MAX,
                              "requestTassn should be invalid before enqueue");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(wqe->GetRequestOpcode()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "requestOpcode should preserve read opcode");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetResponseBytes(),
                              payloadBytes,
                              "CreateWqe should initialize read response bytes");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetResLenBytes(),
                              payloadBytes,
                              "READ WQE resLenBytes should equal request bytes");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetPayloadBytes(),
                              0u,
                              "READ WQE payloadBytes should be zero");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetCarrierBytes(),
                              1u,
                              "READ WQE carrierBytes should force one request packet");
        NS_TEST_ASSERT_MSG_EQ(wqe->NeedsTransactionResponse(),
                              true,
                              "URMA read must require transaction response");

        function->PushWqeToJetty(wqe, jettyNum);
        NS_TEST_ASSERT_MSG_EQ(wqe->GetOriginJettyNum(),
                              jettyNum,
                              "PushWqe must assign originJettyNum from bound Jetty");
        NS_TEST_ASSERT_MSG_EQ(wqe->GetRequestTassn(),
                              wqe->GetTaSsnStart(),
                              "PushWqe must assign requestTassn from WQE TA SSN start");

        Ptr<UbJetty> jetty = function->GetJetty(jettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Jetty should exist after CreateJetty");
        Ptr<UbWqeSegment> segment = jetty->GetNextWqeSegment();
        NS_TEST_ASSERT_MSG_NE(segment, nullptr, "Jetty should generate a segment for queued WQE");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(segment->GetType()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "Segment opcode should remain TA_OPCODE_READ");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(segment->GetSegmentKind()),
                              static_cast<uint8_t>(UbTransactionSegmentKind::REQUEST),
                              "Segment should preserve request kind");
        NS_TEST_ASSERT_MSG_EQ(segment->GetOriginJettyNum(),
                              jettyNum,
                              "Segment should inherit originJettyNum");
        NS_TEST_ASSERT_MSG_EQ(segment->GetRequestTassn(),
                              wqe->GetRequestTassn(),
                              "Segment should inherit requestTassn");
        NS_TEST_ASSERT_MSG_EQ(static_cast<uint8_t>(segment->GetRequestOpcode()),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ),
                              "Segment should preserve requestOpcode");
        NS_TEST_ASSERT_MSG_EQ(segment->GetResponseBytes(),
                              payloadBytes,
                              "Segment should carry read response byte count");
        NS_TEST_ASSERT_MSG_EQ(segment->GetResLenBytes(),
                              payloadBytes,
                              "READ request slice resLenBytes should preserve request bytes");
        NS_TEST_ASSERT_MSG_EQ(segment->GetPayloadBytes(),
                              0u,
                              "READ request slice payloadBytes should be zero");
        NS_TEST_ASSERT_MSG_EQ(segment->GetCarrierBytes(),
                              1u,
                              "READ request slice carrierBytes should be one");
        NS_TEST_ASSERT_MSG_EQ(segment->NeedsTransactionResponse(),
                              true,
                              "Segment should preserve response-required flag");
    }
};

class UbRemoteAddressUsesSegmentTaSsnTest : public TestCase
{
public:
    UbRemoteAddressUsesSegmentTaSsnTest()
        : TestCase("UnifiedBus - remote address offset uses segment TASSN")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbTransaction> transaction = CreateObject<UbTransaction>();
        node->AggregateObject(transaction);

        Ptr<UbWqeSegment> request = CreateObject<UbWqeSegment>();
        request->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
        request->SetRemoteAddress(0x10000000ull);
        request->SetTaSsn(3);
        request->SetRequestTassn(1);

        const uint64_t expected = 0x10000000ull + 3ull * UB_WQE_TA_SEGMENT_BYTE;
        NS_TEST_ASSERT_MSG_EQ(transaction->DeriveRemoteAddressForTest(request),
                              expected,
                              "remote address should use the concrete request segment TASSN");

        Simulator::Destroy();
    }
};

class UbWqeSegmentKeepsLogicalTaSequencesTest : public TestCase
{
public:
    UbWqeSegmentKeepsLogicalTaSequencesTest()
        : TestCase("UnifiedBus - WQE segment keeps logical TA sequence values")
    {
    }

    void DoRun() override
    {
        Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
        segment->SetTaMsn(0x1000001ull);
        segment->SetTaSsn(0x10001u);
        segment->SetRequestTassn(0x10001u);
        segment->SetTpMsn(0x1000001ull);
        segment->SetPsnStart(0x1000001ull);

        NS_TEST_ASSERT_MSG_EQ(segment->GetTaMsn(),
                              0x1000001ull,
                              "TA MSN should remain logical in WQE segment state");
        NS_TEST_ASSERT_MSG_EQ(segment->GetTaSsn(),
                              0x10001u,
                              "TASSN should remain logical in WQE segment state");
        NS_TEST_ASSERT_MSG_EQ(segment->GetRequestTassn(),
                              0x10001u,
                              "request TASSN should remain logical in WQE segment state");
        NS_TEST_ASSERT_MSG_EQ(segment->GetTpMsn(),
                              0x1000001ull,
                              "TPMSN should remain logical in WQE segment state");
        NS_TEST_ASSERT_MSG_EQ(segment->GetPsnStart(),
                              0x1000001ull,
                              "PSN start should remain logical in WQE segment state");
    }
};

class UbJettyCompletesLogicalTassnAcrossWireWrapTest : public TestCase
{
public:
    UbJettyCompletesLogicalTassnAcrossWireWrapTest()
        : TestCase("UnifiedBus - Jetty completes logical TASSN across 16-bit wrap")
    {
    }

    void DoRun() override
    {
        Ptr<UbJetty> jetty = CreateObject<UbJetty>();
        jetty->Init();
        jetty->SetTaSsnSendWindowForTest(0x10000u, 0x10002u);

        const bool complete = jetty->ProcessWqeSegmentComplete(0x10000u);

        NS_TEST_ASSERT_MSG_EQ(complete,
                              true,
                              "logical TASSN 0x10000 should be accepted after 16-bit wire wrap");
        NS_TEST_ASSERT_MSG_EQ(jetty->GetTaSsnSndUnaForTest(),
                              0x10001u,
                              "Jetty send una should advance in logical TASSN space");
    }
};

class UbReceiverKeepsInboundTpMsnLogicalKeyAcrossWireWrapTest : public TestCase
{
public:
    UbReceiverKeepsInboundTpMsnLogicalKeyAcrossWireWrapTest()
        : TestCase("UnifiedBus - inbound TA key uses logical TPMSN across 24-bit wrap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        GlobalValue::Bind("UB_CC_ENABLED", BooleanValue(false));
        UseSelectiveRetransmissionForTest();

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbTransportChannel> rxTp = CreateSelectiveReceiverTp(topo);
        rxTp->SetPsnRecvNxtForTest(0x1000000ull);
        rxTp->SetInboundTpMsnReferenceForTest(kUrmaWriteRegressionSenderTpn, 0);
        rxTp->SetInboundTaSsnReferenceForTest(kUrmaWriteRegressionSenderTpn,
                                              kUrmaWriteRegressionJettyNum,
                                              0);

        rxTp->RecvDataPacket(BuildReceiverDataPacketWithSequences(topo,
                                                                  0,
                                                                  false,
                                                                  0,
                                                                  0,
                                                                  16,
                                                                  32,
                                                                  9100));
        rxTp->PopAckForTest();
        rxTp->SetInboundTpMsnReferenceForTest(kUrmaWriteRegressionSenderTpn, 0x1000000ull);
        rxTp->SetInboundTaSsnReferenceForTest(kUrmaWriteRegressionSenderTpn,
                                              kUrmaWriteRegressionJettyNum,
                                              0x10000u);
        rxTp->RecvDataPacket(BuildReceiverDataPacketWithSequences(topo,
                                                                  1,
                                                                  false,
                                                                  0,
                                                                  0,
                                                                  16,
                                                                  32,
                                                                  9101));

        NS_TEST_ASSERT_MSG_EQ(rxTp->GetInboundTaUnitCountForTest(),
                              2u,
                              "same low wire TPMSN values after wrap should not collide in inbound TA map");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbUrmaWriteCompletionNeedsTransactionResponseTest : public TestCase
{
  public:
    UbUrmaWriteCompletionNeedsTransactionResponseTest()
        : TestCase("UnifiedBus - URMA_WRITE completes on TA response instead of request TP ACK")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaWriteRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaWriteRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaWriteRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(
            MakeCallback(&UbUrmaWriteCompletionNeedsTransactionResponseTest::OnTaskCompleted, this));

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketACKsNotify",
            MakeCallback(&UbUrmaWriteCompletionNeedsTransactionResponseTest::ObserveSenderTpAck, this));
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbUrmaWriteCompletionNeedsTransactionResponseTest::ObserveSenderResponsePacket,
                         this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   64 * 1024,
                                                   kUrmaWriteRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_WRITE);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaWriteRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_requestTpAckObserved,
                              true,
                              "Sender should observe request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_completedImmediatelyAfterRequestTpAck,
                              false,
                              "URMA write must not complete immediately after request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_responsePacketObserved,
                              true,
                              "Sender should receive a transaction response packet");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleted,
                              true,
                              "URMA write should complete after transaction response");
        NS_TEST_ASSERT_MSG_EQ(m_completedTaskId,
                              kUrmaWriteRegressionTaskId,
                              "Completion callback should report the original task");
        const bool completionAfterResponse = m_taskCompleteTime >= m_responsePacketTime;
        NS_TEST_ASSERT_MSG_EQ(completionAfterResponse,
                              true,
                              "Task completion must not precede transaction response arrival");

        Simulator::Destroy();
    }

  private:
    void ObserveSenderTpAck(uint32_t,
                            uint32_t taskId,
                            uint32_t srcTpn,
                            uint32_t dstTpn,
                            uint32_t,
                            uint32_t,
                            uint32_t)
    {
        if (taskId != kUrmaWriteRegressionTaskId ||
            srcTpn != kUrmaWriteRegressionSenderTpn ||
            dstTpn != kUrmaWriteRegressionReceiverTpn)
        {
            return;
        }

        if (!m_requestTpAckObserved)
        {
            m_requestTpAckObserved = true;
            Simulator::ScheduleNow(
                &UbUrmaWriteCompletionNeedsTransactionResponseTest::CheckCompletionAfterRequestTpAck,
                this);
        }
    }

    void ObserveSenderResponsePacket(uint32_t,
                                     uint32_t srcTpn,
                                     uint32_t dstTpn,
                                     uint32_t,
                                     uint32_t,
                                     uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        if (!m_responsePacketObserved)
        {
            m_responsePacketObserved = true;
            m_responsePacketTime = Simulator::Now();
        }
    }

    void CheckCompletionAfterRequestTpAck()
    {
        m_completedImmediatelyAfterRequestTpAck = m_taskCompleted;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        m_taskCompleted = true;
        m_completedTaskId = taskId;
        m_taskCompleteTime = Simulator::Now();
    }

    bool m_requestTpAckObserved{false};
    bool m_responsePacketObserved{false};
    bool m_taskCompleted{false};
    bool m_completedImmediatelyAfterRequestTpAck{false};
    uint32_t m_completedTaskId{UINT32_MAX};
    Time m_responsePacketTime{Seconds(0)};
    Time m_taskCompleteTime{Seconds(0)};
};

class UbUrmaReadCompletionNeedsReadResponseTest : public TestCase
{
  public:
    UbUrmaReadCompletionNeedsReadResponseTest()
        : TestCase("UnifiedBus - URMA_READ completes on READ_RESPONSE instead of request TP ACK")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaReadRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaReadRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaReadRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(
            MakeCallback(&UbUrmaReadCompletionNeedsReadResponseTest::OnTaskCompleted, this));

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketACKsNotify",
            MakeCallback(&UbUrmaReadCompletionNeedsReadResponseTest::ObserveSenderTpAck, this));
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbUrmaReadCompletionNeedsReadResponseTest::ObserveSenderReadResponse,
                         this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   4096,
                                                   kUrmaReadRegressionTaskId,
                                                   TaOpcode::TA_OPCODE_READ);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaReadRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_requestTpAckObserved,
                              true,
                              "Sender should observe read request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_completedImmediatelyAfterRequestTpAck,
                              false,
                              "URMA read must not complete immediately after request TP ACK");
        NS_TEST_ASSERT_MSG_EQ(m_readResponseObserved,
                              true,
                              "Sender should receive a READ_RESPONSE packet");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleted,
                              true,
                              "URMA read should complete after READ_RESPONSE");
        NS_TEST_ASSERT_MSG_EQ(m_completedTaskId,
                              kUrmaReadRegressionTaskId,
                              "Completion callback should report the original read task");
        const bool completionAfterResponse = m_taskCompleteTime >= m_readResponseTime;
        NS_TEST_ASSERT_MSG_EQ(completionAfterResponse,
                              true,
                              "Task completion must not precede READ_RESPONSE arrival");

        Simulator::Destroy();
    }

  private:
    void ObserveSenderTpAck(uint32_t,
                            uint32_t taskId,
                            uint32_t srcTpn,
                            uint32_t dstTpn,
                            uint32_t,
                            uint32_t,
                            uint32_t)
    {
        if (taskId != kUrmaReadRegressionTaskId ||
            srcTpn != kUrmaWriteRegressionSenderTpn ||
            dstTpn != kUrmaWriteRegressionReceiverTpn)
        {
            return;
        }

        if (!m_requestTpAckObserved)
        {
            m_requestTpAckObserved = true;
            Simulator::ScheduleNow(
                &UbUrmaReadCompletionNeedsReadResponseTest::CheckCompletionAfterRequestTpAck,
                this);
        }
    }

    void ObserveSenderReadResponse(uint32_t,
                                   uint32_t srcTpn,
                                   uint32_t dstTpn,
                                   uint32_t,
                                   uint32_t,
                                   uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        if (!m_readResponseObserved)
        {
            m_readResponseObserved = true;
            m_readResponseTime = Simulator::Now();
        }
    }

    void CheckCompletionAfterRequestTpAck()
    {
        m_completedImmediatelyAfterRequestTpAck = m_taskCompleted;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        m_taskCompleted = true;
        m_completedTaskId = taskId;
        m_taskCompleteTime = Simulator::Now();
    }

    bool m_requestTpAckObserved{false};
    bool m_readResponseObserved{false};
    bool m_taskCompleted{false};
    bool m_completedImmediatelyAfterRequestTpAck{false};
    uint32_t m_completedTaskId{UINT32_MAX};
    Time m_readResponseTime{Seconds(0)};
    Time m_taskCompleteTime{Seconds(0)};
};

class UbUrmaReadMultiPacketResponseCountTest : public TestCase
{
  public:
    UbUrmaReadMultiPacketResponseCountTest()
        : TestCase("UnifiedBus - multi-packet URMA_READ generates one READ_RESPONSE")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaReadRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaReadRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");

        Ptr<UbJetty> jetty = senderFunction->GetJetty(kUrmaReadRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(jetty, nullptr, "Sender Jetty should exist");
        jetty->SetClientCallback(
            MakeCallback(&UbUrmaReadMultiPacketResponseCountTest::OnTaskCompleted, this));

        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(&UbUrmaReadMultiPacketResponseCountTest::ObserveSenderReadResponse,
                         this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   64 * 1024,
                                                   kUrmaReadMultiPacketTaskId,
                                                   TaOpcode::TA_OPCODE_READ);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaReadRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_readResponseCount,
                              1u,
                              "A multi-packet read request should generate exactly one READ_RESPONSE");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleteCount,
                              1u,
                              "A multi-packet read request should complete exactly once");

        Simulator::Destroy();
    }

  private:
    void ObserveSenderReadResponse(uint32_t,
                                   uint32_t srcTpn,
                                   uint32_t dstTpn,
                                   uint32_t,
                                   uint32_t,
                                   uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        ++m_readResponseCount;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        if (taskId == kUrmaReadMultiPacketTaskId)
        {
            ++m_taskCompleteCount;
        }
    }

    uint32_t m_readResponseCount{0};
    uint32_t m_taskCompleteCount{0};
};

class UbUrmaReadMultiSliceRequestPacketSemanticsTest : public TestCase
{
  public:
    UbUrmaReadMultiSliceRequestPacketSemanticsTest()
        : TestCase("UnifiedBus - multi-slice URMA_READ request packets carry zero payload")
    {
    }

    void DoRun() override
    {
        (void)UbFlowTag::GetTypeId();
        (void)UbPacketTraceTag::GetTypeId();
        (void)utils::UbUtils::Get();
        GlobalValue::Bind("UB_RECORD_PKT_TRACE", BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);
        m_expectedSrc = topo.sender->GetId();
        m_expectedDst = topo.receiver->GetId();

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbFunction> senderFunction = senderCtrl->GetUbFunction();
        Ptr<UbTransaction> senderTransaction = senderCtrl->GetUbTransaction();
        senderFunction->CreateJetty(topo.sender->GetId(),
                                    topo.receiver->GetId(),
                                    kUrmaReadRegressionJettyNum);
        const std::vector<uint32_t> tpns = {kUrmaWriteRegressionSenderTpn};
        const bool bindOk = senderTransaction->JettyBindTp(topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           kUrmaReadRegressionJettyNum,
                                                           false,
                                                           tpns);
        NS_TEST_ASSERT_MSG_EQ(bindOk, true, "Sender Jetty should bind to static TP pair");
        Ptr<UbJetty> senderJetty = senderFunction->GetJetty(kUrmaReadRegressionJettyNum);
        NS_TEST_ASSERT_MSG_NE(senderJetty, nullptr, "Sender Jetty should exist");
        senderJetty->SetClientCallback(
            MakeCallback(&UbUrmaReadMultiSliceRequestPacketSemanticsTest::OnTaskCompleted, this));

        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> receiverTp = receiverCtrl->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        NS_TEST_ASSERT_MSG_NE(receiverTp, nullptr, "Receiver TP should exist");
        receiverTp->TraceConnectWithoutContext(
            "TpRecvNotify",
            MakeCallback(
                &UbUrmaReadMultiSliceRequestPacketSemanticsTest::ObserveTargetPacketReceive,
                this));
        receiverTp->TraceConnectWithoutContext(
            "WqeSegmentCompletesNotify",
            MakeCallback(
                &UbUrmaReadMultiSliceRequestPacketSemanticsTest::ObserveTargetReadRequestSlice,
                this));
        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        senderTp->TraceConnectWithoutContext(
            "LastPacketReceivesNotify",
            MakeCallback(
                &UbUrmaReadMultiSliceRequestPacketSemanticsTest::ObserveSenderReadResponse,
                this));

        Ptr<UbWqe> wqe = senderFunction->CreateWqe(topo.sender->GetId(),
                                                   topo.receiver->GetId(),
                                                   128 * 1024,
                                                   kUrmaReadMultiPacketTaskId,
                                                   TaOpcode::TA_OPCODE_READ);
        Simulator::ScheduleNow(&UbFunction::PushWqeToJetty,
                               senderFunction,
                               wqe,
                               kUrmaReadRegressionJettyNum);

        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_readRequestPacketCount,
                              2u,
                              "128KiB read should produce 2 read request packets");
        NS_TEST_ASSERT_MSG_EQ(m_zeroPayloadReadRequestCount,
                              2u,
                              "Each read request packet should carry zero payload");
        NS_TEST_ASSERT_MSG_EQ(m_targetReadRequestSliceCount,
                              2u,
                              "128KiB read should complete 2 request slices at TA");
        NS_TEST_ASSERT_MSG_EQ(m_readResponseCount,
                              2u,
                              "128KiB read should generate 2 read responses");
        NS_TEST_ASSERT_MSG_EQ(m_taskCompleteCount,
                              1u,
                              "Multi-slice read should still complete the WQE exactly once");

        Simulator::Destroy();
        GlobalValue::Bind("UB_RECORD_PKT_TRACE", BooleanValue(false));
    }

  private:
    void ObserveTargetPacketReceive(uint32_t,
                                    uint32_t,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t srcTpn,
                                    uint32_t dstTpn,
                                    PacketType type,
                                    uint32_t payloadBytes,
                                    uint32_t taskId,
                                    std::string,
                                    UbPacketTraceTag)
    {
        if (type != PacketType::PACKET || taskId != kUrmaReadMultiPacketTaskId)
        {
            return;
        }
        if (src != m_expectedSrc || dst != m_expectedDst)
        {
            return;
        }
        if (srcTpn != kUrmaWriteRegressionSenderTpn || dstTpn != kUrmaWriteRegressionReceiverTpn)
        {
            return;
        }

        ++m_readRequestPacketCount;
        if (payloadBytes == 0)
        {
            ++m_zeroPayloadReadRequestCount;
        }
    }

    void ObserveTargetReadRequestSlice(uint32_t,
                                       uint32_t taskId,
                                       uint32_t)
    {
        if (taskId == kUrmaReadMultiPacketTaskId)
        {
            ++m_targetReadRequestSliceCount;
        }
    }

    void ObserveSenderReadResponse(uint32_t,
                                   uint32_t srcTpn,
                                   uint32_t dstTpn,
                                   uint32_t,
                                   uint32_t,
                                   uint32_t)
    {
        if (srcTpn != kUrmaWriteRegressionReceiverTpn || dstTpn != kUrmaWriteRegressionSenderTpn)
        {
            return;
        }

        ++m_readResponseCount;
    }

    void OnTaskCompleted(uint32_t taskId, uint32_t)
    {
        if (taskId == kUrmaReadMultiPacketTaskId)
        {
            ++m_taskCompleteCount;
        }
    }

    uint32_t m_readRequestPacketCount{0};
    uint32_t m_zeroPayloadReadRequestCount{0};
    uint32_t m_targetReadRequestSliceCount{0};
    uint32_t m_readResponseCount{0};
    uint32_t m_taskCompleteCount{0};
    uint32_t m_expectedSrc{UINT32_MAX};
    uint32_t m_expectedDst{UINT32_MAX};
};

class UbUrmaWriteOutOfOrderRequestSliceCompletionTest : public TestCase
{
  public:
    UbUrmaWriteOutOfOrderRequestSliceCompletionTest()
        : TestCase("UnifiedBus - out-of-order URMA_WRITE request slice still completes at TA")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        InstallStaticTpPair(topo);

        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        Ptr<UbTransportChannel> senderTp = senderCtrl->GetTpByTpn(kUrmaWriteRegressionSenderTpn);
        Ptr<UbTransportChannel> receiverTp = receiverCtrl->GetTpByTpn(kUrmaWriteRegressionReceiverTpn);
        NS_TEST_ASSERT_MSG_NE(senderTp, nullptr, "Sender TP should exist");
        NS_TEST_ASSERT_MSG_NE(receiverTp, nullptr, "Receiver TP should exist");

        receiverTp->TraceConnectWithoutContext(
            "WqeSegmentCompletesNotify",
            MakeCallback(
                &UbUrmaWriteOutOfOrderRequestSliceCompletionTest::ObserveTargetRequestSliceComplete,
                this));

        Ptr<UbWqeSegment> request = CreateOutOfOrderWriteRequestSegment(topo, senderTp);
        senderTp->UpdatePsnCnt(request->GetPsnSize());
        senderTp->UpDateMsnCnt(1);
        senderTp->PushWqeSegment(request);

        Ptr<Packet> firstPacket = senderTp->GetNextPacket();
        Ptr<Packet> lastPacket = senderTp->GetNextPacket();
        NS_TEST_ASSERT_MSG_NE(firstPacket, nullptr, "First request packet should exist");
        NS_TEST_ASSERT_MSG_NE(lastPacket, nullptr, "Last request packet should exist");

        receiverTp->RecvDataPacket(lastPacket->Copy());
        NS_TEST_ASSERT_MSG_EQ(m_targetRequestSliceCompleteCount,
                              0u,
                              "Out-of-order last packet alone must not complete the TA slice");

        receiverTp->RecvDataPacket(firstPacket->Copy());
        Simulator::Stop(MilliSeconds(1));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(m_targetRequestSliceCompleteCount,
                              1u,
                              "Completing the PSN gap should complete the write request slice");

        Simulator::Destroy();
    }

  private:
    Ptr<UbWqeSegment> CreateOutOfOrderWriteRequestSegment(const LocalTpTopology& topo,
                                                          const Ptr<UbTransportChannel>& senderTp)
    {
        constexpr uint32_t requestBytes = UB_MTU_BYTE + 512;
        Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();
        segment->SetSrc(topo.sender->GetId());
        segment->SetDest(topo.receiver->GetId());
        segment->SetSport(topo.senderPort->GetIfIndex());
        segment->SetDport(topo.receiverPort->GetIfIndex());
        segment->SetType(TaOpcode::TA_OPCODE_WRITE);
        segment->SetSize(requestBytes);
        segment->SetPriority(kUrmaWriteRegressionPriority);
        segment->SetTaskId(kUrmaWriteRegressionTaskId);
        segment->SetWqeSize(requestBytes);
        segment->SetJettyNum(kUrmaWriteRegressionJettyNum);
        segment->SetTaMsn(0);
        segment->SetTaSsn(0);
        segment->SetOrderType(OrderType::ORDER_NO);
        segment->SetTpn(kUrmaWriteRegressionSenderTpn);
        segment->SetTpMsn(senderTp->GetMsnCnt());
        segment->SetPsnStart(senderTp->GetPsnCnt());
        segment->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
        segment->SetOriginJettyNum(kUrmaWriteRegressionJettyNum);
        segment->SetRequestTassn(0);
        segment->SetRequestOpcode(TaOpcode::TA_OPCODE_WRITE);
        segment->SetResponseBytes(0);
        segment->SetNeedsTransactionResponse(true);
        segment->SetResLenBytes(requestBytes);
        segment->SetPayloadBytes(requestBytes);
        segment->SetCarrierBytes(requestBytes);
        return segment;
    }

    void ObserveTargetRequestSliceComplete(uint32_t, uint32_t taskId, uint32_t)
    {
        if (taskId == kUrmaWriteRegressionTaskId)
        {
            ++m_targetRequestSliceCompleteCount;
        }
    }

    uint32_t m_targetRequestSliceCompleteCount{0};
};

class UbCreateNodeSystemIdTest : public TestCase
{
  public:
    UbCreateNodeSystemIdTest()
        : TestCase("UnifiedBus - CreateNode honors systemId column")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-systemid-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << "0,DEVICE,1,1ns,0\n";
        nodeFile << "1,SWITCH,2,1ns,1\n";
        nodeFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());

        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNNodes(), beforeNodes + 2, "CreateNode should create 2 nodes");
        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNode(beforeNodes)->GetSystemId(), 0u,
                              "First created node should preserve systemId 0");
        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNode(beforeNodes + 1)->GetSystemId(), 1u,
                              "Second created node should preserve systemId 1");

        fs::remove_all(caseDir, ec);
    }
};

class UbCsvCrLfTrimTest : public TestCase
{
  public:
    UbCsvCrLfTrimTest()
        : TestCase("UnifiedBus - CSV readers trim CRLF line endings from fields")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-csv-crlf-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay\r\n";
        nodeFile << node0Id << ",DEVICE,1,7ns\r\n";
        nodeFile << node1Id << ",SWITCH,1,11ns\r\n";
        nodeFile.close();

        fs::path topoPath = caseDir / "topology.csv";
        std::ofstream topoFile(topoPath.string());
        topoFile << "nodeId1,portId1,nodeId2,portId2,bandwidth,delay\r\n";
        topoFile << node0Id << ",0," << node1Id << ",0,400Gbps,13ns\r\n";
        topoFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTopo(topoPath.string());

        Ptr<Node> n0 = NodeList::GetNode(node0Id);
        Ptr<Node> n1 = NodeList::GetNode(node1Id);
        Ptr<UbSwitch> sw1 = n1->GetObject<UbSwitch>();
        TimeValue allocationTime;
        sw1->GetAllocator()->GetAttribute("AllocationTime", allocationTime);
        NS_TEST_ASSERT_MSG_EQ(allocationTime.Get(),
                              NanoSeconds(11),
                              "CreateNode should trim CRLF from forwardDelay");

        Ptr<UbPort> p0 = DynamicCast<UbPort>(n0->GetDevice(0));
        Ptr<UbLink> link = DynamicCast<UbLink>(p0->GetChannel());
        NS_TEST_ASSERT_MSG_NE(link, nullptr, "CreateTopo should create a local UbLink");
        NS_TEST_ASSERT_MSG_EQ(link->GetDelay(),
                              NanoSeconds(13),
                              "CreateTopo should trim CRLF from link delay");

        fs::remove_all(caseDir, ec);
    }
};

class UbCreateNodeDelayColumnsTest : public TestCase
{
  public:
    UbCreateNodeDelayColumnsTest()
        : TestCase("UnifiedBus - CreateNode maps forwardDelay and allocationDelay separately")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-allocation-delay-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,allocationDelay,forwardDelay,systemId\n";
        nodeFile << "0,DEVICE,1,3ns,7ns,1\n";
        nodeFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());

        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNNodes(), beforeNodes + 1, "CreateNode should create 1 node");
        Ptr<Node> node = NodeList::GetNode(beforeNodes);
        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Created node should aggregate UbSwitch");
        Ptr<UbSwitchAllocator> allocator = sw->GetAllocator();
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Created switch should have allocator");

        TimeValue inPortProcessingDelay;
        sw->GetAttribute("InPortProcessingDelay", inPortProcessingDelay);
        NS_TEST_ASSERT_MSG_EQ(inPortProcessingDelay.Get(),
                              NanoSeconds(7),
                              "forwardDelay should map to InPortProcessingDelay");

        TimeValue allocationTime;
        allocator->GetAttribute("AllocationTime", allocationTime);
        NS_TEST_ASSERT_MSG_EQ(allocationTime.Get(),
                              NanoSeconds(3),
                              "allocationDelay should map to allocator AllocationTime");

        NS_TEST_ASSERT_MSG_EQ(node->GetSystemId(), 1u, "systemId should still parse after delay fields");

        fs::remove_all(caseDir, ec);
    }
};

class UbCreateNodeForwardDelayDoesNotOverrideAllocationTimeTest : public TestCase
{
  public:
    UbCreateNodeForwardDelayDoesNotOverrideAllocationTimeTest()
        : TestCase("UnifiedBus - CreateNode forwardDelay no longer changes AllocationTime")
    {
    }

    void DoRun() override
    {
        Config::SetDefault("ns3::UbSwitchAllocator::AllocationTime", TimeValue(NanoSeconds(11)));

        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-forward-delay-only-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,allocationDelay,forwardDelay\n";
        nodeFile << "0,SWITCH,2,,7ns\n";
        nodeFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());

        Ptr<UbSwitch> sw = NodeList::GetNode(beforeNodes)->GetObject<UbSwitch>();
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Created node should aggregate UbSwitch");
        Ptr<UbSwitchAllocator> allocator = sw->GetAllocator();
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Created switch should have allocator");

        TimeValue inPortProcessingDelay;
        sw->GetAttribute("InPortProcessingDelay", inPortProcessingDelay);
        NS_TEST_ASSERT_MSG_EQ(inPortProcessingDelay.Get(),
                              NanoSeconds(7),
                              "forwardDelay should map to InPortProcessingDelay");

        TimeValue allocationTime;
        allocator->GetAttribute("AllocationTime", allocationTime);
        NS_TEST_ASSERT_MSG_EQ(allocationTime.Get(),
                              NanoSeconds(11),
                              "forwardDelay should not override AllocationTime");

        fs::remove_all(caseDir, ec);
        Config::Reset();
    }
};

class UbCreateNodeLegacyForwardDelayMapsToAllocationTimeTest : public TestCase
{
  public:
    UbCreateNodeLegacyForwardDelayMapsToAllocationTimeTest()
        : TestCase("UnifiedBus - CreateNode legacy four-column forwardDelay maps to AllocationTime")
    {
    }

    void DoRun() override
    {
        Config::SetDefault("ns3::UbSwitch::InPortProcessingDelay", TimeValue(NanoSeconds(11)));

        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir =
            fs::temp_directory_path() / ("ub-legacy-forward-delay-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay\n";
        nodeFile << "0,SWITCH,2,7ns\n";
        nodeFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());

        Ptr<UbSwitch> sw = NodeList::GetNode(beforeNodes)->GetObject<UbSwitch>();
        NS_TEST_ASSERT_MSG_NE(sw, nullptr, "Created node should aggregate UbSwitch");
        Ptr<UbSwitchAllocator> allocator = sw->GetAllocator();
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Created switch should have allocator");

        TimeValue inPortProcessingDelay;
        sw->GetAttribute("InPortProcessingDelay", inPortProcessingDelay);
        NS_TEST_ASSERT_MSG_EQ(inPortProcessingDelay.Get(),
                              NanoSeconds(11),
                              "legacy four-column forwardDelay should not set InPortProcessingDelay");

        TimeValue allocationTime;
        allocator->GetAttribute("AllocationTime", allocationTime);
        NS_TEST_ASSERT_MSG_EQ(allocationTime.Get(),
                              NanoSeconds(7),
                              "legacy four-column forwardDelay should preserve AllocationTime semantics");

        fs::remove_all(caseDir, ec);
        Config::Reset();
    }
};

class UbSwitchFlowControlModeAttributeTest : public TestCase
{
  public:
    UbSwitchFlowControlModeAttributeTest()
        : TestCase("UnifiedBus - UbSwitch flow-control mode attribute round-trips")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        sw->SetAttribute("FlowControl", EnumValue(FcType::PFC_DYNAMIC_PAPER));

        EnumValue<FcType> modeValue;
        sw->GetAttribute("FlowControl", modeValue);

        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(modeValue.Get()),
                              static_cast<int>(FcType::PFC_DYNAMIC_PAPER),
                              "UbSwitch flow-control mode attribute should round-trip through the Config system");
        Config::Reset();
    }
};

class UbSwitchLocalRuntimeConfigTest : public TestCase
{
  public:
    UbSwitchLocalRuntimeConfigTest()
        : TestCase("UnifiedBus - UbSwitch local runtime config overrides global buffer and PFC defaults")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::PFC_FIXED));
        Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(8192));
        Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(16384));
        Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(2048));
        Config::SetDefault("ns3::UbPort::PfcUpThld", IntegerValue(1000));
        Config::SetDefault("ns3::UbPort::PfcLowThld", IntegerValue(900));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        sw->SetReservePerQueueBytes(100);
        sw->SetSharedPoolBytes(200);
        sw->SetHeadroomPerPortBytes(300);
        sw->SetPfcThresholds(120, 60);

        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbQueueManager> queueManager = sw->GetQueueManager();
        auto profile = queueManager->GetBufferProfileView();
        NS_TEST_ASSERT_MSG_EQ(profile.reserve_per_queue_bytes,
                              100u,
                              "UbSwitch reserve config should override QueueManager global default");
        NS_TEST_ASSERT_MSG_EQ(profile.shared_pool_bytes,
                              200u,
                              "UbSwitch shared-pool config should override QueueManager global default");
        NS_TEST_ASSERT_MSG_EQ(profile.headroom_per_port_bytes,
                              300u,
                              "UbSwitch headroom config should override QueueManager global default");

        Ptr<UbPfc> pfc = DynamicCast<UbPfc>(port->GetFlowControl());
        constexpr uint32_t kPriority = 1;
        queueManager->PushToVoq(0, 0, kPriority, 130);

        Ptr<Packet> pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "UbSwitch local PFC thresholds should override port global defaults");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(kPriority),
                              0u,
                              "Switch-provided PFC xoff threshold should pause the queue");

        queueManager->PopFromVoq(0, 0, kPriority, 130);
        pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "Queue draining below switch-provided xon should emit resume");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "Switch-provided PFC xon threshold should resume the queue");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPortSetDataRateWithoutCongestionControlTest : public TestCase
{
  public:
    UbPortSetDataRateWithoutCongestionControlTest()
        : TestCase("UnifiedBus - UbPort SetDataRate works without attached congestion control")
    {
    }

    void DoRun() override
    {
        Config::Reset();

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);

        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        const DataRate updatedRate("200Gbps");
        port->SetDataRate(updatedRate);

        NS_TEST_ASSERT_MSG_EQ(port->GetDataRate().GetBitRate(),
                              updatedRate.GetBitRate(),
                              "UbPort::SetDataRate should update the port even when no congestion control is attached");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
namespace
{

int RunInChildProcess(const std::function<void()>& fn);

} // namespace
#endif

namespace
{

struct UbSinglePortPfcFixture
{
    Ptr<Node> node;
    Ptr<UbSwitch> sw;
    Ptr<UbPort> port;
    Ptr<UbQueueManager> queueManager;
    Ptr<UbPfc> pfc;
};

struct UbMultiPortSwitchFixture
{
    Ptr<Node> node;
    Ptr<UbSwitch> sw;
    std::vector<Ptr<UbPort>> ports;
    Ptr<UbQueueManager> queueManager;
};

class UbObservingFlowControl : public UbFlowControl
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbObservingFlowControl")
            .SetParent<UbFlowControl>()
            .AddConstructor<UbObservingFlowControl>();
        return tid;
    }

    void Configure(Ptr<UbQueueManager> queueManager, uint32_t observedInPort, uint32_t observedPriority)
    {
        m_queueManager = queueManager;
        m_observedInPort = observedInPort;
        m_observedPriority = observedPriority;
    }

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override
    {
        return false;
    }

    void OnIngressReleased(const UbFlowControlEventContext& context) override
    {
        m_called = true;
        m_observedIngressBytes =
            m_queueManager->GetQueueIngressTotalBytes(m_observedInPort, m_observedPriority);
        m_contextInPortId = context.inPortId;
        m_contextOutPortId = context.outPortId;
        m_contextPriority = context.priority;
    }

    bool m_called {false};
    uint64_t m_observedIngressBytes {0};
    uint32_t m_contextInPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextOutPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextPriority {std::numeric_limits<uint32_t>::max()};

  private:
    Ptr<UbQueueManager> m_queueManager;
    uint32_t m_observedInPort {0};
    uint32_t m_observedPriority {0};
};

class UbRecordingFlowControl : public UbFlowControl
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbRecordingFlowControl")
            .SetParent<UbFlowControl>()
            .AddConstructor<UbRecordingFlowControl>();
        return tid;
    }

    void SetEventLog(std::vector<std::string>* eventLog)
    {
        m_eventLog = eventLog;
    }

    void OnIngressEnqueued(const UbFlowControlEventContext& context) override
    {
        if (m_eventLog != nullptr)
        {
            m_eventLog->push_back("flow-control-enqueue");
        }
        m_contextInPortId = context.inPortId;
        m_contextOutPortId = context.outPortId;
        m_contextPriority = context.priority;
        m_called = true;
    }

    bool m_called {false};
    uint32_t m_contextInPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextOutPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextPriority {std::numeric_limits<uint32_t>::max()};

  private:
    std::vector<std::string>* m_eventLog {nullptr};
};

class UbRecordingCongestionControl : public UbCongestionControl
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbRecordingCongestionControl")
            .SetParent<UbCongestionControl>()
            .AddConstructor<UbRecordingCongestionControl>();
        return tid;
    }

    void SetEventLog(std::vector<std::string>* eventLog)
    {
        m_eventLog = eventLog;
    }

    void OnSwitchPostEnqueue(uint32_t inPort, uint32_t outPort, Ptr<Packet> packet) override
    {
        if (m_eventLog != nullptr)
        {
            m_eventLog->push_back("congestion-enqueue");
        }
        m_contextInPortId = inPort;
        m_contextOutPortId = outPort;
        m_lastPacketUid = packet == nullptr ? 0 : packet->GetUid();
        m_called = true;
    }

    bool m_called {false};
    uint32_t m_contextInPortId {std::numeric_limits<uint32_t>::max()};
    uint32_t m_contextOutPortId {std::numeric_limits<uint32_t>::max()};
    uint64_t m_lastPacketUid {0};

  private:
    std::vector<std::string>* m_eventLog {nullptr};
};

class UbCbfcProbe : public UbCbfc
{
  public:
    static TypeId GetTypeId(void)
    {
        static TypeId tid = TypeId("ns3::UbCbfcProbe")
            .SetParent<UbCbfc>()
            .AddConstructor<UbCbfcProbe>();
        return tid;
    }

    int32_t GetPendingCells(uint32_t vl) const
    {
        return m_crdToReturn.at(vl);
    }

    int32_t GetTxFreeCells(uint32_t vl) const
    {
        return m_crdTxfree.at(vl);
    }

    bool ShouldUseCtrlCrdRtrForTest() const
    {
        return ShouldForceControlReturn();
    }

    Ptr<Packet> MaybeBuildControlReturnPacketForTest(uint32_t targetPortId)
    {
        return MaybeBuildControlReturnPacket(targetPortId);
    }

    void MaybeQueueControlReturnForTest(uint32_t targetPortId)
    {
        MaybeQueueControlReturn(targetPortId);
    }
};

UbSinglePortPfcFixture
CreateSinglePortPfcFixture(FcType mode,
                           uint32_t reserveBytes,
                           uint64_t sharedPoolBytes,
                           uint32_t headroomPerPortBytes,
                           uint32_t resumeGapBytes,
                           uint32_t alphaShift,
                           int32_t hiThresh,
                           int32_t loThresh)
{
    Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(mode));
    Config::SetDefault("ns3::UbPort::PfcUpThld", IntegerValue(hiThresh));
    Config::SetDefault("ns3::UbPort::PfcLowThld", IntegerValue(loThresh));
    Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(reserveBytes));
    Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(sharedPoolBytes));
    Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(headroomPerPortBytes));
    Config::SetDefault("ns3::UbQueueManager::DynamicPfcResumeGapBytes",
                       UintegerValue(resumeGapBytes));
    Config::SetDefault("ns3::UbQueueManager::AlphaShift", UintegerValue(alphaShift));

    UbSinglePortPfcFixture fixture;
    fixture.node = CreateObject<Node>();
    fixture.sw = CreateObject<UbSwitch>();
    fixture.node->AggregateObject(fixture.sw);
    fixture.port = CreateObject<UbPort>();
    fixture.node->AddDevice(fixture.port);
    fixture.sw->Init();
    fixture.queueManager = fixture.sw->GetQueueManager();
    fixture.pfc = DynamicCast<UbPfc>(fixture.port->GetFlowControl());
    return fixture;
}

int
ConsumeEgressPacketForTest(Ptr<Packet>, uint32_t, uint32_t, Ptr<UbPort>)
{
    return 0;
}

UbMultiPortSwitchFixture
CreateMultiPortSwitchFixture(FcType mode,
                             uint32_t portsNum,
                             uint32_t reserveBytes,
                             uint64_t sharedPoolBytes,
                             uint32_t headroomPerPortBytes,
                             uint32_t resumeGapBytes,
                             uint32_t alphaShift,
                             const std::vector<std::pair<int32_t, int32_t>>& pfcThresholds)
{
    Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(mode));
    Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(reserveBytes));
    Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(sharedPoolBytes));
    Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(headroomPerPortBytes));
    Config::SetDefault("ns3::UbQueueManager::DynamicPfcResumeGapBytes",
                       UintegerValue(resumeGapBytes));
    Config::SetDefault("ns3::UbQueueManager::AlphaShift", UintegerValue(alphaShift));

    UbMultiPortSwitchFixture fixture;
    fixture.node = CreateObject<Node>();
    fixture.sw = CreateObject<UbSwitch>();
    fixture.node->AggregateObject(fixture.sw);

    fixture.ports.reserve(portsNum);
    for (uint32_t portId = 0; portId < portsNum; ++portId)
    {
        Ptr<UbPort> port = CreateObject<UbPort>();
        if (portId < pfcThresholds.size())
        {
            port->SetAttribute("PfcUpThld", IntegerValue(pfcThresholds[portId].first));
            port->SetAttribute("PfcLowThld", IntegerValue(pfcThresholds[portId].second));
        }
        fixture.node->AddDevice(port);
        fixture.ports.push_back(port);
    }

    fixture.sw->Init();
    fixture.queueManager = fixture.sw->GetQueueManager();
    return fixture;
}

Ptr<UbQueueManager>
CreateQueueManagerFixture(uint32_t ports,
                          uint32_t vlNum,
                          uint32_t reserveBytes,
                          uint64_t sharedPoolBytes,
                          uint32_t headroomPerPortBytes,
                          uint32_t resumeGapBytes,
                          uint32_t alphaShift)
{
    Config::SetDefault("ns3::UbQueueManager::ReservePerQueueBytes", UintegerValue(reserveBytes));
    Config::SetDefault("ns3::UbQueueManager::SharedPoolBytes", UintegerValue(sharedPoolBytes));
    Config::SetDefault("ns3::UbQueueManager::HeadroomPerPortBytes", UintegerValue(headroomPerPortBytes));
    Config::SetDefault("ns3::UbQueueManager::DynamicPfcResumeGapBytes",
                       UintegerValue(resumeGapBytes));
    Config::SetDefault("ns3::UbQueueManager::AlphaShift", UintegerValue(alphaShift));

    Ptr<UbQueueManager> queueManager = CreateObject<UbQueueManager>();
    queueManager->SetPortsNum(ports);
    queueManager->SetVLNum(vlNum);
    queueManager->Init();
    return queueManager;
}

#ifndef _WIN32
int
RunInChildProcess(const std::function<void()>& fn)
{
    pid_t pid = fork();
    if (pid == -1)
    {
        return -1;
    }

    if (pid == 0)
    {
        fn();
        std::_Exit(0);
    }

    int status = 0;
    int waitRet = waitpid(pid, &status, 0);
    if (waitRet == -1)
    {
        return -1;
    }
    return status;
}
#endif

} // namespace

class UbPfcFixedModeCountsHeadroomTest : public TestCase
{
  public:
    UbPfcFixedModeCountsHeadroomTest()
        : TestCase("UnifiedBus - PFC_FIXED counts headroom occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriority = 1;
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_FIXED,
                                                  100,
                                                  0,
                                                  50,
                                                  10,
                                                  0,
                                                  100,
                                                  40);

        fixture.queueManager->PushToVoq(0, 0, kPriority, 120);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressNonHeadroomBytes(0, kPriority),
                              0u,
                              "Reserve/shared accounting should stay at zero when the packet enters headroom directly");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(0, kPriority),
                              120u,
                              "Headroom bytes should record the packet when sharedPool is zero");

        Ptr<Packet> pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_FIXED should send PAUSE when total ingress occupancy crosses the high watermark");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              0u,
                              "PFC_FIXED PAUSE should clear credits for the congested priority");

        fixture.queueManager->PopFromVoq(0, 0, kPriority, 120);
        pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_FIXED should send RESUME after the queue drains below the low watermark");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "PFC_FIXED RESUME should restore credits for the drained priority");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcDynamicModePauseResumeTest : public TestCase
{
  public:
    UbPfcDynamicModePauseResumeTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC pauses and resumes from shared occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriority = 1;
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC,
                                                  100,
                                                  100,
                                                  50,
                                                  10,
                                                  0,
                                                  1000,
                                                  900);

        fixture.queueManager->PushToVoq(0, 0, kPriority, 120);
        fixture.queueManager->PushToVoq(0, 0, kPriority, 60);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressSharedBytes(0, kPriority),
                              80u,
                              "PFC_DYNAMIC test should build shared occupancy before pause");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(0, kPriority),
                              0u,
                              "PFC_DYNAMIC pause case should be driven by shared bytes, not headroom");

        Ptr<Packet> pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_DYNAMIC should send PAUSE when shared occupancy reaches the dynamic threshold");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              0u,
                              "PFC_DYNAMIC PAUSE should clear credits for the congested priority");

        fixture.queueManager->PopFromVoq(0, 0, kPriority, 60);
        pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "PFC_DYNAMIC should send RESUME after shared occupancy drains below xon");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "PFC_DYNAMIC RESUME should restore credits for the drained priority");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbQueueManagerDynamicPfcDecisionApiTest : public TestCase
{
  public:
    UbQueueManagerDynamicPfcDecisionApiTest()
        : TestCase("UnifiedBus - queue manager exposes unified dynamic PFC pause resume decisions")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriority = 1;
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 4,
                                                                    /*reserveBytes*/ 100,
                                                                    /*sharedPoolBytes*/ 100,
                                                                    /*headroomPerPortBytes*/ 64,
                                                                    /*resumeGapBytes*/ 10,
                                                                    /*alphaShift*/ 0);

        queueManager->PushToVoq(0, 0, kPriority, 120);
        queueManager->PushToVoq(0, 0, kPriority, 60);

        auto queueOcc = queueManager->GetIngressQueueOccupancy(0, kPriority);
        Ptr<UbPfcDynamicDecisionHook> hook = CreateObject<UbPfcDynamicDecisionHook>();
        UbPfcDecision decision = hook->Evaluate(queueManager, 0, kPriority);

        NS_TEST_ASSERT_MSG_EQ(queueOcc.shared_bytes,
                              80u,
                              "test should build shared occupancy before asking for dynamic PFC decision");
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              true,
                              "dynamic PFC pause decision should come from queue-manager state");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              false,
                              "queue above xoff should not be resumable");

        queueManager->PopFromVoq(0, 0, kPriority, 60);
        decision = hook->Evaluate(queueManager, 0, kPriority);
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              false,
                              "after draining below xoff, pause decision should clear");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              true,
                              "after draining below xon, resume decision should come from queue-manager state");
        Config::Reset();
    }
};

class UbQueueManagerPaperPfcDecisionApiTest : public TestCase
{
  public:
    UbQueueManagerPaperPfcDecisionApiTest()
        : TestCase("UnifiedBus - queue manager exposes unified paper PFC pause resume decisions")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 10000;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta", UintegerValue(8));
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC_PAPER,
                                                  /*reserveBytes*/ 0,
                                                  /*sharedPoolBytes*/ kSharedPoolBytes,
                                                  /*headroomPerPortBytes*/ 4096,
                                                  /*resumeGapBytes*/ 64,
                                                  /*alphaShift*/ 0,
                                                  /*hiThresh*/ 1000,
                                                  /*loThresh*/ 900);

        constexpr uint32_t kPriority = 1;
        Ptr<UbPfcPaperDynamicDecisionHook> hook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        const uint64_t xoff = hook->Evaluate(fixture.queueManager, 0, kPriority).xoff_threshold_bytes;
        fixture.queueManager->PushToVoq(0, 0, kPriority, static_cast<uint32_t>(xoff + 100));

        UbPfcDecision decision = hook->Evaluate(fixture.queueManager, 0, kPriority);
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              true,
                              "paper PFC pause decision should come from queue-manager state");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              false,
                              "paused paper-PFC queue should not immediately report resume");

        fixture.queueManager->PopFromVoq(0, 0, kPriority, static_cast<uint32_t>(xoff + 100));
        decision = hook->Evaluate(fixture.queueManager, 0, kPriority);
        NS_TEST_ASSERT_MSG_EQ(decision.pause,
                              false,
                              "after draining, paper PFC pause decision should clear");
        NS_TEST_ASSERT_MSG_EQ(decision.resume,
                              true,
                              "after draining, paper PFC resume decision should come from queue-manager state");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbControlFrameUsesDedicatedAccountingTest : public TestCase
{
  public:
    UbControlFrameUsesDedicatedAccountingTest()
        : TestCase("UnifiedBus - control frames keep dedicated occupancy accounting")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 1,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 1,
                                                    {});

        uint8_t credits[16] = {};
        credits[0] = 1;
        Ptr<Packet> controlPacket = UbDataLink::GenControlCreditPacket(credits);
        uint32_t controlBytes = controlPacket->GetSize();
        fixture.sw->SendControlFrame(controlPacket, 0);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressNonHeadroomBytes(0, 0),
                              0u,
                              "Control frames must not consume data ingress reserve/shared accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressSharedBytes(0, 0),
                              0u,
                              "Control frames must not consume shared-pool accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(0, 0),
                              0u,
                              "Control frames must not consume headroom accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetIngressControlBytes(0, 0),
                              controlBytes,
                              "Control frames should remain observable through dedicated ingress control accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetOutPortControlBytes(0, 0),
                              controlBytes,
                              "Control frames should remain observable through dedicated out-port control accounting");

        fixture.queueManager->PopFromVoq(0, 0, 0, controlBytes);
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetIngressControlBytes(0, 0),
                              0u,
                              "Control accounting should drain when the queued control frame departs");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetOutPortControlBytes(0, 0),
                              0u,
                              "Out-port control accounting should drain when the queued control frame departs");
        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
class UbDataPacketHeaderRejectsPriorityZeroTest : public TestCase
{
  public:
    UbDataPacketHeaderRejectsPriorityZeroTest()
        : TestCase("UnifiedBus - data packet headers reject priority 0 in this simulator model")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Ptr<Packet> packet = Create<Packet>(0);
            UbDataLink::GenPacketHeader(packet,
                                        false,
                                        false,
                                        0,
                                        0,
                                        false,
                                        false,
                                        UbDatalinkHeaderConfig::PACKET_IPV4);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Generating a data packet on priority 0 should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Generating a data packet on priority 0 should fail with SIGABRT");
    }
};

class UbSendControlFrameRejectsDataPacketTest : public TestCase
{
  public:
    UbSendControlFrameRejectsDataPacketTest()
        : TestCase("UnifiedBus - SendControlFrame rejects non-control packets")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Config::Reset();
            auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                        /*portsNum*/ 1,
                                                        /*reserveBytes*/ 0,
                                                        /*sharedPoolBytes*/ 0,
                                                        /*headroomPerPortBytes*/ 0,
                                                        /*resumeGapBytes*/ 16,
                                                        /*alphaShift*/ 1,
                                                        {});

            Ptr<Packet> dataPacket = Create<Packet>(0);
            UbDataLink::GenPacketHeader(dataPacket,
                                        false,
                                        false,
                                        1,
                                        1,
                                        false,
                                        false,
                                        UbDatalinkHeaderConfig::PACKET_IPV4);
            fixture.sw->SendControlFrame(dataPacket, 0);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "SendControlFrame should abort when called with a data packet");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "SendControlFrame misuse should fail with SIGABRT");
    }
};
#endif

class UbQueueManagerStickyHeadroomAccountingTest : public TestCase
{
  public:
    UbQueueManagerStickyHeadroomAccountingTest()
        : TestCase("UnifiedBus - sticky headroom accounts whole packets consistently")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 2,
                                                                    /*reserveBytes*/ 100,
                                                                    /*sharedPoolBytes*/ 50,
                                                                    /*headroomPerPortBytes*/ 256,
                                                                    /*resumeGapBytes*/ 16,
                                                                    /*alphaShift*/ 0);

        queueManager->PushToVoq(0, 0, 1, 180);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, 1),
                              0u,
                              "A crossing packet should enter headroom as a whole in the non-splitting model");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, 1),
                              180u,
                              "A crossing packet should be fully charged to headroom");

        queueManager->PushToVoq(0, 0, 1, 20);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, 1),
                              200u,
                              "Once a queue enters headroom, subsequent packets should stay sticky in headroom");

        queueManager->PopFromVoq(0, 0, 1, 20);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, 1),
                              180u,
                              "Dequeues should release sticky headroom bytes first in the chosen model");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, 1),
                              0u,
                              "Sticky headroom release should not fabricate non-headroom occupancy");

        queueManager->PopFromVoq(0, 0, 1, 180);
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressTotalBytes(0, 1),
                              0u,
                              "Draining the queue should release both headroom and non-headroom accounting");
        Config::Reset();
    }
};

class UbQueueManagerIngressPortOccupancyViewTest : public TestCase
{
  public:
    UbQueueManagerIngressPortOccupancyViewTest()
        : TestCase("UnifiedBus - queue manager exposes structured ingress port occupancy view")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPriorityA = 1;
        constexpr uint32_t kPriorityB = 2;
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 4,
                                                                    /*reserveBytes*/ 100,
                                                                    /*sharedPoolBytes*/ 0,
                                                                    /*headroomPerPortBytes*/ 256,
                                                                    /*resumeGapBytes*/ 16,
                                                                    /*alphaShift*/ 0);

        queueManager->PushToVoq(0, 0, kPriorityA, 80);
        queueManager->PushToVoq(0, 0, kPriorityB, 150);

        auto portOcc = queueManager->GetIngressPortOccupancy(0);
        NS_TEST_ASSERT_MSG_EQ(portOcc.non_headroom_bytes,
                              80u,
                              "port occupancy view should sum reserve/shared bytes across priorities");
        NS_TEST_ASSERT_MSG_EQ(portOcc.headroom_bytes,
                              150u,
                              "port occupancy view should sum headroom bytes across priorities");
        NS_TEST_ASSERT_MSG_EQ(portOcc.total_bytes,
                              230u,
                              "port occupancy total should match non-headroom plus headroom");

        Config::Reset();
    }
};

class UbPfcDynamicModeXoffZeroEmptyQueueTest : public TestCase
{
  public:
    UbPfcDynamicModeXoffZeroEmptyQueueTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC keeps empty queues resumed when xoff collapses to zero")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC,
                                                  /*reserveBytes*/ 100,
                                                  /*sharedPoolBytes*/ 1,
                                                  /*headroomPerPortBytes*/ 32,
                                                  /*resumeGapBytes*/ 16,
                                                  /*alphaShift*/ 1,
                                                  /*hiThresh*/ 1000,
                                                  /*loThresh*/ 900);

        Ptr<UbPfcDynamicDecisionHook> hook = CreateObject<UbPfcDynamicDecisionHook>();
        UbPfcDecision emptyDecision = hook->Evaluate(fixture.queueManager, 0, 1);
        NS_TEST_ASSERT_MSG_EQ(emptyDecision.xoff_threshold_bytes,
                              0u,
                              "This test requires xoff to collapse to zero");
        Ptr<Packet> pfcPacket = fixture.pfc->CheckPfcThreshold(Create<Packet>(1), 0);
        NS_TEST_ASSERT_MSG_EQ(pfcPacket,
                              nullptr,
                              "An empty queue must not send PFC when xoff is zero");
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetCredits(1),
                              UB_CREDIT_MAX_VALUE,
                              "An empty queue must remain resumed when xoff is zero");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcPaperDynamicModePauseResumeTest : public TestCase
{
  public:
    UbPfcPaperDynamicModePauseResumeTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC_PAPER follows paper threshold and 2-MTU resume gap")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 10000;
        constexpr uint32_t kPaperDynamicPfcBeta = 8;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta",
                           UintegerValue(kPaperDynamicPfcBeta));
        auto fixture = CreateSinglePortPfcFixture(FcType::PFC_DYNAMIC_PAPER,
                                                  /*reserveBytes*/ 0,
                                                  /*sharedPoolBytes*/ kSharedPoolBytes,
                                                  /*headroomPerPortBytes*/ 4096,
                                                  /*resumeGapBytes*/ 64,
                                                  /*alphaShift*/ 0,
                                                  /*hiThresh*/ 1000,
                                                  /*loThresh*/ 900);

        constexpr uint32_t kPaperPriorityCount = DEFAULT_PAPER_DYNAMIC_PFC_PRIORITY_COUNT;
        constexpr uint32_t kGlobalOccupancyBytes = 1000;
        Ptr<UbPfcPaperDynamicDecisionHook> hook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        fixture.queueManager->PushToVoq(0, 0, 1, kGlobalOccupancyBytes);
        UbPfcDecision decision = hook->Evaluate(fixture.queueManager, 0, 1);
        fixture.queueManager->PopFromVoq(0, 0, 1, kGlobalOccupancyBytes);
        uint64_t xoff = decision.xoff_threshold_bytes;
        uint64_t xon = decision.xon_threshold_bytes;

        NS_TEST_ASSERT_MSG_EQ(xoff,
                              static_cast<uint64_t>(kPaperDynamicPfcBeta) *
                                  (kSharedPoolBytes - kGlobalOccupancyBytes) / kPaperPriorityCount,
                              "PFC_DYNAMIC_PAPER xoff should follow beta * remaining / priorities");
        uint64_t expectedXon = xoff > 2u * UB_MTU_BYTE ? xoff - 2u * UB_MTU_BYTE : 0u;
        NS_TEST_ASSERT_MSG_EQ(xon,
                              expectedXon,
                              "PFC_DYNAMIC_PAPER xon should be xoff minus two MTUs");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest : public TestCase
{
  public:
    UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC_PAPER admission should not be limited by old dynamic alpha shift")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 20971520;
        constexpr uint32_t kHeadroomPerPortBytes = 65536;
        constexpr uint32_t kPriority = 7;
        constexpr uint32_t kPaperDynamicPfcBeta = 8;
        constexpr uint32_t kAlphaShift = 7;
        constexpr uint32_t kInPort = 0;
        constexpr uint32_t kOutPort = 1;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta",
                           UintegerValue(kPaperDynamicPfcBeta));
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_DYNAMIC_PAPER,
                                                    /*portsNum*/ 3,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ kSharedPoolBytes,
                                                    /*headroomPerPortBytes*/ kHeadroomPerPortBytes,
                                                    /*resumeGapBytes*/ 64,
                                                    /*alphaShift*/ kAlphaShift,
                                                    {{1000, 900}, {1000, 900}, {1000, 900}});

        Ptr<UbPfcDynamicDecisionHook> dynamicHook = CreateObject<UbPfcDynamicDecisionHook>();
        Ptr<UbPfcPaperDynamicDecisionHook> paperHook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        const uint64_t oldDynamicXoff = dynamicHook->Evaluate(fixture.queueManager, kInPort, kPriority).xoff_threshold_bytes;
        const uint64_t paperXoff = paperHook->Evaluate(fixture.queueManager, kInPort, kPriority).xoff_threshold_bytes;
        const uint32_t packetBytes = static_cast<uint32_t>(oldDynamicXoff) + 100;

        NS_TEST_ASSERT_MSG_LT(packetBytes,
                              paperXoff,
                              "test packet should stay below the paper pause threshold");

        fixture.queueManager->PushToVoq(kInPort, kOutPort, kPriority, packetBytes);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressHeadroomBytes(kInPort, kPriority),
                              0u,
                              "PFC_DYNAMIC_PAPER admission should not enter headroom just because old dynamic xoff is small");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              static_cast<uint64_t>(packetBytes),
                              "PFC_DYNAMIC_PAPER admission should keep the packet in ingress occupancy");

        auto pfc = DynamicCast<UbPfc>(fixture.ports[kInPort]->GetFlowControl());
        Ptr<Packet> pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), kInPort);
        NS_TEST_ASSERT_MSG_EQ(pfcPacket,
                              nullptr,
                              "PFC_DYNAMIC_PAPER should not pause before reaching the paper threshold");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kInPort]->GetCredits(kPriority),
                              UB_CREDIT_MAX_VALUE,
                              "PFC_DYNAMIC_PAPER should keep credits when still below the paper threshold");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest : public TestCase
{
  public:
    UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest()
        : TestCase("UnifiedBus - PFC_DYNAMIC_PAPER threshold tracks real switch global occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kSharedPoolBytes = 12000;
        constexpr uint32_t kHeadroomPerPortBytes = 4096;
        constexpr uint32_t kPaperDynamicPfcBeta = 8;
        constexpr uint32_t kPriority = 7;
        constexpr uint32_t kObservedPort = 0;
        constexpr uint32_t kOtherInPort = 1;
        constexpr uint32_t kOtherOutPort = 2;
        Config::SetDefault("ns3::UbQueueManager::PaperDynamicPfcBeta",
                           UintegerValue(kPaperDynamicPfcBeta));
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_DYNAMIC_PAPER,
                                                    /*portsNum*/ 3,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ kSharedPoolBytes,
                                                    /*headroomPerPortBytes*/ kHeadroomPerPortBytes,
                                                    /*resumeGapBytes*/ 64,
                                                    /*alphaShift*/ 0,
                                                    {{1000, 900}, {1000, 900}, {1000, 900}});

        Ptr<UbPfcPaperDynamicDecisionHook> paperHook = CreateObject<UbPfcPaperDynamicDecisionHook>();
        const uint64_t thresholdWithoutLoad =
            paperHook->Evaluate(fixture.queueManager, kObservedPort, kPriority).xoff_threshold_bytes;
        fixture.queueManager->PushToVoq(kOtherInPort, kOtherOutPort, kPriority, 3000);
        const uint64_t measuredGlobalOccupancy =
            fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes;
        const uint64_t thresholdWithLoad =
            paperHook->Evaluate(fixture.queueManager, kObservedPort, kPriority).xoff_threshold_bytes;

        NS_TEST_ASSERT_MSG_EQ(measuredGlobalOccupancy,
                              3000u,
                              "paper dynamic occupancy should include bytes queued on other ports");
        NS_TEST_ASSERT_MSG_LT(thresholdWithLoad,
                              thresholdWithoutLoad,
                              "paper dynamic threshold should shrink when global occupancy increases");

        auto pfc = DynamicCast<UbPfc>(fixture.ports[kObservedPort]->GetFlowControl());
        constexpr uint32_t kObservedBytesBelowPause = 4000;
        fixture.queueManager->PushToVoq(kObservedPort,
                                        kObservedPort,
                                        kPriority,
                                        kObservedBytesBelowPause);
        Ptr<Packet> pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), kObservedPort);
        NS_TEST_ASSERT_MSG_EQ(pfcPacket,
                              nullptr,
                              "queue below the self-consistent paper dynamic boundary should stay resumed");

        fixture.queueManager->PushToVoq(kObservedPort, kObservedPort, kPriority, 600);
        pfcPacket = pfc->CheckPfcThreshold(Create<Packet>(1), kObservedPort);
        NS_TEST_ASSERT_MSG_NE(pfcPacket,
                              nullptr,
                              "queue crossing the self-consistent paper dynamic boundary should emit PFC");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbQueueManagerTotalBufferedBytesTracksEgressTransferTest : public TestCase
{
  public:
    UbQueueManagerTotalBufferedBytesTracksEgressTransferTest()
        : TestCase("UnifiedBus - total buffered bytes keeps VOQ and egress occupancy consistent")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kPacketBytes = 512;
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ kPacketBytes,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 64,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              static_cast<uint64_t>(kPacketBytes),
                              "VOQ enqueue should contribute to total buffered bytes");

        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              static_cast<uint64_t>(kPacketBytes),
                              "moving a packet from VOQ to egress should preserve total buffered bytes");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kEgressPort]->GetUbQueue()->GetCurrentBytes(),
                              static_cast<uint64_t>(kPacketBytes),
                              "packet should now reside in the egress queue");

        fixture.ports[kEgressPort]->SetFaultCallBack(MakeCallback(&ConsumeEgressPacketForTest));
        fixture.ports[kEgressPort]->NotifyLinkUp();
        fixture.ports[kEgressPort]->TriggerTransmit();
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              0u,
                              "egress dequeue should release total buffered bytes");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbQueueManagerInPortProcessingAccountingTest : public TestCase
{
  public:
    UbQueueManagerInPortProcessingAccountingTest()
        : TestCase("UnifiedBus - in-port processing bytes are visible before VOQ")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto queueManager = CreateQueueManagerFixture(/*ports*/ 2,
                                                      /*vlNum*/ 2,
                                                      /*reserveBytes*/ 512,
                                                      /*sharedPoolBytes*/ 0,
                                                      /*headroomPerPortBytes*/ 0,
                                                      /*resumeGapBytes*/ 16,
                                                      /*alphaShift*/ 0);

        constexpr uint32_t kInPort = 0;
        constexpr uint32_t kOutPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        queueManager->PushToInPortProcessing(kInPort, kOutPort, kPriority, kPacketBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(kInPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should consume ingress admission exactly once");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should count as ingress occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetPortIngressNonHeadroomBytes(kInPort),
                              static_cast<uint64_t>(kPacketBytes),
                              "port ingress occupancy should include processing bytes consistently");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetOutPortBufferUsed(kOutPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should count as route-known out-port load");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetTotalOutPortBufferUsed(kOutPort),
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should contribute to total out-port load");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should count in switch total occupancy");

        queueManager->MoveInPortProcessingToVoq(kInPort, kOutPort, kPriority, kPacketBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(kInPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "moving to VOQ should preserve single-count ingress admission");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "moving to VOQ should preserve ingress occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetPortIngressNonHeadroomBytes(kInPort),
                              static_cast<uint64_t>(kPacketBytes),
                              "moving to VOQ should preserve port ingress occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetOutPortBufferUsed(kOutPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "moving to VOQ should preserve out-port occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              static_cast<uint64_t>(kPacketBytes),
                              "moving to VOQ should preserve switch total occupancy");

        queueManager->PopFromVoq(kInPort, kOutPort, kPriority, kPacketBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(kInPort, kPriority),
                              0u,
                              "VOQ pop should release ingress admission");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              0u,
                              "VOQ pop should release ingress occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetPortIngressNonHeadroomBytes(kInPort),
                              0u,
                              "VOQ pop should release port ingress occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetTotalOutPortBufferUsed(kOutPort),
                              0u,
                              "VOQ pop should release out-port occupancy");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetSwitchBufferOccupancy().total_buffered_bytes,
                              0u,
                              "VOQ pop should release total occupancy");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchInPortProcessingDelayHidesPacketFromAllocatorTest : public TestCase
{
  public:
    UbSwitchInPortProcessingDelayHidesPacketFromAllocatorTest()
        : TestCase("UnifiedBus - UbSwitch InPortProcessingDelay hides forwarded packets from allocator")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});
        fixture.sw->SetAttribute("InPortProcessingDelay", TimeValue(MicroSeconds(3)));

        constexpr uint32_t kInPort = 0;
        constexpr uint32_t kOutPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kInPort, kOutPort, kPriority);

        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should count as ingress buffered bytes immediately");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetOutPortBufferUsed(kOutPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "processing bytes should count in route-known outPort load immediately");
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kOutPort]);
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kOutPort]->GetUbQueue()->IsEmpty(),
                              true,
                              "allocator should not see forwarded packet before InPortProcessingDelay expires");

        Simulator::Stop(MicroSeconds(2));
        Simulator::Run();
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kOutPort]);
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kOutPort]->GetUbQueue()->IsEmpty(),
                              true,
                              "allocator should still not see forwarded packet before the delay expires");

        Simulator::Stop(MicroSeconds(4));
        Simulator::Run();
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kOutPort]);
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kOutPort]->GetUbQueue()->GetCurrentBytes(),
                              static_cast<uint64_t>(kPacketBytes),
                              "allocator should move packet to egress after processing completes");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kInPort, kPriority),
                              0u,
                              "allocator dequeue should release ingress accounting after VOQ visibility");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchInPortProcessingDelayDoesNotDelayTpSourceQueueTest : public TestCase
{
  public:
    UbSwitchInPortProcessingDelayDoesNotDelayTpSourceQueueTest()
        : TestCase("UnifiedBus - UbSwitch InPortProcessingDelay does not delay TP source queues")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 1,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});
        fixture.sw->SetAttribute("InPortProcessingDelay", TimeValue(MicroSeconds(10)));

        constexpr uint32_t kPort = 0;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        Ptr<UbPacketQueue> tpQueue = CreateObject<UbPacketQueue>();
        tpQueue->SetInPortId(kPort);
        tpQueue->SetOutPortId(kPort);
        tpQueue->SetIngressPriority(kPriority);
        tpQueue->Push(Create<Packet>(kPacketBytes));
        fixture.sw->RegisterTpWithAllocator(tpQueue, kPort, kPriority);

        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kPort]);

        NS_TEST_ASSERT_MSG_EQ(tpQueue->IsEmpty(),
                              true,
                              "TP source packet should bypass in-port processing delay");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kPort]->GetUbQueue()->GetCurrentBytes(),
                              static_cast<uint64_t>(kPacketBytes),
                              "TP source packet should enter egress immediately");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kPort, kPriority),
                              0u,
                              "TP source dequeue should not create processing ingress accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetTotalOutPortBufferUsed(kPort),
                              0u,
                              "TP source dequeue should not create processing outPort accounting");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchZeroDelayPreservesLegacyEnqueueOrderingTest : public TestCase
{
  public:
    UbSwitchZeroDelayPreservesLegacyEnqueueOrderingTest()
        : TestCase("UnifiedBus - zero processing delay preserves enqueue callback ordering")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_FIXED,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});
        fixture.sw->SetAttribute("InPortProcessingDelay", TimeValue(NanoSeconds(0)));

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        std::vector<std::string> eventLog;
        Ptr<UbRecordingCongestionControl> cc = CreateObject<UbRecordingCongestionControl>();
        cc->SetEventLog(&eventLog);
        fixture.sw->SetCongestionCtrl(cc);

        Ptr<UbRecordingFlowControl> ingressFc = CreateObject<UbRecordingFlowControl>();
        ingressFc->SetEventLog(&eventLog);
        fixture.ports[kIngressPort]->m_flowControl = ingressFc;

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);

        NS_TEST_ASSERT_MSG_EQ(eventLog.size(),
                              2u,
                              "zero-delay SendPacket should finish enqueue callbacks before any allocator work");
        NS_TEST_ASSERT_MSG_EQ(eventLog.at(0),
                              std::string("congestion-enqueue"),
                              "congestion callback should run before ingress flow-control");
        NS_TEST_ASSERT_MSG_EQ(eventLog.at(1),
                              std::string("flow-control-enqueue"),
                              "ingress flow-control callback should run after congestion enqueue");
        NS_TEST_ASSERT_MSG_EQ(cc->m_called,
                              true,
                              "zero-delay forwarding should still notify congestion control after enqueue");
        NS_TEST_ASSERT_MSG_EQ(ingressFc->m_called,
                              true,
                              "zero-delay PFC forwarding should still notify ingress flow control after enqueue");
        NS_TEST_ASSERT_MSG_EQ(ingressFc->m_contextInPortId,
                              kIngressPort,
                              "ingress flow-control callback should receive ingress port");
        NS_TEST_ASSERT_MSG_EQ(ingressFc->m_contextOutPortId,
                              kEgressPort,
                              "ingress flow-control callback should receive egress port");
        NS_TEST_ASSERT_MSG_EQ(ingressFc->m_contextPriority,
                              kPriority,
                              "ingress flow-control callback should receive priority");
        NS_TEST_ASSERT_MSG_EQ(fixture.sw->GetVoqPacketCountForTest(kEgressPort, kPriority, kIngressPort),
                              1u,
                              "zero-delay SendPacket should make the packet allocator-visible immediately");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kEgressPort]->GetUbQueue()->GetCurrentBytes(),
                              0u,
                              "zero-delay SendPacket should not bypass allocator into egress");

        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kEgressPort]->GetUbQueue()->GetCurrentBytes(),
                              static_cast<uint64_t>(kPacketBytes),
                              "allocator should move the now-visible packet into egress after enqueue callbacks");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchControlFramesBypassProcessingDelayTest : public TestCase
{
  public:
    UbSwitchControlFramesBypassProcessingDelayTest()
        : TestCase("UnifiedBus - control frames bypass switch processing delay")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 1,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 1,
                                                    {});
        fixture.sw->SetAttribute("InPortProcessingDelay", TimeValue(MicroSeconds(5)));

        uint8_t credits[16] = {};
        credits[0] = 1;
        Ptr<Packet> controlPacket = UbDataLink::GenControlCreditPacket(credits);
        const uint32_t controlBytes = controlPacket->GetSize();

        fixture.sw->SendControlFrame(controlPacket, 0);

        NS_TEST_ASSERT_MSG_EQ(fixture.sw->GetVoqPacketCountForTest(0, 0, 0),
                              1u,
                              "control frames should enter VOQ immediately even when data processing delay is set");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetIngressControlBytes(0, 0),
                              controlBytes,
                              "control frames should remain in dedicated ingress-control accounting");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(0, 0),
                              0u,
                              "control frames must not create in-port processing ingress occupancy");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetOutPortBufferUsed(0, 0),
                              0u,
                              "control frames must not consume data out-port accounting");

        Simulator::Stop(NanoSeconds(1));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_EQ(fixture.sw->GetVoqPacketCountForTest(0, 0, 0),
                              1u,
                              "control frame visibility should not wait for the data processing delay timer");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPfcForwardingUsesIngressPortConfigTest : public TestCase
{
  public:
    UbPfcForwardingUsesIngressPortConfigTest()
        : TestCase("UnifiedBus - forwarding path uses the ingress port's PFC configuration")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::PFC_FIXED,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 256,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {{100, 40}, {1000, 900}});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        fixture.queueManager->PushToVoq(kIngressPort, kEgressPort, kPriority, 120);

        Ptr<UbPfc> ingressFlowControl = DynamicCast<UbPfc>(fixture.ports[kIngressPort]->GetFlowControl());
        ingressFlowControl->OnIngressReleased({
            .packet = Create<Packet>(120),
            .ingressQueue = nullptr,
            .inPortId = kIngressPort,
            .outPortId = kEgressPort,
            .priority = kPriority,
        });

        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kIngressPort]->GetCredits(kPriority),
                              0u,
                              "Forwarding-triggered PFC checks must use the congested ingress port's thresholds");
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbFlowControlReleaseHookRunsAfterIngressDequeueTest : public TestCase
{
  public:
    UbFlowControlReleaseHookRunsAfterIngressDequeueTest()
        : TestCase("UnifiedBus - flow-control release hook observes post-dequeue ingress occupancy")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        Ptr<UbObservingFlowControl> observingFc = CreateObject<UbObservingFlowControl>();
        observingFc->Configure(fixture.queueManager, kIngressPort, kPriority);
        fixture.ports[kIngressPort]->m_flowControl = observingFc;

        // Keep the port busy so SendPacket/AllocateNextPacket won't try to hit a null channel.
        fixture.ports[kEgressPort]->SetSendState(SendState::BUSY);

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);

        NS_TEST_ASSERT_MSG_EQ(observingFc->m_called,
                              true,
                              "allocator path should trigger flow-control release hook for forwarded packets");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_observedIngressBytes,
                              0u,
                              "release hook should observe ingress occupancy after queue-manager dequeue");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_contextInPortId,
                              kIngressPort,
                              "release hook should receive ingress port through event context");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_contextOutPortId,
                              kEgressPort,
                              "release hook should receive egress port through event context");
        NS_TEST_ASSERT_MSG_EQ(observingFc->m_contextPriority,
                              kPriority,
                              "release hook should receive priority through event context");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest : public TestCase
{
  public:
    UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest()
        : TestCase("UnifiedBus - allocator keeps ingress packet when egress queue is full")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::NONE,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 256,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kPacketBytes = 120;

        Ptr<UbObservingFlowControl> observingFc = CreateObject<UbObservingFlowControl>();
        observingFc->Configure(fixture.queueManager, kIngressPort, kPriority);
        fixture.ports[kEgressPort]->m_flowControl = observingFc;
        fixture.ports[kEgressPort]->GetUbQueue()->SetAttribute("MaxEgressBytes", UintegerValue(0));

        fixture.sw->SendPacket(Create<Packet>(kPacketBytes), kIngressPort, kEgressPort, kPriority);
        fixture.sw->GetAllocator()->AllocateNextPacket(fixture.ports[kEgressPort]);

        NS_TEST_ASSERT_MSG_EQ(observingFc->m_called,
                              false,
                              "release hook must not run when the packet never entered the egress queue");
        NS_TEST_ASSERT_MSG_EQ(fixture.queueManager->GetQueueIngressTotalBytes(kIngressPort, kPriority),
                              static_cast<uint64_t>(kPacketBytes),
                              "allocator must keep ingress accounting when egress queue rejects the packet");
        NS_TEST_ASSERT_MSG_EQ(fixture.ports[kEgressPort]->GetUbQueue()->GetCurrentBytes(),
                              0u,
                              "egress queue occupancy should remain unchanged after a rejected enqueue");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
class UbCbfcRejectsZeroCellGeometryTest : public TestCase
{
  public:
    UbCbfcRejectsZeroCellGeometryTest()
        : TestCase("UnifiedBus - CBFC rejects zero cell geometry at init")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Config::Reset();
            Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
            Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(0));

            Ptr<Node> node = CreateObject<Node>();
            Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
            node->AggregateObject(sw);
            node->AddDevice(CreateObject<UbPort>());
            sw->Init();
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Invalid CBFC cell geometry should abort during initialization");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Invalid CBFC cell geometry should fail with SIGABRT");
        Config::Reset();
    }
};

class UbPfcFixedRejectsNegativeThresholdTest : public TestCase
{
  public:
    UbPfcFixedRejectsNegativeThresholdTest()
        : TestCase("UnifiedBus - PFC_FIXED rejects negative thresholds at init")
    {
    }

    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Config::Reset();
            Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::PFC_FIXED));
            Config::SetDefault("ns3::UbPort::PfcUpThld", IntegerValue(-1));
            Config::SetDefault("ns3::UbPort::PfcLowThld", IntegerValue(-2));

            Ptr<Node> node = CreateObject<Node>();
            Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
            node->AggregateObject(sw);
            node->AddDevice(CreateObject<UbPort>());
            sw->Init();
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Negative PFC_FIXED thresholds should abort during initialization");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Negative PFC_FIXED thresholds should fail with SIGABRT");
        Config::Reset();
    }
};

class UbSwitchLocalCbfcConfigTest : public TestCase
{
  public:
    UbSwitchLocalCbfcConfigTest()
        : TestCase("UnifiedBus - UbSwitch local CBFC config overrides global defaults")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(1000));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        sw->SetCbfcCellGeometry(/*flitLenBytes*/ 10, /*flitsPerCell*/ 2);
        sw->SetCbfcReturnCellGrain(/*dataPacketCells*/ 4, /*controlPacketCells*/ 2);
        sw->SetCbfcCredits(/*initCreditCells*/ 5, /*sharedInitCreditCells*/ 0);
        node->AggregateObject(sw);

        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfc> cbfc = DynamicCast<UbCbfc>(port->GetFlowControl());
        NS_TEST_ASSERT_MSG_NE(cbfc, nullptr, "port should initialize CBFC flow control");

        constexpr uint32_t kPriority = 1;
        Ptr<UbPacketQueue> ingressQ = CreateObject<UbPacketQueue>();
        ingressQ->SetIngressPriority(kPriority);
        ingressQ->SetOutPortId(0);
        ingressQ->SetInPortId(0);
        ingressQ->Push(Create<Packet>(101));

        NS_TEST_ASSERT_MSG_EQ(cbfc->IsFcLimited(ingressQ),
                              true,
                              "Switch-local CBFC cell geometry and credits should override global defaults");

        Simulator::Destroy();
        Config::Reset();
    }
};
#endif

class UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest : public TestCase
{
  public:
    UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest()
        : TestCase("UnifiedBus - CBFC piggyback target VL can differ from packet VL")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        constexpr uint8_t kTargetVl = 5;
        for (int i = 0; i < 4; ++i)
        {
            probe->SetCrdToReturn(kTargetVl, 1, port);
        }

        Ptr<Packet> packet = Create<Packet>(256);
        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    false,
                                    /*crdVl*/ 1,
                                    /*pktVl*/ 2,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);
        Ptr<UbPacketQueue> ingressQueue = CreateObject<UbPacketQueue>();
        ingressQueue->SetIngressPriority(2);
        ingressQueue->SetInPortId(1);
        ingressQueue->SetOutPortId(0);

        probe->OnEgressEnqueued({
            .packet = packet,
            .ingressQueue = ingressQueue,
            .inPortId = 0,
            .outPortId = 0,
            .priority = 2,
        });

        UbDatalinkPacketHeader header;
        packet->PeekHeader(header);
        NS_TEST_ASSERT_MSG_EQ(header.GetCredit(),
                              true,
                              "eligible data packet should carry piggyback credit");
        NS_TEST_ASSERT_MSG_EQ(header.GetCreditTargetVL(),
                              kTargetVl,
                              "piggyback should target the selected pending-return VL, not packet VL");
        NS_TEST_ASSERT_MSG_EQ(header.GetPacketVL(),
                              2u,
                              "piggyback must not alter the packet's own VL");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(kTargetVl),
                              0,
                              "one data-grain worth of pending cells should be consumed by piggyback");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcPiggybackRestoreClearsLocalCreditBitTest : public TestCase
{
  public:
    UbCbfcPiggybackRestoreClearsLocalCreditBitTest()
        : TestCase("UnifiedBus - CBFC piggyback restore is consumed locally and clears header state")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateMultiPortSwitchFixture(FcType::CBFC,
                                                    /*portsNum*/ 1,
                                                    /*reserveBytes*/ 0,
                                                    /*sharedPoolBytes*/ 0,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 1,
                                                    {});

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 10, /*forceThreshold*/ 64, fixture.node->GetId(), fixture.ports[0]->GetIfIndex());
        fixture.ports[0]->m_flowControl = probe;

        const int32_t before = probe->GetTxFreeCells(6);
        Ptr<Packet> packet = Create<Packet>(128);
        UbDataLink::GenPacketHeader(packet,
                                    true,
                                    false,
                                    /*crdVl*/ 6,
                                    /*pktVl*/ 3,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);

        probe->OnDataPacketReceived(packet);

        UbDatalinkPacketHeader header;
        packet->PeekHeader(header);
        NS_TEST_ASSERT_MSG_EQ(header.GetCredit(),
                              false,
                              "piggyback credit flag should be cleared after the local hop consumes it");
        NS_TEST_ASSERT_MSG_EQ(header.GetCreditTargetVL(),
                              0u,
                              "piggyback target VL should be cleared after local consumption");
        NS_TEST_ASSERT_MSG_EQ(probe->GetTxFreeCells(6),
                              before + 4,
                              "piggyback restore should add one data-grain worth of cells to the target VL");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcControlFrameFallbackWithoutDataPacketTest : public TestCase
{
  public:
    UbCbfcControlFrameFallbackWithoutDataPacketTest()
        : TestCase("UnifiedBus - CBFC does not emit control frame below ctrl-credit-return threshold without data packet")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(4, 1, port);
        Ptr<Packet> fallback = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_EQ(fallback,
                              nullptr,
                              "below the ctrl-credit-return threshold, pending credit should stay queued instead of generating a control frame");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest : public TestCase
{
  public:
    UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest()
        : TestCase("UnifiedBus - CBFC switches to control-frame credit return once pending threshold is reached")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(6, 63, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              false,
                              "pending below threshold should not switch to control-frame credit return yet");

        probe->SetCrdToReturn(6, 1, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "pending reaching threshold should switch to control-frame credit return");

        Ptr<Packet> forced = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_NE(forced,
                              nullptr,
                              "the ctrl-credit-return threshold should immediately generate a control frame");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(6),
                              63u,
                              "control-frame credit return must still respect the 6-bit credit-field limit per VL");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              1,
                              "one cell should remain pending after the first control-return frame when 64 cells are queued");

        UbDatalinkHeader dlHeader;
        forced->PeekHeader(dlHeader);
        NS_TEST_ASSERT_MSG_EQ(dlHeader.IsControlCreditHeader(),
                              true,
                              "ctrl-credit-return should use a real control credit frame");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForcedControlReturnFlushesAllEligibleVlsTest : public TestCase
{
  public:
    UbCbfcForcedControlReturnFlushesAllEligibleVlsTest()
        : TestCase("UnifiedBus - CBFC ctrl-credit-return flushes every VL already eligible for control return")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(6, 64, port);
        probe->SetCrdToReturn(3, 4, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "one VL reaching threshold should trigger the immediate ctrl-credit-return path");

        Ptr<Packet> forced = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_NE(forced,
                              nullptr,
                              "ctrl-credit-return path should build a control frame when threshold is hit");

        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              1,
                              "the triggering VL should retain only the residual cells left after one 63-grain control frame");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(3),
                              0,
                              "ctrl-credit-return should also flush other VLs already eligible for control return");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(6),
                              63u,
                              "control frame should clamp the triggering VL to the 6-bit credit-field limit");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(3),
                              4u,
                              "control frame should carry other eligible VL credits in the same packet");

        UbDatalinkHeader dlHeader;
        forced->PeekHeader(dlHeader);
        NS_TEST_ASSERT_MSG_EQ(dlHeader.IsControlCreditHeader(),
                              true,
                              "mixed-VL ctrl-credit-return should still use a real control credit frame");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest : public TestCase
{
  public:
    UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest()
        : TestCase("UnifiedBus - CBFC ctrl-credit-return chunks large pending credit into multiple control frames")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> port = CreateObject<UbPort>();
        node->AddDevice(port);
        sw->Init();

        Ptr<UbCbfcProbe> probe = CreateObject<UbCbfcProbe>();
        probe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                    /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), port->GetIfIndex());
        port->m_flowControl = probe;

        probe->SetCrdToReturn(6, 130, port);
        NS_TEST_ASSERT_MSG_EQ(probe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "pending above threshold should enter ctrl-credit-return mode");

        Ptr<Packet> first = probe->MaybeBuildControlReturnPacketForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_NE(first,
                              nullptr,
                              "large pending credit should still build a first control frame");
        NS_TEST_ASSERT_MSG_EQ(port->GetCredits(6),
                              63u,
                              "a single control frame must clamp one VL to the 6-bit credit-field limit");
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              67,
                              "only the actually encoded 63 cells should be deducted from pending credit");

        probe->MaybeQueueControlReturnForTest(port->GetIfIndex());
        NS_TEST_ASSERT_MSG_EQ(probe->GetPendingCells(6),
                              0,
                              "batch control-return enqueue should drain the remaining pending credit");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest : public TestCase
{
  public:
    UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest()
        : TestCase("UnifiedBus - CBFC forwarded release checks threshold state on the ingress port instance")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        Ptr<Node> node = CreateObject<Node>();
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<UbPort> ingressPort = CreateObject<UbPort>();
        Ptr<UbPort> egressPort = CreateObject<UbPort>();
        node->AddDevice(ingressPort);
        node->AddDevice(egressPort);
        sw->Init();

        Ptr<UbCbfcProbe> ingressProbe = CreateObject<UbCbfcProbe>();
        ingressProbe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                           /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), ingressPort->GetIfIndex());
        ingressPort->m_flowControl = ingressProbe;

        Ptr<UbCbfcProbe> egressProbe = CreateObject<UbCbfcProbe>();
        egressProbe->Init(/*flitLen*/ 20, /*flitsPerCell*/ 8, /*retData*/ 4, /*retCtrl*/ 1,
                          /*initCredit*/ 100, /*forceThreshold*/ 64, node->GetId(), egressPort->GetIfIndex());
        egressPort->m_flowControl = egressProbe;

        ingressProbe->SetCrdToReturn(5, 64, ingressPort);
        NS_TEST_ASSERT_MSG_EQ(ingressProbe->ShouldUseCtrlCrdRtrForTest(),
                              true,
                              "ingress-side flow control should see the ctrl-credit-return threshold reached");
        NS_TEST_ASSERT_MSG_EQ(egressProbe->ShouldUseCtrlCrdRtrForTest(),
                              false,
                              "egress-side flow control should stay below the ctrl-credit-return threshold in this setup");

        Ptr<Packet> packet = Create<Packet>(128);
        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    false,
                                    /*crdVl*/ 1,
                                    /*pktVl*/ 5,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);
        Ptr<UbPacketQueue> ingressQueue = CreateObject<UbPacketQueue>();
        ingressQueue->SetIngressPriority(5);
        ingressQueue->SetInPortId(ingressPort->GetIfIndex());
        ingressQueue->SetOutPortId(egressPort->GetIfIndex());

        ingressProbe->OnIngressReleased({
            .packet = packet,
            .ingressQueue = ingressQueue,
            .inPortId = ingressPort->GetIfIndex(),
            .outPortId = egressPort->GetIfIndex(),
            .priority = 5,
        });

        NS_TEST_ASSERT_MSG_LT(ingressProbe->GetPendingCells(5),
                              64,
                              "forwarded release should drain pending credits on the ingress-port flow-control instance");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCbfcForwardedIngressEnqueueDoesNotAccumulateReturnCreditTest : public TestCase
{
  public:
    UbCbfcForwardedIngressEnqueueDoesNotAccumulateReturnCreditTest()
        : TestCase("UnifiedBus - CBFC forwarded ingress enqueue does not accumulate return credit")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::CBFC));
        Config::SetDefault("ns3::UbPort::CbfcFlitLenByte", UintegerValue(20));
        Config::SetDefault("ns3::UbPort::CbfcFlitsPerCell", UintegerValue(8));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainDataPacket", UintegerValue(4));
        Config::SetDefault("ns3::UbPort::CbfcRetCellGrainControlPacket", UintegerValue(1));
        Config::SetDefault("ns3::UbPort::CbfcInitCreditCell", IntegerValue(100));
        Config::SetDefault("ns3::UbPort::CbfcCtrlCrdRtrThldCell", IntegerValue(64));

        auto fixture = CreateMultiPortSwitchFixture(FcType::CBFC,
                                                    /*portsNum*/ 2,
                                                    /*reserveBytes*/ 4096,
                                                    /*sharedPoolBytes*/ 4096,
                                                    /*headroomPerPortBytes*/ 0,
                                                    /*resumeGapBytes*/ 16,
                                                    /*alphaShift*/ 0,
                                                    {});

        constexpr uint32_t kIngressPort = 0;
        constexpr uint32_t kEgressPort = 1;
        constexpr uint32_t kPriority = 5;
        constexpr uint32_t kPayloadBytes = 128;

        Ptr<UbCbfcProbe> ingressProbe = CreateObject<UbCbfcProbe>();
        ingressProbe->Init(/*flitLen*/ 20,
                           /*flitsPerCell*/ 8,
                           /*retData*/ 4,
                           /*retCtrl*/ 1,
                           /*initCredit*/ 100,
                           /*forceThreshold*/ 64,
                           fixture.node->GetId(),
                           fixture.ports[kIngressPort]->GetIfIndex());
        fixture.ports[kIngressPort]->m_flowControl = ingressProbe;

        Ptr<Packet> packet = Create<Packet>(kPayloadBytes);
        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    false,
                                    /*crdVl*/ kPriority,
                                    /*pktVl*/ kPriority,
                                    false,
                                    false,
                                    UbDatalinkHeaderConfig::PACKET_IPV4);

        fixture.sw->SendPacket(packet, kIngressPort, kEgressPort, kPriority);

        NS_TEST_ASSERT_MSG_EQ(
            ingressProbe->GetPendingCells(kPriority),
            0,
            "forwarded data should return CBFC credit when ingress is released, not when it is enqueued");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifdef NS3_MPI
class UbCreateTopoRemoteLinkTest : public TestCase
{
  public:
    UbCreateTopoRemoteLinkTest()
        : TestCase("UnifiedBus - CreateTopo builds remote link across systemId")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-remote-link-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path nodePath = caseDir / "node.csv";
        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;

        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << node0Id << ",DEVICE,1,1ns,0\n";
        nodeFile << node1Id << ",DEVICE,1,1ns,1\n";
        nodeFile.close();

        fs::path topoPath = caseDir / "topology.csv";
        std::ofstream topoFile(topoPath.string());
        topoFile << "node1,port1,node2,port2,bandwidth,delay\n";
        topoFile << node0Id << ",0," << node1Id << ",0,400Gbps,10ns\n";
        topoFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTopo(topoPath.string());

        NS_TEST_ASSERT_MSG_EQ(NodeList::GetNNodes(), beforeNodes + 2, "CreateNode should create 2 nodes");

        Ptr<Node> n0 = NodeList::GetNode(beforeNodes);
        Ptr<Node> n1 = NodeList::GetNode(beforeNodes + 1);
        Ptr<UbPort> p0 = DynamicCast<UbPort>(n0->GetDevice(0));
        Ptr<UbPort> p1 = DynamicCast<UbPort>(n1->GetDevice(0));
        Ptr<Channel> channel = p0->GetChannel();

        NS_TEST_ASSERT_MSG_NE(channel, nullptr, "Port channel should be created");
        NS_TEST_ASSERT_MSG_EQ(channel->GetInstanceTypeId().GetName(), std::string("ns3::UbRemoteLink"),
                              "Cross-systemId topology should use UbRemoteLink");
        NS_TEST_ASSERT_MSG_EQ(p0->HasMpiReceive(), true, "Remote link endpoint should enable MPI receive");
        NS_TEST_ASSERT_MSG_EQ(p1->HasMpiReceive(), true, "Remote link endpoint should enable MPI receive");
        NS_TEST_ASSERT_MSG_EQ(p1->GetChannel(), p0->GetChannel(), "Both ports should share the same link");

        fs::remove_all(caseDir, ec);
    }
};
#endif

#if defined(NS3_MPI) && defined(NS3_MTP)
class UbCreateTopoPackedSystemIdLocalLinkTest : public TestCase
{
  public:
    UbCreateTopoPackedSystemIdLocalLinkTest()
        : TestCase("UnifiedBus - CreateTopo keeps same-rank packed systemId on local link")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-packed-local-link-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;
        const uint32_t node0SystemId = (0x0001u << 16) | 0x0009u;
        const uint32_t node1SystemId = (0x0002u << 16) | 0x0009u;

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << node0Id << ",DEVICE,1,1ns," << node0SystemId << "\n";
        nodeFile << node1Id << ",DEVICE,1,1ns," << node1SystemId << "\n";
        nodeFile.close();

        fs::path topoPath = caseDir / "topology.csv";
        std::ofstream topoFile(topoPath.string());
        topoFile << "node1,port1,node2,port2,bandwidth,delay\n";
        topoFile << node0Id << ",0," << node1Id << ",0,400Gbps,10ns\n";
        topoFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTopo(topoPath.string());

        Ptr<Node> n0 = NodeList::GetNode(beforeNodes);
        Ptr<Node> n1 = NodeList::GetNode(beforeNodes + 1);
        Ptr<UbPort> p0 = DynamicCast<UbPort>(n0->GetDevice(0));
        Ptr<UbPort> p1 = DynamicCast<UbPort>(n1->GetDevice(0));
        Ptr<Channel> channel = p0->GetChannel();

        NS_TEST_ASSERT_MSG_NE(channel, nullptr, "Port channel should be created");
        NS_TEST_ASSERT_MSG_EQ(channel->GetInstanceTypeId().GetName(), std::string("ns3::UbLink"),
                              "Same MPI rank packed systemId should keep a local UbLink");
        NS_TEST_ASSERT_MSG_EQ(p0->HasMpiReceive(), false,
                              "Local link should not enable MPI receive on the first endpoint");
        NS_TEST_ASSERT_MSG_EQ(p1->HasMpiReceive(), false,
                              "Local link should not enable MPI receive on the second endpoint");
        NS_TEST_ASSERT_MSG_EQ(p1->GetChannel(), p0->GetChannel(), "Both ports should share the same local link");

        fs::remove_all(caseDir, ec);
    }
};
#endif

class UbCreateTpPreloadInstancesTest : public TestCase
{
  public:
    UbCreateTpPreloadInstancesTest()
        : TestCase("UnifiedBus - CreateTp preloads TP instances from config")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const uint32_t beforeNodes = NodeList::GetNNodes();
        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-create-tp-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        const uint32_t node0Id = beforeNodes;
        const uint32_t node1Id = beforeNodes + 1;

        fs::path nodePath = caseDir / "node.csv";
        std::ofstream nodeFile(nodePath.string());
        nodeFile << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
        nodeFile << node0Id << ",DEVICE,1,1ns,0\n";
        nodeFile << node1Id << ",DEVICE,1,1ns,1\n";
        nodeFile.close();

        fs::path tpPath = caseDir / "transport_channel.csv";
        std::ofstream tpFile(tpPath.string());
        tpFile << "nodeId1,portId1,tpn1,nodeId2,portId2,tpn2,priority,metric\n";
        tpFile << node0Id << ",0,11," << node1Id << ",0,22,7,1\n";
        tpFile.close();

        utils::UbUtils::Get()->CreateNode(nodePath.string());
        utils::UbUtils::Get()->CreateTp(tpPath.string());

        Ptr<Node> n0 = NodeList::GetNode(beforeNodes);
        Ptr<Node> n1 = NodeList::GetNode(beforeNodes + 1);
        Ptr<UbController> c0 = n0->GetObject<UbController>();
        Ptr<UbController> c1 = n1->GetObject<UbController>();

        NS_TEST_ASSERT_MSG_EQ(c0->IsTPExists(11), true, "Source-side TP should be preloaded from config");
        NS_TEST_ASSERT_MSG_EQ(c1->IsTPExists(22), true, "Destination-side TP should be preloaded from config");

        fs::remove_all(caseDir, ec);
        Simulator::Destroy();
        Config::Reset();
    }
};

class UbCreateTpUsesPerPortIpTest : public TestCase
{
  public:
    UbCreateTpUsesPerPortIpTest()
        : TestCase("UnifiedBus - CreateTp uses per-port IP addresses")
    {
    }

    void DoRun() override
    {
        Ptr<Node> sender = CreateObject<Node>(0);
        Ptr<Node> receiver = CreateObject<Node>(0);
        InitNode(sender, UB_DEVICE, 3);
        InitNode(receiver, UB_DEVICE, 2);

        Ptr<UbController> controller = sender->GetObject<UbController>();
        Ptr<UbCongestionControl> cc = UbCongestionControl::Create(UB_DEVICE);
        const uint8_t senderPort = 2;
        const uint8_t receiverPort = 1;
        const uint32_t senderTpn = 1101;
        const uint32_t receiverTpn = 2201;

        controller->CreateTp(sender->GetId(),
                             receiver->GetId(),
                             senderPort,
                             receiverPort,
                             kUrmaWriteRegressionPriority,
                             senderTpn,
                             receiverTpn,
                             cc);

        Ptr<UbTransportChannel> tp = controller->GetTpByTpn(senderTpn);
        NS_TEST_ASSERT_MSG_NE(tp, nullptr, "Created TP should be visible by TPN");
        NS_TEST_ASSERT_MSG_EQ(tp->GetSip(),
                              NodeIdToIp(sender->GetId(), senderPort),
                              "Source IP should encode the UB source port");
        NS_TEST_ASSERT_MSG_EQ(tp->GetDip(),
                              NodeIdToIp(receiver->GetId(), receiverPort),
                              "Destination IP should encode the UB destination port");
        NS_TEST_ASSERT_MSG_NE(tp->GetSip(),
                              NodeIdToIp(sender->GetId(), 0),
                              "Source IP should not collapse to the node-level/default port IP");
        NS_TEST_ASSERT_MSG_NE(
            tp->GetDip(),
            NodeIdToIp(receiver->GetId(), 0),
            "Destination IP should not collapse to the node-level/default port IP");
        NS_TEST_ASSERT_MSG_EQ(tp->GetSport(),
                              static_cast<uint16_t>(senderPort),
                              "TP source port metadata should remain the UB port id");
        NS_TEST_ASSERT_MSG_EQ(tp->GetDport(),
                              static_cast<uint16_t>(receiverPort),
                              "TP destination port metadata should remain the UB port id");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbControllerMissingTpnLookupDoesNotInsertTest : public TestCase
{
  public:
    UbControllerMissingTpnLookupDoesNotInsertTest()
        : TestCase("UnifiedBus - missing TPN lookup does not insert a null TP")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_DEVICE, 1);

        Ptr<UbController> controller = node->GetObject<UbController>();
        const uint32_t missingTpn = 9999;
        const uint32_t beforeCount = controller->GetTransportCountForTest();

        NS_TEST_ASSERT_MSG_EQ(controller->GetTpByTpn(missingTpn),
                              nullptr,
                              "Missing TPN lookup should return nullptr");
        NS_TEST_ASSERT_MSG_EQ(controller->GetTransportCountForTest(),
                              beforeCount,
                              "Missing TPN lookup should not mutate the TP map");
        NS_TEST_ASSERT_MSG_EQ(controller->IsTPExists(missingTpn),
                              false,
                              "Missing TPN lookup should not create an existing TPN marker");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbTrafficDrivenReservationDoesNotRemoteCreateReceiverTpTest : public TestCase
{
  public:
    UbTrafficDrivenReservationDoesNotRemoteCreateReceiverTpTest()
        : TestCase("UnifiedBus - traffic-driven TP reservation leaves receiver TP lazy")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();

        senderCtrl->GetTpConnManager()->ReserveTpnsForTraffic(true,
                                                              topo.sender->GetId(),
                                                              topo.receiver->GetId(),
                                                              kUrmaWriteRegressionPriority);
        auto reservedSenderTpns =
            senderCtrl->GetTpConnManager()->GetTpnsByPeerNodePriority(topo.sender->GetId(),
                                                                      topo.receiver->GetId(),
                                                                      kUrmaWriteRegressionPriority);
        auto receiverTpns = receiverCtrl->GetTpConnManager()->GetTpnsByPeerNodePriority(
            topo.receiver->GetId(),
            topo.sender->GetId(),
            kUrmaWriteRegressionPriority);
        NS_TEST_ASSERT_MSG_EQ(reservedSenderTpns.size(),
                              1u,
                              "Traffic load should reserve one sender TPN");
        NS_TEST_ASSERT_MSG_EQ(receiverTpns.size(),
                              1u,
                              "Traffic load should reserve one receiver TPN");
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->IsTPExists(reservedSenderTpns[0].first),
                              false,
                              "Reservation should not materialize the sender endpoint");
        NS_TEST_ASSERT_MSG_EQ(receiverCtrl->IsTPExists(receiverTpns[0].first),
                              false,
                              "Reservation should not materialize the receiver endpoint");

        std::vector<uint32_t> senderTpns =
            senderCtrl->GetTpConnManager()->GetTpns(utils::GetTpnRuleT::BY_PEERNODE_PRIORITY,
                                                    true,
                                                    false,
                                                    topo.sender->GetId(),
                                                    topo.receiver->GetId(),
                                                    UINT32_MAX,
                                                    UINT32_MAX,
                                                    kUrmaWriteRegressionPriority);
        NS_TEST_ASSERT_MSG_EQ(senderTpns.size(),
                              1u,
                              "Single-path reservation should pick one sender TPN");
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->IsTPExists(senderTpns[0]),
                              true,
                              "Sender endpoint should be materialized by TP resolution");

        NS_TEST_ASSERT_MSG_EQ(receiverTpns.size(),
                              1u,
                              "Receiver reservation should be visible in the local manager");
        const uint32_t receiverTpn = receiverTpns[0].first;
        NS_TEST_ASSERT_MSG_EQ(receiverCtrl->IsTPExists(receiverTpn),
                              false,
                              "Receiver endpoint should not be materialized before packet arrival");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbReservedReceiverTpnMaterializesLocallyTest : public TestCase
{
  public:
    UbReservedReceiverTpnMaterializesLocallyTest()
        : TestCase("UnifiedBus - reserved receiver TPN materializes locally")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();

        senderCtrl->GetTpConnManager()->GetTpns(utils::GetTpnRuleT::BY_PEERNODE_PRIORITY,
                                                true,
                                                false,
                                                topo.sender->GetId(),
                                                topo.receiver->GetId(),
                                                UINT32_MAX,
                                                UINT32_MAX,
                                                kUrmaWriteRegressionPriority);
        auto receiverTpns = receiverCtrl->GetTpConnManager()->GetTpnsByPeerNodePriority(
            topo.receiver->GetId(),
            topo.sender->GetId(),
            kUrmaWriteRegressionPriority);
        NS_TEST_ASSERT_MSG_EQ(receiverTpns.size(),
                              1u,
                              "Receiver should have one reserved local TPN");
        const uint32_t receiverTpn = receiverTpns[0].first;

        Ptr<UbTransportChannel> receiverTp = receiverCtrl->CreateReservedTpEndpoint(receiverTpn);
        NS_TEST_ASSERT_MSG_NE(receiverTp,
                              nullptr,
                              "Reserved receiver TPN should create a local endpoint");
        NS_TEST_ASSERT_MSG_EQ(
            receiverCtrl->IsTPExists(receiverTpn),
            true,
            "Materialized reserved TPN should be visible in the receiver controller");
        NS_TEST_ASSERT_MSG_EQ(
            receiverTp->GetSip(),
            NodeIdToIp(topo.receiver->GetId(), topo.receiverPort->GetIfIndex()),
            "Receiver local endpoint source IP should encode the receiver UB port");
        NS_TEST_ASSERT_MSG_EQ(
            receiverTp->GetDip(),
            NodeIdToIp(topo.sender->GetId(), topo.senderPort->GetIfIndex()),
            "Receiver local endpoint destination IP should encode the sender UB port");

        Simulator::Destroy();
        Config::Reset();
    }
};

#ifndef _WIN32
class UbInboundTpChannelKeyValidationTest : public TestCase
{
  public:
    UbInboundTpChannelKeyValidationTest()
        : TestCase("UnifiedBus - inbound RTP packet must match the reserved channel key")
    {
    }

  private:
    enum class Mismatch
    {
        NONE,
        SOURCE_TPN,
        PEER_NODE,
        PEER_PORT,
        LOCAL_PORT,
        PRIORITY,
    };

    static void SendInboundPacket(Mismatch mismatch)
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
        senderCtrl->GetTpConnManager()->ReserveTpnsForTraffic(true,
                                                              topo.sender->GetId(),
                                                              topo.receiver->GetId(),
                                                              kUrmaWriteRegressionPriority);

        const auto senderTpns =
            senderCtrl->GetTpConnManager()->GetTpnsByPeerNodePriority(topo.sender->GetId(),
                                                                      topo.receiver->GetId(),
                                                                      kUrmaWriteRegressionPriority);
        const auto receiverTpns = receiverCtrl->GetTpConnManager()->GetTpnsByPeerNodePriority(
            topo.receiver->GetId(),
            topo.sender->GetId(),
            kUrmaWriteRegressionPriority);
        NS_ABORT_MSG_IF(senderTpns.size() != 1 || receiverTpns.size() != 1,
                        "Expected one reserved TP pair");

        uint32_t srcTpn = senderTpns[0].first;
        const uint32_t dstTpn = receiverTpns[0].first;
        Ipv4Address sourceIp = NodeIdToIp(topo.sender->GetId(), topo.senderPort->GetIfIndex());
        Ipv4Address destinationIp =
            NodeIdToIp(topo.receiver->GetId(), topo.receiverPort->GetIfIndex());
        uint8_t priority = kUrmaWriteRegressionPriority;

        switch (mismatch)
        {
        case Mismatch::SOURCE_TPN:
            ++srcTpn;
            break;
        case Mismatch::PEER_NODE:
            sourceIp = NodeIdToIp(topo.switch0->GetId(), topo.senderPort->GetIfIndex());
            break;
        case Mismatch::PEER_PORT:
            sourceIp = NodeIdToIp(topo.sender->GetId(), topo.senderPort->GetIfIndex() + 1);
            break;
        case Mismatch::LOCAL_PORT:
            destinationIp = NodeIdToIp(topo.receiver->GetId(), topo.receiverPort->GetIfIndex() + 1);
            break;
        case Mismatch::PRIORITY:
            priority = static_cast<uint8_t>(kUrmaWriteRegressionPriority - 1);
            break;
        case Mismatch::NONE:
            break;
        }

        Ptr<Packet> packet =
            BuildInboundTpPacket(topo, srcTpn, dstTpn, sourceIp, destinationIp, priority);
        topo.receiver->GetObject<UbSwitch>()->SwitchHandlePacket(topo.receiverPort, packet);
        NS_ABORT_MSG_IF(!receiverCtrl->IsTPExists(dstTpn),
                        "Valid inbound packet did not materialize its receiver TP");
    }

    void ExpectMismatchAbort(Mismatch mismatch, const std::string& description)
    {
        const int status = RunInChildProcess([mismatch]() { SendInboundPacket(mismatch); });
        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status), 1, description << " should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              description << " should fail with SIGABRT");
    }

    void DoRun() override
    {
        const int validStatus = RunInChildProcess([]() { SendInboundPacket(Mismatch::NONE); });
        NS_TEST_ASSERT_MSG_EQ(WIFEXITED(validStatus), 1, "Matching channel key should not abort");
        NS_TEST_ASSERT_MSG_EQ(WEXITSTATUS(validStatus),
                              0,
                              "Matching channel key should materialize the receiver TP");

        ExpectMismatchAbort(Mismatch::SOURCE_TPN, "Mismatched source TPN");
        ExpectMismatchAbort(Mismatch::PEER_NODE, "Mismatched peer node");
        ExpectMismatchAbort(Mismatch::PEER_PORT, "Mismatched peer port");
        ExpectMismatchAbort(Mismatch::LOCAL_PORT, "Mismatched local port");
        ExpectMismatchAbort(Mismatch::PRIORITY, "Mismatched priority");
    }
};

class UbUnknownInboundTpnFailsWithAutoRemoveTest : public TestCase
{
  public:
    UbUnknownInboundTpnFailsWithAutoRemoveTest()
        : TestCase("UnifiedBus - auto-remove mode does not hide an unknown inbound TPN")
    {
    }

  private:
    void DoRun() override
    {
        const int status = RunInChildProcess([]() {
            LocalTpTopology topo = BuildLocalTpTopology();
            Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();
            receiverCtrl->GetTpConnManager()->SetAttribute("RemoveUselessTp", BooleanValue(true));

            Ptr<Packet> packet = BuildInboundTpPacket(
                topo,
                1234,
                9090,
                NodeIdToIp(topo.sender->GetId(), topo.senderPort->GetIfIndex()),
                NodeIdToIp(topo.receiver->GetId(), topo.receiverPort->GetIfIndex()),
                kUrmaWriteRegressionPriority);
            topo.receiver->GetObject<UbSwitch>()->SwitchHandlePacket(topo.receiverPort, packet);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Unknown inbound TPN should abort even in auto-remove mode");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Unknown inbound TPN should fail with SIGABRT");
    }
};
#endif

class UbConcurrentReservedReceiverTpnMaterializationTest : public TestCase
{
  public:
    UbConcurrentReservedReceiverTpnMaterializationTest()
        : TestCase("UnifiedBus - concurrent reserved receiver TPN materialization is idempotent")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();

        senderCtrl->GetTpConnManager()->GetTpns(utils::GetTpnRuleT::BY_PEERNODE_PRIORITY,
                                                true,
                                                false,
                                                topo.sender->GetId(),
                                                topo.receiver->GetId(),
                                                UINT32_MAX,
                                                UINT32_MAX,
                                                kUrmaWriteRegressionPriority);
        auto receiverTpns = receiverCtrl->GetTpConnManager()->GetTpnsByPeerNodePriority(
            topo.receiver->GetId(),
            topo.sender->GetId(),
            kUrmaWriteRegressionPriority);
        NS_TEST_ASSERT_MSG_EQ(receiverTpns.size(),
                              1u,
                              "Receiver should have one reserved local TPN");
        const uint32_t receiverTpn = receiverTpns[0].first;

        constexpr std::size_t kConcurrentCallCount = 16;
        std::barrier startBarrier(static_cast<std::ptrdiff_t>(kConcurrentCallCount));
        std::vector<Ptr<UbTransportChannel>> results(kConcurrentCallCount);
        std::vector<std::thread> workers;
        workers.reserve(kConcurrentCallCount);
        for (std::size_t index = 0; index < kConcurrentCallCount; ++index)
        {
            workers.emplace_back([&, index]() {
                startBarrier.arrive_and_wait();
                results[index] = receiverCtrl->CreateReservedTpEndpoint(receiverTpn);
            });
        }
        for (auto& worker : workers)
        {
            worker.join();
        }

        Ptr<UbTransportChannel> materialized = receiverCtrl->GetTpByTpn(receiverTpn);
        NS_TEST_ASSERT_MSG_NE(materialized,
                              nullptr,
                              "Concurrent materialization should create the receiver TP");
        for (const auto& result : results)
        {
            NS_TEST_ASSERT_MSG_EQ(result,
                                  materialized,
                                  "All concurrent callers should observe the same receiver TP");
        }
        NS_TEST_ASSERT_MSG_EQ(receiverCtrl->GetTransportCountForTest(),
                              1u,
                              "Concurrent materialization should count exactly one receiver TP");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbRepeatedTpResolutionReusesReservationTest : public TestCase
{
  public:
    UbRepeatedTpResolutionReusesReservationTest()
        : TestCase("UnifiedBus - repeated TP resolution reuses reservation")
    {
    }

    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> senderCtrl = topo.sender->GetObject<UbController>();
        Ptr<UbController> receiverCtrl = topo.receiver->GetObject<UbController>();

        auto resolveTpns = [&]() {
            return senderCtrl->GetTpConnManager()->GetTpns(utils::GetTpnRuleT::BY_PEERNODE_PRIORITY,
                                                           true,
                                                           false,
                                                           topo.sender->GetId(),
                                                           topo.receiver->GetId(),
                                                           UINT32_MAX,
                                                           UINT32_MAX,
                                                           kUrmaWriteRegressionPriority);
        };

        std::vector<uint32_t> firstTpns = resolveTpns();
        const size_t senderConnectionCount = senderCtrl->GetTpConnManager()->GetConnectionCount();
        const size_t receiverConnectionCount =
            receiverCtrl->GetTpConnManager()->GetConnectionCount();
        const uint32_t senderTransportCount = senderCtrl->GetTransportCountForTest();
        const uint32_t receiverTransportCount = receiverCtrl->GetTransportCountForTest();

        std::vector<uint32_t> secondTpns = resolveTpns();

        NS_TEST_ASSERT_MSG_EQ(firstTpns.size(),
                              secondTpns.size(),
                              "Repeated TP resolution should return the same number of TPNs");
        NS_TEST_ASSERT_MSG_EQ(firstTpns.empty(), false, "First TP resolution should return a TPN");
        NS_TEST_ASSERT_MSG_EQ(
            firstTpns[0],
            secondTpns[0],
            "Repeated TP resolution for the same peer and priority should reuse TPNs");
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetTpConnManager()->GetConnectionCount(),
                              senderConnectionCount,
                              "Repeated TP resolution should not add sender connection records");
        NS_TEST_ASSERT_MSG_EQ(receiverCtrl->GetTpConnManager()->GetConnectionCount(),
                              receiverConnectionCount,
                              "Repeated TP resolution should not add receiver connection records");
        NS_TEST_ASSERT_MSG_EQ(senderCtrl->GetTransportCountForTest(),
                              senderTransportCount,
                              "Repeated TP resolution should reuse the sender endpoint");
        NS_TEST_ASSERT_MSG_EQ(receiverCtrl->GetTransportCountForTest(),
                              receiverTransportCount,
                              "Repeated TP resolution should not materialize receiver endpoints");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbUnreservedReceiverTpnDoesNotMaterializeTest : public TestCase
{
  public:
    UbUnreservedReceiverTpnDoesNotMaterializeTest()
        : TestCase("UnifiedBus - unreserved receiver TPN does not materialize")
    {
    }

    void DoRun() override
    {
        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_DEVICE, 1);

        Ptr<UbController> controller = node->GetObject<UbController>();
        const uint32_t unreservedTpn = 9090;
        const uint32_t beforeCount = controller->GetTransportCountForTest();

        NS_TEST_ASSERT_MSG_EQ(controller->CreateReservedTpEndpoint(unreservedTpn),
                              nullptr,
                              "Unreserved TPN should not create a local endpoint");
        NS_TEST_ASSERT_MSG_EQ(controller->GetTransportCountForTest(),
                              beforeCount,
                              "Unreserved TPN materialization attempt should not mutate TP map");
        NS_TEST_ASSERT_MSG_EQ(controller->IsTPExists(unreservedTpn),
                              false,
                              "Unreserved TPN should remain absent");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbTraceDirSetupTest : public TestCase
{
  public:
    UbTraceDirSetupTest()
        : TestCase("UnifiedBus - Trace directory setup tolerates missing runlog")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        auto uniqueSuffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        fs::path caseDir = fs::temp_directory_path() / ("ub-trace-dir-test-" + uniqueSuffix);
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(caseDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary case directory creation should succeed");

        fs::path configPath = caseDir / "network_attribute.txt";
        std::string tracePath = utils::UbUtils::PrepareTraceDir(configPath.string());

        NS_TEST_ASSERT_MSG_EQ(fs::exists(caseDir / "runlog"), true, "runlog directory should be created");
        NS_TEST_ASSERT_MSG_EQ(tracePath.empty(), false, "Returned trace path should not be empty");

        std::ofstream staleFile((caseDir / "runlog" / "stale.tr").string());
        staleFile << "stale";
        staleFile.close();

        tracePath = utils::UbUtils::PrepareTraceDir(configPath.string());
        NS_TEST_ASSERT_MSG_EQ(fs::exists(caseDir / "runlog" / "stale.tr"), false, "Existing runlog contents should be removed");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(caseDir / "runlog"), true, "runlog directory should be recreated");

        fs::remove_all(caseDir, ec);
    }
};

class UbAlgorithmTraceGateDefaultOffTest : public TestCase
{
  public:
    UbAlgorithmTraceGateDefaultOffTest()
        : TestCase("UnifiedBus - algorithm trace files stay off by default even when trace path exists")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-algo-trace-default-off-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_FLOW_CONTROL_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_CONGESTION_CONTROL_TRACE_ENABLE", BooleanValue(false));

        utils::UbUtils::SetTracePathForTest(caseDir.string());

        utils::UbUtils::PfcStateNotify(3, 1, "PAUSE", 7, 12345);
        utils::UbUtils::DcqcnMarkNotify(3, 2, 67890, 0.5);
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "PfcTrace_node_3_port_1.tr"),
                              false,
                              "Flow-control trace should stay off by default");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "DcqcnMarkTrace_node_3_port_2.tr"),
                              false,
                              "Congestion-control trace should stay off by default");

        fs::remove_all(caseDir, ec);
    }
};

class UbAlgorithmTraceCategoryGateTest : public TestCase
{
  public:
    UbAlgorithmTraceCategoryGateTest()
        : TestCase("UnifiedBus - algorithm trace category gates independently control flow and congestion traces")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-algo-trace-category-gate-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_FLOW_CONTROL_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_CONGESTION_CONTROL_TRACE_ENABLE", BooleanValue(false));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        utils::UbUtils::PfcStateNotify(5, 0, "PAUSE", 7, 222);
        utils::UbUtils::DcqcnMarkNotify(5, 0, 333, 0.25);
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "PfcTrace_node_5_port_0.tr"),
                              true,
                              "Flow-control trace file should be emitted when flow-control gate is on");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "DcqcnMarkTrace_node_5_port_0.tr"),
                              false,
                              "Congestion-control trace file should stay off when its gate is off");

        ec.clear();
        fs::remove_all(runlogDir, ec);
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory reset should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_FLOW_CONTROL_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_CONGESTION_CONTROL_TRACE_ENABLE", BooleanValue(true));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        utils::UbUtils::PfcStateNotify(6, 1, "PAUSE", 7, 444);
        utils::UbUtils::DcqcnMarkNotify(6, 1, 555, 0.75);
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "PfcTrace_node_6_port_1.tr"),
                              false,
                              "Flow-control trace file should stay off when flow-control gate is off");
        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "DcqcnMarkTrace_node_6_port_1.tr"),
                              true,
                              "Congestion-control trace file should be emitted when its gate is on");
        fs::remove_all(caseDir, ec);
    }
};

class UbQueueTraceCategoryGateTest : public TestCase
{
  public:
    UbQueueTraceCategoryGateTest()
        : TestCase("UnifiedBus - queue trace gate independently controls queue trace files")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-queue-trace-category-gate-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_QUEUE_SAMPLE_INTERVAL_NS", UintegerValue(1000));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        BuildLocalTpTopology();
        utils::UbUtils::Get()->TopoTraceConnect();
        Simulator::Stop(NanoSeconds(1500));
        Simulator::Run();
        Simulator::Destroy();
        utils::UbUtils::Get()->Destroy();

        NS_TEST_ASSERT_MSG_EQ(fs::exists(runlogDir / "QueueTrace_node_0_port_0.tr"),
                              false,
                              "Queue trace file should stay off when queue-trace gate is off");

        ec.clear();
        fs::remove_all(runlogDir, ec);
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory reset should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_PORT_TRACE_ENABLE", BooleanValue(false));
        GlobalValue::Bind("UB_QUEUE_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_SAMPLE_INTERVAL_NS", UintegerValue(1000));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        LocalTpTopology topo = BuildLocalTpTopology();
        utils::UbUtils::Get()->TopoTraceConnect();
        topo.sender->GetObject<UbSwitch>()->GetQueueManager()->PushToVoq(0, 0, UB_PRIORITY_DEFAULT, 64);
        topo.senderPort->GetUbQueue()->DoEnqueue(std::make_tuple(0, UB_PRIORITY_DEFAULT, Create<Packet>(32)));
        Simulator::Stop(NanoSeconds(1500));
        Simulator::Run();
        Simulator::Destroy();
        utils::UbUtils::Get()->Destroy();

        const fs::path queueTraceFile = runlogDir / "QueueTrace_node_0_port_0.tr";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(queueTraceFile),
                              true,
                              "Queue trace file should be emitted when queue-trace gate is on");
        const std::string text = std::ifstream(queueTraceFile).good()
                                     ? [&queueTraceFile]() {
                                           std::ifstream input(queueTraceFile);
                                           std::ostringstream oss;
                                           oss << input.rdbuf();
                                           return oss.str();
                                       }()
                                     : "";
        NS_TEST_ASSERT_MSG_NE(text.find("source: SAMPLE"),
                              std::string::npos,
                              "Queue trace file should contain sampled queue state when gate is on");
        NS_TEST_ASSERT_MSG_NE(text.find("source: EGRESS_ENQUEUE"),
                              std::string::npos,
                              "QueueTrace should include egress rows even when port trace is disabled");
        NS_TEST_ASSERT_MSG_NE(text.find("ingress"),
                              std::string::npos,
                              "QueueTrace should include ingress occupancy rows");

        fs::remove_all(caseDir, ec);
    }
};

class UbCtpPacketTraceGateTest : public TestCase
{
  public:
    UbCtpPacketTraceGateTest()
        : TestCase("UnifiedBus - CTP packet trace obeys trace gates")
    {
    }

  private:
    void DoRun() override
    {
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(false));
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        NS_TEST_ASSERT_MSG_EQ(service->IsPacketTraceEnabledForTest(), false, "trace off by default gate");
    }
};

class UbCtpTraceConnectDoesNotCreateServiceTest : public TestCase
{
  public:
    UbCtpTraceConnectDoesNotCreateServiceTest()
        : TestCase("UnifiedBus - CTP trace connect does not create CTP service")
    {
    }

  private:
    void DoRun() override
    {
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_PACKET_TRACE_ENABLE", BooleanValue(true));

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> controller = topo.sender->GetObject<UbController>();

        NS_TEST_ASSERT_MSG_EQ(controller->PeekCtpTransportService(), nullptr, "no CTP service initially");
        utils::UbUtils::Get()->TopoTraceConnect();
        NS_TEST_ASSERT_MSG_EQ(controller->PeekCtpTransportService(),
                              nullptr,
                              "packet trace connect should not lazily create CTP service");

        Simulator::Destroy();
    }
};

class UbCtpPacketTraceConnectsLazyServiceTest : public TestCase
{
  public:
    UbCtpPacketTraceConnectsLazyServiceTest()
        : TestCase("UnifiedBus - CTP packet trace connects lazy CTP service")
    {
    }

  private:
    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir = fs::temp_directory_path() / ("ub-ctp-packet-trace-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        utils::UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_PACKET_TRACE_ENABLE", BooleanValue(true));
        utils::UbUtils::SetTracePathForTest(caseDir.string());

        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbController> controller = topo.sender->GetObject<UbController>();
        utils::UbUtils::Get()->TopoTraceConnect();
        NS_TEST_ASSERT_MSG_EQ(controller->PeekCtpTransportService(),
                              nullptr,
                              "trace connect should not create CTP service before CTP traffic");

        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = topo.sender->GetId(),
                           .srcEntityId = 0,
                           .dstNodeId = topo.receiver->GetId(),
                           .dstEntityId = 0,
                           .vl = 7};
        service->SetSourcePortHint(key, topo.senderPort->GetIfIndex());
        service->SetDestinationPortHint(key, topo.receiverPort->GetIfIndex());
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(CreateCtpWriteSegment(0), key),
                              true,
                              "CTP send should emit packet trace");
        service->CompleteFromTaAckForTest(key, 0);

        Simulator::Destroy();
        utils::UbUtils::Get()->Destroy();

        const fs::path legacyTraceFile = runlogDir / "CtpPacketTrace_node_0.tr";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(legacyTraceFile),
                              false,
                              "CTP packet timing should use unified PacketTrace output");

        const fs::path traceFile = runlogDir / "PacketTrace_node_0.tr";
        NS_TEST_ASSERT_MSG_EQ(fs::exists(traceFile),
                              true,
                              "CTP packet timing trace should be emitted for lazy service");
        std::ifstream input(traceFile);
        std::ostringstream oss;
        oss << input.rdbuf();
        const std::string text = oss.str();
        NS_TEST_ASSERT_MSG_NE(text.find("First Packet Sends"),
                              std::string::npos,
                              "CTP packet trace should include first-send timestamp");
        NS_TEST_ASSERT_MSG_NE(text.find("Last Packet ACKs"),
                              std::string::npos,
                              "CTP packet trace should include last-ACK timestamp");
        NS_TEST_ASSERT_MSG_NE(text.find("transport: CTP"),
                              std::string::npos,
                              "CTP packet trace should identify CTP transport");
        NS_TEST_ASSERT_MSG_NE(text.find("taskId: 0"),
                              std::string::npos,
                              "CTP packet trace should include the task id");
        NS_TEST_ASSERT_MSG_NE(text.find("srcNode: 0"),
                              std::string::npos,
                              "CTP packet trace should include source node");
        NS_TEST_ASSERT_MSG_NE(text.find("dstNode: 3"),
                              std::string::npos,
                              "CTP packet trace should include destination node");

        fs::remove_all(caseDir, ec);
    }
};

namespace utils
{

class UbQueueSamplerEventRetentionTest : public TestCase
{
  public:
    UbQueueSamplerEventRetentionTest()
        : TestCase("UnifiedBus - queue sampler keeps only one pending event per port")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-queue-sampler-retention-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        UbUtils::Get()->Destroy();
        GlobalValue::Bind("UB_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_TRACE_ENABLE", BooleanValue(true));
        GlobalValue::Bind("UB_QUEUE_SAMPLE_INTERVAL_NS", UintegerValue(10));
        UbUtils::SetTracePathForTest(caseDir.string());

        BuildLocalTpTopology();
        UbUtils::Get()->TopoTraceConnect();
        const std::size_t initialSamplerEvents = UbUtils::queue_sampler_events.size();
        NS_TEST_ASSERT_MSG_NE(initialSamplerEvents,
                              0u,
                              "Queue sampler should track at least one switch port");

        Simulator::Stop(NanoSeconds(95));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(UbUtils::queue_sampler_events.size(),
                              initialSamplerEvents,
                              "Queue sampler should not retain historical tick EventIds");

        Simulator::Destroy();
        UbUtils::Get()->Destroy();
        fs::remove_all(caseDir, ec);
    }
};

class UbTraceFileConcurrencyTest : public TestCase
{
  public:
    UbTraceFileConcurrencyTest()
        : TestCase("UnifiedBus - trace batching keeps file contents intact under concurrent writers")
    {
    }

    void DoRun() override
    {
        namespace fs = std::filesystem;

        constexpr uint32_t kThreadCount = 8;
        constexpr uint32_t kLinesPerThread = 4000;
        const auto uniqueSuffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path caseDir =
            fs::temp_directory_path() / ("ub-trace-concurrency-test-" + uniqueSuffix);
        const fs::path runlogDir = caseDir / "runlog";
        const fs::path traceFile = runlogDir / "concurrent.tr";

        std::error_code ec;
        fs::remove_all(caseDir, ec);
        ec.clear();
        fs::create_directories(runlogDir, ec);
        NS_TEST_ASSERT_MSG_EQ(ec.value(), 0, "Temporary runlog directory creation should succeed");

        UbUtils::Get()->Destroy();
        UbUtils::trace_path = caseDir.string();
        if (!UbUtils::trace_path.empty() &&
            UbUtils::trace_path.back() != fs::path::preferred_separator)
        {
            UbUtils::trace_path.push_back(fs::path::preferred_separator);
        }

        std::atomic<bool> start{false};
        std::vector<std::thread> writers;
        writers.reserve(kThreadCount);

        for (uint32_t tid = 0; tid < kThreadCount; ++tid)
        {
            writers.emplace_back([&, tid]() {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                const std::string payload(96, static_cast<char>('A' + (tid % 26)));
                for (uint32_t line = 0; line < kLinesPerThread; ++line)
                {
                    std::ostringstream oss;
                    oss << "thread=" << tid << " line=" << line << " payload=" << payload;
                    UbUtils::PrintTraceInfoNoTs(traceFile.string(), oss.str());
                    if ((line & 0x3f) == 0)
                    {
                        std::this_thread::yield();
                    }
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& writer : writers)
        {
            writer.join();
        }

        UbUtils::Get()->Destroy();

        std::ifstream input(traceFile);
        NS_TEST_ASSERT_MSG_EQ(input.is_open(), true, "Concurrent trace file should be created");

        uint64_t lineCount = 0;
        std::string line;
        while (std::getline(input, line))
        {
            ++lineCount;
        }

        NS_TEST_ASSERT_MSG_EQ(lineCount,
                              static_cast<uint64_t>(kThreadCount) * kLinesPerThread,
                              "Concurrent trace writes should preserve every line");

        fs::remove_all(caseDir, ec);
    }
};

} // namespace utils

class UbMpiRankExtractionHelperTest : public TestCase
{
  public:
    UbMpiRankExtractionHelperTest()
        : TestCase("UnifiedBus - ExtractMpiRank follows MPI rank encoding rules")
    {
    }

    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::ExtractMpiRank(7u),
                              7u,
                              "Plain systemId should preserve rank value");

        const uint32_t packedSystemId = (0x1234u << 16) | 0x002au;
#ifdef NS3_MTP
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::ExtractMpiRank(packedSystemId),
                              0x002au,
                              "MTP packed systemId should use low 16 bits as MPI rank");
#else
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::ExtractMpiRank(packedSystemId),
                              packedSystemId,
                              "Non-MTP build should use the full systemId as MPI rank");
#endif
    }
};

class UbSameMpiRankHelperTest : public TestCase
{
  public:
    UbSameMpiRankHelperTest()
        : TestCase("UnifiedBus - IsSameMpiRank compares MPI rank instead of raw packed systemId")
    {
    }

    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(5u, 5u),
                              true,
                              "Identical plain systemId values should be on the same MPI rank");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(5u, 6u),
                              false,
                              "Different plain systemId values should be on different MPI ranks");

        const uint32_t lhsPacked = (0x0001u << 16) | 0x0009u;
        const uint32_t rhsSameRankPacked = (0x0002u << 16) | 0x0009u;
        const uint32_t rhsDifferentRankPacked = (0x0002u << 16) | 0x000au;

#ifdef NS3_MTP
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(lhsPacked, rhsSameRankPacked),
                              true,
                              "MTP packed systemId values with the same low 16 bits should match");
#else
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(lhsPacked, rhsSameRankPacked),
                              false,
                              "Non-MTP build should compare full systemId values");
#endif
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSameMpiRank(lhsPacked, rhsDifferentRankPacked),
                              false,
                              "Different MPI rank encodings should not match");
    }
};

class UbSystemOwnedByRankHelperTest : public TestCase
{
  public:
    UbSystemOwnedByRankHelperTest()
        : TestCase("UnifiedBus - IsSystemOwnedByRank follows packed MPI ownership rules")
    {
    }

    void DoRun() override
    {
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(7u, 7u),
                              true,
                              "Plain systemId should be owned by the same MPI rank");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(7u, 6u),
                              false,
                              "Plain systemId should not be owned by a different MPI rank");

        const uint32_t packedSystemId = (0x1234u << 16) | 0x0009u;

#ifdef NS3_MTP
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, 0x0009u),
                              true,
                              "Packed systemId should be owned by the matching low-16-bit MPI rank");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, 0x000au),
                              false,
                              "Packed systemId should not be owned by a different low-16-bit MPI rank");
#else
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, packedSystemId),
                              true,
                              "Non-MTP build should treat the full systemId as the owner key");
        NS_TEST_ASSERT_MSG_EQ(utils::UbUtils::IsSystemOwnedByRank(packedSystemId, 0x0009u),
                              false,
                              "Non-MTP build should not mask packed systemId values");
#endif
    }
};

class UbQueueManagerReserveOnlyAdmissionTest : public TestCase
{
  public:
    UbQueueManagerReserveOnlyAdmissionTest()
        : TestCase("UnifiedBus - reserve-only admission allows exact-fit enqueue and keeps shared state at zero")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        constexpr uint32_t kReserveBytes = 100;
        constexpr uint32_t kPriority = 1;
        Ptr<UbQueueManager> queueManager = CreateQueueManagerFixture(/*ports*/ 1,
                                                                    /*vlNum*/ 2,
                                                                    kReserveBytes,
                                                                    /*sharedPoolBytes*/ 0,
                                                                    /*headroomPerPortBytes*/ 0,
                                                                    /*resumeGapBytes*/ 16,
                                                                    /*alphaShift*/ 1);

        NS_TEST_ASSERT_MSG_EQ(queueManager->CheckInPortSpace(0, kPriority, kReserveBytes),
                              true,
                              "Reserve-only admission should accept a packet that exactly fills the reserved bytes");

        queueManager->PushToVoq(0, 0, kPriority, kReserveBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, kPriority),
                              kReserveBytes,
                              "Exact-fit enqueue should be reflected in ingress usage");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressSharedBytes(0, kPriority),
                              0u,
                              "Reserve-only admission should not accumulate shared usage");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressHeadroomBytes(0, kPriority),
                              0u,
                              "Reserve-only admission should not accumulate headroom usage");
        NS_TEST_ASSERT_MSG_EQ(queueManager->CheckInPortSpace(0, kPriority, 1),
                              false,
                              "Reserve-only admission should reject packets once the reserved bytes are fully consumed");

        queueManager->PopFromVoq(0, 0, kPriority, kReserveBytes);

        NS_TEST_ASSERT_MSG_EQ(queueManager->GetQueueIngressNonHeadroomBytes(0, kPriority),
                              0u,
                              "PopFromVoq should release reserve-only ingress accounting");
        NS_TEST_ASSERT_MSG_EQ(queueManager->GetSwitchBufferOccupancy().shared_pool_used_bytes,
                              0u,
                              "Reserve-only admission should keep shared-pool accounting at zero after drain");
        Config::Reset();
    }
};

class UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest : public TestCase
{
  public:
    UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest()
        : TestCase("UnifiedBus - sliding bitmap window preserves out-of-order marks while advancing")
    {
    }

    void DoRun() override
    {
        UbSlidingBitmapWindow window(8);
        window.Reset(100);

        NS_TEST_ASSERT_MSG_EQ(window.Mark(102),
                              true,
                              "Marking an in-window out-of-order sequence should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(),
                              0u,
                              "Window must not advance before the gap closes");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 100u, "Base should stay at the first missing sequence");

        NS_TEST_ASSERT_MSG_EQ(window.Mark(100),
                              true,
                              "Marking the base sequence should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(),
                              1u,
                              "Closing the base gap should advance exactly one slot");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 101u, "Base should move to the next missing sequence");

        NS_TEST_ASSERT_MSG_EQ(window.Mark(101),
                              true,
                              "Marking the next gap should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(),
                              2u,
                              "Advance should consume both the newly filled gap and the preserved out-of-order mark");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 103u, "Base should now point past the contiguous run");
    }
};

class UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest : public TestCase
{
  public:
    UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest()
        : TestCase("UnifiedBus - sliding bitmap window reuses slots without reviving stale marks")
    {
    }

    void DoRun() override
    {
        UbSlidingBitmapWindow window(4);
        window.Reset(10);

        NS_TEST_ASSERT_MSG_EQ(window.Mark(10), true, "Base mark should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.Mark(11), true, "Second mark should succeed");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(), 2u, "Two contiguous marks should advance by two");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 12u, "Base should move forward after consuming two marks");

        NS_TEST_ASSERT_MSG_EQ(window.Contains(10),
                              false,
                              "Consumed sequences must not remain marked after their slots are reused");
        NS_TEST_ASSERT_MSG_EQ(window.Mark(14), true, "Reused slot should accept a new in-window sequence");
        NS_TEST_ASSERT_MSG_EQ(window.Mark(12), true, "New base should still be markable after slot reuse");
        NS_TEST_ASSERT_MSG_EQ(window.AdvanceContiguous(), 1u, "Only the rebuilt contiguous prefix should advance");
        NS_TEST_ASSERT_MSG_EQ(window.GetBase(), 13u, "Base should stop at the next missing sequence");
        NS_TEST_ASSERT_MSG_EQ(window.Contains(14),
                              true,
                              "Future out-of-order marks should remain visible after partial advance");
    }
};

class UbBusyPortArrivalPrefetchesNextPacketTest : public TestCase
{
  public:
    UbBusyPortArrivalPrefetchesNextPacketTest()
        : TestCase("UnifiedBus - busy port arrival still prefetches next packet into egress queue")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        auto fixture = CreateSinglePortPfcFixture(FcType::NONE,
                                                  1024,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  0);
        fixture.sw->SetCongestionCtrl(CreateObject<UbCongestionControl>());

        fixture.port->NotifyLinkUp();
        fixture.port->SetSendState(SendState::BUSY);
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetUbQueue()->IsEmpty(),
                              true,
                              "Fixture should start with an empty egress queue");

        fixture.sw->SendPacket(Create<Packet>(128), 0, 0, 1);
        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetUbQueue()->IsEmpty(),
                              true,
                              "Busy ports should not dequeue immediately before allocator latency elapses");

        Simulator::Stop(NanoSeconds(11));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(fixture.port->GetUbQueue()->IsEmpty(),
                              false,
                              "A packet arriving while the out port is busy should still be prefetched "
                              "into the egress queue after AllocationTime elapses");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPacketSprayUsesEvenRoundRobinAcrossEqualPortsTest : public TestCase
{
  public:
    UbPacketSprayUsesEvenRoundRobinAcrossEqualPortsTest()
        : TestCase("UnifiedBus - packet spray evenly round-robins across equal-cost ports")
    {
    }

    void DoRun() override
    {
        Ptr<UbRoutingProcess> routing = CreateObject<UbRoutingProcess>();
        const std::vector<uint16_t> equalPorts = {10, 11, 12};
        routing->AddShortestRoute(NodeIdToIp(42).Get(), equalPorts);

        RoutingKey rtKey{};
        rtKey.sip = NodeIdToIp(1).Get();
        rtKey.dip = NodeIdToIp(42).Get();
        rtKey.dport = 7;
        rtKey.priority = 2;
        rtKey.useShortestPath = true;
        rtKey.usePacketSpray = true;

        std::map<uint16_t, uint32_t> counts;
        for (uint16_t port : equalPorts)
        {
            counts[port] = 0;
        }

        for (uint32_t spraySalt = 1; spraySalt <= 977; ++spraySalt)
        {
            rtKey.sport = static_cast<uint16_t>(spraySalt);
            bool selectedShortestPath = false;
            const int outPort = routing->GetOutPort(rtKey, selectedShortestPath);

            NS_TEST_ASSERT_MSG_EQ(selectedShortestPath,
                                  true,
                                  "Packet spray should stay within the equal shortest-path set");
            NS_TEST_ASSERT_MSG_EQ((counts.count(static_cast<uint16_t>(outPort)) == 1),
                                  true,
                                  "Packet spray must select one of the configured equal-cost ports");
            counts[static_cast<uint16_t>(outPort)]++;
        }

        uint32_t minCount = std::numeric_limits<uint32_t>::max();
        uint32_t maxCount = 0;
        for (uint16_t port : equalPorts)
        {
            minCount = std::min(minCount, counts[port]);
            maxCount = std::max(maxCount, counts[port]);
        }

        NS_TEST_ASSERT_MSG_EQ(((maxCount - minCount) <= 1),
                              true,
                              "Packet spray should differ by at most one packet across equal-cost ports");
    }
};

class UbRoundRobinAllocatorSeedsDifferentInitialPhasesPerOutPortTest : public TestCase
{
  public:
    UbRoundRobinAllocatorSeedsDifferentInitialPhasesPerOutPortTest()
        : TestCase("UnifiedBus - round robin allocator seeds different initial queue phases per out port")
    {
    }

    void DoRun() override
    {
        Config::Reset();

        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_SWITCH, 4);

        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        Ptr<UbRoundRobinAllocator> allocator = DynamicCast<UbRoundRobinAllocator>(sw->GetAllocator());
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Default switch allocator should be round robin");

        constexpr uint32_t kPriority = 1;
        for (uint32_t outPort = 0; outPort < 4; ++outPort)
        {
            for (uint32_t inPort = 0; inPort < 4; ++inPort)
            {
                sw->PushPacketToVoq(Create<Packet>(64), outPort, kPriority, inPort);
            }
        }

        std::vector<uint32_t> firstInPorts;
        for (uint32_t outPort = 0; outPort < 4; ++outPort)
        {
            Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
            Ptr<UbIngressQueue> selected = allocator->SelectNextIngressQueue(port);
            NS_TEST_ASSERT_MSG_NE(selected, nullptr, "Symmetric non-empty VOQs should yield a selected queue");
            firstInPorts.push_back(selected->GetInPortId());
        }

        NS_TEST_ASSERT_MSG_EQ(firstInPorts[0], 0u, "Port 0 should keep the baseline initial phase");
        NS_TEST_ASSERT_MSG_EQ(firstInPorts[1], 1u, "Port 1 should not start from the same ingress queue as port 0");
        NS_TEST_ASSERT_MSG_EQ(firstInPorts[2], 2u, "Port 2 should get its own initial queue phase");
        NS_TEST_ASSERT_MSG_EQ(firstInPorts[3], 3u, "Port 3 should get its own initial queue phase");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbSwitchCreatesVoqsOnDemandTest : public TestCase
{
  public:
    UbSwitchCreatesVoqsOnDemandTest()
        : TestCase("UnifiedBus - UbSwitch creates VOQs only when packets arrive")
    {
    }

    void DoRun() override
    {
        Config::Reset();

        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_SWITCH, 4);

        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        NS_TEST_ASSERT_MSG_EQ(sw->GetAllocatedVoqCountForTest(),
                              0u,
                              "Switch initialization should not allocate all VOQs eagerly");

        constexpr uint32_t kOutPort = 2;
        constexpr uint32_t kPriority = 1;
        constexpr uint32_t kInPort = 3;
        constexpr uint32_t kUnusedInPort = 1;
        NS_TEST_ASSERT_MSG_EQ(sw->HasVoqForTest(kOutPort, kPriority, kUnusedInPort),
                              false,
                              "Checking an unused key should not allocate a VOQ");

        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, kPriority, kInPort);

        NS_TEST_ASSERT_MSG_EQ(sw->HasVoqForTest(kOutPort, kPriority, kInPort),
                              true,
                              "Pushing a packet should allocate the addressed VOQ");
        NS_TEST_ASSERT_MSG_EQ(sw->HasVoqForTest(kOutPort, kPriority, kUnusedInPort),
                              false,
                              "Allocating one VOQ should not materialize neighboring keys");
        NS_TEST_ASSERT_MSG_EQ(sw->GetAllocatedVoqCountForTest(),
                              1u,
                              "Only the addressed VOQ should be allocated");

        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, kPriority, kInPort);
        NS_TEST_ASSERT_MSG_EQ(sw->GetAllocatedVoqCountForTest(),
                              1u,
                              "Reusing the same VOQ should not allocate a duplicate queue");
        NS_TEST_ASSERT_MSG_EQ(sw->HasVoqForTest(kOutPort, kPriority, kUnusedInPort),
                              false,
                              "Repeated pushes to one VOQ should leave other keys unmaterialized");

        constexpr uint32_t kOtherKeyOutPort = 1;
        constexpr uint32_t kOtherKeyPriority = 1;
        constexpr uint32_t kOtherKeyInPort = 0;
        sw->PushPacketToVoq(
            Create<Packet>(64), kOtherKeyOutPort, kOtherKeyPriority, kOtherKeyInPort);
        NS_TEST_ASSERT_MSG_EQ(sw->HasVoqForTest(kOtherKeyOutPort,
                                                kOtherKeyPriority,
                                                kOtherKeyInPort),
                              true,
                              "A distinct on-demand VOQ key should be addressable");
        NS_TEST_ASSERT_MSG_EQ(sw->GetAllocatedVoqCountForTest(),
                              2u,
                              "Allocating a VOQ in another group should allocate only that VOQ");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbVoqAllocatorPreservesIngressQueueSlotPhaseTest : public TestCase
{
  public:
    UbVoqAllocatorPreservesIngressQueueSlotPhaseTest()
        : TestCase("UnifiedBus - VOQ allocator preserves ingress queue slot phase")
    {
    }

    void DoRun() override
    {
        Config::Reset();

        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_SWITCH, 4);

        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        Ptr<UbRoundRobinAllocator> allocator = DynamicCast<UbRoundRobinAllocator>(sw->GetAllocator());
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Default switch allocator should be round robin");

        constexpr uint32_t kOutPort = 2;
        constexpr uint32_t kPriority = 1;
        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, kPriority, 1);
        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, kPriority, 3);

        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(kOutPort));
        Ptr<UbIngressQueue> selected = allocator->SelectNextIngressQueue(port);

        NS_TEST_ASSERT_MSG_NE(selected, nullptr, "Materialized on-demand VOQs should be selectable");
        NS_TEST_ASSERT_MSG_EQ(selected->GetInPortId(),
                              3u,
                              "RR phase should be computed over ingress queue slots");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDwrrAllocatorPreservesIngressQueueSlotsAcrossVlTest : public TestCase
{
  public:
    UbDwrrAllocatorPreservesIngressQueueSlotsAcrossVlTest()
        : TestCase("UnifiedBus - DWRR allocator preserves ingress queue slots across VLs")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::NONE));
        Config::SetDefault("ns3::UbSwitch::VlScheduler", StringValue("DWRR"));

        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_SWITCH, 4);

        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        Ptr<UbDwrrAllocator> allocator = DynamicCast<UbDwrrAllocator>(sw->GetAllocator());
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Switch allocator should be DWRR");

        constexpr uint32_t kOutPort = 2;
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(kOutPort));

        uint8_t credits[16] = {};
        credits[0] = 1;
        sw->SendControlFrame(UbDataLink::GenControlCreditPacket(credits), kOutPort);
        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, 1, 1);
        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, 1, 3);
        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, 2, 0);

        Ptr<UbIngressQueue> selected = allocator->SelectNextIngressQueue(port);
        NS_TEST_ASSERT_MSG_NE(selected, nullptr, "DWRR should select the control VL first");
        NS_TEST_ASSERT_MSG_EQ(selected->GetIngressPriority(),
                              0u,
                              "VL0 control queue should keep its DWRR turn");
        NS_TEST_ASSERT_MSG_EQ(selected->GetInPortId(),
                              kOutPort,
                              "Control frames use the output port as their ingress queue slot");
        NS_TEST_ASSERT_MSG_EQ(selected->IsControlFrame(),
                              true,
                              "Selected VL0 queue should retain control-frame identity");

        selected = allocator->SelectNextIngressQueue(port);
        NS_TEST_ASSERT_MSG_NE(selected, nullptr, "DWRR should advance from VL0 to VL1");
        NS_TEST_ASSERT_MSG_EQ(selected->GetIngressPriority(),
                              1u,
                              "The second DWRR turn should inspect VL1");
        NS_TEST_ASSERT_MSG_EQ(selected->GetInPortId(),
                              3u,
                              "VL1 should start from the stable ingress queue slot after outPort phase");

        selected = allocator->SelectNextIngressQueue(port);
        NS_TEST_ASSERT_MSG_NE(selected, nullptr, "DWRR should advance from VL1 to VL2");
        NS_TEST_ASSERT_MSG_EQ(selected->GetIngressPriority(),
                              2u,
                              "The third DWRR turn should inspect VL2");
        NS_TEST_ASSERT_MSG_EQ(selected->GetInPortId(),
                              0u,
                              "VL2 should wrap to the available ingress queue slot");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbDwrrAllocatorKeepsNonVoqQueuesAfterVoqSlotsTest : public TestCase
{
  public:
    UbDwrrAllocatorKeepsNonVoqQueuesAfterVoqSlotsTest()
        : TestCase("UnifiedBus - DWRR allocator keeps non-VOQ queues after VOQ slots")
    {
    }

    void DoRun() override
    {
        Config::Reset();
        Config::SetDefault("ns3::UbSwitch::FlowControl", EnumValue(FcType::NONE));
        Config::SetDefault("ns3::UbSwitch::VlScheduler", StringValue("DWRR"));

        Ptr<Node> node = CreateObject<Node>(0);
        InitNode(node, UB_SWITCH, 4);

        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        Ptr<UbDwrrAllocator> allocator = DynamicCast<UbDwrrAllocator>(sw->GetAllocator());
        NS_TEST_ASSERT_MSG_NE(allocator, nullptr, "Switch allocator should be DWRR");

        constexpr uint32_t kOutPort = 2;
        constexpr uint32_t kPriority = 3;
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(kOutPort));

        Ptr<UbTestIngressQueue> limitedTp = CreateObject<UbTestIngressQueue>();
        limitedTp->PushPacket(64);
        limitedTp->SetLimited(true);
        sw->RegisterTpWithAllocator(limitedTp, kOutPort, kPriority);

        Ptr<UbTestIngressQueue> readyTp = CreateObject<UbTestIngressQueue>();
        readyTp->PushPacket(64);
        sw->RegisterTpWithAllocator(readyTp, kOutPort, kPriority);

        Ptr<UbIngressQueue> selected = allocator->SelectNextIngressQueue(port);
        NS_TEST_ASSERT_MSG_EQ(selected,
                              DynamicCast<UbIngressQueue>(readyTp),
                              "DWRR should skip a limited non-VOQ queue and choose the next non-VOQ slot");
        NS_TEST_ASSERT_MSG_EQ(selected->GetIngressPriority(),
                              kPriority,
                              "Selected non-VOQ queue should stay in the requested VL");
        NS_TEST_ASSERT_MSG_EQ(selected->GetInPortId(),
                              kOutPort,
                              "TP queues use the output port as their ingress identity");

        sw->PushPacketToVoq(Create<Packet>(64), kOutPort, kPriority, 3);

        selected = allocator->SelectNextIngressQueue(port);
        NS_TEST_ASSERT_MSG_NE(selected, nullptr, "A later materialized VOQ should be selectable");
        NS_TEST_ASSERT_MSG_EQ(static_cast<int>(selected->GetIngressQueueType()),
                              static_cast<int>(IngressQueueType::VOQ),
                              "VOQ slots should stay before non-VOQ queue slots");
        NS_TEST_ASSERT_MSG_EQ(selected->GetInPortId(),
                              3u,
                              "The materialized VOQ should retain its ingress port slot");

        selected = allocator->SelectNextIngressQueue(port);
        NS_TEST_ASSERT_MSG_EQ(selected,
                              DynamicCast<UbIngressQueue>(readyTp),
                              "After the VOQ slot, DWRR should still skip the limited TP and wrap to ready TP");

        Simulator::Destroy();
        Config::Reset();
    }
};

class UbPacketQueueInlineStoragePreservesFifoTest : public TestCase
{
  public:
    UbPacketQueueInlineStoragePreservesFifoTest()
        : TestCase("UnifiedBus - UbPacketQueue inline storage preserves FIFO")
    {
    }

    void DoRun() override
    {
        Ptr<UbPacketQueue> queue = CreateObject<UbPacketQueue>();
        queue->SetInPortId(0);
        queue->SetOutPortId(1);
        queue->SetIngressPriority(1);

        NS_TEST_ASSERT_MSG_EQ(queue->IsEmpty(), true, "New packet queue should be empty");

        Simulator::Schedule(NanoSeconds(10), [queue]() { queue->Push(Create<Packet>(101)); });
        Simulator::Schedule(NanoSeconds(20), [queue]() { queue->Push(Create<Packet>(202)); });
        Simulator::Schedule(NanoSeconds(30), [queue]() { queue->Push(Create<Packet>(303)); });
        Simulator::Run();

        NS_TEST_ASSERT_MSG_EQ(queue->IsEmpty(), false, "Scheduled pushed packets should be visible");
        NS_TEST_ASSERT_MSG_EQ(queue->Front()->GetSize(),
                              101u,
                              "Inline front packet should be readable");
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacketSize(),
                              101u,
                              "Allocator size pre-check should read the inline front packet");
        NS_TEST_ASSERT_MSG_EQ(queue->GetHeadArrivalTime(),
                              NanoSeconds(10),
                              "Head arrival time should be the first packet arrival time");

        queue->Pop();
        NS_TEST_ASSERT_MSG_EQ(queue->Front()->GetSize(),
                              202u,
                              "Direct Pop should promote the first overflow packet");
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacketSize(),
                              202u,
                              "Allocator size pre-check should read the promoted overflow packet");
        NS_TEST_ASSERT_MSG_EQ(queue->GetHeadArrivalTime(),
                              NanoSeconds(30),
                              "Promoting an overflow packet should preserve the old Pop timestamp contract");
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacket()->GetSize(),
                              202u,
                              "Second pop should return the first overflow packet");
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacket()->GetSize(),
                              303u,
                              "Third pop should return the second overflow packet");
        NS_TEST_ASSERT_MSG_EQ(queue->IsEmpty(), true, "Queue should be empty after all pops");

        queue->Push(Create<Packet>(404));
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacket()->GetSize(),
                              404u,
                              "Queue should be reusable after draining");
        NS_TEST_ASSERT_MSG_EQ(queue->IsEmpty(), true, "Queue should drain after reuse");

        Simulator::Destroy();
    }
};

class UbSmallFifoQueueReleasesHeapWhenDrainedTest : public TestCase
{
  public:
    UbSmallFifoQueueReleasesHeapWhenDrainedTest()
        : TestCase("UnifiedBus - UbSmallFifoQueue keeps small FIFO inline and releases heap")
    {
    }

    void DoRun() override
    {
        UbSmallFifoQueue<uint32_t, 2> queue;

        NS_TEST_ASSERT_MSG_EQ(queue.empty(), true, "New small FIFO should be empty");
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::IsUsingHeap(queue),
                              false,
                              "New small FIFO should start in inline storage");
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::HeapCapacity(queue),
                              0u,
                              "New small FIFO should not allocate heap storage");

        queue.push(1);
        queue.push(2);
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::IsUsingHeap(queue),
                              false,
                              "Inline capacity should not allocate heap storage");
        NS_TEST_ASSERT_MSG_EQ(queue.front(), 1u, "FIFO front should be the oldest inline item");

        queue.pop();
        queue.push(3);
        NS_TEST_ASSERT_MSG_EQ(queue.front(), 2u, "Inline ring wrap should preserve FIFO order");
        queue.pop();
        NS_TEST_ASSERT_MSG_EQ(queue.front(), 3u, "Wrapped inline item should remain readable");
        queue.pop();
        NS_TEST_ASSERT_MSG_EQ(queue.empty(), true, "Inline-only queue should drain cleanly");
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::IsUsingHeap(queue),
                              false,
                              "Drained inline-only queue should still use inline storage");

        queue.push(10);
        queue.push(20);
        queue.push(30);
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::IsUsingHeap(queue),
                              true,
                              "Exceeding inline capacity should promote to heap ring storage");
        NS_TEST_ASSERT_MSG_GT(UbSmallFifoQueueTestAccess::HeapCapacity(queue),
                              0u,
                              "Promoted queue should own heap storage");

        NS_TEST_ASSERT_MSG_EQ(queue.front(), 10u, "Heap queue should retain FIFO order");
        queue.pop();
        queue.pop();
        queue.push(40);
        queue.push(50);
        queue.push(60);

        NS_TEST_ASSERT_MSG_EQ(queue.size(), 4u, "Heap ring should track wrapped size");
        const std::array<uint32_t, 4> expected = {30, 40, 50, 60};
        for (uint32_t value : expected)
        {
            NS_TEST_ASSERT_MSG_EQ(queue.front(),
                                  value,
                                  "Heap ring wrap should preserve FIFO order");
            queue.pop();
        }

        NS_TEST_ASSERT_MSG_EQ(queue.empty(), true, "Queue should be empty after draining heap");
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::IsUsingHeap(queue),
                              false,
                              "Draining a heap-backed queue should return to inline storage");
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::HeapCapacity(queue),
                              0u,
                              "Draining a heap-backed queue should release heap capacity");

        queue.push(70);
        NS_TEST_ASSERT_MSG_EQ(UbSmallFifoQueueTestAccess::IsUsingHeap(queue),
                              false,
                              "Reuse after draining should start inline again");
        NS_TEST_ASSERT_MSG_EQ(queue.front(), 70u, "Queue should be reusable after heap release");
    }
};

class UbCtpHeaderEncodeDecodeTest : public TestCase
{
  public:
    UbCtpHeaderEncodeDecodeTest()
        : TestCase("UnifiedBus - CTPH round-trips compact fields")
    {
    }

  private:
    void DoRun() override
    {
        UbCtpHeader header;
        header.SetTPOpcode(CtpOpcode::CTP_DATA);
        header.SetPadding(2);
        header.SetNlp(UB_CTPH_NLP_COMPACT_TAH);

        NS_TEST_ASSERT_MSG_EQ(header.GetSerializedSize(), 1, "CTPH is one byte");

        Ptr<Packet> packet = Create<Packet>();
        packet->AddHeader(header);

        UbCtpHeader decoded;
        packet->RemoveHeader(decoded);

        NS_TEST_ASSERT_MSG_EQ(decoded.GetTPOpcode(),
                              static_cast<uint8_t>(CtpOpcode::CTP_DATA),
                              "opcode round trip");
        NS_TEST_ASSERT_MSG_EQ(decoded.GetPadding(), 2, "padding round trip");
        NS_TEST_ASSERT_MSG_EQ(decoded.GetNlp(), UB_CTPH_NLP_COMPACT_TAH, "NLP round trip");
    }
};

class UbCtpTransactionContextHolTest : public TestCase
{
  public:
    UbCtpTransactionContextHolTest()
        : TestCase("UnifiedBus - CTP TAACK HOL blocks transaction admission")
    {
    }

  private:
    void DoRun() override
    {
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 0,
                           .dstNodeId = 1,
                           .dstEntityId = 0,
                           .vl = 7};
        UbCtpTransactionContext context(key);

        NS_TEST_ASSERT_MSG_EQ(context.TryAdmit(0), true, "seq 0 admitted");
        NS_TEST_ASSERT_MSG_EQ(context.TryAdmit(1), true, "seq 1 admitted");
        context.MarkTaAck(1);
        NS_TEST_ASSERT_MSG_EQ(context.GetCompleteUna(), 0, "out-of-order TAACK does not advance");
        context.MarkTaAck(0);
        NS_TEST_ASSERT_MSG_EQ(context.GetCompleteUna(), 2, "contiguous TAACK advances");
    }
};

class UbCtpTransactionContextAckWindowAdmissionTest : public TestCase
{
  public:
    UbCtpTransactionContextAckWindowAdmissionTest()
        : TestCase("UnifiedBus - CTP transaction admission is bounded by ack bitmap window")
    {
    }

  private:
    void DoRun() override
    {
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 0,
                           .dstNodeId = 1,
                           .dstEntityId = 0,
                           .vl = 7};
        UbCtpTransactionContext context(key);

        for (uint32_t sequence = 0; sequence < UB_JETTY_TASSN_OOO_THRESHOLD; ++sequence)
        {
            NS_TEST_ASSERT_MSG_EQ(context.TryAdmit(sequence),
                                  true,
                                  "Sequence inside the ack bitmap window should be admitted");
        }

        NS_TEST_ASSERT_MSG_EQ(context.TryAdmit(UB_JETTY_TASSN_OOO_THRESHOLD),
                              false,
                              "A full ack bitmap window should block the next transaction");

        context.MarkTaAck(0);

        NS_TEST_ASSERT_MSG_EQ(context.TryAdmit(UB_JETTY_TASSN_OOO_THRESHOLD),
                              true,
                              "Acking the base transaction should reopen one admission slot");
    }
};

class UbCtpServiceDoesNotCreateRtpTpTest : public TestCase
{
  public:
    UbCtpServiceDoesNotCreateRtpTpTest()
        : TestCase("UnifiedBus - CTP context and send path do not create RTP transport channels")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();

        const uint32_t initialTransportCount = controller->GetTransportCountForTest();
        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        service->GetOrCreateTransactionContext(key);
        NS_TEST_ASSERT_MSG_EQ(controller->GetTransportCountForTest(),
                              initialTransportCount,
                              "Creating a CTP context must not allocate RTP transport channels");

        Ptr<UbWqeSegment> segment = CreateCtpWriteSegment(0);
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(segment, key), true, "CTP send should admit seq 0");
        NS_TEST_ASSERT_MSG_EQ(controller->GetTransportCountForTest(),
                              initialTransportCount,
                              "Sending a CTP segment must not allocate RTP transport channels");

        Simulator::Destroy();
    }
};

class UbCtpPortHintsDoNotCreateEntityStateTest : public TestCase
{
  public:
    UbCtpPortHintsDoNotCreateEntityStateTest()
        : TestCase("UnifiedBus - CTP port hints do not create entity state")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};

        service->SetUseShortestPaths(true);
        service->SetUsePacketSpray(false);
        service->SetSourcePortHint(key, 0);
        service->SetDestinationPortHint(key, 3);

        NS_TEST_ASSERT_MSG_EQ(service->GetEntityStateCount(),
                              0u,
                              "routing policy and port hints must not create transaction state");

        service->ClearSourcePortHint(key);
        service->ClearDestinationPortHint(key);
        service->GetOrCreateTransactionContext(key);

        NS_TEST_ASSERT_MSG_EQ(service->GetEntityStateCount(),
                              1u,
                              "entity state count should track transaction contexts");

        Simulator::Destroy();
    }
};

class UbCtpBuildDataPacketHeaderTest : public TestCase
{
  public:
    UbCtpBuildDataPacketHeaderTest()
        : TestCase("UnifiedBus - CTP data packet builds reference compact CTP header stack")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        service->SetSourcePortHint(key, 2);
        service->SetDestinationPortHint(key, 3);
        Ptr<Packet> packet = service->BuildDataPacket(CreateCtpWriteSegment(0), key);
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              100u,
                              "64B CTP write should carry the 36B reference compact header stack");

        UbDatalinkPacketHeader datalinkHeader;
        packet->RemoveHeader(datalinkHeader);
        NS_TEST_ASSERT_MSG_EQ(datalinkHeader.GetConfig(),
                              static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16),
                              "CTP data packet should use CNA16 datalink config");

        UbCna16NetworkHeader cnaHeader;
        packet->RemoveHeader(cnaHeader);
        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetNlp(), UB_CNA_NLP_CTPH, "CNA16 NLP should point to CTPH");
        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetServiceLevel(), 7, "CNA16 service level should carry CTP VL");
        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetScna(),
                              static_cast<uint16_t>(NodeIdToCna16(key.srcNodeId)),
                              "CTP SCNA should use the primary source CNA");
        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetDcna(),
                              static_cast<uint16_t>(NodeIdToCna16(key.dstNodeId, 3)),
                              "CTP destination port hint should be encoded in DCNA");

        UbCtpHeader ctpHeader;
        packet->RemoveHeader(ctpHeader);
        NS_TEST_ASSERT_MSG_EQ(ctpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(CtpOpcode::CTP_DATA),
                              "CTPH opcode should be CTP_DATA");
        NS_TEST_ASSERT_MSG_EQ(ctpHeader.GetNlp(),
                              UB_CTPH_NLP_UPI16_EID40_TAH,
                              "CTPH NLP should point to UPI/EID/TAH");

        UbCompactUpiHeader upiHeader;
        packet->RemoveHeader(upiHeader);
        UbCompactEidHeader eidHeader;
        packet->RemoveHeader(eidHeader);
        NS_TEST_ASSERT_MSG_EQ(eidHeader.GetSourceEid(),
                              key.srcEntityId,
                              "compact EID should carry source entity");
        NS_TEST_ASSERT_MSG_EQ(eidHeader.GetDestinationEid(),
                              key.dstEntityId,
                              "compact EID should carry destination entity");

        UbCompactTransactionHeader compactTah;
        packet->RemoveHeader(compactTah);
        NS_TEST_ASSERT_MSG_EQ(compactTah.GetTaOpcode(),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_WRITE),
                              "compact TAH opcode should come from the WQE segment");
        NS_TEST_ASSERT_MSG_EQ(compactTah.GetIniTaSsn(), 0, "compact TAH should carry TA SSN");

        UbCompactMAExtTah maHeader;
        packet->RemoveHeader(maHeader);
        NS_TEST_ASSERT_MSG_EQ(maHeader.GetLength(),
                              0u,
                              "64B compact MA length should encode one 64B unit");
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(), 64, "payload size should remain intact");

        Simulator::Destroy();
    }
};

class UbCtpBuildResponsePacketTaAckTest : public TestCase
{
  public:
    UbCtpBuildResponsePacketTaAckTest()
        : TestCase("UnifiedBus - CTP TAACK response uses compact ACK transaction header")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = 1,
                           .srcEntityId = 11,
                           .dstNodeId = 0,
                           .dstEntityId = 10,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);
        service->SetDestinationPortHint(key, 0);

        Ptr<Packet> packet =
            service->BuildResponsePacketForTest(CreateCtpTaAckResponseSegment(3), key);
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(),
                              24u,
                              "CTP TAACK should carry the 24B reference compact header stack");

        UbDatalinkPacketHeader datalinkHeader;
        packet->RemoveHeader(datalinkHeader);
        NS_TEST_ASSERT_MSG_EQ(datalinkHeader.GetConfig(),
                              static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_CNA16),
                              "CTP response packet should use CNA16 datalink config");

        UbCna16NetworkHeader cnaHeader;
        packet->RemoveHeader(cnaHeader);
        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetNlp(),
                              UB_CNA_NLP_CTPH,
                              "TAACK response CNA16 NLP should point to CTPH");

        UbCtpHeader ctpHeader;
        packet->RemoveHeader(ctpHeader);
        NS_TEST_ASSERT_MSG_EQ(ctpHeader.GetTPOpcode(),
                              static_cast<uint8_t>(CtpOpcode::CTP_DATA),
                              "TAACK is a transaction response carried by CTP_DATA");
        NS_TEST_ASSERT_MSG_EQ(ctpHeader.GetNlp(),
                              UB_CTPH_NLP_UPI16_EID40_TAH,
                              "TAACK response CTPH NLP should point to UPI/EID/TAH");

        UbCompactUpiHeader upiHeader;
        packet->RemoveHeader(upiHeader);
        UbCompactEidHeader eidHeader;
        packet->RemoveHeader(eidHeader);
        NS_TEST_ASSERT_MSG_EQ(eidHeader.GetSourceEid(),
                              key.srcEntityId,
                              "TAACK compact EID should carry source entity");
        NS_TEST_ASSERT_MSG_EQ(eidHeader.GetDestinationEid(),
                              key.dstEntityId,
                              "TAACK compact EID should carry destination entity");

        UbCompactAckTransactionHeader compactAck;
        packet->RemoveHeader(compactAck);
        NS_TEST_ASSERT_MSG_EQ(compactAck.GetTaOpcode(),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK),
                              "TAACK response should carry compact ACK TA opcode");
        NS_TEST_ASSERT_MSG_EQ(compactAck.GetIniTaSsn(),
                              3u,
                              "TAACK response should acknowledge the request TASSN");
        NS_TEST_ASSERT_MSG_EQ(packet->GetSize(), 0u, "TAACK response should have no payload");

        Simulator::Destroy();
    }
};

class UbCtpLocalDataPacketGeneratesTaAckTest : public TestCase
{
  public:
    UbCtpLocalDataPacketGeneratesTaAckTest()
        : TestCase("UnifiedBus - local CTP data packet generates a compact TAACK")
    {
    }

  private:
    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbCtpTransportService> receiverService =
            topo.receiver->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey requestKey{.srcNodeId = topo.sender->GetId(),
                                  .srcEntityId = 0,
                                  .dstNodeId = topo.receiver->GetId(),
                                  .dstEntityId = 0,
                                  .vl = 7};
        Ptr<Packet> dataPacket =
            receiverService->BuildDataPacket(CreateCtpWriteSegment(0), requestKey);

        topo.receiver->GetObject<UbSwitch>()->SwitchHandlePacket(topo.receiverPort, dataPacket);

        UbCtpEntityKey ackKey{.srcNodeId = topo.receiver->GetId(),
                              .srcEntityId = 0,
                              .dstNodeId = topo.sender->GetId(),
                              .dstEntityId = 0,
                              .vl = 7};
        NS_TEST_ASSERT_MSG_EQ(receiverService->GetQueuedPacketCountForTest(ackKey),
                              1u,
                              "local CTP data sink should enqueue a TAACK response");

        Ptr<UbCompactTransportChannel> ackQueue =
            receiverService->GetOrCreateQueue(ackKey, topo.receiverPort->GetIfIndex(), ackKey.vl);
        Ptr<Packet> ackPacket = ackQueue->GetNextPacket();
        NS_TEST_ASSERT_MSG_NE(ackPacket, nullptr, "TAACK packet should be queued");

        UbDatalinkPacketHeader datalinkHeader;
        ackPacket->RemoveHeader(datalinkHeader);
        UbCna16NetworkHeader cnaHeader;
        ackPacket->RemoveHeader(cnaHeader);
        UbCtpHeader ctpHeader;
        ackPacket->RemoveHeader(ctpHeader);
        UbCompactUpiHeader upiHeader;
        ackPacket->RemoveHeader(upiHeader);
        UbCompactEidHeader eidHeader;
        ackPacket->RemoveHeader(eidHeader);
        UbCompactAckTransactionHeader compactAck;
        ackPacket->RemoveHeader(compactAck);

        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetScna(),
                              utils::NodeIdToCna16(topo.receiver->GetId()),
                              "TAACK SCNA should use the primary response source CNA");
        NS_TEST_ASSERT_MSG_EQ(cnaHeader.GetDcna(),
                              utils::NodeIdToCna16(topo.sender->GetId()),
                              "TAACK should target the original requester");
        NS_TEST_ASSERT_MSG_EQ(eidHeader.GetSourceEid(),
                              ackKey.srcEntityId,
                              "TAACK should encode the response source entity");
        NS_TEST_ASSERT_MSG_EQ(eidHeader.GetDestinationEid(),
                              ackKey.dstEntityId,
                              "TAACK should encode the response destination entity");
        NS_TEST_ASSERT_MSG_EQ(compactAck.GetTaOpcode(),
                              static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK),
                              "local CTP data sink should generate a TAACK opcode");
        NS_TEST_ASSERT_MSG_EQ(compactAck.GetIniTaSsn(),
                              0u,
                              "TAACK should acknowledge the request TASSN");

        Simulator::Destroy();
    }
};

class UbCtpTaAckInheritsPacketSprayRoutingPolicyTest : public TestCase
{
  public:
    UbCtpTaAckInheritsPacketSprayRoutingPolicyTest()
        : TestCase("UnifiedBus - CTP TAACK inherits packet-spray routing policy")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> receiver = CreateObject<Node>(0);
        InitNode(receiver, UB_DEVICE, 2);
        Ptr<UbCtpTransportService> receiverService =
            receiver->GetObject<UbController>()->GetCtpTransportService();

        Ptr<UbRoutingProcess> routing =
            receiver->GetObject<UbSwitch>()->GetRoutingProcess();
        routing->AddShortestRoute(NodeIdToIp(0).Get(), std::vector<uint16_t>{0, 1});

        Ptr<UbCtpTransportService> packetBuilder = CreateObject<UbCtpTransportService>();
        packetBuilder->SetUsePacketSpray(true);

        UbCtpEntityKey requestKey{.srcNodeId = 0,
                                  .srcEntityId = 0,
                                  .dstNodeId = 1,
                                  .dstEntityId = 0,
                                  .vl = 7};
        for (uint32_t taSsn = 0; taSsn < 16; ++taSsn)
        {
            NS_TEST_ASSERT_MSG_EQ(receiverService->HandleReceivedPacket(
                                      packetBuilder->BuildDataPacket(
                                          CreateCtpWriteSegment(taSsn),
                                          requestKey),
                                      0),
                                  true,
                                  "CTP data packet should generate a TAACK");
        }

        UbCtpEntityKey ackKey{.srcNodeId = 1,
                              .srcEntityId = 0,
                              .dstNodeId = 0,
                              .dstEntityId = 0,
                              .vl = 7};
        NS_TEST_ASSERT_MSG_GT(receiverService->GetQueuedPacketCountForTest(ackKey, 1, ackKey.vl),
                              0u,
                              "packet-spray TAACKs should not all be pinned to the receive port");

        Simulator::Destroy();
    }
};

class UbCtpCompactQueueCarriesLocalGenerationMetadataTest : public TestCase
{
  public:
    UbCtpCompactQueueCarriesLocalGenerationMetadataTest()
        : TestCase("UnifiedBus - CTP compact queue carries local generation metadata")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbCtpTransportService> service =
            node->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 0,
                           .dstNodeId = 1,
                           .dstEntityId = 0,
                           .vl = 7};

        Ptr<UbCompactTransportChannel> queue =
            service->GetOrCreateQueue(key, 0, key.vl);

        NS_TEST_ASSERT_MSG_EQ(queue->GetInPortId(),
                              0u,
                              "CTP compact queue should record its source port as ingress port");
        NS_TEST_ASSERT_MSG_EQ(queue->GetOutPortId(),
                              0u,
                              "CTP compact queue should record its selected output port");
        NS_TEST_ASSERT_MSG_EQ(queue->GetIngressPriority(),
                              static_cast<uint32_t>(key.vl),
                              "CTP compact queue should use the entity VL as ingress priority");
        NS_TEST_ASSERT_MSG_EQ(queue->IsGeneratedDataPacket(),
                              true,
                              "CTP compact queue packets should be treated as locally generated");

        Simulator::Destroy();
    }
};

class UbCtpWireTaAckCompletesJettyTest : public TestCase
{
  public:
    UbCtpWireTaAckCompletesJettyTest()
        : TestCase("UnifiedBus - CTP wire TAACK completes the source Jetty")
    {
    }

  private:
    static constexpr uint32_t kJettyNum = 3;
    static constexpr uint32_t kTaskId = 77;

    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();
        Ptr<UbFunction> function = controller->GetUbFunction();

        function->CreateJetty(0, 1, kJettyNum);
        Ptr<UbJetty> jetty = function->GetJetty(kJettyNum);
        m_completed = false;
        jetty->SetClientCallback(MakeCallback(&UbCtpWireTaAckCompletesJettyTest::OnTaskCompleted,
                                              this));
        Ptr<UbWqe> wqe =
            function->CreateWqe(0, 1, UB_MTU_BYTE, kTaskId, TaOpcode::TA_OPCODE_WRITE);
        function->PushWqeToJetty(wqe, kJettyNum);
        Ptr<UbWqeSegment> segment = jetty->GetNextWqeSegment();

        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 0,
                           .dstNodeId = 1,
                           .dstEntityId = 0,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(segment, key),
                              true,
                              "test setup should make one CTP transaction outstanding");

        UbCtpEntityKey ackWireKey{.srcNodeId = 1,
                                  .srcEntityId = 0,
                                  .dstNodeId = 0,
                                  .dstEntityId = 0,
                                  .vl = 7};
        Ptr<Packet> ackPacket =
            service->BuildResponsePacketForTest(CreateCtpTaAckResponseSegment(0), ackWireKey);

        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(ackPacket),
                              true,
                              "wire TAACK should be accepted by the CTP service");
        NS_TEST_ASSERT_MSG_EQ(jetty->GetTaSsnSndUnaForTest(),
                              1u,
                              "wire TAACK should advance the source Jetty ACK point");
        NS_TEST_ASSERT_MSG_EQ(m_completed,
                              true,
                              "wire TAACK should fire the Jetty completion callback");

        Simulator::Destroy();
    }

    void OnTaskCompleted(uint32_t completedTaskId, uint32_t completedJettyNum)
    {
        if (completedTaskId == kTaskId && completedJettyNum == kJettyNum)
        {
            m_completed = true;
        }
    }

    bool m_completed{false};
};

class UbCtpOutOfOrderWireTaAckCompletesContiguousJettyRangeTest : public TestCase
{
  public:
    UbCtpOutOfOrderWireTaAckCompletesContiguousJettyRangeTest()
        : TestCase("UnifiedBus - CTP wire TAACK completes contiguous Jetty range after out-of-order ACK")
    {
    }

  private:
    static constexpr uint32_t kJettyNum = 4;

    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();
        Ptr<UbFunction> function = controller->GetUbFunction();

        function->CreateJetty(0, 1, kJettyNum);
        Ptr<UbJetty> jetty = function->GetJetty(kJettyNum);
        jetty->SetClientCallback(
            MakeCallback(&UbCtpOutOfOrderWireTaAckCompletesContiguousJettyRangeTest::
                             OnTaskCompleted,
                         this));
        Ptr<UbWqe> wqe =
            function->CreateWqe(0,
                                1,
                                2u * UB_WQE_TA_SEGMENT_BYTE,
                                kTaskId,
                                TaOpcode::TA_OPCODE_WRITE);
        function->PushWqeToJetty(wqe, kJettyNum);

        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 0,
                           .dstNodeId = 1,
                           .dstEntityId = 0,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        Ptr<UbWqeSegment> firstSegment = jetty->GetNextWqeSegment();
        Ptr<UbWqeSegment> secondSegment = jetty->GetNextWqeSegment();
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(firstSegment, key),
                              true,
                              "first CTP transaction should be outstanding");
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(secondSegment, key),
                              true,
                              "second CTP transaction should be outstanding");

        UbCtpEntityKey ackWireKey{.srcNodeId = 1,
                                  .srcEntityId = 0,
                                  .dstNodeId = 0,
                                  .dstEntityId = 0,
                                  .vl = 7};
        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(
                                  service->BuildResponsePacketForTest(
                                      CreateCtpTaAckResponseSegment(1),
                                      ackWireKey)),
                              true,
                              "out-of-order TAACK should be accepted");
        NS_TEST_ASSERT_MSG_EQ(jetty->GetTaSsnSndUnaForTest(),
                              0u,
                              "out-of-order TAACK must wait for the base TASSN");
        NS_TEST_ASSERT_MSG_EQ(m_completed,
                              false,
                              "WQE should not complete while the base TASSN is missing");

        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(
                                  service->BuildResponsePacketForTest(
                                      CreateCtpTaAckResponseSegment(0),
                                      ackWireKey)),
                              true,
                              "base TAACK should complete the contiguous range");
        NS_TEST_ASSERT_MSG_EQ(jetty->GetTaSsnSndUnaForTest(),
                              2u,
                              "base TAACK should advance through the already ACKed TASSN");
        NS_TEST_ASSERT_MSG_EQ(m_completed,
                              true,
                              "WQE should complete after both logical TASSNs are contiguous");

        Simulator::Destroy();
    }

    void OnTaskCompleted(uint32_t completedTaskId, uint32_t completedJettyNum)
    {
        if (completedTaskId == kTaskId && completedJettyNum == kJettyNum)
        {
            m_completed = true;
        }
    }

    static constexpr uint32_t kTaskId = 88;
    bool m_completed{false};
};

class UbCtpOutOfOrderWireTaAckTracesArrivalTest : public TestCase
{
  public:
    UbCtpOutOfOrderWireTaAckTracesArrivalTest()
        : TestCase("UnifiedBus - CTP out-of-order wire TAACK traces arrival immediately")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbController> controller = node->GetObject<UbController>();
        Ptr<UbFunction> function = controller->GetUbFunction();

        function->CreateJetty(0, 1, kJettyNum);
        Ptr<UbJetty> jetty = function->GetJetty(kJettyNum);
        Ptr<UbWqe> wqe =
            function->CreateWqe(0,
                                1,
                                2u * UB_WQE_TA_SEGMENT_BYTE,
                                kTaskId,
                                TaOpcode::TA_OPCODE_WRITE);
        function->PushWqeToJetty(wqe, kJettyNum);

        Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
        service->TraceConnectWithoutContext(
            "LastPacketACKsNotify",
            MakeCallback(&UbCtpOutOfOrderWireTaAckTracesArrivalTest::OnLastPacketAck, this));

        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 0,
                           .dstNodeId = 1,
                           .dstEntityId = 0,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        Ptr<UbWqeSegment> firstSegment = jetty->GetNextWqeSegment();
        Ptr<UbWqeSegment> secondSegment = jetty->GetNextWqeSegment();
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(firstSegment, key),
                              true,
                              "first CTP transaction should be outstanding");
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(secondSegment, key),
                              true,
                              "second CTP transaction should be outstanding");

        UbCtpEntityKey ackWireKey{.srcNodeId = 1,
                                  .srcEntityId = 0,
                                  .dstNodeId = 0,
                                  .dstEntityId = 0,
                                  .vl = 7};
        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(
                                  service->BuildResponsePacketForTest(
                                      CreateCtpTaAckResponseSegment(1),
                                      ackWireKey)),
                              true,
                              "out-of-order TAACK should be accepted");
        NS_TEST_ASSERT_MSG_EQ(jetty->GetTaSsnSndUnaForTest(),
                              0u,
                              "out-of-order TAACK should not advance contiguous Jetty completion");
        NS_TEST_ASSERT_MSG_EQ(m_ackedTaSsn,
                              1u,
                              "out-of-order TAACK should trace the ACKed logical TASSN immediately");

        Simulator::Destroy();
    }

    void OnLastPacketAck(uint32_t,
                         uint32_t,
                         uint32_t,
                         uint32_t,
                         uint32_t,
                         uint32_t,
                         uint32_t,
                         uint32_t taSsn,
                         uint32_t,
                         uint32_t,
                         uint32_t)
    {
        m_ackedTaSsn = taSsn;
    }

    static constexpr uint32_t kJettyNum = 5;
    static constexpr uint32_t kTaskId = 89;
    uint32_t m_ackedTaSsn{std::numeric_limits<uint32_t>::max()};
};

class UbCtpCnpDoesNotAckTransactionTest : public TestCase
{
  public:
    UbCtpCnpDoesNotAckTransactionTest()
        : TestCase("UnifiedBus - CTP CNP records congestion without TAACK completion")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(key);

        NS_TEST_ASSERT_MSG_EQ(context->TryAdmit(0), true, "seq 0 should be outstanding");

        service->RecordCnpForTest(key);

        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              0u,
                              "CNP must not advance transaction ACK state");
        NS_TEST_ASSERT_MSG_EQ(service->GetCongestionStateCountForTest(),
                              1u,
                              "CNP should record one congestion signal state");

        Simulator::Destroy();
    }
};

class UbCtpTaAckCompletionHelperTest : public TestCase
{
  public:
    UbCtpTaAckCompletionHelperTest()
        : TestCase("UnifiedBus - CTP TAACK helper advances complete UNA while CNP does not")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(key);

        NS_TEST_ASSERT_MSG_EQ(context->TryAdmit(0), true, "seq 0 should be outstanding");
        NS_TEST_ASSERT_MSG_EQ(context->TryAdmit(1), true, "seq 1 should be outstanding");

        service->RecordCnpForTest(key);
        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              0u,
                              "CNP must not complete a transaction");

        service->CompleteFromTaAckForTest(key, 1);
        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              0u,
                              "out-of-order TAACK should wait for the base sequence");

        service->CompleteFromTaAckForTest(key, 0);
        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              2u,
                              "contiguous TAACKs should advance complete UNA");

        Simulator::Destroy();
    }
};

class UbCtpWireAckRequiresExactEntityContextTest : public TestCase
{
  public:
    UbCtpWireAckRequiresExactEntityContextTest()
        : TestCase("UnifiedBus - CTP wire TAACK does not fallback across entity contexts")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey entity5{.srcNodeId = 0,
                               .srcEntityId = 5,
                               .dstNodeId = 1,
                               .dstEntityId = 50,
                               .vl = 7};
        UbCtpEntityKey entity7{.srcNodeId = 0,
                               .srcEntityId = 7,
                               .dstNodeId = 1,
                               .dstEntityId = 70,
                               .vl = 7};
        Ptr<UbCtpTransactionContext> entity5Context =
            service->GetOrCreateTransactionContext(entity5);
        service->GetOrCreateTransactionContext(entity7);
        NS_TEST_ASSERT_MSG_EQ(entity5Context->TryAdmit(0),
                              true,
                              "entity 5 should have one outstanding transaction");

        UbCtpEntityKey ackWireKey{.srcNodeId = 1,
                                  .srcEntityId = 0,
                                  .dstNodeId = 0,
                                  .dstEntityId = 0,
                                  .vl = 7};
        Ptr<Packet> ackPacket =
            service->BuildResponsePacketForTest(CreateCtpTaAckResponseSegment(0), ackWireKey);

        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(ackPacket),
                              false,
                              "wire TAACK without entity ids must not use node/VL fallback");
        NS_TEST_ASSERT_MSG_EQ(entity5Context->GetCompleteUna(),
                              0u,
                              "wire TAACK without entity ids must not complete entity 5");

        Simulator::Destroy();
    }
};

class UbCtpWireTaAckCompletesReverseEntityZeroContextTest : public TestCase
{
  public:
    UbCtpWireTaAckCompletesReverseEntityZeroContextTest()
        : TestCase("UnifiedBus - CTP wire TAACK completes reverse entity-zero context")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey requestKey{.srcNodeId = 0,
                                  .srcEntityId = 0,
                                  .dstNodeId = 1,
                                  .dstEntityId = 0,
                                  .vl = 7};
        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(requestKey);
        NS_TEST_ASSERT_MSG_EQ(context->TryAdmit(0),
                              true,
                              "entity-zero request context should admit sequence 0");

        UbCtpEntityKey responseWireKey{.srcNodeId = 1,
                                       .srcEntityId = 0,
                                       .dstNodeId = 0,
                                       .dstEntityId = 0,
                                       .vl = 7};
        Ptr<Packet> ackPacket =
            service->BuildResponsePacketForTest(CreateCtpTaAckResponseSegment(0), responseWireKey);

        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(ackPacket),
                              true,
                              "response-direction wire TAACK should match request context");
        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              1u,
                              "request context should complete sequence 0");

        Simulator::Destroy();
    }
};

class UbCtpWireTaAckDoesNotCompleteNonzeroEntityWithoutWireEntityTest : public TestCase
{
  public:
    UbCtpWireTaAckDoesNotCompleteNonzeroEntityWithoutWireEntityTest()
        : TestCase("UnifiedBus - CTP wire TAACK cannot complete nonzero entity without wire entity")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey entity5{.srcNodeId = 0,
                               .srcEntityId = 5,
                               .dstNodeId = 1,
                               .dstEntityId = 5,
                               .vl = 7};
        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(entity5);
        NS_TEST_ASSERT_MSG_EQ(context->TryAdmit(0),
                              true,
                              "nonzero entity context should have one outstanding transaction");

        UbCtpEntityKey responseWireKey{.srcNodeId = 1,
                                       .srcEntityId = 0,
                                       .dstNodeId = 0,
                                       .dstEntityId = 0,
                                       .vl = 7};
        Ptr<Packet> ackPacket =
            service->BuildResponsePacketForTest(CreateCtpTaAckResponseSegment(0), responseWireKey);

        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(ackPacket),
                              false,
                              "wire TAACK without entity ids must not complete nonzero entity");
        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              0u,
                              "nonzero entity context should remain incomplete");

        Simulator::Destroy();
    }
};

class UbCtpWireTaAckUnwrapsLogicalTassnTest : public TestCase
{
  public:
    UbCtpWireTaAckUnwrapsLogicalTassnTest()
        : TestCase("UnifiedBus - CTP wire TAACK unwraps logical TASSN")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey requestKey{.srcNodeId = 0,
                                  .srcEntityId = 0,
                                  .dstNodeId = 1,
                                  .dstEntityId = 0,
                                  .vl = 7};
        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(requestKey);
        context->SetWindowForTest(65536, 65537);

        UbCtpEntityKey responseWireKey{.srcNodeId = 1,
                                       .srcEntityId = 0,
                                       .dstNodeId = 0,
                                       .dstEntityId = 0,
                                       .vl = 7};
        Ptr<Packet> ackPacket =
            service->BuildResponsePacketForTest(CreateCtpTaAckResponseSegment(65536),
                                                responseWireKey);

        NS_TEST_ASSERT_MSG_EQ(service->HandleReceivedPacket(ackPacket),
                              true,
                              "wire TAACK should unwrap low 16 bits into outstanding range");
        NS_TEST_ASSERT_MSG_EQ(context->GetCompleteUna(),
                              65537u,
                              "logical TASSN 65536 should complete from wire ACK 0");

        Simulator::Destroy();
    }
};

class UbCtpSendSegmentFragmentsPayloadTest : public TestCase
{
  public:
    UbCtpSendSegmentFragmentsPayloadTest()
        : TestCase("UnifiedBus - CTP SendSegment fragments payload into MTU-sized packets")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbCtpTransportService> service = node->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        constexpr uint32_t payloadBytes = 4u * 1024u * 1024u;
        constexpr uint32_t expectedPackets = payloadBytes / UB_MTU_BYTE;
        Ptr<UbWqeSegment> segment = CreateCtpWriteSegment(0);
        segment->SetSize(payloadBytes);
        segment->SetPayloadBytes(payloadBytes);
        segment->SetResLenBytes(payloadBytes);
        segment->SetCarrierBytes(payloadBytes);

        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(segment, key),
                              true,
                              "4MiB CTP segment should be accepted for fragmentation");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              expectedPackets,
                              "CTP should queue one packet per MTU-sized fragment");

        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(key);
        NS_TEST_ASSERT_MSG_EQ(context->GetSendNext(),
                              1u,
                              "fragmented WQE segment should consume one compact TA sequence");
        NS_TEST_ASSERT_MSG_EQ(context->GetOutstandingCount(),
                              1u,
                              "fragmented WQE segment should occupy one outstanding transaction slot");

        Simulator::Destroy();
    }
};

class UbCtpSendSegmentRemainderPayloadTest : public TestCase
{
  public:
    UbCtpSendSegmentRemainderPayloadTest()
        : TestCase("UnifiedBus - CTP SendSegment keeps a short remainder fragment payload")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbCtpTransportService> service = node->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        constexpr uint32_t payloadBytes = UB_MTU_BYTE + 123u;
        Ptr<UbWqeSegment> segment = CreateCtpWriteSegment(0);
        segment->SetSize(payloadBytes);
        segment->SetPayloadBytes(payloadBytes);
        segment->SetResLenBytes(payloadBytes);
        segment->SetCarrierBytes(payloadBytes);

        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(segment, key),
                              true,
                              "CTP segment with a remainder should be accepted");

        Ptr<UbCompactTransportChannel> queue =
            service->GetOrCreateQueue(key, 0, key.vl);
        NS_TEST_ASSERT_MSG_EQ(queue->GetQueuedPacketCountForTest(),
                              2u,
                              "payload slightly over MTU should produce two packets");

        Ptr<Packet> firstPacket = queue->GetNextPacket();
        Ptr<Packet> secondPacket = queue->GetNextPacket();
        NS_TEST_ASSERT_MSG_NE(firstPacket, nullptr, "first fragment packet should be present");
        NS_TEST_ASSERT_MSG_NE(secondPacket, nullptr, "second fragment packet should be present");

        const DecodedCtpCompactDataPacket first = DecodeCtpCompactDataPacket(firstPacket);
        const DecodedCtpCompactDataPacket second = DecodeCtpCompactDataPacket(secondPacket);
        NS_TEST_ASSERT_MSG_EQ(first.payloadBytes,
                              UB_MTU_BYTE,
                              "first fragment should carry one full MTU");
        NS_TEST_ASSERT_MSG_EQ(second.payloadBytes,
                              123u,
                              "second fragment should carry only the payload remainder");
        NS_TEST_ASSERT_MSG_EQ(first.taSsn,
                              second.taSsn,
                              "all fragments for one WQE segment should carry the same TA SSN");

        Simulator::Destroy();
    }
};

class UbCtpFragmentedSegmentDoesNotBlockNextTaSsnTest : public TestCase
{
  public:
    UbCtpFragmentedSegmentDoesNotBlockNextTaSsnTest()
        : TestCase("UnifiedBus - CTP fragmented segment leaves next logical TASSN admissible")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbCtpTransportService> service = node->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        Ptr<UbWqeSegment> first = CreateCtpWriteSegment(0);
        first->SetSize(16u * UB_MTU_BYTE);
        first->SetPayloadBytes(16u * UB_MTU_BYTE);
        first->SetResLenBytes(16u * UB_MTU_BYTE);
        first->SetCarrierBytes(16u * UB_MTU_BYTE);

        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(first, key),
                              true,
                              "first 64KiB WQE segment should be accepted");
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(CreateCtpWriteSegment(1), key),
                              true,
                              "next WQE segment should use TASSN 1 after fragmented TASSN 0");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              17u,
                              "64KiB segment plus the next segment should queue 17 packets");

        Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(key);
        NS_TEST_ASSERT_MSG_EQ(context->GetSendNext(),
                              2u,
                              "two WQE segments should occupy two logical transaction sequences");
        NS_TEST_ASSERT_MSG_EQ(context->GetOutstandingCount(),
                              2u,
                              "two WQE segments should occupy two outstanding slots");

        Simulator::Destroy();
    }
};

class UbCtpSendSegmentQueueTest : public TestCase
{
  public:
    UbCtpSendSegmentQueueTest()
        : TestCase("UnifiedBus - CTP SendSegment admits once and queues one packet")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<Node> node = BuildSinglePortCtpNode();
        Ptr<UbCtpTransportService> service = node->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};
        service->SetSourcePortHint(key, 0);

        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(CreateCtpWriteSegment(0), key),
                              true,
                              "first in-order CTP segment should be queued");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              1u,
                              "successful CTP send should enqueue exactly one packet");

        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(CreateCtpWriteSegment(0), key),
                              false,
                              "duplicate TA SSN should be rejected by TryAdmit");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key),
                              1u,
                              "rejected CTP segment must not enqueue a packet");

        Simulator::Destroy();
    }
};

#ifndef _WIN32
class UbCtpRejectedSegmentDoesNotCreateQueueOrRequireRouteTest : public TestCase
{
  public:
    UbCtpRejectedSegmentDoesNotCreateQueueOrRequireRouteTest()
        : TestCase("UnifiedBus - CTP rejected segment does not create queue or require route")
    {
    }

  private:
    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
            UbCtpEntityKey key{.srcNodeId = 0,
                               .srcEntityId = 10,
                               .dstNodeId = 1,
                               .dstEntityId = 11,
                               .vl = 7};

            Ptr<UbCtpTransactionContext> context = service->GetOrCreateTransactionContext(key);
            if (!context->TryAdmit(0))
            {
                std::_Exit(10);
            }

            if (service->SendSegment(CreateCtpWriteSegment(0), key))
            {
                std::_Exit(11);
            }
            if (service->GetQueuedPacketCountForTest(key) != 0)
            {
                std::_Exit(12);
            }
            if (service->GetRegisteredQueueCountForTest() != 0)
            {
                std::_Exit(13);
            }
            Simulator::Destroy();
        });

        NS_TEST_ASSERT_MSG_EQ(WIFEXITED(status),
                              1,
                              "Rejected duplicate CTP segment should return false without aborting");
        NS_TEST_ASSERT_MSG_EQ(WEXITSTATUS(status),
                              0,
                              "Rejected duplicate CTP segment subprocess should exit cleanly");
    }
};

class UbCtpPortHintRejectsUnencodableCna16PortTest : public TestCase
{
  public:
    UbCtpPortHintRejectsUnencodableCna16PortTest()
        : TestCase("UnifiedBus - CTP port hints reject unencodable CNA16 ports")
    {
    }

  private:
    void ExpectPortHintAbort(const std::function<void(Ptr<UbCtpTransportService>,
                                                      const UbCtpEntityKey&)>& setHint)
    {
        int status = RunInChildProcess([&setHint]() {
            Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
            UbCtpEntityKey key{.srcNodeId = 0,
                               .srcEntityId = 10,
                               .dstNodeId = 1,
                               .dstEntityId = 11,
                               .vl = 7};
            setHint(service, key);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Unencodable CTP port hint should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Unencodable CTP port hint should fail with SIGABRT");
    }

    void DoRun() override
    {
        ExpectPortHintAbort([](Ptr<UbCtpTransportService> service, const UbCtpEntityKey& key) {
            service->SetSourcePortHint(key, 15);
        });
        ExpectPortHintAbort([](Ptr<UbCtpTransportService> service, const UbCtpEntityKey& key) {
            service->SetDestinationPortHint(key, 15);
        });
        ExpectPortHintAbort([](Ptr<UbCtpTransportService> service, const UbCtpEntityKey& key) {
            service->SetDestinationPortHint(key, 16);
        });
    }
};
#endif

class UbCtpSendSegmentRegistersAndTriggersPortTest : public TestCase
{
  public:
    UbCtpSendSegmentRegistersAndTriggersPortTest()
        : TestCase("UnifiedBus - CTP SendSegment selects outPort, registers queue, and triggers allocator")
    {
    }

  private:
    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbCtpTransportService> service = topo.sender->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey key{.srcNodeId = topo.sender->GetId(),
                           .srcEntityId = 10,
                           .dstNodeId = topo.receiver->GetId(),
                           .dstEntityId = 11,
                           .vl = 7};
        const uint32_t outPort = topo.senderPort->GetIfIndex();

        service->SetSourcePortHint(key, outPort);
        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(CreateCtpWriteSegment(0), key),
                              true,
                              "CTP send should admit and enqueue seq 0");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key, outPort, key.vl),
                              1u,
                              "CTP send should enqueue on the selected outPort/priority queue");
        NS_TEST_ASSERT_MSG_EQ(service->GetRegisteredQueueCountForTest(),
                              1u,
                              "first CTP send should register exactly one compact queue");

        NS_TEST_ASSERT_MSG_EQ(service->SendSegment(CreateCtpWriteSegment(1), key),
                              true,
                              "CTP send should admit and enqueue seq 1");
        NS_TEST_ASSERT_MSG_EQ(service->GetQueuedPacketCountForTest(key, outPort, key.vl),
                              2u,
                              "second CTP send should reuse the selected outPort/priority queue");
        NS_TEST_ASSERT_MSG_EQ(service->GetRegisteredQueueCountForTest(),
                              1u,
                              "reusing a compact queue must not duplicate allocator registration");

        Simulator::Stop(NanoSeconds(100));
        Simulator::Run();
        NS_TEST_ASSERT_MSG_GT(topo.senderPort->GetTxBytes(),
                              0u,
                              "triggered allocator should move queued CTP packets to the source port");

        Simulator::Destroy();
    }
};

class UbSwitchLdstRoutingKeyKeepsLegacySourcePortTest : public TestCase
{
  public:
    UbSwitchLdstRoutingKeyKeepsLegacySourcePortTest()
        : TestCase("UnifiedBus - LDST routing key keeps legacy source port decoding")
    {
    }

  private:
    void DoRun() override
    {
        constexpr uint32_t srcNodeId = 7;
        constexpr uint32_t dstNodeId = 8;
        constexpr uint32_t sourcePortHint = 15;
        constexpr uint8_t priority = 6;
        constexpr uint8_t loadBalance = 3;

        Ptr<Packet> packet = Create<Packet>(64);
        UbDummyTransactionHeader dummyTah;
        dummyTah.SetTaOpcode(TaOpcode::TA_OPCODE_WRITE);
        packet->AddHeader(dummyTah);

        UbCna16NetworkHeader cnaHeader;
        cnaHeader.SetScna(static_cast<uint16_t>(NodeIdToCna16(srcNodeId, sourcePortHint)));
        cnaHeader.SetDcna(static_cast<uint16_t>(NodeIdToCna16(dstNodeId)));
        cnaHeader.SetLb(loadBalance);
        cnaHeader.SetServiceLevel(priority);
        cnaHeader.SetNlp(UB_CNA_NLP_RTPH);
        packet->AddHeader(cnaHeader);

        UbDataLink::GenPacketHeader(packet,
                                    false,
                                    false,
                                    priority,
                                    priority,
                                    true,
                                    true,
                                    UbDatalinkHeaderConfig::PACKET_UB_MEM);

        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        const RoutingKey rtKey = sw->GetLdstRoutingKeyForTest(packet);

        NS_TEST_ASSERT_MSG_EQ(rtKey.sip,
                              NodeIdToIp(srcNodeId, Cna16ToPortId(NodeIdToCna16(srcNodeId,
                                                                                 sourcePortHint)))
                                  .Get(),
                              "LDST routing key should preserve legacy CNA16 source port decoding");
        NS_TEST_ASSERT_MSG_EQ(rtKey.sport,
                              loadBalance,
                              "LDST routing key should continue to use CNA LB as sport hash salt");
    }
};

class UbSwitchClassifiesCtpCnaPacketTest : public TestCase
{
  public:
    UbSwitchClassifiesCtpCnaPacketTest()
        : TestCase("UnifiedBus - switch classifies CNA16 CTPH packets as CTP data")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = 0,
                           .srcEntityId = 10,
                           .dstNodeId = 1,
                           .dstEntityId = 11,
                           .vl = 7};

        Ptr<Packet> ctpPacket = service->BuildDataPacket(CreateCtpWriteSegment(0), key);
        NS_TEST_ASSERT_MSG_EQ(sw->GetPacketTypeForTest(ctpPacket),
                              UB_CTP_DATA_PACKET,
                              "CNA16 CTPH packet should classify as CTP data");

        Ptr<Packet> ctp24Packet = Create<Packet>(64);
        UbCtpHeader ctpHeader;
        ctpHeader.SetTPOpcode(CtpOpcode::CTP_DATA);
        ctpHeader.SetNlp(UB_CTPH_NLP_COMPACT_TAH);
        ctp24Packet->AddHeader(ctpHeader);
        UbCna24NetworkHeader cna24Header;
        cna24Header.SetScna(NodeIdToCna24(key.srcNodeId, 0));
        cna24Header.SetDcna(NodeIdToCna24(key.dstNodeId, 0));
        cna24Header.SetServiceLevel(key.vl);
        cna24Header.SetNlp(UB_CNA_NLP_CTPH);
        ctp24Packet->AddHeader(cna24Header);
        UbDataLink::GenPacketHeader(ctp24Packet,
                                    false,
                                    false,
                                    key.vl,
                                    key.vl,
                                    false,
                                    true,
                                    UbDatalinkHeaderConfig::PACKET_CNA24);
        NS_TEST_ASSERT_MSG_EQ(sw->GetPacketTypeForTest(ctp24Packet),
                              UB_CTP_DATA_PACKET,
                              "CNA24 CTPH packet should classify as CTP data");

        Ptr<Packet> ldstPacket = service->BuildDataPacket(CreateCtpWriteSegment(0), key);
        UbDatalinkPacketHeader dlHeader;
        UbCna16NetworkHeader cnaHeader;
        ldstPacket->RemoveHeader(dlHeader);
        ldstPacket->RemoveHeader(cnaHeader);
        cnaHeader.SetNlp(UB_CNA_NLP_RTPH);
        ldstPacket->AddHeader(cnaHeader);
        ldstPacket->AddHeader(dlHeader);
        NS_TEST_ASSERT_MSG_EQ(sw->GetPacketTypeForTest(ldstPacket),
                              UB_LDST_DATA_PACKET,
                              "CNA16 non-CTPH packet should preserve the LDST classification path");

        Simulator::Destroy();
    }
};

class UbSwitchCtpRoutingKeyMatchesReferenceCna16SourceDecodeTest : public TestCase
{
  public:
    UbSwitchCtpRoutingKeyMatchesReferenceCna16SourceDecodeTest()
        : TestCase("UnifiedBus - CTP routing key matches reference CNA16 source decode")
    {
    }

  private:
    void DoRun() override
    {
        UbCtpEntityKey key{.srcNodeId = 7,
                           .srcEntityId = 10,
                           .dstNodeId = 8,
                           .dstEntityId = 11,
                           .vl = 6};

        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        Ptr<Packet> packet = service->BuildDataPacket(CreateCtpWriteSegment(0), key);
        Ptr<Packet> view = packet->Copy();
        UbDatalinkPacketHeader datalinkHeader;
        UbCna16NetworkHeader cnaHeader;
        view->RemoveHeader(datalinkHeader);
        view->RemoveHeader(cnaHeader);

        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        const RoutingKey rtKey = sw->GetCtpRoutingKeyForTest(packet);

        NS_TEST_ASSERT_MSG_EQ(rtKey.sip,
                              NodeIdToIp(key.srcNodeId,
                                         Cna16ToPortId(NodeIdToCna16(key.srcNodeId)))
                                  .Get(),
                              "CTP routing key should match reference CNA16 source decode");
        NS_TEST_ASSERT_MSG_EQ(rtKey.sport,
                              cnaHeader.GetLb(),
                              "CTP routing key should use CNA LB as sport hash salt");

        Simulator::Destroy();
    }
};

class UbSwitchForwardsCtpCnaPacketTest : public TestCase
{
  public:
    UbSwitchForwardsCtpCnaPacketTest()
        : TestCase("UnifiedBus - switch forwards CNA16 CTPH packets by CNA destination")
    {
    }

  private:
    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbCtpTransportService> service = CreateObject<UbCtpTransportService>();
        UbCtpEntityKey key{.srcNodeId = topo.sender->GetId(),
                           .srcEntityId = 10,
                           .dstNodeId = topo.receiver->GetId(),
                           .dstEntityId = 11,
                           .vl = 7};
        Ptr<Packet> packet = service->BuildDataPacket(CreateCtpWriteSegment(0), key);

        topo.switch0->GetObject<UbSwitch>()->SwitchHandlePacket(topo.switch0DevicePort, packet);

        NS_TEST_ASSERT_MSG_EQ(topo.switch0->GetObject<UbSwitch>()->GetVoqPacketCountForTest(
                                  topo.switch0CorePort->GetIfIndex(),
                                  key.vl,
                                  topo.switch0DevicePort->GetIfIndex()),
                              1u,
                              "CTP packet should enqueue to the VOQ selected by CNA destination routing");

        Simulator::Destroy();
    }
};

class UbSwitchLocalCtpSinkParsesCnpTest : public TestCase
{
  public:
    UbSwitchLocalCtpSinkParsesCnpTest()
        : TestCase("UnifiedBus - local CTP sink passes CNP packets to transport service")
    {
    }

  private:
    void DoRun() override
    {
        LocalTpTopology topo = BuildLocalTpTopology();
        Ptr<UbCtpTransportService> receiverService =
            topo.receiver->GetObject<UbController>()->GetCtpTransportService();
        UbCtpEntityKey cnpKey{.srcNodeId = topo.sender->GetId(),
                              .srcEntityId = 0,
                              .dstNodeId = topo.receiver->GetId(),
                              .dstEntityId = 0,
                              .vl = 7};
        Ptr<Packet> cnpPacket = BuildCtpCnpPacket(cnpKey);

        topo.receiver->GetObject<UbSwitch>()->SwitchHandlePacket(topo.receiverPort, cnpPacket);

        NS_TEST_ASSERT_MSG_EQ(receiverService->GetCongestionStateCountForTest(),
                              1u,
                              "local CTP sink should make CNP parsing reachable");

        Simulator::Destroy();
    }
};

class UbCompactTransportChannelPriorityTest : public TestCase
{
  public:
    UbCompactTransportChannelPriorityTest()
        : TestCase("UnifiedBus - compact channel prioritizes CNP then ACK then data")
    {
    }

  private:
    void DoRun() override
    {
        Ptr<UbCompactTransportChannel> queue = CreateObject<UbCompactTransportChannel>();
        Ptr<Packet> data = Create<Packet>(64);
        Ptr<Packet> ack = Create<Packet>(32);
        Ptr<Packet> cnp = Create<Packet>(16);

        queue->EnqueueData(data);
        queue->EnqueueAck(ack);
        queue->EnqueueCnp(cnp);

        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacket(), cnp, "CNP first");
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacket(), ack, "ACK second");
        NS_TEST_ASSERT_MSG_EQ(queue->GetNextPacket(), data, "data last");
        NS_TEST_ASSERT_MSG_EQ(queue->IsEmpty(), true, "all packets popped");
    }
};

#ifndef _WIN32
class UbCompactTransportChannelRejectsNullPacketTest : public TestCase
{
  public:
    UbCompactTransportChannelRejectsNullPacketTest()
        : TestCase("UnifiedBus - compact channel rejects null packets at enqueue")
    {
    }

  private:
    void DoRun() override
    {
        int status = RunInChildProcess([]() {
            Ptr<UbCompactTransportChannel> queue = CreateObject<UbCompactTransportChannel>();
            queue->EnqueueData(nullptr);
        });

        NS_TEST_ASSERT_MSG_EQ(WIFSIGNALED(status),
                              1,
                              "Enqueueing a null CTP packet should abort");
        NS_TEST_ASSERT_MSG_EQ(WTERMSIG(status),
                              SIGABRT,
                              "Null CTP packet enqueue should fail with SIGABRT");
    }
};
#endif

/**
 * @brief Unified-bus test suite
 */
class UbTestSuite : public TestSuite
{
public:
    UbTestSuite();
};

class UbDcqcnMarkingTestSuite : public TestSuite
{
  public:
    UbDcqcnMarkingTestSuite();
};

class UbWireFormatTestSuite : public TestSuite
{
  public:
    UbWireFormatTestSuite();
};

class UbTransportRetransTestSuite : public TestSuite
{
public:
    UbTransportRetransTestSuite();
};

class UbTransportPipelineTestSuite : public TestSuite
{
public:
    UbTransportPipelineTestSuite();
};

class UbTransactionUrmaTestSuite : public TestSuite
{
public:
    UbTransactionUrmaTestSuite();
};

class UbFlowControlTestSuite : public TestSuite
{
  public:
    UbFlowControlTestSuite();
};

class UbCongestionControlTestSuite : public TestSuite
{
  public:
    UbCongestionControlTestSuite();
};

class UbRuntimeToolsTestSuite : public TestSuite
{
  public:
    UbRuntimeToolsTestSuite();
};

class UbSwitchVoqTestSuite : public TestSuite
{
  public:
    UbSwitchVoqTestSuite();
};

class UbCreateNodeDelayMappingTestSuite : public TestSuite
{
  public:
    UbCreateNodeDelayMappingTestSuite();
};

class UbTpReservationTestSuite : public TestSuite
{
  public:
    UbTpReservationTestSuite();
};

class UbInitialTaskStartOffsetTestSuite : public TestSuite
{
  public:
    UbInitialTaskStartOffsetTestSuite();
};

class UbLinkDelayOffsetTestSuite : public TestSuite
{
  public:
    UbLinkDelayOffsetTestSuite();
};

UbTestSuite::UbTestSuite()
    : TestSuite("unified-bus", Type::UNIT)
{
    AddTestCase(new UbFunctionalityTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbControllerCtpLazyServiceTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficConfigEntityFieldsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAppCtpTransportModeEntryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenRuntimeTaskEntityFieldsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAppCtpRuntimeTaskEntityFallbackTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbAppCtpTrafficRecordRejectsInvalidPriorityTest(),
                TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbAppCtpAdmissionBackpressurePreservesJettyTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbAppCtpForwardsRoutingFlagsToServiceTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenPhaseDependencyMemoryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenSparseTaskIdFallbackTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenReserveHintTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenDenseTaskStoreTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenDuplicateDependencyPhaseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenRecordViewDependencyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenReadyOrderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficGenIntegerDelayParserTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpHeaderEncodeDecodeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpTransactionContextHolTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpTransactionContextAckWindowAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpServiceDoesNotCreateRtpTpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpPortHintsDoNotCreateEntityStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpBuildDataPacketHeaderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpBuildResponsePacketTaAckTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpLocalDataPacketGeneratesTaAckTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpTaAckInheritsPacketSprayRoutingPolicyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpCompactQueueCarriesLocalGenerationMetadataTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCtpWireTaAckCompletesJettyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpOutOfOrderWireTaAckCompletesContiguousJettyRangeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCtpOutOfOrderWireTaAckTracesArrivalTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpCnpDoesNotAckTransactionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpTaAckCompletionHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpWireAckRequiresExactEntityContextTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpWireTaAckCompletesReverseEntityZeroContextTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCtpWireTaAckDoesNotCompleteNonzeroEntityWithoutWireEntityTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCtpWireTaAckUnwrapsLogicalTassnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpSendSegmentFragmentsPayloadTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpSendSegmentRemainderPayloadTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpFragmentedSegmentDoesNotBlockNextTaSsnTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCtpSendSegmentQueueTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbCtpRejectedSegmentDoesNotCreateQueueOrRequireRouteTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCtpPortHintRejectsUnencodableCna16PortTest(),
                TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbCtpSendSegmentRegistersAndTriggersPortTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLdstRoutingKeyKeepsLegacySourcePortTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchCtpRoutingKeyMatchesReferenceCna16SourceDecodeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchClassifiesCtpCnaPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchForwardsCtpCnaPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalCtpSinkParsesCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCompactTransportChannelPriorityTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbCompactTransportChannelRejectsNullPacketTest(), TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbDcqcnFactoryCreatesHostAndSwitchTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnHostAckCeTphDefaultTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmHostRttUsesSmoothedEstimateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpHeaderRoundTripTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchMarksFecnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchNoMarkPreservesPacketHeadersTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchMarksAboveKmaxEvenWhenPmaxIsZeroTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnReceiverSuppressesBurstCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnControlPriorityPrefersCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmReceiverAggregatesLogicalPsnAcrossWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbModuloSequenceUnwrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphRoundTripTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphBitmapBoundaryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphEncodingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphReservedEncodingRejectTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphWireBitOrderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportResponseStatusFieldsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbFaultClassifiesTransportResponseStatusTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportRetransmissionModeDefaultsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRetransDisabledDoesNotRetainSentPacketsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportShallowPipelineIgnoresUnackedSegmentsTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbAckWithoutCetphCarriesNoCetphHeaderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRetransDisabledReceiveGapDoesNotEmitSackTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverTpsackGapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbReceiverUnwrapsDataPsnAcrossWireWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbReceiverRejectsOutOfWindowFuturePsnWithoutPollutingMaxRcvPsnTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverAckAfterGapClosesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverExplicitBitmapWidthTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverFirstPacketLossTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverDuplicateGapTpsackTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverTpsackWithCetphOrderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportRecvTpsackDoesNotMisparseSaetphTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchDispatchesTpsackAsTransportResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchRejectsReservedTpOpcodeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnReceiverKeepsOutOfOrderAckSilentTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnReceiverEmitsSpecTpnakWireFormatTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnTpnakCcWireAndConsumptionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderRecordsMissingWithoutFastRetransmitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderFastRetransmitQueuesMissingOnceTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveRetransmitTraceReportsSparsePsnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderBitmapBoundaryIgnoresPaddingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderMaxRcvPsnSuppressesPaddingHolesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderUnwrapsTpsackAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderIgnoresOutOfWindowTpsackAcrossWireWrapTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderAcceptsTpsackAtCurrentAckBaseTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSenderUnwrapsPlainTpackAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnSenderUnwrapsTpnakAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderCumulativeAckClearsRetainedStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckedGapStateKeepsStateButSkipsRetransmitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderRetransmitsRetainedMissingPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderQueueContractTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderDropsAckedStaleRetransmitEntriesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderQueuePriorityTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderRtoEnqueuesOutstandingPsnsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveRetransmissionAccountsDcqcnSendStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveRetransmissionUsesPayloadBytesForCongestionControlTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmSelectiveAckWithCetphAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportTpsackCcReportsRetransmitBytesOnceTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpOpcodeIsValidTransportOpcodeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRoutingProcessRangeRouteTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchDoesNotRemarkMarkedFecnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSenderCutsRateOnCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpDoesNotAdvanceAckStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnRecoveryTimerIncreasesRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnByteCounterIncreasesRateBeforeTimerTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnHyperIncreaseUsesHaiRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnRateNeverExceedsLineRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnPacingWakeupResumesQueuedSendTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpCutRescalesOutstandingPacingDebtTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCompletedFlowReleasesHostActiveSlotTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnIdleFlowCancelsRecoveryStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadWqeMetadataPropagationTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRemoteAddressUsesSegmentTaSsnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbWqeSegmentKeepsLogicalTaSequencesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbJettyCompletesLogicalTassnAcrossWireWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbReceiverKeepsInboundTpMsnLogicalKeyAcrossWireWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaWriteCompletionNeedsTransactionResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadCompletionNeedsReadResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadMultiPacketResponseCountTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadMultiSliceRequestPacketSemanticsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTraceDirSetupTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAlgorithmTraceGateDefaultOffTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAlgorithmTraceCategoryGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueTraceCategoryGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpPacketTraceGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpTraceConnectDoesNotCreateServiceTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpPacketTraceConnectsLazyServiceTest(), TestCase::Duration::QUICK);
    AddTestCase(new utils::UbQueueSamplerEventRetentionTest(), TestCase::Duration::QUICK);
    AddTestCase(new utils::UbTraceFileConcurrencyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbMpiRankExtractionHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSameMpiRankHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSystemOwnedByRankHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeSystemIdTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCsvCrLfTrimTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeDelayColumnsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeForwardDelayDoesNotOverrideAllocationTimeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeLegacyForwardDelayMapsToAllocationTimeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchFlowControlModeAttributeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalRuntimeConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbControlFrameUsesDedicatedAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcFixedModeCountsHeadroomTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcDynamicModePauseResumeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerDynamicPfcDecisionApiTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcDynamicModeXoffZeroEmptyQueueTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModePauseResumeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerPaperPfcDecisionApiTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerTotalBufferedBytesTracksEgressTransferTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerInPortProcessingAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchInPortProcessingDelayHidesPacketFromAllocatorTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchInPortProcessingDelayDoesNotDelayTpSourceQueueTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchZeroDelayPreservesLegacyEnqueueOrderingTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchControlFramesBypassProcessingDelayTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbPfcForwardingUsesIngressPortConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbFlowControlReleaseHookRunsAfterIngressDequeueTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerReserveOnlyAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerStickyHeadroomAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerIngressPortOccupancyViewTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbBusyPortArrivalPrefetchesNextPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchCreatesVoqsOnDemandTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbVoqAllocatorPreservesIngressQueueSlotPhaseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDwrrAllocatorPreservesIngressQueueSlotsAcrossVlTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbDwrrAllocatorKeepsNonVoqQueuesAfterVoqSlotsTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbPacketQueueInlineStoragePreservesFifoTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSmallFifoQueueReleasesHeapWhenDrainedTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbDataPacketHeaderRejectsPriorityZeroTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSendControlFrameRejectsDataPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcRejectsZeroCellGeometryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcFixedRejectsNegativeThresholdTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalCbfcConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcPiggybackRestoreClearsLocalCreditBitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcControlFrameFallbackWithoutDataPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForcedControlReturnFlushesAllEligibleVlsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForwardedIngressEnqueueDoesNotAccumulateReturnCreditTest(),
                TestCase::Duration::QUICK);
#endif
#ifdef NS3_MPI
    AddTestCase(new UbCreateTopoRemoteLinkTest(), TestCase::Duration::QUICK);
#endif
#if defined(NS3_MPI) && defined(NS3_MTP)
    AddTestCase(new UbCreateTopoPackedSystemIdLocalLinkTest(), TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbCreateTpPreloadInstancesTest(), TestCase::Duration::QUICK);
}

// Register the test suite
static UbTestSuite g_ubTestSuite;

UbTpReservationTestSuite::UbTpReservationTestSuite()
    : TestSuite("unified-bus-tp-reservation", Type::UNIT)
{
    AddTestCase(new UbCreateTpUsesPerPortIpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbControllerMissingTpnLookupDoesNotInsertTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTrafficDrivenReservationDoesNotRemoteCreateReceiverTpTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbReservedReceiverTpnMaterializesLocallyTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbInboundTpChannelKeyValidationTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUnknownInboundTpnFailsWithAutoRemoveTest(), TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbConcurrentReservedReceiverTpnMaterializationTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbRepeatedTpResolutionReusesReservationTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUnreservedReceiverTpnDoesNotMaterializeTest(), TestCase::Duration::QUICK);
}

static UbTpReservationTestSuite g_ubTpReservationTestSuite;

UbInitialTaskStartOffsetTestSuite::UbInitialTaskStartOffsetTestSuite()
    : TestSuite("unified-bus-initial-task-start-offset", Type::UNIT)
{
    AddTestCase(new UbTrafficGenInitialTaskStartOffsetTest(), TestCase::Duration::QUICK);
}

static UbInitialTaskStartOffsetTestSuite g_ubInitialTaskStartOffsetTestSuite;

UbLinkDelayOffsetTestSuite::UbLinkDelayOffsetTestSuite()
    : TestSuite("unified-bus-link-delay-offset", Type::UNIT)
{
    AddTestCase(new UbLinkDelayOffsetTest(), TestCase::Duration::QUICK);
}

static UbLinkDelayOffsetTestSuite g_ubLinkDelayOffsetTestSuite;

UbWireFormatTestSuite::UbWireFormatTestSuite()
    : TestSuite("unified-bus-wire-format", Type::UNIT)
{
    AddTestCase(new UbDcqcnCnpHeaderRoundTripTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphRoundTripTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphBitmapBoundaryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphEncodingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphReservedEncodingRejectTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckExtTphWireBitOrderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportResponseStatusFieldsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbFaultClassifiesTransportResponseStatusTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAckWithoutCetphCarriesNoCetphHeaderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpOpcodeIsValidTransportOpcodeTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbDataPacketHeaderRejectsPriorityZeroTest(), TestCase::Duration::QUICK);
#endif
}

static UbWireFormatTestSuite g_ubWireFormatTestSuite;

UbTransportRetransTestSuite::UbTransportRetransTestSuite()
    : TestSuite("unified-bus-transport-retrans", Type::UNIT)
{
    AddTestCase(new UbTransportRetransmissionModeDefaultsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRetransDisabledDoesNotRetainSentPacketsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportShallowPipelineIgnoresUnackedSegmentsTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbRetransDisabledReceiveGapDoesNotEmitSackTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverTpsackGapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbReceiverUnwrapsDataPsnAcrossWireWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbReceiverRejectsOutOfWindowFuturePsnWithoutPollutingMaxRcvPsnTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverAckAfterGapClosesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverExplicitBitmapWidthTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverFirstPacketLossTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverDuplicateGapTpsackTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveReceiverTpsackWithCetphOrderTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportRecvTpsackDoesNotMisparseSaetphTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchDispatchesTpsackAsTransportResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchRejectsReservedTpOpcodeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnReceiverKeepsOutOfOrderAckSilentTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnReceiverEmitsSpecTpnakWireFormatTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnTpnakCcWireAndConsumptionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderRecordsMissingWithoutFastRetransmitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderFastRetransmitQueuesMissingOnceTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveRetransmitTraceReportsSparsePsnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderBitmapBoundaryIgnoresPaddingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderMaxRcvPsnSuppressesPaddingHolesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderUnwrapsTpsackAcrossWirePsnWrapTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderIgnoresOutOfWindowTpsackAcrossWireWrapTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderAcceptsTpsackAtCurrentAckBaseTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSenderUnwrapsPlainTpackAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbGbnSenderUnwrapsTpnakAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderCumulativeAckClearsRetainedStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveAckedGapStateKeepsStateButSkipsRetransmitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderRetransmitsRetainedMissingPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderQueueContractTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderDropsAckedStaleRetransmitEntriesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderQueuePriorityTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveSenderRtoEnqueuesOutstandingPsnsTest(), TestCase::Duration::QUICK);
}

static UbTransportRetransTestSuite g_ubTransportRetransTestSuite;

UbTransportPipelineTestSuite::UbTransportPipelineTestSuite()
    : TestSuite("unified-bus-transport-pipeline", Type::UNIT)
{
    AddTestCase(new UbTransportShallowPipelineIgnoresUnackedSegmentsTest(),
                TestCase::Duration::QUICK);
}

static UbTransportPipelineTestSuite g_ubTransportPipelineTestSuite;

UbTransactionUrmaTestSuite::UbTransactionUrmaTestSuite()
    : TestSuite("unified-bus-transaction-urma", Type::UNIT)
{
    AddTestCase(new UbUrmaReadWqeMetadataPropagationTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbRemoteAddressUsesSegmentTaSsnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbWqeSegmentKeepsLogicalTaSequencesTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbJettyCompletesLogicalTassnAcrossWireWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbReceiverKeepsInboundTpMsnLogicalKeyAcrossWireWrapTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaWriteCompletionNeedsTransactionResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadCompletionNeedsReadResponseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadMultiPacketResponseCountTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaReadMultiSliceRequestPacketSemanticsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbUrmaWriteOutOfOrderRequestSliceCompletionTest(), TestCase::Duration::QUICK);
}

static UbTransactionUrmaTestSuite g_ubTransactionUrmaTestSuite;

UbFlowControlTestSuite::UbFlowControlTestSuite()
    : TestSuite("unified-bus-flow-control", Type::UNIT)
{
    AddTestCase(new UbSwitchFlowControlModeAttributeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbControlFrameUsesDedicatedAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcFixedModeCountsHeadroomTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcDynamicModePauseResumeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerDynamicPfcDecisionApiTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcDynamicModeXoffZeroEmptyQueueTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModePauseResumeTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerPaperPfcDecisionApiTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModeIgnoresAlphaShiftForAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcPaperDynamicModeUsesRealGlobalOccupancyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerTotalBufferedBytesTracksEgressTransferTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerInPortProcessingAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchInPortProcessingDelayHidesPacketFromAllocatorTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchInPortProcessingDelayDoesNotDelayTpSourceQueueTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchZeroDelayPreservesLegacyEnqueueOrderingTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchControlFramesBypassProcessingDelayTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbPfcForwardingUsesIngressPortConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbFlowControlReleaseHookRunsAfterIngressDequeueTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAllocatorKeepsIngressPacketWhenEgressQueueIsFullTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerReserveOnlyAdmissionTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerStickyHeadroomAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueManagerIngressPortOccupancyViewTest(), TestCase::Duration::QUICK);
#ifndef _WIN32
    AddTestCase(new UbSendControlFrameRejectsDataPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcRejectsZeroCellGeometryTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbPfcFixedRejectsNegativeThresholdTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalCbfcConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcPiggybackTargetVlCanDifferFromPacketVlTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcPiggybackRestoreClearsLocalCreditBitTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcControlFrameFallbackWithoutDataPacketTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcCtrlCrdRtrThresholdTriggersControlFrameTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForcedControlReturnFlushesAllEligibleVlsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForcedControlReturnChunksControlFramesAtSixBitLimitTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForwardedReleaseUsesIngressPortThresholdStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCbfcForwardedIngressEnqueueDoesNotAccumulateReturnCreditTest(),
                TestCase::Duration::QUICK);
#endif
}

static UbFlowControlTestSuite g_ubFlowControlTestSuite;

UbCongestionControlTestSuite::UbCongestionControlTestSuite()
    : TestSuite("unified-bus-congestion-control", Type::UNIT)
{
    AddTestCase(new UbDcqcnHostAckCeTphDefaultTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmHostRttUsesSmoothedEstimateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchMarksFecnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchNoMarkPreservesPacketHeadersTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchMarksAboveKmaxEvenWhenPmaxIsZeroTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnReceiverSuppressesBurstCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnControlPriorityPrefersCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmReceiverAggregatesLogicalPsnAcrossWrapTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveRetransmissionAccountsDcqcnSendStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSelectiveRetransmissionUsesPayloadBytesForCongestionControlTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCaqmSelectiveAckWithCetphAccountingTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbTransportTpsackCcReportsRetransmitBytesOnceTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSwitchDoesNotRemarkMarkedFecnTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnSenderCutsRateOnCnpTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpDoesNotAdvanceAckStateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnRecoveryTimerIncreasesRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnByteCounterIncreasesRateBeforeTimerTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnHyperIncreaseUsesHaiRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnRateNeverExceedsLineRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnBusyHostStartsSecondFlowAtInitialRateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnPacingWakeupResumesQueuedSendTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCnpCutRescalesOutstandingPacingDebtTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnCompletedFlowReleasesHostActiveSlotTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDcqcnIdleFlowCancelsRecoveryStateTest(), TestCase::Duration::QUICK);
}

static UbCongestionControlTestSuite g_ubCongestionControlTestSuite;

UbRuntimeToolsTestSuite::UbRuntimeToolsTestSuite()
    : TestSuite("unified-bus-runtime-tools", Type::UNIT)
{
    AddTestCase(new UbTraceDirSetupTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAlgorithmTraceGateDefaultOffTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbAlgorithmTraceCategoryGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbQueueTraceCategoryGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpPacketTraceGateTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpTraceConnectDoesNotCreateServiceTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCtpPacketTraceConnectsLazyServiceTest(), TestCase::Duration::QUICK);
    AddTestCase(new utils::UbQueueSamplerEventRetentionTest(), TestCase::Duration::QUICK);
    AddTestCase(new utils::UbTraceFileConcurrencyTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbMpiRankExtractionHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSameMpiRankHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSystemOwnedByRankHelperTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeSystemIdTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCsvCrLfTrimTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeDelayColumnsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeForwardDelayDoesNotOverrideAllocationTimeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeLegacyForwardDelayMapsToAllocationTimeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchLocalRuntimeConfigTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSlidingBitmapWindowAdvancesWithoutLosingOutOfOrderMarksTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSlidingBitmapWindowReusesSlotsWithoutGhostMarksTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbBusyPortArrivalPrefetchesNextPacketTest(), TestCase::Duration::QUICK);
#ifdef NS3_MPI
    AddTestCase(new UbCreateTopoRemoteLinkTest(), TestCase::Duration::QUICK);
#endif
#if defined(NS3_MPI) && defined(NS3_MTP)
    AddTestCase(new UbCreateTopoPackedSystemIdLocalLinkTest(), TestCase::Duration::QUICK);
#endif
    AddTestCase(new UbCreateTpPreloadInstancesTest(), TestCase::Duration::QUICK);
}

static UbRuntimeToolsTestSuite g_ubRuntimeToolsTestSuite;

UbSwitchVoqTestSuite::UbSwitchVoqTestSuite()
    : TestSuite("unified-bus-switch-voq", Type::UNIT)
{
    AddTestCase(new UbRoundRobinAllocatorSeedsDifferentInitialPhasesPerOutPortTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbSwitchCreatesVoqsOnDemandTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbVoqAllocatorPreservesIngressQueueSlotPhaseTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbDwrrAllocatorPreservesIngressQueueSlotsAcrossVlTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbDwrrAllocatorKeepsNonVoqQueuesAfterVoqSlotsTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbPacketQueueInlineStoragePreservesFifoTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbSmallFifoQueueReleasesHeapWhenDrainedTest(), TestCase::Duration::QUICK);
}

static UbSwitchVoqTestSuite g_ubSwitchVoqTestSuite;

UbCreateNodeDelayMappingTestSuite::UbCreateNodeDelayMappingTestSuite()
    : TestSuite("unified-bus-create-node-delay-mapping", Type::UNIT)
{
    AddTestCase(new UbCreateNodeDelayColumnsTest(), TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeForwardDelayDoesNotOverrideAllocationTimeTest(),
                TestCase::Duration::QUICK);
    AddTestCase(new UbCreateNodeLegacyForwardDelayMapsToAllocationTimeTest(),
                TestCase::Duration::QUICK);
}

static UbCreateNodeDelayMappingTestSuite g_ubCreateNodeDelayMappingTestSuite;

class UbRetransCongestionControlRegressionTestSuite : public TestSuite
{
public:
    UbRetransCongestionControlRegressionTestSuite()
        : TestSuite("unified-bus-retrans-cc-regression", Type::UNIT)
    {
        AddTestCase(new UbSelectiveRetransmissionAccountsDcqcnSendStateTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbSelectiveRetransmissionUsesPayloadBytesForCongestionControlTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbCaqmSelectiveAckWithCetphAccountingTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbTransportTpsackCcReportsRetransmitBytesOnceTest(),
                    TestCase::Duration::QUICK);
    }
};

static UbRetransCongestionControlRegressionTestSuite g_ubRetransCongestionControlRegressionTestSuite;

class UbModuloSequenceRegressionTestSuite : public TestSuite
{
public:
    UbModuloSequenceRegressionTestSuite()
        : TestSuite("unified-bus-modulo-sequence-regression", Type::UNIT)
    {
        AddTestCase(new UbModuloSequenceUnwrapTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReceiverUnwrapsDataPsnAcrossWireWrapTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReceiverRejectsOutOfWindowFuturePsnWithoutPollutingMaxRcvPsnTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbSelectiveSenderUnwrapsTpsackAcrossWirePsnWrapTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbSelectiveSenderIgnoresOutOfWindowTpsackAcrossWireWrapTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbSelectiveSenderAcceptsTpsackAtCurrentAckBaseTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbSenderUnwrapsPlainTpackAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbGbnSenderUnwrapsTpnakAcrossWirePsnWrapTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbCaqmReceiverAggregatesLogicalPsnAcrossWrapTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbWqeSegmentKeepsLogicalTaSequencesTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbJettyCompletesLogicalTassnAcrossWireWrapTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbReceiverKeepsInboundTpMsnLogicalKeyAcrossWireWrapTest(),
                    TestCase::Duration::QUICK);
    }
};

static UbModuloSequenceRegressionTestSuite g_ubModuloSequenceRegressionTestSuite;

UbDcqcnMarkingTestSuite::UbDcqcnMarkingTestSuite()
    : TestSuite("unified-bus-dcqcn-marking", Type::UNIT)
{
    AddTestCase(new UbDcqcnSwitchMarksAboveKmaxEvenWhenPmaxIsZeroTest(), TestCase::Duration::QUICK);
}

static UbDcqcnMarkingTestSuite g_ubDcqcnMarkingTestSuite;

class UbOutOfOrderRegressionTestSuite : public TestSuite
{
  public:
    UbOutOfOrderRegressionTestSuite()
        : TestSuite("unified-bus-transport-ooo-regression", Type::UNIT)
    {
        AddTestCase(new UbUrmaWriteOutOfOrderRequestSliceCompletionTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbSelectiveRetransmissionUsesPayloadBytesForCongestionControlTest(),
                    TestCase::Duration::QUICK);
    }
};

static UbOutOfOrderRegressionTestSuite g_ubOutOfOrderRegressionTestSuite;

namespace
{

std::filesystem::path
LocateRepoRoot();

std::pair<int, std::string>
RunQuickExampleCommand(const std::string& testFile,
                       const std::string& extraArgs,
                       const std::string& commandPrefix,
                       const std::string& casePathRelative = "");

std::pair<int, std::string>
RunNs3RunCommand(const std::string& testFile,
                 const std::string& programAndArgs,
                 const std::string& commandPrefix = "",
                 bool noBuild = true);

} // namespace

#ifdef NS3_MTP
class UbQuickExampleLocalMtpSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalMtpSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local MTP mode runs without MPI init failure")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename("ub-quick-example-local-mtp.log"),
                                   "--mtp-threads=2",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example local MTP mode should exit successfully");
        NS_TEST_ASSERT_MSG_EQ(output.find("MPI_Testany() ... before MPI_INIT"), std::string::npos,
                              "ub-quick-example local MTP mode should not touch MPI before MPI_Init");
    }
};
#else
class UbQuickExampleMtpRequestedWithoutMtpSystemTest : public TestCase
{
  public:
    UbQuickExampleMtpRequestedWithoutMtpSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects MTP request when MTP is not compiled")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename("ub-quick-example-no-mtp-request.log"),
                                   "--mtp-threads=2",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example should reject MTP requests when not compiled with MTP");
        NS_TEST_ASSERT_MSG_NE(output.find("[ERROR] MTP requested but not compiled"),
                              std::string::npos,
                              "ub-quick-example should print a clear no-MTP build error");
        NS_TEST_ASSERT_MSG_EQ(output.find("[case] Run case:"),
                              std::string::npos,
                              "ub-quick-example should fail before starting the case");
    }
};
#endif

#ifdef NS3_MPI
class UbQuickExampleSpoofedMpiEnvSystemTest : public TestCase
{
  public:
    UbQuickExampleSpoofedMpiEnvSystemTest()
        : TestCase("UnifiedBus - ub-quick-example ignores spoofed MPI env without launcher")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename("ub-quick-example-spoofed-mpi-env.log"),
                                   "--test",
                                   "env OMPI_COMM_WORLD_SIZE=2",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "spoofed MPI environment without launcher should stay on local runtime");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "spoofed MPI environment case should still complete locally");
    }
};
#endif

namespace
{

bool
HasQuickExampleBinary(const std::filesystem::path& repoRoot)
{
    return std::filesystem::exists(repoRoot / "build/scratch/ns3.44-ub-quick-example") ||
           std::filesystem::exists(
               repoRoot / "build/src/unified-bus/examples/ns3.44-ub-quick-example-default") ||
           std::filesystem::exists(
               repoRoot / "build/src/unified-bus/examples/ns3.44-ub-quick-example");
}

std::filesystem::path
LocateRepoRoot()
{
    std::filesystem::path repoRoot = PROJECT_SOURCE_PATH;
    if (HasQuickExampleBinary(repoRoot))
    {
        return repoRoot;
    }

    repoRoot = NS_TEST_SOURCEDIR;
    if (repoRoot.is_relative())
    {
        repoRoot = std::filesystem::path(PROJECT_SOURCE_PATH) / repoRoot;
    }
    for (uint32_t i = 0; i < 4 && !HasQuickExampleBinary(repoRoot); ++i)
    {
        repoRoot = repoRoot.parent_path();
    }
    return repoRoot;
}

std::filesystem::path
LocateQuickExampleBinary(const std::filesystem::path& repoRoot)
{
    const std::vector<std::filesystem::path> candidates = {
        "build/scratch/ns3.44-ub-quick-example",
        "build/src/unified-bus/examples/ns3.44-ub-quick-example",
        "build/src/unified-bus/examples/ns3.44-ub-quick-example-default",
    };

    for (const auto& candidate : candidates)
    {
        const std::filesystem::path binaryPath = repoRoot / candidate;
        if (std::filesystem::exists(binaryPath))
        {
            return binaryPath;
        }
    }

    const std::filesystem::path examplesDir = repoRoot / "build/src/unified-bus/examples";
    if (std::filesystem::exists(examplesDir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(examplesDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::string filename = entry.path().filename().string();
            if (filename.find("ub-quick-example") != std::string::npos)
            {
                return entry.path();
            }
        }
    }

    return repoRoot / candidates.front();
}

std::pair<int, std::string>
RunQuickExampleCommand(const std::string& testFile,
                       const std::string& extraArgs,
                       const std::string& commandPrefix,
                       const std::string& casePathRelative)
{
    const std::filesystem::path repoRoot = LocateRepoRoot();
    const std::filesystem::path binaryPath = LocateQuickExampleBinary(repoRoot);

    std::string command;
    if (!commandPrefix.empty())
    {
        command += commandPrefix + " ";
    }
    command += "\"" + binaryPath.string() + "\"";
    if (!casePathRelative.empty())
    {
        const std::filesystem::path casePath = repoRoot / casePathRelative;
        command += " --case-path=\"" + casePath.string() + "\"";
    }
    if (!extraArgs.empty())
    {
        command += " " + extraArgs;
    }
    command += " > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());

    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

std::pair<int, std::string>
RunNs3RunCommand(const std::string& testFile,
                 const std::string& programAndArgs,
                 const std::string& commandPrefix,
                 bool noBuild)
{
    const std::filesystem::path repoRoot = LocateRepoRoot();
    const std::string pythonCommand =
        std::system("command -v python3.12 >/dev/null 2>&1") == 0 ? "python3.12" : "python3";

    std::string command;
    if (!commandPrefix.empty())
    {
        command += commandPrefix + " ";
    }
    command += pythonCommand + " \"" + (repoRoot / "ns3").string() + "\" run \"" + programAndArgs +
               "\"";
    if (noBuild)
    {
        command += " --no-build";
    }
    command += " > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());

    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

std::string
NormalizeTestPath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::filesystem::path
CreateSafeTempBasename(const std::string& prefix)
{
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return (std::filesystem::temp_directory_path() / (prefix + "-" + uniqueSuffix)).lexically_normal();
}

std::filesystem::path
CopyCaseDirWithoutFile(const std::string& sourceCasePathRelative, const std::string& omittedFilename)
{
    namespace fs = std::filesystem;

    const fs::path repoRoot = LocateRepoRoot();
    const fs::path sourceCaseDir = repoRoot / sourceCasePathRelative;
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tempCaseDir =
        (fs::temp_directory_path() / ("ub-quick-example-case-copy-" + uniqueSuffix)).lexically_normal();

    fs::create_directories(tempCaseDir);
    for (const auto& entry : fs::directory_iterator(sourceCaseDir))
    {
        const fs::path destination = tempCaseDir / entry.path().filename();
        if (entry.path().filename() == omittedFilename)
        {
            continue;
        }
        fs::copy(entry.path(), destination, fs::copy_options::recursive);
    }

    return tempCaseDir;
}

std::filesystem::path
CopyCaseDirWithTrafficFile(const std::string& sourceCasePathRelative, const std::string& trafficCsvContent)
{
    namespace fs = std::filesystem;

    const fs::path repoRoot = LocateRepoRoot();
    const fs::path sourceCaseDir = repoRoot / sourceCasePathRelative;
    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tempCaseDir =
        (fs::temp_directory_path() / ("ub-quick-example-traffic-copy-" + uniqueSuffix))
            .lexically_normal();

    fs::create_directories(tempCaseDir);
    for (const auto& entry : fs::directory_iterator(sourceCaseDir))
    {
        const fs::path destination = tempCaseDir / entry.path().filename();
        fs::copy(entry.path(), destination, fs::copy_options::recursive);
    }

    std::ofstream trafficFile(tempCaseDir / "traffic.csv");
    trafficFile << trafficCsvContent;
    trafficFile.close();

    return tempCaseDir;
}

void
ReplaceInFile(const std::filesystem::path& filePath,
              const std::string& needle,
              const std::string& replacement)
{
    std::ifstream input(filePath);
    if (!input.is_open())
    {
        throw std::runtime_error("failed to open file for rewrite helper: " + filePath.string());
    }
    std::stringstream buffer;
    buffer << input.rdbuf();
    std::string content = buffer.str();
    const auto pos = content.find(needle);
    if (pos == std::string::npos)
    {
        throw std::runtime_error("expected text not found in file rewrite helper: " + needle);
    }
    content.replace(pos, needle.size(), replacement);
    std::ofstream output(filePath, std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("failed to open file for write helper: " + filePath.string());
    }
    output << content;
}

// The old ub-local-hybrid-minimal fixture was removed as a non-core sample, so
// local single-thread quick-example tests reuse the maintained 2-node case.
constexpr const char* kLocalSingleThreadQuickExampleCase = "scratch/2nodes_single-tp";

struct TaskTimes
{
    uint64_t startNs{0};
    uint64_t finishNs{0};
    bool hasStart{false};
    bool hasFinish{false};
};

std::vector<std::string>
SplitCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream input(line);
    std::string field;
    while (std::getline(input, field, ','))
    {
        fields.push_back(field);
    }
    return fields;
}

std::map<uint32_t, TaskTimes>
ReadTaskStatisticsTimes(const std::filesystem::path& caseDir)
{
    std::map<uint32_t, TaskTimes> taskTimes;
    std::ifstream input(caseDir / "test" / "task_statistics.csv");
    if (!input.is_open())
    {
        return taskTimes;
    }

    std::string header;
    if (!std::getline(input, header))
    {
        return taskTimes;
    }
    const std::vector<std::string> columns = SplitCsvLine(header);
    auto columnIndex = [&](const std::string& name) -> std::optional<std::size_t> {
        auto it = std::find(columns.begin(), columns.end(), name);
        if (it == columns.end())
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(std::distance(columns.begin(), it));
    };

    const auto taskIdIndex = columnIndex("taskId");
    const auto startIndex = columnIndex("taskStartTime(us)");
    const auto finishIndex = columnIndex("taskCompletesTime(us)");
    if (!taskIdIndex || !startIndex || !finishIndex)
    {
        return taskTimes;
    }

    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }
        const std::vector<std::string> fields = SplitCsvLine(line);
        if (fields.size() <= std::max({*taskIdIndex, *startIndex, *finishIndex}))
        {
            continue;
        }

        const uint32_t taskId = static_cast<uint32_t>(std::stoul(fields[*taskIdIndex]));
        TaskTimes& times = taskTimes[taskId];
        times.startNs = static_cast<uint64_t>(std::stod(fields[*startIndex]) * 1000.0);
        times.finishNs = static_cast<uint64_t>(std::stod(fields[*finishIndex]) * 1000.0);
        times.hasStart = true;
        times.hasFinish = true;
    }

    return taskTimes;
}

bool
HasDependencyVisibilityTaskTimes(const std::map<uint32_t, TaskTimes>& taskTimes)
{
    const bool hasTask0 = taskTimes.count(0) == 1;
    const bool hasTask1 = taskTimes.count(1) == 1;
    if (!hasTask0 || !hasTask1)
    {
        return false;
    }

    const bool hasTask0Finish = taskTimes.at(0).hasFinish;
    const bool hasTask1Start = taskTimes.at(1).hasStart;
    return hasTask0Finish && hasTask1Start;
}

std::string
ReadCanonicalBytes(const std::filesystem::path& basePath)
{
    std::vector<std::string> lines;
    const std::filesystem::path parent = basePath.parent_path();
    const std::string prefix = basePath.filename().string() + ".rank";
    for (const auto& entry : std::filesystem::directory_iterator(parent))
    {
        const std::string filename = entry.path().filename().string();
        if (filename.rfind(prefix, 0) != 0)
        {
            continue;
        }

        std::ifstream input(entry.path());
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty())
            {
                lines.push_back(line);
            }
        }
    }

    std::sort(lines.begin(), lines.end());
    std::ostringstream output;
    for (const auto& line : lines)
    {
        output << line << '\n';
    }
    return output.str();
}

std::size_t
CountNonEmptyLines(const std::string& text)
{
    std::size_t count = 0;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty())
        {
            ++count;
        }
    }
    return count;
}

void
WriteCanonicalClosNodeFile(const std::filesystem::path& caseDir, bool splitRanks)
{
    std::ofstream output(caseDir / "node.csv", std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("failed to open CLOS node.csv for rewrite");
    }

    output << "nodeId,nodeType,portNum,forwardDelay,systemId\n";
    for (uint32_t nodeId = 0; nodeId < 32; ++nodeId)
    {
        const uint32_t rank = splitRanks && nodeId >= 16 ? 1 : 0;
        output << nodeId << ",DEVICE,1,1ns," << rank << "\n";
    }
    for (uint32_t nodeId = 32; nodeId < 36; ++nodeId)
    {
        const uint32_t rank = splitRanks && nodeId >= 34 ? 1 : 0;
        output << nodeId << ",SWITCH,16,1ns," << rank << "\n";
    }
    for (uint32_t nodeId = 36; nodeId < 44; ++nodeId)
    {
        const uint32_t rank = splitRanks && nodeId >= 40 ? 1 : 0;
        output << nodeId << ",SWITCH,4,1ns," << rank << "\n";
    }
}

std::map<uint32_t, TaskTimes>
ExtractCanonicalTaskTimes(const std::string& canonicalBytes)
{
    std::map<uint32_t, TaskTimes> taskTimes;
    std::istringstream input(canonicalBytes);
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }
        const std::vector<std::string> fields = SplitCsvLine(line);
        if (fields.size() < 5)
        {
            continue;
        }

        const uint64_t timeNs = static_cast<uint64_t>(std::stoull(fields[0]));
        const std::string& type = fields[1];
        const uint32_t taskId = static_cast<uint32_t>(std::stoul(fields[2]));
        TaskTimes& times = taskTimes[taskId];
        if (type == "START")
        {
            times.startNs = timeNs;
            times.hasStart = true;
        }
        else if (type == "COMPLETE_VISIBLE")
        {
            times.finishNs = timeNs;
            times.hasFinish = true;
        }
    }
    return taskTimes;
}

std::pair<int, std::string>
RunQuickExampleAbsoluteCaseCommand(const std::string& testFile,
                                   const std::string& extraArgs,
                                   const std::string& commandPrefix,
                                   const std::filesystem::path& casePath)
{
    const std::filesystem::path repoRoot = LocateRepoRoot();
    const std::filesystem::path binaryPath = LocateQuickExampleBinary(repoRoot);

    std::string command;
    if (!commandPrefix.empty())
    {
        command += commandPrefix + " ";
    }
    command += "\"" + binaryPath.string() + "\"";
    command += " --case-path=\"" + casePath.string() + "\"";
    if (!extraArgs.empty())
    {
        command += " " + extraArgs;
    }
    command += " > \"" + testFile + "\" 2>&1";

    const int status = std::system(command.c_str());

    std::ifstream input(testFile);
    std::stringstream buffer;
    buffer << input.rdbuf();
    return {status, buffer.str()};
}

} // namespace

class UbQuickExampleMissingCasePathSystemTest : public TestCase
{
  public:
    UbQuickExampleMissingCasePathSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects missing case-path")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] = RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                                       "",
                                                       "");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example without case-path should exit with failure");
        NS_TEST_ASSERT_MSG_NE(output.find("missing required case path (--case-path or casePath)"),
                              std::string::npos,
                              "ub-quick-example should print a clear missing case-path error");
    }
};

class UbQuickExampleMissingCaseDirSystemTest : public TestCase
{
  public:
    UbQuickExampleMissingCaseDirSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects missing case directory")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path missingCaseDir = repoRoot / "scratch/ub-case-does-not-exist";
        const std::string expectedError =
            "case path does not exist: " + NormalizeTestPath(missingCaseDir);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" + missingCaseDir.string() + "\"",
                                   "",
                                   "");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example should fail when case-path directory is missing");
        NS_TEST_ASSERT_MSG_NE(output.find(expectedError),
                              std::string::npos,
                              "ub-quick-example should print a clear missing case directory error");
    }
};

class UbQuickExampleMissingCaseFileSystemTest : public TestCase
{
  public:
    UbQuickExampleMissingCaseFileSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects case directory with missing required files")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            std::filesystem::path(CreateTempDirFilename("ub-quick-example-missing-files-case"));
        std::filesystem::create_directories(caseDir);
        const std::filesystem::path expectedMissingFile = caseDir / "network_attribute.txt";
        const std::string expectedError =
            "missing required case file: " + NormalizeTestPath(expectedMissingFile);

        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" + caseDir.string() + "\"",
                                   "",
                                   "");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example should fail when required case files are missing");
        NS_TEST_ASSERT_MSG_NE(output.find(expectedError),
                              std::string::npos,
                              "ub-quick-example should identify the first missing required case file");
    }
};

class UbQuickExampleHelpTextSystemTest : public TestCase
{
  public:
    UbQuickExampleHelpTextSystemTest()
        : TestCase("UnifiedBus - ub-quick-example help marks case-path as required")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"), "--help", "");

        NS_TEST_ASSERT_MSG_EQ(status, 0, "ub-quick-example --help should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("Required path to the unified-bus case directory"),
                              std::string::npos,
                              "help text should describe case-path as required");
        NS_TEST_ASSERT_MSG_NE(output.find("Typical usage:"),
                              std::string::npos,
                              "help text should include quick-example usage guidance");
        NS_TEST_ASSERT_MSG_NE(output.find("initial-task-start-offset-window"),
                              std::string::npos,
                              "help text should expose the initial task offset window");
        NS_TEST_ASSERT_MSG_NE(output.find("link-delay-offset-window"),
                              std::string::npos,
                              "help text should expose the link delay offset window");
        NS_TEST_ASSERT_MSG_NE(output.find("timing-offset-seed"),
                              std::string::npos,
                              "help text should expose the shared timing offset seed");
        NS_TEST_ASSERT_MSG_EQ(output.find("traffic.csv / UbTrafficGen is single-process only"),
                              std::string::npos,
                              "help text should not advertise the removed MPI TrafficGen boundary");
    }
};

class UbQuickExampleRuntimeCatalogQuerySystemTest : public TestCase
{
  public:
    UbQuickExampleRuntimeCatalogQuerySystemTest()
        : TestCase("UnifiedBus - ub-quick-example exposes runtime parameter catalog metadata")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [attributeStatus, attributeOutput] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + "-attribute.log"),
                                   "--ClassName=ns3::UbPort --AttributeName=UbDataRate",
                                   "");
        NS_TEST_ASSERT_MSG_EQ(attributeStatus,
                              0,
                              "runtime attribute query should exit successfully");
        NS_TEST_ASSERT_MSG_NE(attributeOutput.find("Attribute: UbDataRate"),
                              std::string::npos,
                              "runtime attribute query should print the requested attribute");

        auto [globalStatus, globalOutput] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + "-global.log"),
                                   "--PrintUbGlobals",
                                   "");
        NS_TEST_ASSERT_MSG_EQ(globalStatus,
                              0,
                              "runtime Unified Bus global query should exit successfully");
        NS_TEST_ASSERT_MSG_NE(globalOutput.find("Global: UB_TRACE_ENABLE"),
                              std::string::npos,
                              "runtime global query should print Unified Bus globals");

        auto [singleGlobalStatus, singleGlobalOutput] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + "-single-global.log"),
                                   "--GlobalName=UB_TRACE_ENABLE",
                                   "");
        NS_TEST_ASSERT_MSG_EQ(singleGlobalStatus,
                              0,
                              "single runtime Unified Bus global query should exit successfully");
        NS_TEST_ASSERT_MSG_NE(singleGlobalOutput.find("Global: UB_TRACE_ENABLE"),
                              std::string::npos,
                              "single runtime global query should print the requested global");
    }
};

class UbQuickExampleLocalSingleThreadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalSingleThreadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local mtp-threads=1 runs as single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--mtp-threads=1",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example local mtp-threads=1 should exit successfully");
        NS_TEST_ASSERT_MSG_EQ(output.find("MPI_Testany() ... before MPI_INIT"), std::string::npos,
                              "ub-quick-example local mtp-threads=1 should not touch MPI before MPI_Init");
    }
};

class UbQuickScratchLegacyAliasSystemTest : public TestCase
{
  public:
    UbQuickScratchLegacyAliasSystemTest()
        : TestCase("UnifiedBus - legacy scratch ub-quick-example remains usable")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path casePath = repoRoot / "scratch/2nodes_single-tp";
        auto [status, output] =
            RunNs3RunCommand(CreateTempDirFilename(GetName() + ".log"),
                             "scratch/ub-quick-example --case-path=" + casePath.string() +
                                 " --test",
                             "",
                             false);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "legacy scratch quick-example should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "legacy scratch quick-example should complete the local case");
    }
};

class UbQuickExampleSameCasePathSystemTest : public TestCase
{
  public:
    UbQuickExampleSameCasePathSystemTest()
        : TestCase("UnifiedBus - ub-quick-example accepts equivalent duplicated case-path inputs")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path sameCasePath =
            (repoRoot / "scratch/2nodes_single-tp/../2nodes_single-tp").lexically_normal();
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" +
                                       (repoRoot / "scratch/2nodes_single-tp").string() + "\" \"" +
                                       sameCasePath.string() + "\" --stop-ms=1",
                                   "",
                                   "");

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example should accept equivalent duplicated case-path inputs");
        NS_TEST_ASSERT_MSG_EQ(output.find("conflicting case paths provided via --case-path and casePath"),
                              std::string::npos,
                              "equivalent duplicated case-path inputs should not trigger a conflict");
    }
};

class UbQuickExampleConflictingCasePathSystemTest : public TestCase
{
  public:
    UbQuickExampleConflictingCasePathSystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects conflicting case-path inputs")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path repoRoot = LocateRepoRoot();
        const std::filesystem::path positionalCasePath = repoRoot / "scratch/ub-mpi-minimal";
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "\"" + positionalCasePath.string() + "\"",
                                   "",
                                   "scratch/2nodes_single-tp");

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "ub-quick-example with conflicting case paths should exit with failure");
        NS_TEST_ASSERT_MSG_NE(output.find("conflicting case paths provided via --case-path and casePath"),
                              std::string::npos,
                              "ub-quick-example should print a clear conflicting case-path error");
    }
};

class UbQuickExampleOptionalTransportChannelSystemTest : public TestCase
{
  public:
    UbQuickExampleOptionalTransportChannelSystemTest()
        : TestCase("UnifiedBus - ub-quick-example succeeds without transport_channel.csv")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            CopyCaseDirWithoutFile("scratch/2nodes_single-tp", "transport_channel.csv");
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   "--case-path=\"" + caseDir.string() + "\"",
                                   "",
                                   "");

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "ub-quick-example should accept case directories without transport_channel.csv");
    }
};

class UbQuickExampleLegacyNetworkAttributeHintSystemTest : public TestCase
{
  public:
    UbQuickExampleLegacyNetworkAttributeHintSystemTest()
        : TestCase("UnifiedBus - ub-quick-example reports migration hint for legacy network attribute keys")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            CopyCaseDirWithoutFile("scratch/2nodes_single-tp", "transport_channel.csv");
        std::ofstream config(caseDir / "network_attribute.txt", std::ios::app);
        config << "\ndefault ns3::UbQueueManager::ResumeOffset \"4096\"\n";
        config.close();

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--stop-ms=1",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "legacy network attribute key should reject the case before ConfigStore");
        NS_TEST_ASSERT_MSG_NE(
            output.find("Legacy network_attribute.txt key: ns3::UbQueueManager::ResumeOffset"),
            std::string::npos,
            "legacy key diagnostic should name the old key");
        NS_TEST_ASSERT_MSG_NE(
            output.find("Use ns3::UbQueueManager::DynamicPfcResumeGapBytes instead"),
            std::string::npos,
            "legacy key diagnostic should name the replacement key");
        NS_TEST_ASSERT_MSG_EQ(
            output.find("Could not set default value for ns3::UbQueueManager::ResumeOffset"),
            std::string::npos,
            "legacy key should be caught before the generic ConfigStore failure");
    }
};

class UbQuickExampleCtpMaxOutstandingMigrationHintSystemTest : public TestCase
{
  public:
    UbQuickExampleCtpMaxOutstandingMigrationHintSystemTest()
        : TestCase("UnifiedBus - ub-quick-example reports migration hint for legacy CTP max outstanding")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            CopyCaseDirWithoutFile("scratch/2nodes_single-tp", "transport_channel.csv");
        std::ofstream config(caseDir / "network_attribute.txt", std::ios::app);
        config << "\ndefault ns3::UbCtpTransportService::MaxOutstandingTransactions \"2\"\n";
        config.close();

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--stop-ms=1",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status,
                              0,
                              "legacy CTP max outstanding key should reject the case before ConfigStore");
        NS_TEST_ASSERT_MSG_NE(
            output.find("Legacy network_attribute.txt key: "
                        "ns3::UbCtpTransportService::MaxOutstandingTransactions"),
            std::string::npos,
            "legacy key diagnostic should name the old CTP key");
        NS_TEST_ASSERT_MSG_NE(output.find("Use ns3::UbJetty::UbJettyInflightMax instead"),
                              std::string::npos,
                              "legacy key diagnostic should name the Jetty replacement key");
        NS_TEST_ASSERT_MSG_EQ(
            output.find("Could not set default value for "
                        "ns3::UbCtpTransportService::MaxOutstandingTransactions"),
            std::string::npos,
            "legacy key should be caught before the generic ConfigStore failure");
    }
};

class UbQuickExampleDisabledRetransStopsOnDropSystemTest : public TestCase
{
  public:
    UbQuickExampleDisabledRetransStopsOnDropSystemTest()
        : TestCase("UnifiedBus - ub-quick-example stops when packet drops with retransmission disabled")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,131072,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbSwitch::FlowControl \"CBFC\"",
                      "default ns3::UbSwitch::FlowControl \"NONE\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"1048576\"",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"512\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"7\"",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"64\"");

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --stop-ms=5",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "run should fail-fast after packet drop without retransmission");
        NS_TEST_ASSERT_MSG_NE(output.find("Packet dropped while retransmission is disabled"),
                              std::string::npos,
                              "run should explain why it stopped early");
    }
};

class UbQuickExampleConfiguredGbnRetransCanContinueSystemTest : public TestCase
{
  public:
    UbQuickExampleConfiguredGbnRetransCanContinueSystemTest()
        : TestCase("UnifiedBus - ub-quick-example accepts GBN retransmission config")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,131072,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbSwitch::FlowControl \"CBFC\"",
                      "default ns3::UbSwitch::FlowControl \"NONE\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"7\"",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"64\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbTransportChannel::EnableRetrans \"false\"",
                      "default ns3::UbTransportChannel::EnableRetrans \"true\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"1048576\"",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"512\"");

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --stop-ms=5",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(output.find("Packet dropped while retransmission is disabled"),
                              std::string::npos,
                              "GBN retransmission run should not print the disabled-retrans diagnostic");
        NS_TEST_ASSERT_MSG_EQ(status, 0, "GBN retransmission run should not fail-fast");
    }
};

class UbQuickExampleSelectiveRetransConfigSystemTest : public TestCase
{
  public:
    UbQuickExampleSelectiveRetransConfigSystemTest()
        : TestCase("UnifiedBus - ub-quick-example accepts selective retransmission config")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,131072,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbSwitch::FlowControl \"CBFC\"",
                      "default ns3::UbSwitch::FlowControl \"NONE\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"7\"",
                      "default ns3::UbTransportChannel::MaxRetransAttempts \"64\"");
        ReplaceInFile(caseDir / "network_attribute.txt",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"1048576\"",
                      "default ns3::UbQueueManager::ReservePerQueueBytes \"512\"");
        std::ofstream config(caseDir / "network_attribute.txt", std::ios::app);
        config << "\ndefault ns3::UbTransportChannel::EnableRetrans \"true\"\n";
        config << "default ns3::UbTransportChannel::RetransmissionMode \"SELECTIVE\"\n";
        config << "default ns3::UbTransportChannel::EnableFastRetrans \"true\"\n";
        config.close();

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --stop-ms=5",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(output.find("Packet dropped while retransmission is disabled"),
                              std::string::npos,
                              "selective retransmission run should not print the disabled-retrans diagnostic");
        NS_TEST_ASSERT_MSG_EQ(status, 0, "selective retransmission run should not fail-fast");
    }
};

class UbQuickExampleLocalDependentDagSingleThreadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalDependentDagSingleThreadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local dependent DAG runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,\n"
            "2,0,1,4096,URMA_WRITE,7,10ns,30,10 20\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/2nodes_single-tp", trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "single-thread dependent DAG case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "single-thread dependent DAG case should report PASSED");
    }
};

class UbQuickExampleLocalSingleUrmaReadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalSingleUrmaReadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local single URMA_READ runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_READ,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "single URMA_READ case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "single URMA_READ case should report PASSED");
    }
};

class UbQuickExampleLocalSingleUrmaWriteSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalSingleUrmaWriteSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local single URMA_WRITE runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "single URMA_WRITE case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "single URMA_WRITE case should report PASSED");
    }
};

class UbQuickExampleLocalWriteThenReadSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalWriteThenReadSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local URMA_WRITE then URMA_READ runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,0,1,4096,URMA_READ,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "dependent URMA write/read case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "dependent URMA write/read case should report PASSED");
    }
};

class UbQuickExampleLocalMixedUrmaReadWriteSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalMixedUrmaReadWriteSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local mixed URMA read-write workload runs in single-thread")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,0,1,8192,URMA_READ,7,10ns,20,\n"
            "2,1,0,4096,URMA_WRITE,7,10ns,30,\n"
            "3,1,0,8192,URMA_READ,7,10ns,40,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(
                CreateTempDirFilename("ub-quick-example-local-mixed-urma-read-write.log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "mixed URMA read/write case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "mixed URMA read/write case should report PASSED");
    }
};

class UbQuickExampleLocalDependentDagMtpRedSystemTest : public TestCase
{
  public:
    UbQuickExampleLocalDependentDagMtpRedSystemTest()
        : TestCase("UnifiedBus - ub-quick-example local dependent DAG fanout runs in MTP")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        std::ostringstream traffic;
        traffic << "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,"
                   "dependOnPhases\n";
        traffic << "0,0,1,4096,URMA_WRITE,7,10ns,10,\n";
        traffic << "1,1,0,4096,URMA_WRITE,7,10ns,20,\n";
        for (uint32_t taskId = 2; taskId <= 40; ++taskId)
        {
            traffic << taskId << ",0,1,4096,URMA_WRITE,7,10ns," << (100 + taskId) << ",10 20\n";
        }
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/2nodes_single-tp", traffic.str());

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --test "
                                               "--dependency-visibility-delay=20ns",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "MTP dependent DAG fanout case should exit successfully");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "MTP dependent DAG fanout case should report PASSED");
    }
};

class UbQuickExampleInvalidTrafficPrioritySystemTest : public TestCase
{
  public:
    UbQuickExampleInvalidTrafficPrioritySystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects out-of-range traffic priority")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,256,10ns,10,\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "out-of-range traffic priority should fail the run");
        NS_TEST_ASSERT_MSG_NE(output.find("Invalid priority field in traffic.csv"),
                              std::string::npos,
                              "traffic priority diagnostic should name the invalid field");
    }
};

class UbQuickExampleLocalDependencyVisibilityDelaySystemTest : public TestCase
{
  public:
    UbQuickExampleLocalDependencyVisibilityDelaySystemTest()
        : TestCase("UnifiedBus - ub-quick-example local dependency visibility delay applies")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,0,1,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns",
                                               "",
                                               caseDir);

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "local dependency visibility delay case should exit successfully");
        if (status != 0)
        {
            std::error_code ec;
            std::filesystem::remove_all(caseDir, ec);
            return;
        }
        const auto taskTimes = ReadTaskStatisticsTimes(caseDir);
        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);
        const bool hasTaskTimes = HasDependencyVisibilityTaskTimes(taskTimes);
        NS_TEST_ASSERT_MSG_EQ(hasTaskTimes, true, "dependent tasks should have start/finish times");
        if (!hasTaskTimes)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ((taskTimes.at(1).startNs >= taskTimes.at(0).finishNs + 20 + 10),
                              true,
                              "dependent task should start after finish, visibility delay, and "
                              "task delay");
    }
};

class UbQuickExampleMtpDependencyVisibilityDelaySystemTest : public TestCase
{
  public:
    UbQuickExampleMtpDependencyVisibilityDelaySystemTest()
        : TestCase("UnifiedBus - ub-quick-example MTP dependency visibility delay applies")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --test",
                                               "",
                                               caseDir);

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "MTP dependency visibility delay should be inferred from UB links");
        if (status != 0)
        {
            std::error_code ec;
            std::filesystem::remove_all(caseDir, ec);
            return;
        }
        const auto taskTimes = ReadTaskStatisticsTimes(caseDir);
        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);
        const bool hasTaskTimes = HasDependencyVisibilityTaskTimes(taskTimes);
        NS_TEST_ASSERT_MSG_EQ(hasTaskTimes, true, "dependent tasks should have start/finish times");
        if (!hasTaskTimes)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ((taskTimes.at(1).startNs >= taskTimes.at(0).finishNs + 20 + 10),
                              true,
                              "dependent task should start after finish, visibility delay, and "
                              "task delay");
    }
};

#ifdef NS3_MTP
class UbQuickExampleMtpZeroDependencyVisibilityDelaySystemTest : public TestCase
{
  public:
    UbQuickExampleMtpZeroDependencyVisibilityDelaySystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects zero MTP dependency visibility delay")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile(kLocalSingleThreadQuickExampleCase, trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --test "
                                               "--dependency-visibility-delay=0ns",
                                               "",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "zero-delay MTP dependency DAG should be rejected");
        NS_TEST_ASSERT_MSG_NE(
            output.find("parallel traffic.csv dependencies require a positive dependency visibility delay"),
            std::string::npos,
            "zero-delay MTP dependency DAG should explain the positive-delay requirement");
    }
};
#endif

#ifdef NS3_MPI
class UbQuickExampleMpiDependencyVisibilityDelaySystemTest : public TestCase
{
  public:
    UbQuickExampleMpiDependencyVisibilityDelaySystemTest()
        : TestCase("UnifiedBus - ub-quick-example MPI dependency visibility delay applies")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/ub-mpi-minimal", trafficCsv);
        const std::filesystem::path canonicalBase =
            CreateSafeTempBasename("ub-mpi-dependency-canonical");

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns "
                                               "--canonical-output=\"" +
                                                   canonicalBase.string() + "\"",
                                               "mpirun -np 2",
                                               caseDir);

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "MPI dependency visibility delay case should exit successfully");
        if (status != 0)
        {
            std::error_code ec;
            std::filesystem::remove_all(caseDir, ec);
            return;
        }
        const std::string canonicalBytes = ReadCanonicalBytes(canonicalBase);
        const auto taskTimes = ExtractCanonicalTaskTimes(canonicalBytes);
        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);
        const bool hasTaskTimes = HasDependencyVisibilityTaskTimes(taskTimes);
        NS_TEST_ASSERT_MSG_EQ(hasTaskTimes, true, "dependent tasks should have start/finish times");
        if (!hasTaskTimes)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ((taskTimes.at(1).startNs >= taskTimes.at(0).finishNs + 10),
                              true,
                              "dependent task should start after dependency visibility and task delay");
    }
};

class UbQuickExampleMpiZeroDependencyVisibilityDelaySystemTest : public TestCase
{
  public:
    UbQuickExampleMpiZeroDependencyVisibilityDelaySystemTest()
        : TestCase("UnifiedBus - ub-quick-example rejects zero MPI dependency visibility delay")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/ub-mpi-minimal", trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=0ns",
                                               "mpirun -np 2",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "zero-delay MPI dependency DAG should be rejected");
        NS_TEST_ASSERT_MSG_NE(
            output.find("parallel traffic.csv dependencies require a positive dependency visibility delay"),
            std::string::npos,
            "zero-delay MPI dependency DAG should explain the positive-delay requirement");
    }
};

class UbQuickExampleHybridDependencyVisibilityDelaySystemTest : public TestCase
{
  public:
    UbQuickExampleHybridDependencyVisibilityDelaySystemTest()
        : TestCase("UnifiedBus - ub-quick-example hybrid dependency visibility delay applies")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/ub-mpi-minimal", trafficCsv);
        const std::filesystem::path canonicalBase =
            CreateSafeTempBasename("ub-hybrid-dependency-canonical");

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --test "
                                               "--dependency-visibility-delay=20ns "
                                               "--canonical-output=\"" +
                                                   canonicalBase.string() + "\"",
                                               "mpirun -np 2",
                                               caseDir);

        NS_TEST_ASSERT_MSG_EQ(status,
                              0,
                              "hybrid dependency visibility delay case should exit successfully");
        if (status != 0)
        {
            std::error_code ec;
            std::filesystem::remove_all(caseDir, ec);
            return;
        }
        const std::string canonicalBytes = ReadCanonicalBytes(canonicalBase);
        const auto taskTimes = ExtractCanonicalTaskTimes(canonicalBytes);
        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);
        const bool hasTaskTimes = HasDependencyVisibilityTaskTimes(taskTimes);
        NS_TEST_ASSERT_MSG_EQ(hasTaskTimes, true, "hybrid dependent tasks should have start/finish times");
        if (!hasTaskTimes)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_EQ((taskTimes.at(1).startNs >= taskTimes.at(0).finishNs + 10),
                              true,
                              "hybrid dependent task should start after visibility and task delay");
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "hybrid dependency visibility delay case should report PASSED");
    }
};

class UbQuickExampleMpiCrossRankPhaseDependencySystemTest : public TestCase
{
  public:
    UbQuickExampleMpiCrossRankPhaseDependencySystemTest()
        : TestCase("UnifiedBus - ub-quick-example runs cross-rank phase dependency under MPI")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,1,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,0,4096,URMA_WRITE,7,10ns,20,10\n";
        const std::filesystem::path caseDir =
            CopyCaseDirWithTrafficFile("scratch/ub-mpi-minimal", trafficCsv);

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns",
                                               "mpirun -np 2",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "MPI cross-rank dependent DAG command should complete");
        if (status != 0)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "MPI cross-rank dependent DAG should report PASSED");
    }
};

class UbQuickExampleCanonicalOutputMatchesMpiSystemTest : public TestCase
{
  public:
    UbQuickExampleCanonicalOutputMatchesMpiSystemTest()
        : TestCase("UnifiedBus - canonical output matches local, MTP, MPI, and hybrid DAG runs")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        constexpr std::size_t kTaskCount = 16;
        const std::string trafficCsv =
            "taskId,sourceNode,destNode,dataSize(Byte),opType,priority,delay,phaseId,dependOnPhases\n"
            "0,0,16,4096,URMA_WRITE,7,10ns,10,\n"
            "1,1,17,4096,URMA_WRITE,7,10ns,11,\n"
            "2,16,0,4096,URMA_WRITE,7,10ns,12,\n"
            "3,17,1,4096,URMA_WRITE,7,10ns,13,\n"
            "4,2,18,4096,URMA_WRITE,7,10ns,20,10 11\n"
            "5,18,2,4096,URMA_WRITE,7,10ns,21,12 13\n"
            "6,3,19,4096,URMA_WRITE,7,10ns,22,10 12\n"
            "7,19,3,4096,URMA_WRITE,7,10ns,23,11 13\n"
            "8,4,20,4096,URMA_WRITE,7,10ns,30,20 21\n"
            "9,20,4,4096,URMA_WRITE,7,10ns,31,22 23\n"
            "10,5,21,4096,URMA_WRITE,7,10ns,32,20 23\n"
            "11,21,5,4096,URMA_WRITE,7,10ns,33,21 22\n"
            "12,6,22,4096,URMA_WRITE,7,10ns,40,30 31\n"
            "13,22,6,4096,URMA_WRITE,7,10ns,41,32 33\n"
            "14,7,23,4096,URMA_WRITE,7,10ns,42,30 33\n"
            "15,23,7,4096,URMA_WRITE,7,10ns,43,31 32\n";

        const std::filesystem::path localCase =
            CopyCaseDirWithTrafficFile("scratch/clos_32hosts-4leafs-8spines_pod2pod", trafficCsv);
        const std::filesystem::path mtpCase =
            CopyCaseDirWithTrafficFile("scratch/clos_32hosts-4leafs-8spines_pod2pod", trafficCsv);
        const std::filesystem::path mpiCase =
            CopyCaseDirWithTrafficFile("scratch/clos_32hosts-4leafs-8spines_pod2pod", trafficCsv);
        const std::filesystem::path hybridCase =
            CopyCaseDirWithTrafficFile("scratch/clos_32hosts-4leafs-8spines_pod2pod", trafficCsv);
        WriteCanonicalClosNodeFile(localCase, false);
        WriteCanonicalClosNodeFile(mtpCase, false);
        WriteCanonicalClosNodeFile(mpiCase, true);
        WriteCanonicalClosNodeFile(hybridCase, true);

        const std::filesystem::path localBase =
            CreateSafeTempBasename("ub-canonical-local");
        const std::filesystem::path mtpBase =
            CreateSafeTempBasename("ub-canonical-mtp");
        const std::filesystem::path mpiBase =
            CreateSafeTempBasename("ub-canonical-mpi");
        const std::filesystem::path hybridBase =
            CreateSafeTempBasename("ub-canonical-hybrid");

        auto [localStatus, localOutput] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + "-local.log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns "
                                               "--canonical-output=\"" +
                                                   localBase.string() + "\"",
                                               "",
                                               localCase);
        auto [mtpStatus, mtpOutput] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + "-mtp.log"),
                                               "--mtp-threads=2 --test "
                                               "--dependency-visibility-delay=20ns "
                                               "--canonical-output=\"" +
                                                   mtpBase.string() + "\"",
                                               "",
                                               mtpCase);
        auto [mpiStatus, mpiOutput] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + "-mpi.log"),
                                               "--mtp-threads=1 --test "
                                               "--dependency-visibility-delay=20ns "
                                               "--canonical-output=\"" +
                                                   mpiBase.string() + "\"",
                                               "mpirun -np 2",
                                               mpiCase);
        auto [hybridStatus, hybridOutput] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + "-hybrid.log"),
                                               "--mtp-threads=2 --test "
                                               "--dependency-visibility-delay=20ns "
                                               "--canonical-output=\"" +
                                                   hybridBase.string() + "\"",
                                               "mpirun -np 2",
                                               hybridCase);

        const std::string localBytes = ReadCanonicalBytes(localBase);
        const std::string mtpBytes = ReadCanonicalBytes(mtpBase);
        const std::string mpiBytes = ReadCanonicalBytes(mpiBase);
        const std::string hybridBytes = ReadCanonicalBytes(hybridBase);

        std::error_code ec;
        std::filesystem::remove_all(localCase, ec);
        std::filesystem::remove_all(mtpCase, ec);
        std::filesystem::remove_all(mpiCase, ec);
        std::filesystem::remove_all(hybridCase, ec);

        NS_TEST_ASSERT_MSG_EQ(localStatus, 0, "local canonical run should pass");
        NS_TEST_ASSERT_MSG_EQ(mtpStatus, 0, "MTP canonical run should pass");
        NS_TEST_ASSERT_MSG_EQ(mpiStatus, 0, "MPI canonical run should pass");
        NS_TEST_ASSERT_MSG_EQ(hybridStatus, 0, "hybrid canonical run should pass");
        const bool localPassed = localOutput.find("TEST : 00000 : PASSED") != std::string::npos;
        const bool mtpPassed = mtpOutput.find("TEST : 00000 : PASSED") != std::string::npos;
        const bool mpiPassed = mpiOutput.find("TEST : 00000 : PASSED") != std::string::npos;
        const bool hybridPassed =
            hybridOutput.find("TEST : 00000 : PASSED") != std::string::npos;
        NS_TEST_ASSERT_MSG_EQ(localPassed, true, "local canonical run should print PASSED");
        NS_TEST_ASSERT_MSG_EQ(mtpPassed, true, "MTP canonical run should print PASSED");
        NS_TEST_ASSERT_MSG_EQ(mpiPassed, true, "MPI canonical run should print PASSED");
        NS_TEST_ASSERT_MSG_EQ(hybridPassed, true, "hybrid canonical run should print PASSED");
        NS_TEST_ASSERT_MSG_EQ(localBytes.empty(), false, "canonical output should not be empty");
        NS_TEST_ASSERT_MSG_EQ(CountNonEmptyLines(localBytes),
                              kTaskCount * 2,
                              "canonical output should contain START and COMPLETE_VISIBLE per task");
        NS_TEST_ASSERT_MSG_EQ(CountNonEmptyLines(mtpBytes),
                              kTaskCount * 2,
                              "MTP canonical output should contain every task event");
        NS_TEST_ASSERT_MSG_EQ(CountNonEmptyLines(mpiBytes),
                              kTaskCount * 2,
                              "MPI canonical output should contain every task event");
        NS_TEST_ASSERT_MSG_EQ(CountNonEmptyLines(hybridBytes),
                              kTaskCount * 2,
                              "hybrid canonical output should contain every task event");
        NS_TEST_ASSERT_MSG_EQ(localBytes, mtpBytes, "local and MTP canonical output bytes should match");
        NS_TEST_ASSERT_MSG_EQ(localBytes, mpiBytes, "local and MPI canonical output bytes should match");
        NS_TEST_ASSERT_MSG_EQ(localBytes,
                              hybridBytes,
                              "local and hybrid canonical output bytes should match");
    }
};

class UbQuickExampleMpiSystemTest : public TestCase
{
  public:
    UbQuickExampleMpiSystemTest(const std::string& name,
                                const std::string& casePathRelative,
                                const std::string& extraArgs)
        : TestCase(name),
          m_casePathRelative(casePathRelative),
          m_extraArgs(extraArgs)
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::string args = m_extraArgs.empty() ? "--test" : m_extraArgs + " --test";
        auto [status, output] =
            RunQuickExampleCommand(CreateTempDirFilename(GetName() + ".log"),
                                   args,
                                   "mpirun -np 2",
                                   m_casePathRelative);

        NS_TEST_ASSERT_MSG_EQ(status, 0, "ub-quick-example MPI invocation should complete");
        if (status != 0)
        {
            return;
        }
        NS_TEST_ASSERT_MSG_NE(output.find("TEST : 00000 : PASSED"),
                              std::string::npos,
                              "MPI quick-example should report PASSED");
    }

  private:
    std::string m_casePathRelative;
    std::string m_extraArgs;
};

class UbQuickExampleHybridEmptyRankSystemTest : public TestCase
{
  public:
    UbQuickExampleHybridEmptyRankSystemTest()
        : TestCase("UnifiedBus - hybrid mode rejects an MPI process without owned nodes")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);
        const std::filesystem::path caseDir =
            CopyCaseDirWithoutFile("scratch/ub-mpi-minimal", "node.csv");
        {
            std::ofstream nodeFile(caseDir / "node.csv");
            nodeFile << "nodeId,nodeType,portNum,allocationDelay,forwardDelay,systemId\n"
                        "0,DEVICE,1,1ns,,0\n"
                        "1,DEVICE,1,1ns,,0\n";
        }

        auto [status, output] =
            RunQuickExampleAbsoluteCaseCommand(CreateTempDirFilename(GetName() + ".log"),
                                               "--mtp-threads=2 --test",
                                               "mpirun -np 2",
                                               caseDir);

        std::error_code ec;
        std::filesystem::remove_all(caseDir, ec);

        NS_TEST_ASSERT_MSG_NE(status, 0, "hybrid mode must reject a process without owned nodes");
        NS_TEST_ASSERT_MSG_NE(
            output.find("requires every MPI process to own at least one node"),
            std::string::npos,
            "hybrid empty-process failure should explain the ownership requirement: " << output);
    }
};
#endif

class UbQuickExampleTimingOffsetRegressionSystemTest : public TestCase
{
  public:
    UbQuickExampleTimingOffsetRegressionSystemTest()
        : TestCase("UnifiedBus - timing offset regression is stable across single and MTP")
    {
    }

    void DoRun() override
    {
        SetDataDir(NS_TEST_SOURCEDIR);

        struct RunResult
        {
            int status{0};
            std::string output;
            std::string canonical;
        };

        const auto run = [&](const std::string& label, const std::string& args) {
            const std::filesystem::path canonicalBase =
                CreateSafeTempBasename("ub-timing-offset-" + label);
            auto [status, output] =
                RunQuickExampleCommand(CreateTempDirFilename(GetName() + "-" + label + ".log"),
                                       args +
                                           " --test --dependency-visibility-delay=10ns "
                                           "--canonical-output=\"" +
                                           canonicalBase.string() + "\"",
                                       "",
                                       "scratch/ub_parallel_timing_offset_demo");
            return RunResult{status, std::move(output), ReadCanonicalBytes(canonicalBase)};
        };

        const RunResult noOffsetSingle = run("no-offset-single", "--mtp-threads=1");
        const RunResult noOffsetMtp = run("no-offset-mtp", "--mtp-threads=4");
        const RunResult seedOneSingle = run("seed-1-single",
                                            "--mtp-threads=1 "
                                            "--initial-task-start-offset-window=32ps "
                                            "--link-delay-offset-window=8ps "
                                            "--timing-offset-seed=1");
        const RunResult seedOneMtp = run("seed-1-mtp",
                                         "--mtp-threads=4 "
                                         "--initial-task-start-offset-window=32ps "
                                         "--link-delay-offset-window=8ps "
                                         "--timing-offset-seed=1");
        const RunResult seedTwoSingle = run("seed-2-single",
                                            "--mtp-threads=1 "
                                            "--initial-task-start-offset-window=32ps "
                                            "--link-delay-offset-window=8ps "
                                            "--timing-offset-seed=2");
        const RunResult seedTwoMtp = run("seed-2-mtp",
                                         "--mtp-threads=4 "
                                         "--initial-task-start-offset-window=32ps "
                                         "--link-delay-offset-window=8ps "
                                         "--timing-offset-seed=2");

        const std::array<std::pair<const char*, const RunResult*>, 6> results{{
            {"no-offset single", &noOffsetSingle},
            {"no-offset MTP", &noOffsetMtp},
            {"seed 1 single", &seedOneSingle},
            {"seed 1 MTP", &seedOneMtp},
            {"seed 2 single", &seedTwoSingle},
            {"seed 2 MTP", &seedTwoMtp},
        }};
        for (const auto& [label, result] : results)
        {
            NS_TEST_ASSERT_MSG_EQ(result->status, 0, label << " run should exit successfully");
            NS_TEST_ASSERT_MSG_NE(result->output.find("TEST : 00000 : PASSED"),
                                  std::string::npos,
                                  label << " run should report PASSED");
        }

        NS_TEST_ASSERT_MSG_EQ(CountNonEmptyLines(noOffsetSingle.canonical),
                              48u,
                              "regression output should contain two events for each task");
        NS_TEST_ASSERT_MSG_EQ(noOffsetSingle.canonical,
                              noOffsetMtp.canonical,
                              "runtime fixes should stabilize the no-offset regression case");
        NS_TEST_ASSERT_MSG_EQ(seedOneSingle.canonical,
                              seedOneMtp.canonical,
                              "seed 1 should be stable across single and MTP");
        NS_TEST_ASSERT_MSG_EQ(seedTwoSingle.canonical,
                              seedTwoMtp.canonical,
                              "seed 2 should be stable across single and MTP");
        NS_TEST_ASSERT_MSG_NE(seedOneSingle.canonical,
                              seedTwoSingle.canonical,
                              "different seeds should produce different release schedules");
        NS_TEST_ASSERT_MSG_NE(
            noOffsetMtp.output.find("Initial task start offset is off (default)"),
            std::string::npos,
            "MTP should guide users when initial task offsets are disabled");
        NS_TEST_ASSERT_MSG_NE(noOffsetMtp.output.find("events with the same simulation time"),
                              std::string::npos,
                              "MTP should report its same-time event ordering boundary");
        NS_TEST_ASSERT_MSG_NE(
            seedOneMtp.output.find(
                "Initial task start offset enabled: window=32ps, seed=1"),
            std::string::npos,
            "enabled initial task offset should report its window and shared seed");
        NS_TEST_ASSERT_MSG_NE(
            seedOneMtp.output.find(
                "Link delay offset enabled: window=8ps, seed=1, positive-links="),
            std::string::npos,
            "enabled link delay offset should report its window and shared seed");
    }
};

class UbQuickExampleSystemTestSuite : public TestSuite
{
  public:
    UbQuickExampleSystemTestSuite()
        : TestSuite("unified-bus-examples", Type::SYSTEM)
    {
        AddTestCase(new UbQuickExampleMissingCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMissingCaseDirSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMissingCaseFileSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleHelpTextSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleRuntimeCatalogQuerySystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleThreadSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickScratchLegacyAliasSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleSameCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleConflictingCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleOptionalTransportChannelSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLegacyNetworkAttributeHintSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleCtpMaxOutstandingMigrationHintSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleDisabledRetransStopsOnDropSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleConfiguredGbnRetransCanContinueSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleSelectiveRetransConfigSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleUrmaWriteSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleUrmaReadSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalWriteThenReadSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalMixedUrmaReadWriteSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalDependentDagSingleThreadSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalDependentDagMtpRedSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleInvalidTrafficPrioritySystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalDependencyVisibilityDelaySystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMtpDependencyVisibilityDelaySystemTest(),
                    TestCase::Duration::QUICK);
#ifdef NS3_MTP
        AddTestCase(new UbQuickExampleMtpZeroDependencyVisibilityDelaySystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalMtpSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleTimingOffsetRegressionSystemTest(),
                    TestCase::Duration::QUICK);
#else
        AddTestCase(new UbQuickExampleMtpRequestedWithoutMtpSystemTest(),
                    TestCase::Duration::QUICK);
#endif
#ifdef NS3_MPI
        AddTestCase(new UbQuickExampleHybridEmptyRankSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiDependencyVisibilityDelaySystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiZeroDependencyVisibilityDelaySystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleHybridDependencyVisibilityDelaySystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleCanonicalOutputMatchesMpiSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleSpoofedMpiEnvSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example runs MPI minimal case",
                                                    "scratch/ub-mpi-minimal",
                                                    ""),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example runs MPI mtp-threads=1 case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=1"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example runs hybrid minimal case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=2"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example runs hybrid ldst case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=2"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiSystemTest("UnifiedBus - ub-quick-example runs hybrid multi-remote case",
                                                    "scratch/ub-mpi-minimal",
                                                    "--mtp-threads=2"),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMpiCrossRankPhaseDependencySystemTest(),
                    TestCase::Duration::QUICK);
#endif
    }
};

static UbQuickExampleSystemTestSuite g_ubQuickExampleSystemTestSuite;

class UbQuickExampleSmokeTestSuite : public TestSuite
{
  public:
    UbQuickExampleSmokeTestSuite()
        : TestSuite("unified-bus-examples-smoke", Type::SYSTEM)
    {
        AddTestCase(new UbQuickExampleLocalSingleThreadSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleOptionalTransportChannelSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleConfiguredGbnRetransCanContinueSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleSelectiveRetransConfigSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleUrmaWriteSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLocalSingleUrmaReadSystemTest(),
                    TestCase::Duration::QUICK);
    }
};

static UbQuickExampleSmokeTestSuite g_ubQuickExampleSmokeTestSuite;

class UbQuickExampleCompatTestSuite : public TestSuite
{
  public:
    UbQuickExampleCompatTestSuite()
        : TestSuite("unified-bus-examples-compat", Type::SYSTEM)
    {
        AddTestCase(new UbQuickExampleMissingCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMissingCaseDirSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleMissingCaseFileSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleHelpTextSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickScratchLegacyAliasSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleSameCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleConflictingCasePathSystemTest(), TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleLegacyNetworkAttributeHintSystemTest(),
                    TestCase::Duration::QUICK);
        AddTestCase(new UbQuickExampleCtpMaxOutstandingMigrationHintSystemTest(),
                    TestCase::Duration::QUICK);
    }
};

static UbQuickExampleCompatTestSuite g_ubQuickExampleCompatTestSuite;
