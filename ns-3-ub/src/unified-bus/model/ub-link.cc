// SPDX-License-Identifier: GPL-2.0-only
#include "ub-link.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE("UbLink");

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbLink);

TypeId UbLink::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbLink")
            .SetParent<Channel>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbLink>()
            .AddAttribute("Delay",
                          "Propagation delay of the point-to-point link.",
                          TimeValue(Seconds(0)),
                          MakeTimeAccessor(&UbLink::m_delay),
                          MakeTimeChecker());
    return tid;
}

UbLink::UbLink() : PointToPointChannel()
{
    NS_LOG_FUNCTION_NOARGS();
    m_nDevices = 0;
}

void UbLink::Attach(Ptr<UbPort> device)
{
    NS_LOG_FUNCTION(this << device);
    NS_ASSERT_MSG(m_nDevices < nDevices, "Only two devices permitted");
    NS_ASSERT(device != nullptr);

    m_link[m_nDevices++].m_src = device;
    //
    // If we have both devices connected to the link, then finish introducing
    // the two halves and set the links to IDLE.
    //
    if (m_nDevices == nDevices) {
        m_link[0].m_dst = m_link[1].m_src;
        m_link[1].m_dst = m_link[0].m_src;
        m_link[0].m_state = WireState::IDLE;
        m_link[1].m_state = WireState::IDLE;
    }
}

bool UbLink::TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime)
{
    NS_LOG_FUNCTION(this << p << src);
    NS_LOG_LOGIC("UID is " << p->GetUid() << ")");

    NS_ASSERT(m_link[0].m_state != WireState::INITIALIZING);
    NS_ASSERT(m_link[1].m_state != WireState::INITIALIZING);

    uint32_t wire = src == m_link[0].m_src ? 0 : 1;

    Simulator::ScheduleWithContext(m_link[wire].m_dst->GetNode()->GetId(), txTime + m_delay, &UbPort::Receive,
                                   m_link[wire].m_dst, p);

    return true;
}

size_t UbLink::GetNDevices(void) const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_nDevices;
}

Ptr<UbPort> UbLink::GetUbPort(uint32_t i) const
{
    NS_LOG_FUNCTION_NOARGS();
    NS_ASSERT(i < nDevices);
    return m_link[i].m_src;
}

Ptr<NetDevice> UbLink::GetDevice(std::size_t i) const
{
    NS_LOG_FUNCTION_NOARGS();
    return GetUbPort(i);
}

Time UbLink::GetDelay(void) const
{
    return m_delay;
}

Ptr<UbPort> UbLink::GetSource(uint32_t i) const
{
    return m_link[i].m_src;
}

Ptr<UbPort> UbLink::GetDestination(uint32_t i) const
{
    return m_link[i].m_dst;
}

Ptr<UbPort> UbLink::GetDestination(Ptr<UbPort> src) const
{
    uint32_t wire = src == m_link[0].m_src ? 0 : 1;
    return m_link[wire].m_dst;
}

bool UbLink::IsRemote(void) const
{
    return false;
}

bool UbLink::IsInitialized(void) const
{
    NS_ASSERT(m_link[0].m_state != WireState::INITIALIZING);
    NS_ASSERT(m_link[1].m_state != WireState::INITIALIZING);
    return true;
}

} // namespace ns3
