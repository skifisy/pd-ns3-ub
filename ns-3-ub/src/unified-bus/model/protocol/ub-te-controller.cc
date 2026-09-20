// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-te-controller.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-te-solver.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/fatal-error.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace ns3 {

namespace
{

constexpr uint64_t kDebugFlowProgressInterval = 100;

const char*
TeRouteDecisionSourceName(TeRouteDecisionSource source)
{
    switch (source)
    {
    case TeRouteDecisionSource::FALLBACK:
        return "fallback";
    case TeRouteDecisionSource::HISTORICAL_POLICY:
        return "historical-policy";
    case TeRouteDecisionSource::PINNED:
        return "pinned";
    case TeRouteDecisionSource::TRANSIT_FORWARD:
        return "transit-forward";
    case TeRouteDecisionSource::NONE:
    default:
        return "none";
    }
}

} // namespace

UbTeController& UbTeController::Get()
{
    static UbTeController controller;
    return controller;
}

bool UbTeController::DebugEnabled() const
{
    return m_enabled.load(std::memory_order_acquire) &&
           m_debug.load(std::memory_order_relaxed);
}

UbTeController::TrafficWindows UbTeController::SnapshotDemandWindows()
{
    TrafficWindows snapshot;
    for (auto& shard : m_demandShards)
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (const auto& [window, traffic] : shard.windows)
        {
            for (const auto& [encodedOd, bytes] : traffic)
            {
                const Od od{static_cast<uint32_t>(encodedOd >> 32),
                            static_cast<uint32_t>(encodedOd)};
                snapshot[window][od] += bytes;
            }
        }
    }
    return snapshot;
}

UbTeController::TrafficWindows UbTeController::ExtractDemandWindows(uint64_t beforeWindow)
{
    TrafficWindows extracted;
    for (auto& shard : m_demandShards)
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (auto window = shard.windows.begin(); window != shard.windows.end();)
        {
            if (window->first >= beforeWindow)
            {
                ++window;
                continue;
            }
            for (const auto& [encodedOd, bytes] : window->second)
            {
                const Od od{static_cast<uint32_t>(encodedOd >> 32),
                            static_cast<uint32_t>(encodedOd)};
                extracted[window->first][od] += bytes;
            }
            window = shard.windows.erase(window);
        }
    }
    return extracted;
}

void UbTeController::ClearDemandWindows()
{
    for (auto& shard : m_demandShards)
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        shard.windows.clear();
    }
}

void UbTeController::DebugLog(const std::string& message) const
{
    if (!m_debug.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_debugMutex);
    std::ofstream(m_outputDir + "/te_debug.log", std::ios::app)
        << "[JUPITER-TE] " << message << '\n';
}

void UbTeController::WriteDebugProgressLocked(const std::string& trigger)
{
    if (!m_debug.load(std::memory_order_relaxed))
    {
        return;
    }
    const auto observed = SnapshotDemandWindows();
    size_t currentOdEntries = 0;
    for (const auto& [window, traffic] : observed)
    {
        currentOdEntries += traffic.size();
    }
    const auto wallTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
    std::ofstream(m_outputDir + "/live_status.csv", std::ios::app)
        << std::setprecision(17) << Simulator::Now().GetSeconds() << ',' << wallTimeMs << ','
        << trigger << ',' << m_totalTasks << ',' << m_pendingTasks << ',' << m_readyTasks << ','
        << m_runningTasks << ',' << m_completedTasks << ',' << m_newDataCallbacks.load() << ','
        << m_acceptedDataPackets.load() << ',' << m_businessBytes.load() << ','
        << m_zeroPayloadPackets.load() << ',' << m_sourceMappingMisses.load() << ','
        << m_destinationMappingMisses.load() << ',' << m_sameLeafPackets.load() << ','
        << m_ocsPackets.load() << ',' << m_linkWireBytes.load() << ',' << m_totalFlows << ','
        << m_routeControllerCalls.load() << ',' << m_totalDirectFlows << ','
        << m_totalTransitFlows << ',' << observed.size() << ',' << currentOdEntries << '\n';

    std::ofstream current(m_outputDir + "/observed-current.csv");
    current << "snapshot_time_seconds,window,src_leaf,dst_leaf,bytes\n";
    current << std::setprecision(17);
    for (const auto& [window, traffic] : observed)
    {
        for (const auto& [od, bytes] : traffic)
        {
            current << Simulator::Now().GetSeconds() << ',' << window << ',' << od.first << ','
                    << od.second << ',' << bytes << '\n';
        }
    }
    m_pathDecisionStream.flush();
    m_transitForwardStream.flush();
    {
        std::lock_guard<std::mutex> debugLock(m_debugMutex);
        m_wcmpSelectionStream.flush();
        m_demandEventStream.flush();
    }

    std::ostringstream message;
    message << "[PROGRESS] t=" << Simulator::Now().GetSeconds()
            << "s wall_time_unix_ms=" << wallTimeMs << " trigger=" << trigger
            << " tasks=" << m_completedTasks << '/' << m_totalTasks
            << " task_states=" << m_pendingTasks << "/" << m_readyTasks << '/'
            << m_runningTasks << '/' << m_completedTasks
            << " new_data_callbacks=" << m_newDataCallbacks.load()
            << " accepted_data_packets=" << m_acceptedDataPackets.load()
            << " rejected_data_packets="
            << (m_zeroPayloadPackets.load() + m_sourceMappingMisses.load() +
                m_destinationMappingMisses.load() + m_sameLeafPackets.load())
            << " rejection_breakdown=" << m_zeroPayloadPackets.load() << '/'
            << m_sourceMappingMisses.load() << '/' << m_destinationMappingMisses.load() << '/'
            << m_sameLeafPackets.load()
            << " business_bytes=" << m_businessBytes.load()
            << " ocs_packets=" << m_ocsPackets.load()
            << " ocs_wire_bytes=" << m_linkWireBytes.load() << " flow_decisions=" << m_totalFlows
            << " route_controller_calls=" << m_routeControllerCalls.load()
            << " current_observed_windows=" << observed.size()
            << " current_od_entries=" << currentOdEntries;
    DebugLog(message.str());
}

void UbTeController::WriteDemandEvent(const std::string& result,
                                      uint32_t sourceHost, uint16_t sourcePort,
                                      uint32_t destinationHost, uint16_t destinationPort,
                                      uint32_t payloadBytes, int32_t sourceLeaf,
                                      int32_t destinationLeaf)
{
    if (!m_debug.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_debugMutex);
    if (!m_enabled.load(std::memory_order_acquire) ||
        !m_debug.load(std::memory_order_relaxed) || !m_demandEventStream)
    {
        return;
    }
    m_demandEventStream << std::setprecision(17) << Simulator::Now().GetSeconds() << ',' << result
                        << ',' << sourceHost << ',' << sourcePort << ',' << destinationHost << ','
                        << destinationPort << ',' << payloadBytes << ',' << sourceLeaf << ','
                        << destinationLeaf << '\n';
    m_demandEventStream.flush();
}

void
UbTeController::WriteWcmpSelectionTrace(uint32_t taskId,
                                        const RoutingKey& key,
                                        uint64_t flowHash,
                                        const TeRouteDecision& decision)
{
    if (!m_debug.load(std::memory_order_relaxed) || !decision.managed ||
        !decision.sourceDecision)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_debugMutex);
    if (!m_enabled.load(std::memory_order_acquire) ||
        !m_debug.load(std::memory_order_relaxed) || !m_wcmpSelectionStream)
    {
        return;
    }
    m_wcmpSelectionStream << std::setprecision(17) << Simulator::Now().GetSeconds() << ','
                          << taskId << ',' << decision.epoch << ',' << flowHash << ',' << key.sip
                          << ',' << key.dip << ',' << key.sport << ',' << key.dport << ','
                          << static_cast<uint32_t>(key.priority) << ',' << decision.sourceLeaf << ','
                          << decision.destinationLeaf << ',' << decision.transitLeaf << ','
                          << decision.nextLeaf << ',' << decision.outPort << ','
                          << TeRouteDecisionSourceName(decision.source) << '\n';
}

void UbTeController::StopPeriodicEvents(const std::string& reason)
{
    // The atomic gate prevents an already-running callback from publishing a
    // successor event after normal completion.  Event cancellation removes
    // callbacks that are still pending in the simulator queue.
    if (!m_periodicEventsActive.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    if (m_recomputeEvent.IsPending())
    {
        Simulator::Cancel(m_recomputeEvent);
    }
    if (m_linkWindowEvent.IsPending())
    {
        Simulator::Cancel(m_linkWindowEvent);
    }
    if (m_debug.load(std::memory_order_relaxed))
    {
        DebugLog("[CONTROL] periodic_events=stopped reason=" + reason);
    }
}

void UbTeController::Disable()
{
    StopPeriodicEvents("controller-disable");
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_enabled.load(std::memory_order_acquire) &&
        m_debug.load(std::memory_order_relaxed))
    {
        WriteDebugProgressLocked("final");
        std::ostringstream message;
        message << "[FINAL] epochs=" << m_epoch
                << " business_bytes=" << m_businessBytes.load()
                << " ocs_wire_bytes=" << m_linkWireBytes.load()
                << " pinned_flows=" << m_totalFlows
                << " direct_flows=" << m_totalDirectFlows
                << " transit_flows=" << m_totalTransitFlows
                << " route_controller_calls=" << m_routeControllerCalls.load()
                << " output=" << m_outputDir;
        DebugLog(message.str());
    }
    m_enabled.store(false, std::memory_order_release);
    m_pathDecisionStream.close();
    m_transitForwardStream.close();
    {
        std::lock_guard<std::mutex> debugLock(m_debugMutex);
        m_debug.store(false, std::memory_order_relaxed);
        m_wcmpSelectionStream.close();
        m_demandEventStream.close();
    }
    m_hostPortToLeaf.clear();
    m_leafPorts.clear();
    m_edgeCapacityBps.clear();
    m_leafPortToPeer.clear();
    ClearDemandWindows();
    m_linkCounters.clear();
    m_history.clear();
    m_policy.clear();
    m_flowTransit.clear();
    m_loggedTransitFlows.clear();
    m_newDataCallbacks.store(0, std::memory_order_relaxed);
    m_acceptedDataPackets.store(0, std::memory_order_relaxed);
    m_businessBytes.store(0, std::memory_order_relaxed);
    m_zeroPayloadPackets.store(0, std::memory_order_relaxed);
    m_sourceMappingMisses.store(0, std::memory_order_relaxed);
    m_destinationMappingMisses.store(0, std::memory_order_relaxed);
    m_sameLeafPackets.store(0, std::memory_order_relaxed);
    m_ocsPackets.store(0, std::memory_order_relaxed);
    m_linkWireBytes.store(0, std::memory_order_relaxed);
    m_routeControllerCalls.store(0, std::memory_order_relaxed);
    m_newFlowsSinceEpoch = 0;
    m_directFlowsSinceEpoch = 0;
    m_transitFlowsSinceEpoch = 0;
    m_totalFlows = 0;
    m_totalDirectFlows = 0;
    m_totalTransitFlows = 0;
    m_totalTasks = 0;
    m_pendingTasks = 0;
    m_readyTasks = 0;
    m_runningTasks = 0;
    m_completedTasks = 0;
    m_epoch = 0;
    m_casePath.clear();
    m_outputDir.clear();
}

void UbTeController::Configure(const std::string& casePath, double recomputeSeconds,
                               uint32_t historyWindows, double s, const std::string& outputDir,
                               bool debug)
{
    NS_ABORT_MSG_IF(recomputeSeconds <= 0 || historyWindows == 0 || s < 0 || s > 1,
                    "TE requires positive interval, history and S in [0,1]");
    NS_ABORT_MSG_IF(!UbTeSolver::IsAvailable(),
                    "Jupiter TE requires the in-process HiGHS C++ backend; install HiGHS and "
                    "reconfigure ns-3 with -Dhighs_DIR=<path-to-highs-cmake>");
    Disable();
    std::ifstream input(casePath + "/topology.csv");
    NS_ABORT_MSG_IF(!input, "TE cannot read topology.csv");
    std::string line;
    std::getline(input, line);
    std::lock_guard<std::mutex> lock(m_mutex);
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::stringstream row(line);
        std::string part;
        std::vector<uint32_t> fields;
        for (int i = 0; i < 4; ++i) {
            NS_ABORT_MSG_IF(!std::getline(row, part, ','), "bad TE topology row");
            fields.push_back(static_cast<uint32_t>(std::stoul(part)));
        }
        const auto a = fields[0], ap = fields[1], b = fields[2], bp = fields[3];
        if (a >= 37 && b >= 37) {
            m_leafPorts[{a, b}].push_back(static_cast<uint16_t>(ap));
            m_leafPorts[{b, a}].push_back(static_cast<uint16_t>(bp));
            m_leafPortToPeer[{a, static_cast<uint16_t>(ap)}] = b;
            m_leafPortToPeer[{b, static_cast<uint16_t>(bp)}] = a;
        } else {
            const auto host = a < 37 ? a : b;
            const auto hp = a < 37 ? ap : bp;
            const auto leaf = a < 37 ? b : a;
            NS_ABORT_MSG_IF(!m_hostPortToLeaf
                                 .emplace(std::make_pair(host, static_cast<uint16_t>(hp)), leaf)
                                 .second,
                            "duplicate TE host port in topology");
        }
    }
    for (auto& [edge, ports] : m_leafPorts) std::sort(ports.begin(), ports.end());
    NS_ABORT_MSG_IF(m_leafPorts.empty() || m_hostPortToLeaf.empty(),
                    "TE topology has no OCS or host links");
    for (const auto& [endpoint, peer] : m_leafPortToPeer)
    {
        const uint32_t leaf = endpoint.first;
        const uint16_t portId = endpoint.second;
        NS_ABORT_MSG_IF(leaf >= NodeList::GetNNodes(),
                        "TE leaf node is missing: " << leaf);
        Ptr<Node> node = NodeList::GetNode(leaf);
        NS_ABORT_MSG_IF(portId >= node->GetNDevices(),
                        "TE leaf port is missing: node=" << leaf << " port=" << portId);
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
        NS_ABORT_MSG_IF(!port,
                        "TE leaf device is not an UbPort: node=" << leaf << " port=" << portId);
        const double capacityBps = static_cast<double>(port->GetDataRate().GetBitRate());
        NS_ABORT_MSG_IF(!std::isfinite(capacityBps) || capacityBps <= 0.0,
                        "TE leaf port has invalid capacity: node=" << leaf
                                                                   << " port=" << portId);
        m_edgeCapacityBps[{leaf, peer}] += capacityBps;
        m_linkCounters.push_back(
            {leaf, peer, portId, port->GetTxBytes(), port->GetTxPackets()});
    }
    m_casePath = casePath;
    m_intervalSeconds = recomputeSeconds;
    m_historyWindows = historyWindows;
    m_s = s;
    m_debug.store(debug, std::memory_order_relaxed);
    if (m_debug.load(std::memory_order_relaxed))
    {
        m_outputDir = outputDir.empty() ? casePath + "/jupiter_te" : outputDir;
        std::filesystem::create_directories(m_outputDir);
    }
    if (m_debug.load(std::memory_order_relaxed))
    {
        std::ofstream(m_outputDir + "/te_debug.log");
        std::ofstream(m_outputDir + "/observed.csv")
            << "window,src_leaf,dst_leaf,bytes,complete\n";
        std::ofstream(m_outputDir + "/prediction.csv")
            << "epoch,src_leaf,dst_leaf,bps\n";
        std::ofstream(m_outputDir + "/weights.csv")
            << "epoch,src_leaf,dst_leaf,transit_leaf,weight\n";
        std::ofstream(m_outputDir + "/link_bytes.csv")
            << "window,src_leaf,dst_leaf,wire_bytes,complete\n";
        std::ofstream(m_outputDir + "/epoch_summary.csv")
            << "epoch,sim_time_seconds,complete_windows,history_windows,active_ods,"
               "predicted_bps,solver_ran,min_max_utilization,policy_paths,new_flows,"
               "direct_flows,transit_flows\n";
        std::ofstream(m_outputDir + "/live_status.csv")
            << "sim_time_seconds,wall_time_unix_ms,trigger,total_tasks,pending_tasks,ready_tasks,"
               "running_tasks,completed_tasks,new_data_callbacks,accepted_data_packets,"
               "business_bytes,"
               "zero_payload_packets,source_mapping_misses,destination_mapping_misses,"
               "same_leaf_packets,ocs_packets,ocs_wire_bytes,flow_decisions,"
               "route_controller_calls,direct_flows,transit_flows,current_observed_windows,"
               "current_od_entries\n";
        std::ofstream(m_outputDir + "/observed-current.csv")
            << "snapshot_time_seconds,window,src_leaf,dst_leaf,bytes\n";
        m_pathDecisionStream.open(m_outputDir + "/path_decisions.csv");
        m_pathDecisionStream
            << "sim_time_seconds,epoch,flow_hash,sip,dip,sport,dport,priority,src_leaf,dst_leaf,"
               "transit_leaf,next_leaf,out_port,decision_source\n";
        m_wcmpSelectionStream.open(m_outputDir + "/WcmpSelectionTrace.csv");
        m_wcmpSelectionStream
            << "sim_time_seconds,task_id,epoch,flow_hash,sip,dip,sport,dport,priority,"
               "src_leaf,dst_leaf,transit_leaf,next_leaf,out_port,decision_source\n";
        m_transitForwardStream.open(m_outputDir + "/transit_forwards.csv");
        m_transitForwardStream
            << "sim_time_seconds,epoch,flow_hash,sip,dip,sport,dport,priority,source_leaf,"
               "current_leaf,dst_leaf,out_port\n";
        m_demandEventStream.open(m_outputDir + "/demand_events.csv");
        m_demandEventStream
            << "sim_time_seconds,result,source_host,source_port,destination_host,"
               "destination_port,payload_bytes,source_leaf,destination_leaf\n";
        NS_ABORT_MSG_IF(!m_pathDecisionStream || !m_wcmpSelectionStream ||
                            !m_transitForwardStream || !m_demandEventStream,
                        "cannot open Jupiter TE debug route files in " << m_outputDir);
        std::ofstream(m_outputDir + "/matrix-current.csv")
            << "src_leaf,dst_leaf,bps\n";
        std::ofstream(m_outputDir + "/policy-current.csv")
            << "src_leaf,dst_leaf,transit_leaf,weight\n";
        std::ofstream(m_outputDir + "/solver-summary-current.csv")
            << "active_od_count,path_count,min_max_utilization\n";
    }
    m_enabled.store(true, std::memory_order_release);
    m_periodicEventsActive.store(true, std::memory_order_release);
    if (m_debug.load(std::memory_order_relaxed))
    {
        std::set<uint32_t> leaves;
        size_t circuitCount = 0;
        for (const auto& [edge, ports] : m_leafPorts)
        {
            leaves.insert(edge.first);
            leaves.insert(edge.second);
            circuitCount += ports.size();
        }
        std::ostringstream message;
        message << "[INIT] case=" << m_casePath << " solver=in-process-highs"
                << " interval_seconds=" << m_intervalSeconds
                << " history_windows=" << m_historyWindows << " s=" << m_s
                << " host_ports=" << m_hostPortToLeaf.size() << " leaves=" << leaves.size()
                << " directed_logical_edges=" << m_leafPorts.size()
                << " directed_circuits=" << circuitCount;
        DebugLog(message.str());
        DebugLog("[INIT] epoch 0 uses direct-first fallback until the first historical matrix "
                 "is solved");
        WriteDebugProgressLocked("init");
        m_linkWindowEvent =
            Simulator::Schedule(Seconds(30.0), &UbTeController::SampleLinkWindow, this);
    }
    m_recomputeEvent =
        Simulator::Schedule(Seconds(m_intervalSeconds), &UbTeController::Recompute, this);
}

std::vector<uint16_t> UbTeController::PhysicalPorts(uint32_t src, uint32_t dst) const
{
    const auto it = m_leafPorts.find({src, dst});
    return it == m_leafPorts.end() ? std::vector<uint16_t>{} : it->second;
}

void UbTeController::OnNewDataPacket(uint32_t sourceHost, uint16_t sourcePort,
                                     uint32_t destinationHost, uint16_t destinationPort,
                                     uint32_t payloadBytes)
{
    if (!m_enabled.load(std::memory_order_acquire))
    {
        return;
    }
    m_newDataCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (payloadBytes == 0)
    {
        const uint64_t count = m_zeroPayloadPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 20)
        {
            WriteDemandEvent("zero-payload", sourceHost, sourcePort, destinationHost,
                             destinationPort, payloadBytes, -1, -1);
        }
        return;
    }
    const auto source = m_hostPortToLeaf.find({sourceHost, sourcePort});
    const auto destination = m_hostPortToLeaf.find({destinationHost, destinationPort});
    if (source == m_hostPortToLeaf.end())
    {
        const uint64_t count = m_sourceMappingMisses.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 20)
        {
            WriteDemandEvent("source-mapping-miss", sourceHost, sourcePort,
                             destinationHost, destinationPort, payloadBytes, -1, -1);
        }
        return;
    }
    if (destination == m_hostPortToLeaf.end())
    {
        const uint64_t count =
            m_destinationMappingMisses.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 20)
        {
            WriteDemandEvent("destination-mapping-miss", sourceHost, sourcePort,
                             destinationHost, destinationPort, payloadBytes,
                             static_cast<int32_t>(source->second), -1);
        }
        return;
    }
    if (source->second == destination->second)
    {
        const uint64_t count = m_sameLeafPackets.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 20)
        {
            WriteDemandEvent("same-leaf", sourceHost, sourcePort, destinationHost,
                             destinationPort, payloadBytes,
                             static_cast<int32_t>(source->second),
                             static_cast<int32_t>(destination->second));
        }
        return;
    }
    const uint64_t window =
        static_cast<uint64_t>(std::floor(Simulator::Now().GetSeconds() / 30.0));
    static thread_local const size_t shardIndex =
        std::hash<std::thread::id>{}(std::this_thread::get_id()) % kDemandShardCount;
    {
        auto& shard = m_demandShards[shardIndex];
        std::lock_guard<std::mutex> lock(shard.mutex);
        const uint64_t encodedOd = (static_cast<uint64_t>(source->second) << 32) |
                                   static_cast<uint64_t>(destination->second);
        shard.windows[window][encodedOd] += payloadBytes;
    }
    const uint64_t accepted =
        m_acceptedDataPackets.fetch_add(1, std::memory_order_relaxed) + 1;
    m_businessBytes.fetch_add(payloadBytes, std::memory_order_relaxed);
    if (accepted <= 20)
    {
        WriteDemandEvent("accepted", sourceHost, sourcePort, destinationHost,
                         destinationPort, payloadBytes,
                         static_cast<int32_t>(source->second),
                         static_cast<int32_t>(destination->second));
    }
    if (accepted == 1 && m_debug.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        WriteDebugProgressLocked("new-data");
    }
}

std::map<UbTeController::Od, uint64_t> UbTeController::CollectLinkDeltas()
{
    std::map<Od, uint64_t> deltas;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    for (auto& counter : m_linkCounters)
    {
        Ptr<Node> node = NodeList::GetNode(counter.sourceLeaf);
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(counter.portId));
        NS_ABORT_MSG_IF(!port,
                        "TE leaf device disappeared: node=" << counter.sourceLeaf
                                                             << " port=" << counter.portId);
        const uint64_t currentBytes = port->GetTxBytes();
        const uint64_t currentPackets = port->GetTxPackets();
        const uint64_t byteDelta = currentBytes >= counter.lastBytes
                                       ? currentBytes - counter.lastBytes
                                       : currentBytes;
        const uint64_t packetDelta = currentPackets >= counter.lastPackets
                                         ? currentPackets - counter.lastPackets
                                         : currentPackets;
        counter.lastBytes = currentBytes;
        counter.lastPackets = currentPackets;
        if (byteDelta > 0)
        {
            deltas[{counter.sourceLeaf, counter.destinationLeaf}] += byteDelta;
        }
        bytes += byteDelta;
        packets += packetDelta;
    }
    m_linkWireBytes.fetch_add(bytes, std::memory_order_relaxed);
    m_ocsPackets.fetch_add(packets, std::memory_order_relaxed);
    return deltas;
}

void UbTeController::SampleLinkWindow()
{
    if (!m_enabled.load(std::memory_order_acquire) ||
        !m_debug.load(std::memory_order_relaxed) ||
        !m_periodicEventsActive.load(std::memory_order_acquire))
    {
        return;
    }
    std::map<Od, uint64_t> deltas;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        deltas = CollectLinkDeltas();
    }
    const double now = Simulator::Now().GetSeconds();
    const uint64_t window = static_cast<uint64_t>(
        std::floor(std::max(0.0, now - 1e-9) / 30.0));
    std::ofstream output(m_outputDir + "/link_bytes.csv", std::ios::app);
    for (const auto& [edge, bytes] : deltas)
    {
        output << window << ',' << edge.first << ',' << edge.second << ',' << bytes << ",1\n";
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        WriteDebugProgressLocked("link-window");
    }
    if (m_enabled.load(std::memory_order_acquire) &&
        m_periodicEventsActive.load(std::memory_order_acquire))
    {
        m_linkWindowEvent =
            Simulator::Schedule(Seconds(30.0), &UbTeController::SampleLinkWindow, this);
    }
}

void UbTeController::OnSimulationProgress(uint32_t totalTasks, uint32_t pendingTasks,
                                           uint32_t readyTasks, uint32_t runningTasks,
                                           uint32_t completedTasks)
{
    if (!m_enabled.load(std::memory_order_acquire) ||
        !m_debug.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_totalTasks = totalTasks;
    m_pendingTasks = pendingTasks;
    m_readyTasks = readyTasks;
    m_runningTasks = runningTasks;
    m_completedTasks = completedTasks;
    WriteDebugProgressLocked("heartbeat");
}

void UbTeController::FlushFinalWindow()
{
    if (!m_enabled.load(std::memory_order_acquire))
    {
        return;
    }
    const auto remaining = ExtractDemandWindows(std::numeric_limits<uint64_t>::max());
    std::map<Od, uint64_t> linkDeltas;
    if (m_debug.load(std::memory_order_relaxed))
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            linkDeltas = CollectLinkDeltas();
        }
        std::ofstream observed(m_outputDir + "/observed.csv", std::ios::app);
        std::ofstream edges(m_outputDir + "/link_bytes.csv", std::ios::app);
        for (const auto& [window, traffic] : remaining)
        {
            for (const auto& [od, bytes] : traffic)
            {
                observed << window << ',' << od.first << ',' << od.second << ',' << bytes
                         << ",0\n";
            }
        }
        const uint64_t linkWindow = static_cast<uint64_t>(
            std::floor(Simulator::Now().GetSeconds() / 30.0));
        for (const auto& [edge, bytes] : linkDeltas)
        {
            edges << linkWindow << ',' << edge.first << ',' << edge.second << ',' << bytes
                  << ",0\n";
        }
        std::ostringstream message;
        message << "[FLUSH] incomplete_business_windows=" << remaining.size()
                << " incomplete_link_window=" << (linkDeltas.empty() ? 0 : 1);
        DebugLog(message.str());
        std::lock_guard<std::mutex> lock(m_mutex);
        WriteDebugProgressLocked("flush");
    }
}

std::vector<std::pair<int32_t, double>> UbTeController::Fallback(uint32_t src, uint32_t dst) const
{
    if (!PhysicalPorts(src, dst).empty())
    {
        return {{-1, 1.0}};
    }
    std::vector<std::pair<int32_t, double>> result;
    for (const auto& [edge, ports] : m_leafPorts)
    {
        if (edge.first != src || edge.second == dst)
        {
            continue;
        }
        auto second = PhysicalPorts(edge.second, dst);
        if (!second.empty())
        {
            result.emplace_back(static_cast<int32_t>(edge.second),
                                static_cast<double>(std::min(ports.size(), second.size())));
        }
    }
    return result;
}

int UbTeController::SelectLeafOutPort(uint32_t leaf,
                                     const RoutingKey& key,
                                     uint64_t hash,
                                     uint16_t inPort,
                                     bool& selectedDirect,
                                     TeRouteDecision& decision)
{
    decision = {};
    if (!m_enabled.load(std::memory_order_acquire))
    {
        return -2;
    }
    m_routeControllerCalls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto firstEdge = m_leafPorts.lower_bound({leaf, 0});
    if (firstEdge == m_leafPorts.end() || firstEdge->first.first != leaf)
    {
        return -2;
    }
    const auto sourceHost = utils::IpToNodeId(Ipv4Address(key.sip));
    const auto destHost = utils::IpToNodeId(Ipv4Address(key.dip));
    const uint32_t sourceLow = key.sip & 255u, destLow = key.dip & 255u;
    if (!sourceLow || !destLow)
    {
        return -2;
    }
    auto srcIt = m_hostPortToLeaf.find({sourceHost, static_cast<uint16_t>(sourceLow - 1)});
    auto dstIt = m_hostPortToLeaf.find({destHost, static_cast<uint16_t>(destLow - 1)});
    if (srcIt == m_hostPortToLeaf.end() || dstIt == m_hostPortToLeaf.end())
    {
        return -2;
    }
    const uint32_t sourceLeaf = srcIt->second, destLeaf = dstIt->second;
    decision.managed = true;
    decision.sourceLeaf = sourceLeaf;
    decision.destinationLeaf = destLeaf;
    if (sourceLeaf == destLeaf || leaf == destLeaf)
    {
        return -2;
    }
    NS_ABORT_MSG_IF(key.usePacketSpray, "Jupiter TE requires per-flow packet routing");
    if (leaf != sourceLeaf)
    {
        if (!key.useShortestPath)
        {
            return -1;
        }
        // A transit leaf always makes its final OCS hop directly to the destination.
        auto ports = PhysicalPorts(leaf, destLeaf);
        if (ports.empty())
        {
            return -1;
        }
        selectedDirect = true;
        const uint16_t chosen = ports[hash % ports.size()];
        decision.sourceDecision = false;
        decision.epoch = m_epoch;
        decision.transitLeaf = static_cast<int32_t>(leaf);
        decision.nextLeaf = destLeaf;
        decision.outPort = chosen;
        decision.source = TeRouteDecisionSource::TRANSIT_FORWARD;
        if (m_debug.load(std::memory_order_relaxed) &&
            m_loggedTransitFlows.insert({leaf, hash}).second)
        {
            m_transitForwardStream << std::setprecision(17) << Simulator::Now().GetSeconds()
                                   << ',' << m_epoch << ',' << hash << ',' << key.sip << ','
                                   << key.dip << ',' << key.sport << ',' << key.dport << ','
                                   << static_cast<uint32_t>(key.priority) << ',' << sourceLeaf
                                   << ',' << leaf << ',' << destLeaf << ',' << chosen << '\n';
        }
        return chosen;
    }
    if (m_leafPortToPeer.count({leaf, inPort}))
    {
        return -1; // no OCS-to-OCS re-selection at the source
    }
    auto flow = FlowId{leaf, hash};
    int32_t transit = -1;
    auto pin = m_flowTransit.find(flow);
    bool newFlow = false;
    TeRouteDecisionSource decisionSource = TeRouteDecisionSource::PINNED;
    if (pin != m_flowTransit.end())
    {
        transit = pin->second;
    }
    else
    {
        std::vector<std::pair<int32_t, double>> paths;
        auto policy = m_policy.find({sourceLeaf, destLeaf});
        if (policy != m_policy.end())
        {
            for (auto [mid, weight] : policy->second)
            {
                const uint32_t next = mid < 0 ? destLeaf : static_cast<uint32_t>(mid);
                if (weight > 0 && !PhysicalPorts(sourceLeaf, next).empty())
                {
                    paths.emplace_back(mid, weight);
                }
            }
        }
        decisionSource = paths.empty() ? TeRouteDecisionSource::FALLBACK
                                       : TeRouteDecisionSource::HISTORICAL_POLICY;
        if (paths.empty())
        {
            paths = Fallback(sourceLeaf, destLeaf);
        }
        if (paths.empty())
        {
            return -1;
        }
        double sum = 0;
        for (auto [mid, weight] : paths)
        {
            sum += weight;
        }
        double draw = static_cast<double>(hash >> 11) /
                      static_cast<double>(uint64_t{1} << 53) * sum;
        transit = paths.back().first;
        for (auto [mid, weight] : paths)
        {
            if (draw < weight)
            {
                transit = mid;
                break;
            }
            draw -= weight;
        }
        m_flowTransit.emplace(flow, transit);
        newFlow = true;
    }
    const uint32_t nextLeaf = transit < 0 ? destLeaf : static_cast<uint32_t>(transit);
    auto ports = PhysicalPorts(leaf, nextLeaf);
    if (ports.empty())
    {
        return -1;
    }
    selectedDirect = transit < 0;
    // Low hash bits reproduce the old per-flow ECMP choice across four circuits.
    const uint16_t chosen = ports[hash % ports.size()];
    if (chosen == inPort)
    {
        return -1;
    }
    decision.sourceDecision = true;
    decision.epoch = m_epoch;
    decision.transitLeaf = transit;
    decision.nextLeaf = nextLeaf;
    decision.outPort = chosen;
    decision.source = decisionSource;
    if (newFlow)
    {
        if (m_debug.load(std::memory_order_relaxed))
        {
            ++m_newFlowsSinceEpoch;
            ++m_totalFlows;
            if (selectedDirect)
            {
                ++m_directFlowsSinceEpoch;
                ++m_totalDirectFlows;
            }
            else
            {
                ++m_transitFlowsSinceEpoch;
                ++m_totalTransitFlows;
            }
            m_pathDecisionStream << std::setprecision(17) << Simulator::Now().GetSeconds() << ','
                                 << m_epoch << ',' << hash << ',' << key.sip << ',' << key.dip
                                 << ',' << key.sport << ',' << key.dport << ','
                                 << static_cast<uint32_t>(key.priority) << ',' << sourceLeaf << ','
                                 << destLeaf << ',' << transit << ',' << nextLeaf << ',' << chosen
                                 << ',' << TeRouteDecisionSourceName(decisionSource) << '\n';
            if (m_totalFlows == 1 || m_totalFlows % kDebugFlowProgressInterval == 0)
            {
                WriteDebugProgressLocked("flow-decision");
            }
        }
    }
    return chosen;
}

void UbTeController::Recompute()
{
    if (!m_enabled.load(std::memory_order_acquire) ||
        !m_periodicEventsActive.load(std::memory_order_acquire))
    {
        return;
    }
    const auto now = Simulator::Now().GetSeconds();
    const uint64_t complete = static_cast<uint64_t>(std::floor((now + 1e-9) / 30.0));
    std::map<Od, uint64_t> maxima;
    TrafficWindows completed = ExtractDemandWindows(complete);
    uint64_t epoch;
    uint64_t newFlows;
    uint64_t directFlows;
    uint64_t transitFlows;
    size_t historyWindowCount;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_epoch;
        // Keep complete 30-second windows even when the recomputation interval differs.
        m_history.insert(completed.begin(), completed.end());
        for (auto it = m_history.begin(); it != m_history.end();)
        {
            if (it->first + m_historyWindows < complete)
            {
                it = m_history.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (const auto& [window, traffic] : m_history)
        {
            if (window < complete && window + m_historyWindows >= complete)
            {
                for (const auto& [od, bytes] : traffic)
                {
                    maxima[od] = std::max(maxima[od], bytes);
                }
            }
        }
        historyWindowCount = m_history.size();
        newFlows = m_newFlowsSinceEpoch;
        directFlows = m_directFlowsSinceEpoch;
        transitFlows = m_transitFlowsSinceEpoch;
        m_newFlowsSinceEpoch = 0;
        m_directFlowsSinceEpoch = 0;
        m_transitFlowsSinceEpoch = 0;
    }
    if (m_debug.load(std::memory_order_relaxed))
    {
        std::ofstream observed(m_outputDir + "/observed.csv", std::ios::app);
        for (const auto& [window, traffic] : completed)
        {
            for (const auto& [od, bytes] : traffic)
            {
                observed << window << ',' << od.first << ',' << od.second << ',' << bytes
                         << ",1\n";
            }
        }
    }
    std::map<Od, double> predictedDemands;
    double predictedBps = 0.0;
    std::ofstream matrix;
    std::ofstream prediction;
    if (m_debug.load(std::memory_order_relaxed))
    {
        matrix.open(m_outputDir + "/matrix-current.csv");
        matrix << std::setprecision(17) << "src_leaf,dst_leaf,bps\n";
        prediction.open(m_outputDir + "/prediction.csv", std::ios::app);
        prediction << std::setprecision(17);
    }
    for (const auto& [od, bytes] : maxima)
    {
        const double bps = 8.0 * static_cast<double>(bytes) / 30.0;
        predictedDemands[od] = bps;
        predictedBps += bps;
        if (m_debug.load(std::memory_order_relaxed))
        {
            matrix << od.first << ',' << od.second << ',' << bps << '\n';
            prediction << epoch << ',' << od.first << ',' << od.second << ',' << bps << '\n';
        }
    }
    if (m_debug.load(std::memory_order_relaxed))
    {
        matrix.close();
        prediction.close();
    }
    bool solverRan = false;
    double maxUtilization = 0.0;
    size_t policyPathCount = 0;
    if (!maxima.empty())
    {
        solverRan = true;
        UbTeSolver::Result result;
        try
        {
            result = UbTeSolver::Solve(m_edgeCapacityBps, predictedDemands, m_s);
        }
        catch (const std::exception& error)
        {
            NS_ABORT_MSG("Jupiter TE in-process LP solver failed: " << error.what());
        }
        std::map<Od, PathWeights> next = std::move(result.policy);
        maxUtilization = result.minMaxUtilization;
        policyPathCount = result.pathCount;
        std::ofstream weights;
        std::ofstream currentPolicy;
        if (m_debug.load(std::memory_order_relaxed))
        {
            weights.open(m_outputDir + "/weights.csv", std::ios::app);
            weights << std::setprecision(17);
            currentPolicy.open(m_outputDir + "/policy-current.csv");
            currentPolicy << std::setprecision(17)
                          << "src_leaf,dst_leaf,transit_leaf,weight\n";
        }
        for (const auto& [od, bytes] : maxima)
        {
            (void)bytes;
            const auto policy = next.find(od);
            NS_ABORT_MSG_IF(policy == next.end(),
                            "Jupiter TE solver omitted active OD " << od.first << "->"
                                                                    << od.second);
            double totalWeight = 0.0;
            for (const auto& [mid, weight] : policy->second)
            {
                (void)mid;
                totalWeight += weight;
            }
            NS_ABORT_MSG_IF(!std::isfinite(totalWeight) ||
                                std::abs(totalWeight - 1.0) > 1e-6,
                            "Jupiter TE weights do not sum to one for "
                                << od.first << "->" << od.second << ": " << totalWeight);
            // Eliminate harmless solver/CSV round-off before publishing.
            for (auto& [mid, weight] : policy->second)
            {
                weight /= totalWeight;
                if (m_debug.load(std::memory_order_relaxed))
                {
                    weights << epoch << ',' << od.first << ',' << od.second << ',' << mid << ','
                            << weight << '\n';
                    currentPolicy << od.first << ',' << od.second << ',' << mid << ',' << weight
                                  << '\n';
                }
            }
        }
        if (m_debug.load(std::memory_order_relaxed))
        {
            std::ofstream(m_outputDir + "/solver-summary-current.csv")
                << std::setprecision(17)
                << "active_od_count,path_count,min_max_utilization\n"
                << predictedDemands.size() << ',' << policyPathCount << ',' << maxUtilization
                << '\n';
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_policy.swap(next); // atomic policy publication; existing flows stay pinned.
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_policy.clear();
        if (m_debug.load(std::memory_order_relaxed))
        {
            std::ofstream(m_outputDir + "/policy-current.csv")
                << "src_leaf,dst_leaf,transit_leaf,weight\n";
            std::ofstream(m_outputDir + "/solver-summary-current.csv")
                << "active_od_count,path_count,min_max_utilization\n";
        }
    }
    if (m_debug.load(std::memory_order_relaxed))
    {
        if (solverRan)
        {
            std::ostringstream solverMessage;
            solverMessage << "[SOLVER] epoch=" << epoch << " active_ods=" << maxima.size()
                          << " policy_paths=" << policyPathCount
                          << " min_max_utilization=" << maxUtilization;
            DebugLog(solverMessage.str());
        }
        std::ofstream(m_outputDir + "/epoch_summary.csv", std::ios::app)
            << std::setprecision(17) << epoch << ',' << now << ',' << completed.size() << ','
            << historyWindowCount << ',' << maxima.size() << ',' << predictedBps << ','
            << (solverRan ? 1 : 0) << ',' << maxUtilization << ',' << policyPathCount << ','
            << newFlows << ',' << directFlows << ',' << transitFlows << '\n';
        std::ostringstream message;
        message << "[EPOCH " << epoch << "] t=" << now
                << "s complete_windows=" << completed.size()
                << " history_windows=" << historyWindowCount << " active_ods=" << maxima.size()
                << " predicted_bps=" << predictedBps
                << " solver=" << (solverRan ? "ok" : "skipped(no history)")
                << " min_max_utilization=" << maxUtilization
                << " policy_paths=" << policyPathCount << " new_flows=" << newFlows
                << " direct_flows=" << directFlows << " transit_flows=" << transitFlows;
        DebugLog(message.str());
        std::lock_guard<std::mutex> lock(m_mutex);
        WriteDebugProgressLocked("epoch");
    }
    if (m_enabled.load(std::memory_order_acquire) &&
        m_periodicEventsActive.load(std::memory_order_acquire))
    {
        m_recomputeEvent =
            Simulator::Schedule(Seconds(m_intervalSeconds), &UbTeController::Recompute, this);
    }
}

} // namespace ns3
