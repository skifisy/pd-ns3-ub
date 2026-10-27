// SPDX-License-Identifier: GPL-2.0-only
#include "ub-replay-logger.h"

#include "ns3/abort.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-tag.h"
#include "ns3/ub-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/ub-network-address.h"
#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif
#include <algorithm>
#include <limits>

#include <array>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <sstream>

namespace ns3
{
namespace
{
std::string
Csv(const std::string& value)
{
    std::string result = "\"";
    for (char c : value)
    {
        if (c == '"') result += '"';
        result += c;
    }
    return result + '"';
}

// The runner uses picosecond resolution. Preserve its sub-nanosecond event
// ordering instead of truncating topology boundaries to integer nanoseconds.
std::string
Ns(Time time)
{
    const int64_t ps = time.GetPicoSeconds();
    std::ostringstream value;
    value << ps / 1000 << '.' << std::setw(3) << std::setfill('0') << ps % 1000;
    return value.str();
}

bool
IsSwitch(Ptr<Node> node)
{
    const auto sw = node->GetObject<UbSwitch>();
    return sw && sw->GetNodeType() == UB_SWITCH;
}
} // namespace

UbReplayLogger&
UbReplayLogger::Get()
{
    static UbReplayLogger logger;
    return logger;
}

void
UbReplayLogger::Configure(const std::string& casePath,
                          const std::string& outputDir,
                          const std::string& caseId,
                          const std::string& mode,
                          Time interval,
                          bool fabricOnly,
                          bool queues,
                          bool aggregatePairs,
                          bool taskEvents,
                          const std::string& detailWindows,
                          bool parallel)
{
    NS_ABORT_MSG_IF(m_enabled, "Replay logger is already configured");
    m_parallel = parallel;
#ifdef NS3_MTP
    m_parallel = m_parallel || MtpInterface::isEnabled();
#endif
    RequirePublicLp("configure");
    // Automatic local MTP partitioning creates no more worker LPs than nodes.
    // Allocate before Run, so no vector registration/resize occurs on the hot path.
    m_shards.clear();
    m_shards.resize(NodeList::GetNNodes()+1);
    m_unattributedPathPackets = 0;
    NS_ABORT_MSG_IF(!interval.IsStrictlyPositive(), "Replay sample interval must be positive");
    const auto directory = std::filesystem::path(outputDir.empty() ? casePath + "/replay"
                                                                 : outputDir);
    std::filesystem::create_directories(directory);
    m_caseId = caseId.empty() ? mode : caseId;
    m_mode = mode;
    m_interval = interval;
    m_queues = queues;
    m_aggregatePairs = aggregatePairs;
    m_taskEvents = taskEvents;
    m_detailWindows.clear();
    if (detailWindows == "all")
        m_detailWindows.emplace_back(Time(0), Time::Max());
    else if (!detailWindows.empty() && detailWindows != "none")
    {
        std::istringstream ranges(detailWindows);
        std::string range;
        while (std::getline(ranges, range, ','))
        {
            const auto colon = range.find(':');
            NS_ABORT_MSG_IF(colon == std::string::npos, "Invalid internal replay detail range: expected start:end");
            Time lo(range.substr(0,colon)), hi(range.substr(colon+1));
            NS_ABORT_MSG_IF(lo < Simulator::Now() || hi <= lo, "Invalid replay detail window");
            m_detailWindows.emplace_back(lo,hi);
        }
        std::sort(m_detailWindows.begin(),m_detailWindows.end());
        for (size_t i=1;i<m_detailWindows.size();++i)
            NS_ABORT_MSG_IF(m_detailWindows[i].first < m_detailWindows[i-1].second,
                            "Replay detail windows must not overlap");
    }
    m_toePhase = "steady";
    m_sampleId = m_eventId = m_topologyEpoch = m_teEpoch = m_toeDecisionEpoch = 0;
    m_lastSample = Simulator::Now();
    m_links.open(directory / "replay_link_samples.csv", std::ios::trunc);
    m_nodePaths.open(directory / "replay_node_path_samples.csv", std::ios::trunc);
    m_queueSamples.open(directory / "replay_queue_samples.csv", std::ios::trunc);
    m_topology.open(directory / "replay_topology.csv", std::ios::trunc);
    m_events.open(directory / "replay_events.csv", std::ios::trunc);
    m_tasks.open(directory / "replay_task_events.csv", std::ios::trunc);
    m_metadata.open(directory / "replay_run.csv", std::ios::trunc);
    CheckStreams();
    m_links << std::setprecision(17)
            << "sample_id,case_id,mode,start_ns,end_ns,src_node,src_port,dst_node,dst_port,"
               "link_kind,topology_epoch,te_epoch,toe_decision_epoch,toe_phase,data_rate_bps,"
               "connected_duration_ns,capacity_bits,tx_wire_bytes,tx_packets,boundary_reason,aggregation,circuit_count,read_response_tx_wire_bytes\n";
    m_nodePaths << "sample_id,case_id,start_ns,end_ns,topology_epoch,te_epoch,toe_decision_epoch,src_node,dst_node,src_leaf,dst_leaf,transit_leaf,tx_payload_bytes,tx_packets\n";
    m_queueSamples << "case_id,mode,time_ns,src_node,src_port,dst_node,dst_port,link_kind,"
                      "topology_epoch,te_epoch,toe_decision_epoch,toe_phase,voq_bytes,"
                      "egress_bytes,total_bytes,reason\n";
    m_topology << "case_id,mode,time_ns,topology_epoch,src_node,src_port,dst_node,dst_port,"
                  "link_kind,data_rate_bps\n";
    m_events << "event_id,case_id,mode,time_ns,event,topology_epoch,te_epoch,"
                "toe_decision_epoch,toe_phase\n";
    m_tasks << "case_id,mode,time_ns,event,task_kind,node_id,task_id\n";
    m_metadata << "key,value\n";
    DiscoverPorts();
    if (fabricOnly)
    {
        std::vector<PortState> fabricPorts;
        for (const auto& state : m_ports)
        {
            if (state.fabric) fabricPorts.push_back(state);
        }
        m_ports.swap(fabricPorts);
    }
    m_enabled = true;
    Metadata("schema_version", "2");
    Metadata("replay_logger_version", "5");
    Metadata("storage_decode_semantics",
             "host-to-leaf successful READ_RESPONSE wire bytes including headers and retransmissions");
    Metadata("sampling_context", m_parallel ? "public-lp-after-worker-barrier" : "single-thread");
    Metadata("window_semantics", "counter snapshots after workers through end time; boundary-time sends counted before public control update");
    Metadata("task_event_order", "buffered per LP; rows may be interleaved, use time_ns and task_id");
    Metadata("link_aggregation", aggregatePairs ? "node_pair" : "physical_port");
    Metadata("task_events", taskEvents ? "true" : "false");
    Metadata("node_path_semantics", "source-leaf successful fabric transmission payload; selected next hop; not destination delivery");
    Metadata("detail_windows", detailWindows);
    Metadata("node_path_enabled", m_detailWindows.empty() ? "false" : "true");
    Metadata("case_id", m_caseId);
    Metadata("mode", m_mode);
    Metadata("case_path", casePath);
    Metadata("sample_interval_ns", Ns(interval));
    Metadata("scope", fabricOnly ? "fabric" : "all");
    Metadata("queue_sampling", queues ? "true" : "false");
    Metadata("start_ns", Ns(Simulator::Now()));
    Metadata("timestamp_unit", "ns, exact to 0.001ns");
    Metadata("tx_counter_semantics", "successful transmission starts; packet bytes including headers and retransmissions");
    Metadata("capacity_semantics", "physical configured capacity; business pause is represented separately by toe_phase");
    Metadata("queue_semantics", "instantaneous snapshots, not interval averages or maxima");
    Metadata("selected_directed_ports", std::to_string(m_ports.size()));
    Metadata("queue_state", "instantaneous; repeated boundary times use topology, phase and reason");
    WriteTopology();
    WriteQueues("initial", Simulator::Now());
    Event("run-start");
    for (const auto& window : m_detailWindows)
    {
        for (Time boundary : {window.first,window.second})
            if (boundary > Simulator::Now() && boundary != Time::Max())
                m_detailEvents.push_back(Simulator::Schedule(boundary-Simulator::Now(),
                    &UbReplayLogger::Sample,this,std::string("detail-boundary")));
    }
    m_tick = Simulator::Schedule(interval, &UbReplayLogger::Tick, this);
}

void
UbReplayLogger::DiscoverPorts()
{
    m_ports.clear();
    m_storageAccessPorts.clear();
    m_hostPortToLeaf.clear();m_portPeers.clear();m_fabricPairs.clear();
    for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
    {
        const auto node = NodeList::GetNode(nodeId);
        // Match Dashboard topology.py's compute (64 UbPorts) / storage (other)
        // role distinction. Count UbPorts rather than unrelated NetDevices.
        uint32_t hostPorts = 0;
        if (!IsSwitch(node))
            for (uint32_t k = 0; k < node->GetNDevices(); ++k)
                if (DynamicCast<UbPort>(node->GetDevice(k))) ++hostPorts;
        const bool storage = !IsSwitch(node) && hostPorts != 64;
        for (uint32_t deviceId = 0; deviceId < node->GetNDevices(); ++deviceId)
        {
            const auto port = DynamicCast<UbPort>(node->GetDevice(deviceId));
            const auto link = port ? DynamicCast<UbLink>(port->GetChannel()) : nullptr;
            if (!link || link->IsRemote() || link->GetNDevices() != 2) continue;
            const auto peer = link->GetDestination(port);
            if (!peer) continue;
            m_portPeers[{node->GetId(),port->GetIfIndex()}] = peer->GetNode()->GetId();
            if (!IsSwitch(node) && IsSwitch(peer->GetNode()))
            {
                m_hostPortToLeaf[{node->GetId(),port->GetIfIndex()}] = peer->GetNode()->GetId();
                if (storage)
                    m_storageAccessPorts.insert({node->GetId(),port->GetIfIndex()});
            }
            if (IsSwitch(node) && IsSwitch(peer->GetNode()))
                m_fabricPairs.insert({node->GetId(),peer->GetNode()->GetId()});
            m_ports.push_back({port, node->GetId(), port->GetIfIndex(),
                               peer->GetNode()->GetId(), peer->GetIfIndex(),
                               IsSwitch(node) && IsSwitch(peer->GetNode()),
                               port->GetDataRate().GetBitRate(),
                               port->GetTxBytes(), port->GetTxPackets()});
        }
    }
}

void
UbReplayLogger::RefreshPeers()
{
    // Called synchronously after BeforeTopologyChange and physical rewiring;
    // no packet events can run between the checkpoint and this refresh.
    m_fabricPairs.clear();
    for (auto& state : m_ports)
    {
        const auto link = DynamicCast<UbLink>(state.port->GetChannel());
        NS_ABORT_MSG_IF(!link || link->GetNDevices() != 2,
                        "Replay port is missing its local point-to-point link");
        const auto peer = link->GetDestination(state.port);
        NS_ABORT_MSG_IF(!peer, "Replay port has no peer after topology apply");
        state.destinationNode = peer->GetNode()->GetId();
        state.destinationPort = peer->GetIfIndex();
        m_portPeers[{state.sourceNode,state.sourcePort}] = state.destinationNode;
        state.fabric = IsSwitch(state.port->GetNode()) && IsSwitch(peer->GetNode());
        if (state.fabric) m_fabricPairs.insert({state.sourceNode,state.destinationNode});
        state.rateBps = state.port->GetDataRate().GetBitRate();
        state.lastBytes = state.port->GetTxBytes();
        state.lastPackets = state.port->GetTxPackets();
    }
}

void
UbReplayLogger::Sample(const std::string& reason)
{
    SampleAt(Simulator::Now(), reason);
}

void
UbReplayLogger::SampleAt(Time now, const std::string& reason)
{
    if (!m_enabled) return;
    RequirePublicLp("sample");
    if (now > m_lastSample)
    {
        ++m_sampleId;
        const Time duration = now - m_lastSample;
        struct Aggregate { uint64_t rate{0},bytes{0},packets{0},circuits{0},readResponseBytes{0}; bool fabric{false}; };
        std::map<std::pair<uint32_t,uint32_t>,Aggregate> pairs;
        // Sampling runs on public LP after the worker barrier. Merge and reset
        // worker-local counters at the same boundary as the port Tx counters.
        std::map<std::pair<uint32_t,uint32_t>,uint64_t> readResponseByPort;
        for (auto& shard : m_shards)
        {
            for (const auto& [key,bytes] : shard.storageReadResponseBytes)
                readResponseByPort[key] += bytes;
            shard.storageReadResponseBytes.clear();
        }
        auto write = [&](uint32_t src, int64_t port, uint32_t dst, int64_t dp,
                         bool fabric, uint64_t rate, uint64_t bytes, uint64_t packets,
                         uint64_t circuits, uint64_t readResponseBytes,
                         const char* aggregation) {
            m_links << m_sampleId << ',' << Csv(m_caseId) << ',' << Csv(m_mode) << ','
                    << Ns(m_lastSample) << ',' << Ns(now) << ',' << src << ',' << port << ','
                    << dst << ',' << dp << ',' << (fabric ? "fabric" : "access") << ','
                    << m_topologyEpoch << ',' << m_teEpoch << ',' << m_toeDecisionEpoch << ','
                    << m_toePhase << ',' << rate << ',' << Ns(duration) << ','
                    << static_cast<double>(rate)*duration.GetSeconds() << ',' << bytes << ','
                    << packets << ',' << Csv(reason) << ',' << aggregation << ',' << circuits
                    << ',' << readResponseBytes << '\n';
        };
        for (auto& state : m_ports)
        {
            const uint64_t bytes=state.port->GetTxBytes(),packets=state.port->GetTxPackets();
            NS_ABORT_MSG_IF(bytes<state.lastBytes || packets<state.lastPackets,
                            "Replay transmit counters unexpectedly decreased");
            const uint64_t deltaBytes=bytes-state.lastBytes,deltaPackets=packets-state.lastPackets;
            const auto key=std::make_pair(state.sourceNode,state.sourcePort);
            const uint64_t readBytes=readResponseByPort[key];
            NS_ABORT_MSG_IF(readBytes > deltaBytes,
                            "READ_RESPONSE sent bytes exceed physical port transmit counter delta");
            if (m_aggregatePairs)
            {
                auto& pair=pairs[{state.sourceNode,state.destinationNode}];
                pair.rate+=state.rateBps; pair.bytes+=deltaBytes; pair.packets+=deltaPackets;
                pair.readResponseBytes+=readBytes;
                ++pair.circuits; pair.fabric=state.fabric;
            }
            else write(state.sourceNode,state.sourcePort,state.destinationNode,state.destinationPort,
                       state.fabric,state.rateBps,deltaBytes,deltaPackets,1,readBytes,"physical_port");
            state.lastBytes=bytes;state.lastPackets=packets;
        }
        for (const auto& [key,value] : pairs)
            write(key.first,-1,key.second,-1,value.fabric,value.rate,value.bytes,value.packets,
                  value.circuits,value.readResponseBytes,"node_pair");
        for (auto& shard : m_shards)
        {
            for (const auto& [key,value] : shard.paths)
            {
                const auto [sn,dn,src,dst,mid]=key;
                m_nodePaths << m_sampleId << ',' << Csv(m_caseId) << ',' << Ns(m_lastSample) << ','
                            << Ns(now) << ',' << m_topologyEpoch << ',' << m_teEpoch << ','
                            << m_toeDecisionEpoch << ',' << sn << ',' << dn << ',' << src << ','
                            << dst << ',' << mid << ',' << value.first << ',' << value.second << '\n';
            }
            shard.paths.clear();
            m_unattributedPathPackets += shard.unattributedPackets;
            shard.unattributedPackets = 0;
        }
        m_lastSample = now;
    }
    FlushTaskBuffers();
    WriteQueues(reason, now);
    CheckStreams();
}

void
UbReplayLogger::Tick()
{
    if (!m_enabled) return;
    RequirePublicLp("tick");
#ifdef NS3_MTP
    if (m_parallel && m_sampleId == 0)
        Metadata("worker_lp_count", std::to_string(MtpInterface::GetSize()-1));
#endif
    Sample("periodic");
    m_tick = Simulator::Schedule(m_interval, &UbReplayLogger::Tick, this);
}

void
UbReplayLogger::WriteQueues(const std::string& reason, Time now)
{
    if (!m_queues) return;
    for (const auto& state : m_ports)
    {
        const auto sw = state.port->GetNode()->GetObject<UbSwitch>();
        const auto qm = sw ? sw->GetQueueManager() : nullptr;
        const uint64_t voq = qm ? qm->GetTotalOutPortBufferUsed(state.sourcePort) : 0;
        const uint64_t egress = state.port->GetUbQueue()->GetCurrentBytes();
        m_queueSamples << Csv(m_caseId) << ',' << Csv(m_mode) << ',' << Ns(now)
                       << ',' << state.sourceNode << ',' << state.sourcePort << ','
                       << state.destinationNode << ',' << state.destinationPort << ','
                       << (state.fabric ? "fabric" : "access") << ','
                       << m_topologyEpoch << ',' << m_teEpoch << ',' << m_toeDecisionEpoch << ','
                       << m_toePhase << ',' << voq << ',' << egress << ',' << voq + egress
                       << ',' << Csv(reason) << '\n';
    }
}

void
UbReplayLogger::WriteTopology()
{
    for (const auto& state : m_ports)
    {
        m_topology << Csv(m_caseId) << ',' << Csv(m_mode) << ',' << Ns(Simulator::Now()) << ','
                   << m_topologyEpoch << ',' << state.sourceNode << ',' << state.sourcePort
                   << ',' << state.destinationNode << ',' << state.destinationPort << ','
                   << (state.fabric ? "fabric" : "access") << ',' << state.rateBps << '\n';
    }
}

void
UbReplayLogger::Metadata(const std::string& key, const std::string& value)
{
    RequirePublicLp("metadata");
    if (m_enabled) m_metadata << Csv(key) << ',' << Csv(value) << '\n';
}

void
UbReplayLogger::InputFingerprint(const std::string& name, const std::string& path)
{
    if (!m_enabled) return;
    Metadata(name + "_path", path);
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        Metadata(name + "_fnv1a64", "unavailable");
        return;
    }
    uint64_t hash = 14695981039346656037ULL;
    std::array<char, 65536> buffer;
    while (input)
    {
        input.read(buffer.data(), buffer.size());
        for (std::streamsize i = 0; i < input.gcount(); ++i)
        {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    NS_ABORT_MSG_IF(!input.eof(), "Cannot finish reading replay input fingerprint: " << path);
    std::ostringstream value;
    value << std::hex << std::setw(16) << std::setfill('0') << hash;
    Metadata(name + "_fnv1a64", value.str());
}

bool
UbReplayLogger::DetailsActive() const
{
    if (!m_enabled || m_detailWindows.empty()) return false;
    const Time now=Simulator::Now();
    for (const auto& [lo,hi] : m_detailWindows)
        if (lo<=now && now<hi) return true;
    return false;
}

void
UbReplayLogger::PathMetadata(Ptr<Packet> packet, uint32_t leaf, uint32_t sip, uint32_t dip,
                              uint32_t payloadBytes)
{
    if (!DetailsActive() || !payloadBytes) return;
    const uint32_t sn=utils::IpToNodeId(Ipv4Address(sip));
    const uint32_t dn=utils::IpToNodeId(Ipv4Address(dip));
    if (!(sip&255u) || !(dip&255u)) return;
    const auto src=m_hostPortToLeaf.find({sn,utils::IpToPortId(Ipv4Address(sip))});
    const auto dst=m_hostPortToLeaf.find({dn,utils::IpToPortId(Ipv4Address(dip))});
    if (src==m_hostPortToLeaf.end() || dst==m_hostPortToLeaf.end() ||
        src->second!=leaf || src->second==dst->second) return;
    UbReplayPathTag tag;
    packet->RemovePacketTag(tag);
    tag.sourceNode=sn; tag.destinationNode=dn; tag.sourceLeaf=src->second;
    tag.destinationLeaf=dst->second; tag.payloadBytes=payloadBytes;
    packet->AddPacketTag(tag);
}

void
UbReplayLogger::SuccessfulTransmit(Ptr<Packet> packet, uint32_t node, uint32_t port)
{
    if (!m_enabled) return;
    // This runs for successful Tx starts only. Count storage read replies at
    // their physical injection port, not again on each fabric hop.
    if (m_storageAccessPorts.count({node,port}))
    {
        auto copy=packet->Copy();
        UbDatalinkPacketHeader dl;
        copy->RemoveHeader(dl);
        if (dl.GetConfig()==static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4))
        {
            UbIpBasedNetworkHeader nh; Ipv4Header ip; UdpHeader udp; UbTransportHeader tp;
            copy->RemoveHeader(nh);copy->RemoveHeader(ip);copy->RemoveHeader(udp);copy->RemoveHeader(tp);
            if (tp.GetTPOpcode()==static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA))
            {
                UbTransactionHeader ta;copy->RemoveHeader(ta);
                if (ta.GetTaOpcode()==static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ_RESPONSE))
                    CurrentShard().storageReadResponseBytes[{node,port}]+=packet->GetSize();
            }
        }
    }
    if (!DetailsActive()) return;
    const auto peer=m_portPeers.find({node,port});
    if (peer==m_portPeers.end() || !m_fabricPairs.count({node,peer->second})) return;
    UbReplayPathTag tag;
    if (!packet->PeekPacketTag(tag))
    {
        // Packets can have queued before the detail range began. Parse only in
        // that fallback, at successful fabric send, without changing wire bytes.
        auto copy=packet->Copy();
        UbDatalinkPacketHeader dl;
        copy->RemoveHeader(dl);
        if (dl.GetConfig()!=static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4)) return;
        UbIpBasedNetworkHeader nh; Ipv4Header ip; UdpHeader udp; UbTransportHeader tp;
        copy->RemoveHeader(nh);copy->RemoveHeader(ip);copy->RemoveHeader(udp);copy->RemoveHeader(tp);
        if (tp.GetTPOpcode()!=static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA)) return;
        UbTransactionHeader ta;copy->RemoveHeader(ta);
        if (ta.GetTaOpcode()!=static_cast<uint8_t>(TaOpcode::TA_OPCODE_WRITE) &&
            ta.GetTaOpcode()!=static_cast<uint8_t>(TaOpcode::TA_OPCODE_READ_RESPONSE)) return;
        UbMAExtTah extension;copy->RemoveHeader(extension);
        PathMetadata(packet,node,ip.GetSource().Get(),ip.GetDestination().Get(),copy->GetSize());
        if (!packet->PeekPacketTag(tag)) return;
    }
    if (tag.sourceLeaf!=node) return;
    // Worker hooks must never sample, write shared streams or advance the
    // common window. The public boundary event performs that work after all LPs.
    if (!m_parallel)
        for (const auto& window : m_detailWindows)
            if (window.first<=Simulator::Now() && m_lastSample<window.first)
                Sample("detail-start-before-packet");
    const uint32_t next=peer->second;
    // A non-direct first hop is followed by shortest routing in this repository.
    // Count a selected path only if the corresponding second hop actually exists now.
    if (next!=tag.destinationLeaf)
    {
        if (!m_fabricPairs.count({next,tag.destinationLeaf}))
        { ++CurrentShard().unattributedPackets;return; }
    }
    auto& count=CurrentShard().paths[{tag.sourceNode,tag.destinationNode,tag.sourceLeaf,
                               tag.destinationLeaf,next==tag.destinationLeaf ? -1 : static_cast<int32_t>(next)}];
    count.first+=tag.payloadBytes;++count.second;
}

UbReplayLogger::Shard&
UbReplayLogger::CurrentShard()
{
    uint32_t lp = 0;
#ifdef NS3_MTP
    if (m_parallel) lp = Simulator::GetSystemId();
#endif
    NS_ABORT_MSG_IF(lp >= m_shards.size(), "Replay LP id exceeds preallocated node/LP shards");
    return m_shards[lp];
}

void
UbReplayLogger::RequirePublicLp(const char* operation) const
{
#ifdef NS3_MTP
    NS_ABORT_MSG_IF(m_parallel && Simulator::GetSystemId() != 0,
                    "Replay " << operation << " must execute on the MTP public LP");
#else
    NS_ABORT_MSG_IF(m_parallel, "Replay " << operation << " requested MTP without NS3_MTP support");
#endif
}

void
UbReplayLogger::FlushTaskBuffer(std::string& buffer)
{
    if (buffer.empty()) return;
    // Only a bounded chunk write takes this lock, never a packet/path counter.
    std::lock_guard<std::mutex> lock(m_taskWriteMutex);
    m_tasks.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    NS_ABORT_MSG_IF(!m_tasks, "Cannot write replay task events");
    buffer.clear();
}

void
UbReplayLogger::FlushTaskBuffers()
{
    RequirePublicLp("flush task buffers");
    for (auto& shard : m_shards) FlushTaskBuffer(shard.tasks);
}

void
UbReplayLogger::TaskEvent(const char* event, const char* kind,
                          uint32_t nodeId, uint32_t taskId)
{
    if (!m_enabled || !m_taskEvents) return;
    auto& buffer = CurrentShard().tasks;
    buffer += Csv(m_caseId) + ',' + Csv(m_mode) + ',' + Ns(Simulator::Now()) + ',' +
              event + ',' + kind + ',' + std::to_string(nodeId) + ',' +
              std::to_string(taskId) + '\n';
    if (buffer.size() >= 65536) FlushTaskBuffer(buffer);
}

void
UbReplayLogger::Event(const std::string& name)
{
    EventAt(name, Simulator::Now());
}

void
UbReplayLogger::EventAt(const std::string& name, Time now)
{
    RequirePublicLp("event");
    m_events << ++m_eventId << ',' << Csv(m_caseId) << ',' << Csv(m_mode) << ','
             << Ns(now) << ',' << Csv(name) << ',' << m_topologyEpoch << ','
             << m_teEpoch << ',' << m_toeDecisionEpoch << ',' << m_toePhase << '\n';
}

Time
UbReplayLogger::FinalTime() const
{
    Time end = Simulator::Now();
#ifdef NS3_MTP
    // Run() has joined every worker. An early worker Stop may leave LP0 behind
    // other LPs; use the latest executed time for the final partial interval.
    if (m_parallel)
        for (uint32_t lp=0;lp<MtpInterface::GetSize();++lp)
            end = Max(end, MtpInterface::GetSystem(lp)->Now());
#endif
    return end;
}

void
UbReplayLogger::TePolicyPublished(uint64_t epoch, bool solverRan)
{
    if (!m_enabled) return;
    Sample("te-policy-boundary");
    m_teEpoch = epoch;
    Event(solverRan ? "te-policy-published" : "te-fallback-published");
}

void
UbReplayLogger::ToePhase(uint64_t decisionEpoch, const std::string& phase)
{
    if (!m_enabled) return;
    Sample("toe-phase-boundary");
    m_toeDecisionEpoch = decisionEpoch;
    m_toePhase = phase;
    Event("toe-" + phase);
}

void
UbReplayLogger::BeforeTopologyChange()
{
    if (!m_enabled) return;
    Sample("topology-before-apply");
    Event("topology-before-apply");
}

void
UbReplayLogger::TopologyApplied(uint64_t decisionEpoch)
{
    if (!m_enabled) return;
    RefreshPeers();
    m_topologyEpoch = decisionEpoch;
    m_toeDecisionEpoch = decisionEpoch;
    m_toePhase = "applied";
    WriteTopology();
    WriteQueues("topology-applied", Simulator::Now());
    Event("topology-applied");
}

void
UbReplayLogger::Finish(const std::string& reason)
{
    if (!m_enabled) return;
    RequirePublicLp("finish");
    if (m_tick.IsPending()) Simulator::Cancel(m_tick);
    for (auto event : m_detailEvents) if (event.IsPending()) Simulator::Cancel(event);
    m_detailEvents.clear();
    const Time end = FinalTime();
    SampleAt(end, "final");
    EventAt("run-stop", end);
    Metadata("end_ns", Ns(end));
    Metadata("stop_reason", reason);
    Metadata("unattributed_path_packets", std::to_string(m_unattributedPathPackets));
    // Surface write errors, including failures reported only on flush.
    for (auto* stream : {&m_links, &m_queueSamples, &m_topology, &m_events, &m_tasks, &m_metadata, &m_nodePaths})
        stream->flush();
    CheckStreams();
    for (auto* stream : {&m_links, &m_queueSamples, &m_topology, &m_events, &m_tasks, &m_metadata, &m_nodePaths})
        stream->close();
    m_ports.clear();
    m_storageAccessPorts.clear();
    m_shards.clear();
    m_enabled = false;
}

void
UbReplayLogger::CheckStreams() const
{
    NS_ABORT_MSG_IF(!m_links || !m_queueSamples || !m_topology || !m_events || !m_tasks ||
                        !m_metadata || !m_nodePaths,
                    "Cannot open or write replay telemetry files");
}

} // namespace ns3
