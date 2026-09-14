/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/**
 * @file
 * @ingroup mtp
 *
 * Verify that an absolute-time delivery cannot insert an event behind the LP's
 * current simulation time.
 */

#include "ns3/core-module.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"

#include <iostream>
#include <limits>

using namespace ns3;

namespace
{

int64_t g_eventTimeNs = std::numeric_limits<int64_t>::max();

void
RunAbsoluteTimeEvent()
{
    g_eventTimeNs = Simulator::Now().GetNanoSeconds();
}

void
InjectAbsoluteTimeEvent(uint32_t nodeId, Time targetTime)
{
    MtpInterface::GetSystem()->ScheduleAt(nodeId, targetTime, MakeEvent(&RunAbsoluteTimeEvent));
}

void
ScheduleLateDelivery(uint32_t nodeId, Time targetTime)
{
    Simulator::Schedule(NanoSeconds(10), &InjectAbsoluteTimeEvent, nodeId, targetTime);
}

void
SendZeroDelayRemoteEvent(uint32_t receiverNodeId)
{
    Simulator::ScheduleWithContext(receiverNodeId, TimeStep(0), &RunAbsoluteTimeEvent);
}

void
ScheduleNegativeLocalDelay()
{
    Simulator::Schedule(Time(-1), &RunAbsoluteTimeEvent);
}

void
SendNegativeDelayRemoteEvent(uint32_t receiverNodeId)
{
    Simulator::ScheduleWithContext(receiverNodeId, Time(-1), &RunAbsoluteTimeEvent);
}

void
AdvanceReceiverTime()
{
}

} // namespace

int
main(int argc, char* argv[])
{
    bool testing = false;
    bool sameTime = false;
    bool remotePast = false;
    bool negativeTime = false;
    bool negativeLocalDelay = false;
    bool negativeRemoteDelay = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Print regression-test output", testing);
    cmd.AddValue("same-time",
                 "Schedule the absolute-time event at the LP's current time",
                 sameTime);
    cmd.AddValue("remote-past",
                 "Send a remote event behind the receiver LP's current time",
                 remotePast);
    cmd.AddValue("negative-time",
                 "Schedule the absolute-time event at a negative time",
                 negativeTime);
    cmd.AddValue("negative-local-delay",
                 "Schedule a local event with a negative relative delay",
                 negativeLocalDelay);
    cmd.AddValue("negative-remote-delay",
                 "Schedule a remote event with a negative relative delay",
                 negativeRemoteDelay);
    cmd.Parse(argc, argv);

    const uint32_t selectedModes =
        static_cast<uint32_t>(sameTime) + static_cast<uint32_t>(remotePast) +
        static_cast<uint32_t>(negativeTime) + static_cast<uint32_t>(negativeLocalDelay) +
        static_cast<uint32_t>(negativeRemoteDelay);
    NS_ABORT_MSG_IF(selectedModes > 1, "validation modes are mutually exclusive");
    if (remotePast || negativeRemoteDelay)
    {
        MtpInterface::Enable(1, 2);
        NodeContainer sender;
        sender.Create(1, 1);
        NodeContainer receiver;
        receiver.Create(1, 2);

        // With no modeled link between these LPs, a zero-delay cross-LP event violates lookahead.
        // LP 2 reaches 5ns before it receives the event produced by LP 1 at 1ns.
        if (negativeRemoteDelay)
        {
            Simulator::ScheduleWithContext(sender.Get(0)->GetId(),
                                           NanoSeconds(1),
                                           &SendNegativeDelayRemoteEvent,
                                           receiver.Get(0)->GetId());
        }
        else
        {
            Simulator::ScheduleWithContext(sender.Get(0)->GetId(),
                                           NanoSeconds(1),
                                           &SendZeroDelayRemoteEvent,
                                           receiver.Get(0)->GetId());
        }
        Simulator::ScheduleWithContext(receiver.Get(0)->GetId(),
                                       NanoSeconds(5),
                                       &AdvanceReceiverTime);
    }
    else
    {
        MtpInterface::Enable(1, 1);
        NodeContainer nodes;
        nodes.Create(1, 1);

        if (negativeLocalDelay)
        {
            Simulator::ScheduleWithContext(nodes.Get(0)->GetId(),
                                           TimeStep(0),
                                           &ScheduleNegativeLocalDelay);
        }
        else
        {
            Simulator::ScheduleWithContext(
                nodes.Get(0)->GetId(),
                TimeStep(0),
                &ScheduleLateDelivery,
                nodes.Get(0)->GetId(),
                negativeTime ? Time(-1) : (sameTime ? NanoSeconds(10) : NanoSeconds(5)));
        }
    }
    Simulator::Run();

    const bool passed = selectedModes == 1 && sameTime && g_eventTimeNs == 10;
    if (testing)
    {
        std::cout << "TEST : 0 : " << (passed ? "PASSED" : "FAILED")
                  << " event-time-ns=" << g_eventTimeNs << std::endl;
    }

    Simulator::Destroy();
    return passed ? 0 : 1;
}
