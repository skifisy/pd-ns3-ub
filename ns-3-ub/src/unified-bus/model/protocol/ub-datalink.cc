// SPDX-License-Identifier: GPL-2.0-only
#include "ub-datalink.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbDataLink");
NS_OBJECT_ENSURE_REGISTERED(UbDataLink);

UbDataLink::UbDataLink() {}

UbDataLink::~UbDataLink() {}

TypeId UbDataLink::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbDataLink")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbDataLink>();
    return tid;
}

UbDatalinkControlCreditHeader UbDataLink::ParseCreditHeader(Ptr<Packet> p, Ptr<UbPort> port)
{
    UbDatalinkControlCreditHeader header;
    p->PeekHeader(header);
    uint8_t credits[16];
    header.GetAllCreditsVL(credits);
    port->ResetCredits();
    for (int i = 0; i < 16; i++) {
        port->SetCredits(i, credits[i]);
    }
    p->RemoveHeader(header);
    return header;
}

UbDatalinkPacketHeader UbDataLink::ParsePacketHeader(Ptr<Packet> p)
{
    UbDatalinkPacketHeader header;
    p->RemoveHeader(header);
    return header;
}

Ptr<Packet> UbDataLink::GenControlCreditPacket(const uint8_t credits[16])
{
    Ptr<Packet> p = Create<Packet>(0);
    UbDatalinkControlCreditHeader controlCreditHeader;
    controlCreditHeader.SetAllCreditsVL(credits);
    controlCreditHeader.SetSD(1);
    controlCreditHeader.SetType(1);
    p->AddHeader(controlCreditHeader);
    return p;
}

void UbDataLink::GenPacketHeader(Ptr<Packet> p, bool credit, bool ack, uint8_t crdVl, uint8_t pktVl, bool mode,
                                 bool policy, UbDatalinkHeaderConfig config)
{
    NS_ABORT_MSG_IF(config != UbDatalinkHeaderConfig::CONTROL && pktVl == 0,
                    "Unified-bus reserves priority 0 for locally generated control frames in the "
                    "simulator model. Data packets must use priority 1..15.");

    UbDatalinkPacketHeader linkPacketHeader;
    linkPacketHeader.SetCredit(credit);             // 报文是否返回信用证
    linkPacketHeader.SetACK(ack);                   // 报文是否释放retry buffer空间
    linkPacketHeader.SetCreditTargetVL(crdVl);      // 4 bits: 指定接收credit的VL
    linkPacketHeader.SetPacketVL(pktVl);            // 4 bits: 数据包的VL
    linkPacketHeader.SetLoadBalanceMode(mode);      // 1 bit: 0=per flow, 1=per packet
    linkPacketHeader.SetRoutingPolicy(policy);      // 1 bit: 0=all paths, 1=shortest paths
    linkPacketHeader.SetConfig(static_cast<uint8_t>(config));
    p->AddHeader(linkPacketHeader);
}

} // namespace ns3
