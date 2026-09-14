/*
 * SPDX-License-Identifier: GPL-2.0-only
 */

/**
 * @file
 * @ingroup mtp
 *
 * Verify stable keyed ordering when worker LPs submit same-time public events.
 */

#include "ns3/core-module.h"
#include "ns3/mtp-interface.h"
#include "ns3/network-module.h"

#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

using namespace ns3;

namespace
{

constexpr uint32_t kBatchCount = 100;
constexpr uint32_t kTotalBatchCount = kBatchCount * 2;
std::vector<std::pair<uint32_t, uint32_t>> g_received;
bool g_wrongLp = false;
bool g_publicFollowupRan = false;

void
NoOp()
{
}

void
RecordValue(uint32_t batch, uint32_t value)
{
    g_wrongLp |= Simulator::GetSystemId() != 0;
    g_received.emplace_back(batch, value);
}

void
SubmitValue(Time targetTime, uint64_t orderKey, uint32_t batch, uint32_t value)
{
    MtpInterface::ScheduleGlobalAtOrdered(targetTime, orderKey, &RecordValue, batch, value);
}

void
RecordPublicFollowup()
{
    g_wrongLp |= Simulator::GetSystemId() != 0;
    g_publicFollowupRan = true;
}

void
SubmitPublicFollowup(Time targetTime)
{
    MtpInterface::ScheduleGlobalAtOrdered(targetTime, 0, &RecordPublicFollowup);
}

} // namespace

int
main(int argc, char* argv[])
{
    bool testing = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("test", "Print regression-test output", testing);
    cmd.Parse(argc, argv);

    MtpInterface::Enable(2, 2);
    NodeContainer highKeySender;
    highKeySender.Create(1, 1);
    NodeContainer lowKeySender;
    lowKeySender.Create(1, 2);

    for (uint32_t batch = 0; batch < kBatchCount; ++batch)
    {
        const uint32_t concurrentBatch = batch * 2;
        const Time concurrentSubmitTime = NanoSeconds(batch * 40 + 1);
        const Time concurrentTargetTime = NanoSeconds(batch * 40 + 10);
        Simulator::ScheduleWithContext(highKeySender.Get(0)->GetId(),
                                       concurrentSubmitTime,
                                       &SubmitValue,
                                       concurrentTargetTime,
                                       2,
                                       concurrentBatch,
                                       2);
        Simulator::ScheduleWithContext(lowKeySender.Get(0)->GetId(),
                                       concurrentSubmitTime,
                                       &SubmitValue,
                                       concurrentTargetTime,
                                       1,
                                       concurrentBatch,
                                       1);

        const uint32_t staggeredBatch = concurrentBatch + 1;
        const Time highKeySubmitTime = NanoSeconds(batch * 40 + 21);
        const Time barrierTime = NanoSeconds(batch * 40 + 22);
        const Time lowKeySubmitTime = NanoSeconds(batch * 40 + 23);
        const Time staggeredTargetTime = NanoSeconds(batch * 40 + 30);
        Simulator::ScheduleWithContext(highKeySender.Get(0)->GetId(),
                                       highKeySubmitTime,
                                       &SubmitValue,
                                       staggeredTargetTime,
                                       2,
                                       staggeredBatch,
                                       2);
        MtpInterface::ScheduleGlobalAt(barrierTime, &NoOp);
        Simulator::ScheduleWithContext(lowKeySender.Get(0)->GetId(),
                                       lowKeySubmitTime,
                                       &SubmitValue,
                                       staggeredTargetTime,
                                       1,
                                       staggeredBatch,
                                       1);
    }
    MtpInterface::ScheduleGlobalAt(NanoSeconds(kBatchCount * 40),
                                   &SubmitPublicFollowup,
                                   NanoSeconds(kBatchCount * 40 + 10));

    Simulator::Run();

    bool passed = !g_wrongLp && g_publicFollowupRan && g_received.size() == kTotalBatchCount * 2;
    for (uint32_t batch = 0; passed && batch < kTotalBatchCount; ++batch)
    {
        passed &= g_received[batch * 2] == std::make_pair(batch, 1u);
        passed &= g_received[batch * 2 + 1] == std::make_pair(batch, 2u);
    }

    if (testing)
    {
        std::cout << "TEST : 0 : " << (passed ? "PASSED" : "FAILED")
                  << " concurrent=" << kBatchCount << " staggered=" << kBatchCount
                  << " wrong-lp=" << g_wrongLp << " public-followup=" << g_publicFollowupRan
                  << std::endl;
    }

    Simulator::Destroy();
    return passed ? 0 : 1;
}
