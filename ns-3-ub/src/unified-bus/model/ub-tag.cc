// SPDX-License-Identifier: GPL-2.0-only
#include "ub-tag.h"

#include "ns3/object-base.h"

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(UbPacketTraceTag);
NS_OBJECT_ENSURE_REGISTERED(UbFlowTag);
NS_OBJECT_ENSURE_REGISTERED(UbSimOriginJettyTag);

UbSimOriginJettyTag::UbSimOriginJettyTag(uint32_t originJetty)
    : m_originJetty(originJetty)
{
}

TypeId UbSimOriginJettyTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UbSimOriginJettyTag")
                            .SetParent<Tag>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbSimOriginJettyTag>();
    return tid;
}

TypeId UbSimOriginJettyTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t UbSimOriginJettyTag::GetSerializedSize() const
{
    return 4;
}

void UbSimOriginJettyTag::Serialize(TagBuffer buffer) const
{
    buffer.WriteU32(m_originJetty);
}

void UbSimOriginJettyTag::Deserialize(TagBuffer buffer)
{
    m_originJetty = buffer.ReadU32();
}

void UbSimOriginJettyTag::Print(std::ostream& os) const
{
    os << "OriginJetty=" << m_originJetty;
}

uint32_t UbSimOriginJettyTag::GetOriginJetty() const
{
    return m_originJetty;
}

} // namespace ns3
