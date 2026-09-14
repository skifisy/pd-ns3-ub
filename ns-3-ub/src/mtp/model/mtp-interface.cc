/*
 * Copyright (c) 2023 State Key Laboratory for Novel Software Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Songyuan Bai <i@f5soft.site>
 */

/**
 * \file
 * \ingroup mtp
 *  Implementation of classes ns3::MtpInterface
 */

#include "mtp-interface.h"

#include "ns3/assert.h"
#include "ns3/config.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <thread>
#include <tuple>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("MtpInterface");

namespace
{

constexpr uint32_t INVALID_NODE_LOCAL_LP_ID = std::numeric_limits<uint32_t>::max();

inline void
CpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

} // namespace

void
MtpInterface::Enable()
{
    g_nodeLocalLpIdsFrozen = false;
#ifdef NS3_MPI
    GlobalValue::Bind("SimulatorImplementationType", StringValue("ns3::HybridSimulatorImpl"));
#else
    GlobalValue::Bind("SimulatorImplementationType",
                      StringValue("ns3::MultithreadedSimulatorImpl"));
#endif
    g_enabled = true;
}

void
MtpInterface::Enable(const uint32_t threadCount)
{
#ifdef NS3_MPI
    Config::SetDefault("ns3::HybridSimulatorImpl::MaxThreads", UintegerValue(threadCount));
#else
    Config::SetDefault("ns3::MultithreadedSimulatorImpl::MaxThreads", UintegerValue(threadCount));
#endif
    MtpInterface::Enable();
}

void
MtpInterface::Enable(const uint32_t threadCount, const uint32_t systemCount)
{
    NS_ASSERT_MSG(threadCount > 0, "There must be at least one thread");

    // called by manual partition
    if (!g_enabled)
    {
        GlobalValue::Bind("SimulatorImplementationType",
                          StringValue("ns3::MultithreadedSimulatorImpl"));
    }

    // set size
    g_threadCount = threadCount;
    g_systemCount = systemCount;
    g_manualPartition = systemCount > 0;
    g_nodeLocalLpIdsFrozen = false;

    // allocate systems
    g_systems = new LogicalProcess[g_systemCount + 1]; // include the public LP
    for (uint32_t i = 0; i <= g_systemCount; i++)
    {
        g_systems[i].Enable(i, g_systemCount + 1);
    }

    StringValue s;
    g_sortMethod.GetValue(s);
    if (s.Get() == "ByExecutionTime")
    {
        g_sortFunc = SortByExecutionTime;
    }
    else if (s.Get() == "ByPendingEventCount")
    {
        g_sortFunc = SortByPendingEventCount;
    }
    else if (s.Get() == "ByEventCount")
    {
        g_sortFunc = SortByEventCount;
    }
    else if (s.Get() == "BySimulationTime")
    {
        g_sortFunc = SortBySimulationTime;
    }

    UintegerValue ui;
    g_sortPeriod.GetValue(ui);
    if (ui.Get() == 0)
    {
        g_period = std::ceil(std::log2(g_systemCount) / 4 + 1);
        NS_LOG_INFO("Secheduling period is automatically set to " << g_period);
    }
    else
    {
        g_period = ui.Get();
    }

    // create a thread local storage key
    // so that we can access the currently assigned LP of each thread
    pthread_key_create(&g_key, nullptr);
    pthread_setspecific(g_key, &g_systems[0]);
}

void
MtpInterface::EnableNew(const uint32_t newSystemCount)
{
    g_manualPartition = false;
    const LogicalProcess* oldSystems = g_systems;
    g_systems = new LogicalProcess[g_systemCount + newSystemCount + 1];
    for (uint32_t i = 0; i <= g_systemCount; i++)
    {
        g_systems[i] = oldSystems[i];
    }
    delete[] oldSystems;

    g_systemCount += newSystemCount;
    for (uint32_t i = 0; i <= g_systemCount; i++)
    {
        g_systems[i].Enable(i, g_systemCount + 1);
    }

    UintegerValue ui;
    g_sortPeriod.GetValue(ui);
    if (ui.Get() == 0)
    {
        g_period = std::ceil(std::log2(g_systemCount) / 4 + 1);
        NS_LOG_INFO("Secheduling period is automatically set to " << g_period);
    }
    else
    {
        g_period = ui.Get();
    }

    // create a thread local storage key
    // so that we can access the currently assigned LP of each thread
    pthread_key_create(&g_key, nullptr);
    pthread_setspecific(g_key, &g_systems[0]);
}

void
MtpInterface::EnableNew(const uint32_t threadCount, const uint32_t newSystemCount)
{
    g_threadCount = threadCount;
    EnableNew(newSystemCount);
}

void
MtpInterface::Disable()
{
    g_threadCount = 0;
    g_systemCount = 0;
    g_sortFunc = nullptr;
    g_lookAheadBound = TimeStep(0);
    g_nodeLocalLpIds.clear();
    g_nodeLocalLpIdsFrozen = false;
    g_manualPartition = false;
    g_globalFinished.store(false, std::memory_order_release);
    g_recvMsgStage.store(false, std::memory_order_release);
    g_systemIndex.store(0, std::memory_order_release);
    g_finishedSystemCount.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_orderedGlobalEventsMutex);
        for (auto& orderedEvent : g_orderedGlobalEvents)
        {
            orderedEvent.event->Unref();
        }
        g_orderedGlobalEvents.clear();
    }
    delete[] g_systems;
    delete[] g_threads;
    delete[] g_sortedSystemIndices;
}

void
MtpInterface::Run()
{
    RunBefore();
    while (!g_globalFinished.load(std::memory_order_acquire))
    {
        ProcessOneRound();
        CalculateSmallestTime();
    }
    RunAfter();
}

void
MtpInterface::RunBefore()
{
    if (g_manualPartition)
    {
        InitializeManualNodeLocalLpIds();
    }
    FreezeNodeLocalLpIds();
    CalculateLookAhead();

    // LP index for sorting & holding worker threads
    g_sortedSystemIndices = new uint32_t[g_systemCount];
    for (uint32_t i = 0; i < g_systemCount; i++)
    {
        g_sortedSystemIndices[i] = i + 1;
    }
    g_systemIndex.store(g_systemCount, std::memory_order_release);

    // start threads
    g_threads = new pthread_t[g_threadCount - 1]; // exclude the main thread
    for (uint32_t i = 0; i < g_threadCount - 1; i++)
    {
        pthread_create(&g_threads[i], nullptr, ThreadFunc, nullptr);
    }
}

void
MtpInterface::ProcessOneRound()
{
    // assign logical process to threads

    // determine the priority of logical processes
    if (g_sortFunc != nullptr && g_round++ % g_period == 0)
    {
        std::sort(g_sortedSystemIndices, g_sortedSystemIndices + g_systemCount, g_sortFunc);
    }

    // stage 1: process events
    g_recvMsgStage.store(false, std::memory_order_release);
    g_finishedSystemCount.store(0, std::memory_order_relaxed);
    g_systemIndex.store(0, std::memory_order_release);
    // main thread also needs to process an LP to reduce an extra thread overhead
    while (true)
    {
        uint32_t index = g_systemIndex.fetch_add(1, std::memory_order_acquire);
        if (index >= g_systemCount)
        {
            break;
        }
        LogicalProcess* system = &g_systems[g_sortedSystemIndices[index]];
        system->ProcessOneRound();
        g_finishedSystemCount.fetch_add(1, std::memory_order_release);
    }

    // logical process barriar synchronization
    while (g_finishedSystemCount.load(std::memory_order_acquire) != g_systemCount)
    {
        CpuRelax();
    };

    // stage 2: process the public LP
    FlushOrderedGlobalEvents(g_smallestTime);
    g_systems[0].ProcessOneRound();

    // stage 3: receive messages
    g_recvMsgStage.store(true, std::memory_order_release);
    g_finishedSystemCount.store(0, std::memory_order_relaxed);
    g_systemIndex.store(0, std::memory_order_release);
    while (true)
    {
        uint32_t index = g_systemIndex.fetch_add(1, std::memory_order_acquire);
        if (index >= g_systemCount)
        {
            break;
        }
        LogicalProcess* system = &g_systems[g_sortedSystemIndices[index]];
        system->ReceiveMessages();
        g_finishedSystemCount.fetch_add(1, std::memory_order_release);
    }

    // logical process barriar synchronization
    while (g_finishedSystemCount.load(std::memory_order_acquire) != g_systemCount)
    {
        CpuRelax();
    };
}

void
MtpInterface::EnqueueOrderedGlobalEvent(const Time& time, uint64_t orderKey, EventImpl* event)
{
    const int64_t targetTs = time.GetTimeStep();
    NS_ABORT_MSG_IF(targetTs < 0 || time < g_systems[0].Now() || time < GetSystem()->Now(),
                    "ordered public event time is earlier than its submitting or public LP");

    std::lock_guard<std::mutex> lock(g_orderedGlobalEventsMutex);
    g_orderedGlobalEvents.push_back({targetTs, orderKey, event});
}

void
MtpInterface::FlushOrderedGlobalEvents(const Time& throughTime)
{
    std::vector<OrderedGlobalEvent> pendingEvents;
    {
        std::lock_guard<std::mutex> lock(g_orderedGlobalEventsMutex);
        std::sort(g_orderedGlobalEvents.begin(),
                  g_orderedGlobalEvents.end(),
                  [](const OrderedGlobalEvent& lhs, const OrderedGlobalEvent& rhs) {
                      return std::tie(lhs.targetTs, lhs.orderKey) <
                             std::tie(rhs.targetTs, rhs.orderKey);
                  });
        const int64_t throughTs = throughTime.GetTimeStep();
        const auto firstFutureEvent = std::find_if(
            g_orderedGlobalEvents.begin(),
            g_orderedGlobalEvents.end(),
            [throughTs](const OrderedGlobalEvent& event) { return event.targetTs > throughTs; });
        pendingEvents.insert(pendingEvents.end(),
                             std::make_move_iterator(g_orderedGlobalEvents.begin()),
                             std::make_move_iterator(firstFutureEvent));
        g_orderedGlobalEvents.erase(g_orderedGlobalEvents.begin(), firstFutureEvent);
    }

    for (auto& orderedEvent : pendingEvents)
    {
        g_systems[0].ScheduleAt(Simulator::NO_CONTEXT,
                                TimeStep(orderedEvent.targetTs),
                                orderedEvent.event);
    }
}

void
MtpInterface::CalculateSmallestTime()
{
    std::optional<Time> nextOrderedGlobalTime;
    {
        std::lock_guard<std::mutex> lock(g_orderedGlobalEventsMutex);
        for (const auto& orderedEvent : g_orderedGlobalEvents)
        {
            const Time targetTime = TimeStep(orderedEvent.targetTs);
            if (!nextOrderedGlobalTime || targetTime < *nextOrderedGlobalTime)
            {
                nextOrderedGlobalTime = targetTime;
            }
        }
    }

    // update smallest time
    g_smallestTime = Time::Max() / 2;
    for (uint32_t i = 0; i <= g_systemCount; i++)
    {
        Time nextTime = g_systems[i].Next();
        if (nextTime < g_smallestTime)
        {
            g_smallestTime = nextTime;
        }
    }
    g_nextPublicTime = g_systems[0].Next();
    if (nextOrderedGlobalTime)
    {
        g_smallestTime = Min(g_smallestTime, *nextOrderedGlobalTime);
        g_nextPublicTime = Min(g_nextPublicTime, *nextOrderedGlobalTime);
    }

    // test if global finished
    bool globalFinished = !nextOrderedGlobalTime.has_value();
    for (uint32_t i = 0; i <= g_systemCount; i++)
    {
        globalFinished &= g_systems[i].isLocalFinished();
    }
    g_globalFinished.store(globalFinished, std::memory_order_release);
}

void
MtpInterface::RunAfter()
{
    // global finished, terminate threads
    g_systemIndex.store(0, std::memory_order_release);
    for (uint32_t i = 0; i < g_threadCount - 1; i++)
    {
        pthread_join(g_threads[i], nullptr);
    }
}

bool
MtpInterface::isEnabled()
{
    return g_enabled;
}

bool
MtpInterface::isPartitioned()
{
    return g_threadCount != 0;
}

void
MtpInterface::ClearNodeLocalLpIds()
{
    NS_ABORT_MSG_IF(g_nodeLocalLpIdsFrozen, "MTP worker partition ownership is already frozen");
    g_nodeLocalLpIds.clear();
}

void
MtpInterface::SetNodeLocalLpId(uint32_t nodeId, uint32_t localLpId)
{
    NS_ABORT_MSG_IF(g_nodeLocalLpIdsFrozen, "MTP worker partition ownership is already frozen");
    NS_ABORT_MSG_IF(localLpId == 0, "MTP worker partition 0 cannot own node " << nodeId);
    if (nodeId >= g_nodeLocalLpIds.size())
    {
        g_nodeLocalLpIds.resize(nodeId + 1, INVALID_NODE_LOCAL_LP_ID);
    }
    g_nodeLocalLpIds[nodeId] = localLpId;
}

void
MtpInterface::FreezeNodeLocalLpIds()
{
    g_nodeLocalLpIdsFrozen = true;
}

std::optional<uint32_t>
MtpInterface::FindNodeLocalLpId(uint32_t nodeId)
{
    if (nodeId >= g_nodeLocalLpIds.size() || g_nodeLocalLpIds[nodeId] == INVALID_NODE_LOCAL_LP_ID)
    {
        return std::nullopt;
    }
    return g_nodeLocalLpIds[nodeId];
}

uint32_t
MtpInterface::RequireNodeLocalLpId(uint32_t nodeId)
{
    auto localLpId = FindNodeLocalLpId(nodeId);
    if (!localLpId && g_manualPartition && !g_nodeLocalLpIdsFrozen)
    {
        InitializeManualNodeLocalLpId(nodeId);
        localLpId = FindNodeLocalLpId(nodeId);
    }
    NS_ABORT_MSG_IF(!localLpId,
                    "MTP worker partition ownership is missing for node "
                        << nodeId << "; ownership is fixed before simulation starts");
    NS_ABORT_MSG_IF(*localLpId == 0 || *localLpId > g_systemCount,
                    "MTP worker partition for node " << nodeId << " is outside range [1, "
                                                     << g_systemCount << "]: " << *localLpId);
    return *localLpId;
}

void
MtpInterface::InitializeManualNodeLocalLpId(uint32_t nodeId)
{
    // NodeList::Add schedules initialization before Node::m_id receives the returned index.
    NS_ABORT_MSG_IF(nodeId >= NodeList::GetNNodes(), "MTP node " << nodeId << " does not exist");
    const uint32_t localLpId = NodeList::GetNode(nodeId)->GetSystemId();
    NS_ABORT_MSG_IF(localLpId == 0 || localLpId > g_systemCount,
                    "MTP worker partition ownership is missing for node "
                        << nodeId << ": manual partition requires Node::systemId in [1, "
                        << g_systemCount << "], got " << localLpId);
    SetNodeLocalLpId(nodeId, localLpId);
}

void
MtpInterface::InitializeManualNodeLocalLpIds()
{
    for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
    {
        InitializeManualNodeLocalLpId(nodeId);
    }
}

void
MtpInterface::CalculateLookAhead()
{
    for (uint32_t i = 1; i <= g_systemCount; i++)
    {
        g_systems[i].CalculateLookAhead();
        if (g_lookAheadBound.IsStrictlyPositive())
        {
            g_systems[i].BoundLookAhead(g_lookAheadBound);
        }
    }
}

void
MtpInterface::BoundLookAhead(Time lookAhead)
{
    if (!lookAhead.IsStrictlyPositive())
    {
        NS_LOG_WARN("attempted to set MTP lookahead bound to a non-positive time: " << lookAhead);
        return;
    }

    if (!g_lookAheadBound.IsStrictlyPositive() || lookAhead < g_lookAheadBound)
    {
        g_lookAheadBound = lookAhead;
    }

    for (uint32_t i = 1; i <= g_systemCount; i++)
    {
        g_systems[i].BoundLookAhead(lookAhead);
    }
}

void*
MtpInterface::ThreadFunc(void* arg)
{
    while (!g_globalFinished.load(std::memory_order_acquire))
    {
        uint32_t index = g_systemIndex.fetch_add(1, std::memory_order_acquire);
        if (index >= g_systemCount)
        {
            while (!g_globalFinished.load(std::memory_order_acquire) &&
                   g_systemIndex.load(std::memory_order_acquire) >= g_systemCount)
            {
                CpuRelax();
            };
            continue;
        }
        LogicalProcess* system = &g_systems[g_sortedSystemIndices[index]];
        if (g_recvMsgStage.load(std::memory_order_acquire))
        {
            system->ReceiveMessages();
        }
        else
        {
            system->ProcessOneRound();
        }
        g_finishedSystemCount.fetch_add(1, std::memory_order_release);
    }
    return nullptr;
}

bool
MtpInterface::SortByExecutionTime(const uint32_t& i, const uint32_t& j)
{
    auto lhs = g_systems[i].GetExecutionTime();
    auto rhs = g_systems[j].GetExecutionTime();
    return lhs == rhs ? i < j : lhs > rhs;
}

bool
MtpInterface::SortByEventCount(const uint32_t& i, const uint32_t& j)
{
    auto lhs = g_systems[i].GetEventCount();
    auto rhs = g_systems[j].GetEventCount();
    return lhs == rhs ? i < j : lhs > rhs;
}

bool
MtpInterface::SortByPendingEventCount(const uint32_t& i, const uint32_t& j)
{
    auto lhs = g_systems[i].GetPendingEventCount();
    auto rhs = g_systems[j].GetPendingEventCount();
    return lhs == rhs ? i < j : lhs > rhs;
}

bool
MtpInterface::SortBySimulationTime(const uint32_t& i, const uint32_t& j)
{
    auto lhs = g_systems[i].Now();
    auto rhs = g_systems[j].Now();
    return lhs == rhs ? i < j : lhs > rhs;
}

bool (*MtpInterface::g_sortFunc)(const uint32_t&, const uint32_t&) = nullptr;

GlobalValue MtpInterface::g_sortMethod =
    GlobalValue("PartitionSchedulingMethod",
                "The scheduling method to determine which partition runs first",
                StringValue("ByExecutionTime"),
                MakeStringChecker());

GlobalValue MtpInterface::g_sortPeriod = GlobalValue("PartitionSchedulingPeriod",
                                                     "The scheduling period of partitions",
                                                     UintegerValue(0),
                                                     MakeUintegerChecker<uint32_t>(0));

uint32_t MtpInterface::g_period = 0;

pthread_t* MtpInterface::g_threads = nullptr;

LogicalProcess* MtpInterface::g_systems = nullptr;

uint32_t MtpInterface::g_threadCount = 0;

uint32_t MtpInterface::g_systemCount = 0;

uint32_t* MtpInterface::g_sortedSystemIndices = nullptr;

std::atomic<uint32_t> MtpInterface::g_systemIndex;

std::atomic<uint32_t> MtpInterface::g_finishedSystemCount;

uint32_t MtpInterface::g_round = 0;

Time MtpInterface::g_smallestTime = TimeStep(0);

Time MtpInterface::g_nextPublicTime = TimeStep(0);

Time MtpInterface::g_lookAheadBound = TimeStep(0);

std::vector<uint32_t> MtpInterface::g_nodeLocalLpIds;

bool MtpInterface::g_nodeLocalLpIdsFrozen = false;

std::atomic<bool> MtpInterface::g_recvMsgStage(false);

std::atomic<bool> MtpInterface::g_globalFinished(false);

bool MtpInterface::g_manualPartition = false;

bool MtpInterface::g_enabled = false;

pthread_key_t MtpInterface::g_key;

std::atomic<bool> MtpInterface::g_inCriticalSection(false);

std::mutex MtpInterface::g_orderedGlobalEventsMutex;

std::vector<MtpInterface::OrderedGlobalEvent> MtpInterface::g_orderedGlobalEvents;

} // namespace ns3
