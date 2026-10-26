// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_REPLAY_LOGGER_H
#define UB_REPLAY_LOGGER_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <string>
#include <vector>

namespace ns3
{

class UbPort;
class Packet;

/** Optional replay telemetry with public-LP sampling and LP-owned hot-path counters.
 * Samples existing successful-transmit counters; never schedules network actions.
 */
class UbReplayLogger
{
  public:
    static UbReplayLogger& Get();
    void Configure(const std::string& casePath,
                   const std::string& outputDir,
                   const std::string& caseId,
                   const std::string& mode,
                   Time interval,
                   bool fabricOnly,
                   bool queues,
                   bool aggregatePairs,
                   bool taskEvents,
                   const std::string& detailWindows,
                   bool parallel = false);
    bool DetailsActive() const;
    void PathMetadata(Ptr<Packet> packet, uint32_t leaf, uint32_t sip, uint32_t dip,
                      uint32_t payloadBytes);
    void SuccessfulTransmit(Ptr<Packet> packet, uint32_t node, uint32_t port);
    void Metadata(const std::string& key, const std::string& value);
    void InputFingerprint(const std::string& name, const std::string& path);
    void TaskEvent(const char* event, const char* kind, uint32_t nodeId, uint32_t taskId);
    void TePolicyPublished(uint64_t epoch, bool solverRan);
    void ToePhase(uint64_t decisionEpoch, const std::string& phase);
    void BeforeTopologyChange();
    void TopologyApplied(uint64_t decisionEpoch);
    void Finish(const std::string& reason);
    bool Enabled() const { return m_enabled; }

  private:
    struct PortState
    {
        Ptr<UbPort> port;
        uint32_t sourceNode;
        uint32_t sourcePort;
        uint32_t destinationNode;
        uint32_t destinationPort;
        bool fabric;
        uint64_t rateBps;
        uint64_t lastBytes;
        uint64_t lastPackets;
    };

    UbReplayLogger() = default;
    void DiscoverPorts();
    void RefreshPeers();
    void Sample(const std::string& reason);
    void SampleAt(Time now, const std::string& reason);
    void RequirePublicLp(const char* operation) const;
    void FlushTaskBuffer(std::string& buffer);
    void FlushTaskBuffers();
    Time FinalTime() const;
    void Tick();
    void WriteQueues(const std::string& reason, Time now);
    void WriteTopology();
    void Event(const std::string& name);
    void EventAt(const std::string& name, Time now);
    void CheckStreams() const;

    bool m_enabled{false};
    bool m_parallel{false};
    bool m_queues{false};
    bool m_aggregatePairs{true};
    bool m_taskEvents{false};
    std::vector<std::pair<Time, Time>> m_detailWindows;
    std::vector<EventId> m_detailEvents;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> m_hostPortToLeaf;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> m_portPeers;
    using PathKey = std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,int32_t>;
    struct Shard
    {
        std::map<PathKey, std::pair<uint64_t,uint64_t>> paths;
        std::string tasks;
        uint64_t unattributedPackets{0};
    };
    Shard& CurrentShard();
    // Partition IDs, rather than OS thread IDs, survive LP migration between workers.
    // Workers own one shard exclusively; public-LP callbacks merge after the barrier.
    std::vector<Shard> m_shards;
    std::mutex m_taskWriteMutex;
    std::set<std::pair<uint32_t,uint32_t>> m_fabricPairs;
    uint64_t m_unattributedPathPackets{0};
    std::ofstream m_nodePaths;
    std::string m_caseId;
    std::string m_mode;
    std::string m_toePhase{"steady"};
    Time m_interval{Seconds(0)};
    Time m_lastSample{Seconds(0)};
    EventId m_tick;
    uint64_t m_sampleId{0};
    uint64_t m_eventId{0};
    uint64_t m_topologyEpoch{0};
    uint64_t m_teEpoch{0};
    uint64_t m_toeDecisionEpoch{0};
    std::vector<PortState> m_ports;
    std::ofstream m_links;
    std::ofstream m_queueSamples;
    std::ofstream m_topology;
    std::ofstream m_events;
    std::ofstream m_tasks;
    std::ofstream m_metadata;
};

} // namespace ns3

#endif // UB_REPLAY_LOGGER_H
