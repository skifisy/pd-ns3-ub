// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TE_CONTROLLER_H
#define UB_TE_CONTROLLER_H

#include "ns3/event-id.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ns3 {

struct RoutingKey;
struct TeRouteDecision;

/** One process-wide controller for a static OCS case. In MPI mode each rank
 * would require a distributed reduction, so the example rejects MPI + TE. */
class UbTeController {
public:
    static UbTeController& Get();
    void Configure(const std::string& casePath, double recomputeSeconds,
                   uint32_t historyWindows, double s,
                   const std::string& outputDir, bool debug);
    void Disable();
    void FlushFinalWindow();
    void StopPeriodicEvents(const std::string& reason);
    bool Enabled() const { return m_enabled.load(std::memory_order_acquire); }
    bool DebugEnabled() const;
    void OnNewDataPacket(uint32_t sourceHost, uint16_t sourcePort,
                         uint32_t destinationHost, uint16_t destinationPort,
                         uint32_t payloadBytes);
    void OnSimulationProgress(uint32_t totalTasks, uint32_t pendingTasks,
                              uint32_t readyTasks, uint32_t runningTasks,
                              uint32_t completedTasks);

    // -2: not a TE-managed leaf flow; -1: no safe TE route; >=0: output port.
    int SelectLeafOutPort(uint32_t leaf, const RoutingKey& key, uint64_t flowHash,
                          uint16_t inPort, bool& selectedDirect, TeRouteDecision& decision);
    void WriteWcmpSelectionTrace(uint32_t taskId, const RoutingKey& key,
                                 uint64_t flowHash, const TeRouteDecision& decision);

private:
    using Od = std::pair<uint32_t, uint32_t>;
    using TrafficWindows = std::map<uint64_t, std::map<Od, uint64_t>>;
    using PathWeights = std::map<int32_t, double>; // -1 direct; otherwise transit leaf
    using FlowId = std::pair<uint32_t, uint64_t>;  // source leaf and existing ECMP hash

    static constexpr size_t kDemandShardCount = 64;

    struct DemandShard
    {
        std::mutex mutex;
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint64_t>> windows;
    };

    struct LinkCounter
    {
        uint32_t sourceLeaf;
        uint32_t destinationLeaf;
        uint16_t portId;
        uint64_t lastBytes;
        uint64_t lastPackets;
    };

    UbTeController() = default;
    void Recompute();
    void SampleLinkWindow();
    TrafficWindows ExtractDemandWindows(uint64_t beforeWindow);
    TrafficWindows SnapshotDemandWindows();
    void ClearDemandWindows();
    std::map<Od, uint64_t> CollectLinkDeltas();
    std::vector<uint16_t> PhysicalPorts(uint32_t src, uint32_t dst) const;
    std::vector<std::pair<int32_t, double>> Fallback(uint32_t src, uint32_t dst) const;
    void DebugLog(const std::string& message) const;
    void WriteDebugProgressLocked(const std::string& trigger);
    void WriteDemandEvent(const std::string& result,
                          uint32_t sourceHost, uint16_t sourcePort,
                          uint32_t destinationHost, uint16_t destinationPort,
                          uint32_t payloadBytes, int32_t sourceLeaf,
                          int32_t destinationLeaf);
    std::atomic<bool> m_periodicEventsActive{false};
    EventId m_recomputeEvent;
    EventId m_linkWindowEvent;
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_debug{false};
    std::string m_casePath;
    std::string m_outputDir;
    double m_intervalSeconds = 30.0;
    uint32_t m_historyWindows = 120;
    double m_s = 0.0;
    uint64_t m_epoch = 0;
    std::map<std::pair<uint32_t, uint16_t>, uint32_t> m_hostPortToLeaf;
    std::map<Od, std::vector<uint16_t>> m_leafPorts;
    std::map<Od, double> m_edgeCapacityBps;
    std::map<std::pair<uint32_t, uint16_t>, uint32_t> m_leafPortToPeer;
    std::array<DemandShard, kDemandShardCount> m_demandShards;
    std::vector<LinkCounter> m_linkCounters;
    std::map<uint64_t, std::map<Od, uint64_t>> m_history; // completed windows
    std::map<Od, PathWeights> m_policy;
    std::map<FlowId, int32_t> m_flowTransit; // pin active flows across epochs
    std::set<FlowId> m_loggedTransitFlows;
    std::ofstream m_pathDecisionStream;
    std::ofstream m_wcmpSelectionStream;
    std::ofstream m_transitForwardStream;
    std::ofstream m_demandEventStream;
    std::atomic<uint64_t> m_newDataCallbacks{0};
    std::atomic<uint64_t> m_acceptedDataPackets{0};
    std::atomic<uint64_t> m_businessBytes{0};
    std::atomic<uint64_t> m_zeroPayloadPackets{0};
    std::atomic<uint64_t> m_sourceMappingMisses{0};
    std::atomic<uint64_t> m_destinationMappingMisses{0};
    std::atomic<uint64_t> m_sameLeafPackets{0};
    std::atomic<uint64_t> m_ocsPackets{0};
    std::atomic<uint64_t> m_linkWireBytes{0};
    std::atomic<uint64_t> m_routeControllerCalls{0};
    uint64_t m_newFlowsSinceEpoch = 0;
    uint64_t m_directFlowsSinceEpoch = 0;
    uint64_t m_transitFlowsSinceEpoch = 0;
    uint64_t m_totalFlows = 0;
    uint64_t m_totalDirectFlows = 0;
    uint64_t m_totalTransitFlows = 0;
    uint32_t m_totalTasks = 0;
    uint32_t m_pendingTasks = 0;
    uint32_t m_readyTasks = 0;
    uint32_t m_runningTasks = 0;
    uint32_t m_completedTasks = 0;
    mutable std::mutex m_mutex;
    mutable std::mutex m_debugMutex;
};

} // namespace ns3
#endif
