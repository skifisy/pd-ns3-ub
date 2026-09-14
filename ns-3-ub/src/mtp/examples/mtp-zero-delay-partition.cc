/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/**
 * @file
 * @ingroup mtp
 *
 * Verify that automatic MTP partitioning keeps zero-delay neighbors on one LP.
 */

#include "ns3/core-module.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-helper.h"

#include <iostream>
#include <limits>

using namespace ns3;

namespace
{

uint32_t g_firstLpId = std::numeric_limits<uint32_t>::max();
uint32_t g_secondLpId = std::numeric_limits<uint32_t>::max();

void
RecordLpId(uint32_t* result)
{
    *result = Simulator::GetSystemId();
}

} // namespace

int
main(int argc, char* argv[])
{
    bool testing = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Print regression-test output", testing);
    cmd.Parse(argc, argv);

    Config::SetDefault("ns3::MultithreadedSimulatorImpl::MaxThreads", UintegerValue(2));
    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::MultithreadedSimulatorImpl"));

    NodeContainer nodes;
    nodes.Create(2);
    PointToPointHelper link;
    link.SetChannelAttribute("Delay", TimeValue(TimeStep(0)));
    link.Install(nodes);

    Simulator::ScheduleWithContext(nodes.Get(0)->GetId(),
                                   NanoSeconds(1),
                                   &RecordLpId,
                                   &g_firstLpId);
    Simulator::ScheduleWithContext(nodes.Get(1)->GetId(),
                                   NanoSeconds(2),
                                   &RecordLpId,
                                   &g_secondLpId);
    Simulator::Run();

    const bool passed = g_firstLpId != 0 && g_firstLpId == g_secondLpId;
    if (testing)
    {
        std::cout << "TEST : 0 : " << (passed ? "PASSED" : "FAILED") << " first-lp=" << g_firstLpId
                  << " second-lp=" << g_secondLpId << std::endl;
    }

    Simulator::Destroy();
    return passed ? 0 : 1;
}
