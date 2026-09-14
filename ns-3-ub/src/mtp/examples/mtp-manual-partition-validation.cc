/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/**
 * @file
 * @ingroup mtp
 *
 * Verify that manual MTP partitioning rejects a node that has no worker LP.
 */

#include "ns3/core-module.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"

#include <iostream>
#include <limits>

using namespace ns3;

namespace
{

uint32_t g_eventLpId = std::numeric_limits<uint32_t>::max();

void
RunNodeEvent()
{
    g_eventLpId = Simulator::GetSystemId();
}

} // namespace

int
main(int argc, char* argv[])
{
    bool testing = false;
    bool valid = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Print regression-test output", testing);
    cmd.AddValue("valid", "Assign the node to worker partition 1", valid);
    cmd.Parse(argc, argv);

    MtpInterface::Enable(1, 1);

    // Worker LP IDs start at 1; LP 0 is the public LP and cannot own a node.
    NodeContainer firstNode;
    firstNode.Create(1, 1);
    NodeContainer node;
    node.Create(1, valid ? 1 : 0);

    Simulator::ScheduleWithContext(node.Get(0)->GetId(), NanoSeconds(1), &RunNodeEvent);
    Simulator::Run();

    const bool passed = valid && g_eventLpId == 1;
    if (testing)
    {
        std::cout << "TEST : 0 : " << (passed ? "PASSED" : "FAILED") << " lp=" << g_eventLpId
                  << std::endl;
    }

    Simulator::Destroy();
    return passed ? 0 : 1;
}
