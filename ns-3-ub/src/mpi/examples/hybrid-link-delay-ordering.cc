/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/**
 * @file
 * @ingroup mpi
 *
 * Verify that a hybrid MPI/MTP rank cannot advance past an incoming packet whose
 * receive time is bounded by the only link connected to its local node.
 */

#include "ns3/core-module.h"
#include "ns3/mpi-interface.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-helper.h"

#include <iostream>
#include <limits>
#include <mpi.h>

using namespace ns3;

namespace
{

bool g_sendSucceeded = false;
bool g_receiveSeen = false;
bool g_markerSeen = false;
bool g_timeRegressed = false;
uint64_t g_lastObservedTime = 0;
uint64_t g_receiveTime = std::numeric_limits<uint64_t>::max();
uint64_t g_markerTime = std::numeric_limits<uint64_t>::max();

void
ObserveTime(uint64_t now)
{
    if (now < g_lastObservedTime)
    {
        g_timeRegressed = true;
    }
    g_lastObservedTime = now;
}

bool
ReceivePacket(Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&)
{
    g_receiveSeen = true;
    g_receiveTime = Simulator::Now().GetTimeStep();
    ObserveTime(g_receiveTime);
    return true;
}

void
RunMarker()
{
    g_markerSeen = true;
    g_markerTime = Simulator::Now().GetTimeStep();
    ObserveTime(g_markerTime);
}

void
SendPacket(Ptr<NetDevice> source, Address destination)
{
    g_sendSucceeded = source->Send(Create<Packet>(64), destination, 0x0800);
}

void
ScheduleSend(Ptr<NetDevice> source, Address destination)
{
    Simulator::Schedule(NanoSeconds(1), &SendPacket, source, destination);
}

void
ScheduleMarker()
{
    Simulator::Schedule(NanoSeconds(20), &RunMarker);
}

} // namespace

int
main(int argc, char* argv[])
{
    Time::SetResolution(Time::PS);

    bool testing = false;
    Time linkDelay = NanoSeconds(5);
    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Print regression-test output", testing);
    cmd.AddValue("link-delay", "Cross-rank point-to-point channel delay", linkDelay);
    cmd.Parse(argc, argv);

    MtpInterface::Enable(2);
    MpiInterface::Enable(&argc, &argv);

    const uint32_t rank = MpiInterface::GetSystemId();
    if (MpiInterface::GetSize() != 2)
    {
        std::cerr << "hybrid-link-delay-ordering requires exactly two MPI ranks" << std::endl;
        MpiInterface::Disable();
        return 2;
    }

    NodeContainer nodes;
    nodes.Add(CreateObject<Node>(0));
    nodes.Add(CreateObject<Node>(1));

    PointToPointHelper link;
    link.SetDeviceAttribute("DataRate", StringValue("400Gbps"));
    link.SetChannelAttribute("Delay", TimeValue(linkDelay));
    NetDeviceContainer devices = link.Install(nodes);
    devices.Get(1)->SetReceiveCallback(MakeCallback(&ReceivePacket));

    // The 64-byte payload and PPP header arrive at 7.32ns, well before the 20ns marker.
    if (rank == 0)
    {
        Simulator::ScheduleWithContext(nodes.Get(0)->GetId(),
                                       TimeStep(0),
                                       &ScheduleSend,
                                       devices.Get(0),
                                       devices.Get(1)->GetAddress());
    }
    else
    {
        Simulator::ScheduleWithContext(nodes.Get(1)->GetId(), TimeStep(0), &ScheduleMarker);
    }

    Simulator::Run();

    bool localPassed = rank == 0 ? g_sendSucceeded
                                 : g_receiveSeen && g_markerSeen && !g_timeRegressed &&
                                       g_receiveTime < g_markerTime;
    int localResult = localPassed ? 1 : 0;
    int globalResult = 0;
    MPI_Allreduce(&localResult,
                  &globalResult,
                  1,
                  MPI_INT,
                  MPI_MIN,
                  MpiInterface::GetCommunicator());

    if (testing)
    {
        std::cout << "TEST : " << rank << " : " << (globalResult ? "PASSED" : "FAILED");
        if (rank == 1)
        {
            std::cout << " receive=" << g_receiveTime << " marker=" << g_markerTime
                      << " regressed=" << g_timeRegressed;
        }
        std::cout << std::endl;
    }

    Simulator::Destroy();
    MpiInterface::Disable();
    return globalResult ? 0 : 1;
}
