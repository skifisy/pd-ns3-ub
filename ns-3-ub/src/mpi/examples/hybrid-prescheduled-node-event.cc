/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/**
 * @file
 * @ingroup mpi
 *
 * Verify that hybrid partitioning preserves a future node event scheduled
 * before Simulator::Run().
 */

#include "ns3/core-module.h"
#include "ns3/mpi-interface.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"

#include <iostream>
#include <limits>
#include <mpi.h>

using namespace ns3;

namespace
{

int64_t g_eventTimeNs = std::numeric_limits<int64_t>::max();

void
RunPrescheduledEvent()
{
    g_eventTimeNs = Simulator::Now().GetNanoSeconds();
}

} // namespace

int
main(int argc, char* argv[])
{
    bool testing = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Print regression-test output", testing);
    cmd.Parse(argc, argv);

    MtpInterface::Enable(2);
    MpiInterface::Enable(&argc, &argv);

    const uint32_t rank = MpiInterface::GetSystemId();
    if (MpiInterface::GetSize() != 2)
    {
        std::cerr << "hybrid-prescheduled-node-event requires exactly two MPI ranks" << std::endl;
        MpiInterface::Disable();
        return 2;
    }

    NodeContainer nodes;
    nodes.Add(CreateObject<Node>(0));
    nodes.Add(CreateObject<Node>(1));

    if (rank == 0)
    {
        Simulator::ScheduleWithContext(nodes.Get(0)->GetId(),
                                       NanoSeconds(10),
                                       &RunPrescheduledEvent);
    }

    Simulator::Run();

    const bool localPassed = rank != 0 || g_eventTimeNs == 10;
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
        if (rank == 0)
        {
            std::cout << " event-time-ns=" << g_eventTimeNs;
        }
        std::cout << std::endl;
    }

    Simulator::Destroy();
    MpiInterface::Disable();
    return globalResult ? 0 : 1;
}
