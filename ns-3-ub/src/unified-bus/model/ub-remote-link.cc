// SPDX-License-Identifier: GPL-2.0-only
#include "ub-remote-link.h"

#include "ns3/log.h"
#include "ns3/mpi-interface.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbRemoteLink");
NS_OBJECT_ENSURE_REGISTERED(UbRemoteLink);

TypeId
UbRemoteLink::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRemoteLink")
                            .SetParent<UbLink>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbRemoteLink>();
    return tid;
}

UbRemoteLink::UbRemoteLink()
    : UbLink()
{
}

UbRemoteLink::~UbRemoteLink() = default;

bool
UbRemoteLink::TransmitStart(Ptr<Packet> p, Ptr<UbPort> src, Time txTime)
{
    NS_LOG_FUNCTION(this << p << src);
    NS_LOG_LOGIC("UID is " << p->GetUid() << ")");

    IsInitialized();

    Ptr<UbPort> dst = GetDestination(src);
    if (!m_transmitObserver.IsNull())
    {
        m_transmitObserver(p->Copy(), src, dst);
    }

    Time rxTime = Simulator::Now() + txTime + GetDelay();
    MpiInterface::SendPacket(p->Copy(), rxTime, dst->GetNode()->GetId(), dst->GetIfIndex());
    return true;
}

bool
UbRemoteLink::IsRemote(void) const
{
    return true;
}

void
UbRemoteLink::SetTransmitObserver(TransmitObserver observer)
{
    m_transmitObserver = observer;
}

void
UbRemoteLink::ClearTransmitObserver()
{
    m_transmitObserver = MakeNullCallback<void, Ptr<const Packet>, Ptr<UbPort>, Ptr<UbPort>>();
}

} // namespace ns3
