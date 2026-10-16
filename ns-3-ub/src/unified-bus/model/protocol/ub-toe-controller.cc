// SPDX-License-Identifier: GPL-2.0-only
#include "ub-toe-controller.h"

#include "ns3/fatal-error.h"
#include "ns3/ipv4-address.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-link.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-te-controller.h"
#include "ns3/ub-transport.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ns3 {

UbToeController& UbToeController::Get()
{
    static UbToeController controller;
    return controller;
}

void UbToeController::Disable()
{
    if (m_windowEvent.IsPending()) Simulator::Cancel(m_windowEvent);
    if (m_recomputeEvent.IsPending()) Simulator::Cancel(m_recomputeEvent);
    if (m_drainEvent.IsPending()) Simulator::Cancel(m_drainEvent);
    if (m_timeoutEvent.IsPending()) Simulator::Cancel(m_timeoutEvent);
    if (m_applyEvent.IsPending()) Simulator::Cancel(m_applyEvent);
    m_enabled = false;
    m_paused = false;
    m_hostPortToLeaf.clear();
    m_groups.clear();
    m_leafPorts.clear();
    m_linkPorts.clear();
    m_currentCircuits.clear();
    m_plan = {};
    m_guardTxPackets.clear();
    m_unpausedPayloadBytes = 0;
    m_linkDelay = NanoSeconds(0);
    m_maxLinkDelay = NanoSeconds(0);
    m_windows.clear();
    m_history.clear();
    m_pendingTopology.clear();
    m_epoch = 0;
    m_drainPolls = 0;
}

void UbToeController::Configure(const std::string& casePath,
                                Mode mode,
                                double intervalSeconds,
                                uint32_t historyWindows,
                                double mluSlack,
                                double switchDelaySeconds,
                                const std::string& outputDir,
                                ReconfigurationMode reconfigurationMode)
{
    NS_ABORT_MSG_IF(!UbToeSolver::IsAvailable(), "ToE requires the in-process HiGHS backend");
    NS_ABORT_MSG_IF(intervalSeconds <= 0 || historyWindows == 0 || mluSlack < 0 ||
                        mluSlack > 1 || switchDelaySeconds < 0,
                    "invalid ToE configuration");
    Disable();
    m_casePath = casePath;
    m_mode = mode;
    NS_ABORT_MSG_IF(mode == Mode::WITH_TE &&
                        reconfigurationMode == ReconfigurationMode::SELECTIVE,
                    "selective ToE reconfiguration requires --toe-mode=only");
    m_selective = mode == Mode::ONLY && reconfigurationMode != ReconfigurationMode::GLOBAL;
    m_intervalSeconds = intervalSeconds;
    m_historyWindows = historyWindows;
    m_mluSlack = mluSlack;
    m_switchDelay = Seconds(switchDelaySeconds);
    m_outputDir = outputDir.empty() ? casePath + "/toe" : outputDir;
    std::filesystem::create_directories(m_outputDir);

    std::ifstream input(casePath + "/topology.csv");
    NS_ABORT_MSG_IF(!input, "ToE cannot read topology.csv");
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line))
    {
        if (line.empty() || line[0] == '#') continue;
        std::stringstream row(line);
        std::string part;
        std::vector<std::string> fields;
        while (std::getline(row, part, ',')) fields.push_back(part);
        NS_ABORT_MSG_IF(fields.size() < 4, "malformed ToE topology row: " << line);
        const uint32_t a = static_cast<uint32_t>(std::stoul(fields[0]));
        const uint32_t ap = static_cast<uint32_t>(std::stoul(fields[1]));
        const uint32_t b = static_cast<uint32_t>(std::stoul(fields[2]));
        const uint32_t bp = static_cast<uint32_t>(std::stoul(fields[3]));
        if (a >= 37 && a <= 56 && b >= 37 && b <= 56)
        {
            m_currentCircuits.push_back(a < b
                ? PortPair{a, static_cast<uint16_t>(ap), b, static_cast<uint16_t>(bp)}
                : PortPair{b, static_cast<uint16_t>(bp), a, static_cast<uint16_t>(ap)});
            m_linkPorts[{a, b}].push_back(static_cast<uint16_t>(ap));
            m_linkPorts[{b, a}].push_back(static_cast<uint16_t>(bp));
            m_leafPorts[a].push_back(DynamicCast<UbPort>(NodeList::GetNode(a)->GetDevice(ap)));
            m_leafPorts[b].push_back(DynamicCast<UbPort>(NodeList::GetNode(b)->GetDevice(bp)));
            m_groups[a] = (a - 37) / 4;
            m_groups[b] = (b - 37) / 4;
            Ptr<UbPort> port = DynamicCast<UbPort>(NodeList::GetNode(a)->GetDevice(ap));
            if (port && port->GetChannel())
            {
                const Time delay = DynamicCast<UbLink>(port->GetChannel())->GetDelay();
                if (m_linkDelay.IsZero() && !delay.IsZero()) m_linkDelay = delay;
                m_maxLinkDelay = std::max(m_maxLinkDelay, delay);
            }
        }
        else if (a < 37 && b >= 37 && b <= 56)
        {
            m_hostPortToLeaf[{a, static_cast<uint16_t>(ap)}] = b;
        }
        else if (b < 37 && a >= 37 && a <= 56)
        {
            m_hostPortToLeaf[{b, static_cast<uint16_t>(bp)}] = a;
        }
    }
    NS_ABORT_MSG_IF(m_leafPorts.size() != 20 || m_hostPortToLeaf.empty(),
                    "ToE expects the current 20-leaf Mooncake topology");
    for (auto& [leaf, ports] : m_leafPorts)
    {
        std::sort(ports.begin(), ports.end(), [](const Ptr<UbPort>& a, const Ptr<UbPort>& b) {
            return a->GetIfIndex() < b->GetIfIndex();
        });
        NS_ABORT_MSG_IF(ports.size() != 64, "ToE expects 64 OCS ports on leaf " << leaf);
    }
    // Keep candidate ordering stable for unaffected bundles across every apply.
    for (auto& [edge, ports] : m_linkPorts) std::sort(ports.begin(), ports.end());
    // Validate endpoint uniqueness before accepting the physical baseline.
    (void)UbToeReconfiguration::Build(m_currentCircuits, CurrentTopology());
    NS_ABORT_MSG_IF(!VerifyPhysicalTopology(), "ToE physical baseline differs from topology.csv");
    m_enabled = true;
    std::ofstream(m_outputDir + "/toe_summary.csv")
        << "epoch,time_seconds,status,min_mlu,applied_mlu_bound,changed_circuits,"
           "solver_optimal,drain_seconds,pause_seconds\n";
    std::ofstream(m_outputDir + "/toe_topology.csv")
        << "epoch,time_seconds,status,src_leaf,dst_leaf,circuit_count,"
           "capacity_gbps_per_direction\n";
    std::ofstream(m_outputDir + "/toe_reconfiguration.csv")
        << "epoch,time_seconds,status,scope,affected_leaf_pairs,paused_tp_channels,"
           "total_tp_channels,retained_circuits,rewired_circuits,"
           "unpaused_new_payload_bytes,pause_seconds\n";
    std::ofstream(m_outputDir + "/toe_circuits.csv")
        << "epoch,time_seconds,status,src_leaf,src_port,dst_leaf,dst_port\n";
    std::ofstream(m_outputDir + "/toe_demand.csv")
        << "epoch,decision_seconds,src_leaf,dst_leaf,demand_bps,demand_gbps,"
           "peak_window_bytes,latest_window_bytes,latest_gbps,sample_window_seconds,"
           "history_windows,completed_window,matrix_complete,matrix_kind\n";
    WriteCircuits(m_currentCircuits, "initial");
    m_windowEvent = Simulator::Schedule(Seconds(30), &UbToeController::SampleWindow, this);
    m_recomputeEvent = Simulator::Schedule(Seconds(intervalSeconds) + NanoSeconds(1),
                                            &UbToeController::Recompute, this);
}

void UbToeController::SampleWindow()
{
    if (!m_enabled) return;
    const uint64_t window = static_cast<uint64_t>(std::floor(Simulator::Now().GetSeconds() / 30.0));
    std::map<Od, uint64_t> bytes;
    for (auto it = m_windows.begin(); it != m_windows.end();)
    {
        if (it->first < window)
        {
            for (const auto& [od, count] : it->second) bytes[od] += count;
            it = m_windows.erase(it);
        }
        else ++it;
    }
    if (!bytes.empty()) m_history[window - 1] = std::move(bytes);
    while (!m_history.empty() && m_history.begin()->first + m_historyWindows < window)
        m_history.erase(m_history.begin());
    m_windowEvent = Simulator::Schedule(Seconds(30), &UbToeController::SampleWindow, this);
}

void UbToeController::OnNewDataPacket(uint32_t sourceHost, uint16_t sourcePort,
                                      uint32_t destinationHost, uint16_t destinationPort,
                                      uint32_t payloadBytes)
{
    if (!m_enabled || payloadBytes == 0) return;
    if (m_paused && !PauseNewBusinessData(sourceHost, sourcePort,
                                          destinationHost, destinationPort))
        m_unpausedPayloadBytes += payloadBytes;
    auto src = m_hostPortToLeaf.find({sourceHost, sourcePort});
    auto dst = m_hostPortToLeaf.find({destinationHost, destinationPort});
    if (src == m_hostPortToLeaf.end() || dst == m_hostPortToLeaf.end() ||
        src->second == dst->second)
        return;
    const uint64_t window = static_cast<uint64_t>(std::floor(Simulator::Now().GetSeconds() / 30.0));
    m_windows[window][{src->second, dst->second}] += payloadBytes;
}

std::map<UbToeController::Od, uint32_t> UbToeController::CurrentTopology() const
{
    std::map<Od, uint32_t> topology;
    for (const auto& [edge, ports] : m_linkPorts)
        if (edge.first < edge.second) topology[edge] = ports.size();
    return topology;
}

std::vector<uint16_t> UbToeController::PortsBetween(uint32_t src, uint32_t dst) const
{
    auto it = m_linkPorts.find({src, dst});
    return it == m_linkPorts.end() ? std::vector<uint16_t>{} : it->second;
}

void UbToeController::Recompute()
{
    if (!m_enabled || m_paused) return;
    const uint64_t completedWindow = static_cast<uint64_t>(
        std::floor(Simulator::Now().GetSeconds() / 30.0));
    std::map<Od, uint64_t> peak;
    for (const auto& [window, traffic] : m_history)
    {
        if (window + m_historyWindows < completedWindow) continue;
        for (const auto& [od, bytes] : traffic) peak[od] = std::max(peak[od], bytes);
    }
    std::map<Od, double> demand;
    for (const auto& [od, bytes] : peak) demand[od] = 8.0 * bytes / 30.0;
    ++m_epoch;
    // Export the exact directional input to Solve before drain/apply changes time.
    // Complete matrices include explicit zeros; OD peaks need not share a window.
    {
        std::ofstream output(m_outputDir + "/toe_demand.csv", std::ios::app);
        NS_ABORT_MSG_IF(!output, "Cannot write ToE solver-demand matrix");
        output << std::setprecision(17);
        const auto latestWindow = completedWindow > 0
            ? m_history.find(completedWindow - 1) : m_history.end();
        for (const auto& [src, srcGroup] : m_groups)
        {
            (void)srcGroup;
            for (const auto& [dst, dstGroup] : m_groups)
            {
                (void)dstGroup;
                const Od od{src, dst};
                const auto demandIt = demand.find(od);
                const double bps = demandIt == demand.end() ? 0.0 : demandIt->second;
                const auto peakIt = peak.find(od);
                const uint64_t peakBytes = peakIt == peak.end() ? 0 : peakIt->second;
                uint64_t latestBytes = 0;
                if (latestWindow != m_history.end())
                {
                    const auto latest = latestWindow->second.find(od);
                    if (latest != latestWindow->second.end()) latestBytes = latest->second;
                }
                output << m_epoch << ',' << Simulator::Now().GetSeconds() << ','
                       << src << ',' << dst << ',' << bps << ',' << bps / 1e9 << ','
                       << peakBytes << ',' << latestBytes << ',' << 8.0 * latestBytes / 30.0 / 1e9
                       << ",30," << m_historyWindows << ',' << completedWindow
                       << ",1,solver_input\n";
            }
        }
    }
    try
    {
        auto result = UbToeSolver::Solve({37,38,39,40,41,42,43,44,45,46,
                                          47,48,49,50,51,52,53,54,55,56},
                                         m_groups, CurrentTopology(), demand,
                                         m_mode == Mode::ONLY ? UbToeSolver::RoutingMode::ECMP
                                                              : UbToeSolver::RoutingMode::WCMP,
                                         m_mluSlack, 30.0);
        WriteSummary(result, "solved");
        WriteTopology(result.topology, "target");
        if (result.changedCircuits > 0) BeginReconfiguration(result.topology, result);
        else WriteTopology(result.topology, "applied");
    }
    catch (const std::exception& error)
    {
        UbToeSolver::Result empty;
        WriteSummary(empty, "solver-error");
        std::ofstream errors(m_outputDir + "/toe_errors.log", std::ios::app);
        errors << std::setprecision(12) << Simulator::Now().GetSeconds() << ','
               << error.what() << '\n';
    }
    if (m_enabled && !m_paused)
        m_recomputeEvent = Simulator::Schedule(Seconds(m_intervalSeconds) + NanoSeconds(1),
                                                &UbToeController::Recompute, this);
}

void UbToeController::WriteSummary(const UbToeSolver::Result& result, const char* status)
{
    std::ofstream output(m_outputDir + "/toe_summary.csv", std::ios::app);
    output << std::setprecision(12)
        << m_epoch << ',' << Simulator::Now().GetSeconds() << ',' << status << ','
        << result.minMaxUtilization << ',' << result.appliedUtilizationBound << ','
        << result.changedCircuits << ',' << result.optimal << ",0,0\n";
}

void UbToeController::WriteTopology(const UbToeSolver::Topology& topology,
                                    const char* status) const
{
    constexpr uint32_t kCircuitCapacityGbps = 400;
    std::ofstream output(m_outputDir + "/toe_topology.csv", std::ios::app);
    output << std::setprecision(12);
    for (const auto& [pair, circuits] : topology)
    {
        output << m_epoch << ',' << Simulator::Now().GetSeconds() << ',' << status << ','
               << pair.first << ',' << pair.second << ',' << circuits << ','
               << circuits * kCircuitCapacityGbps << '\n';
    }
}

bool UbToeController::PauseNewBusinessData(uint32_t sourceHost, uint16_t sourcePort,
                                           uint32_t destinationHost,
                                           uint16_t destinationPort) const
{
    if (!m_paused) return false;
    if (!m_selective) return true;
    const auto src = m_hostPortToLeaf.find({sourceHost, sourcePort});
    const auto dst = m_hostPortToLeaf.find({destinationHost, destinationPort});
    // Unknown endpoints must not send through ports whose peer is changing.
    if (src == m_hostPortToLeaf.end() || dst == m_hostPortToLeaf.end()) return true;
    return UbToeReconfiguration::AffectsEcmpOd(src->second, dst->second,
                                               m_groups, m_plan.changedPairs);
}

void UbToeController::WriteCircuits(const std::vector<PortPair>& circuits,
                                    const char* status) const
{
    std::ofstream output(m_outputDir + "/toe_circuits.csv", std::ios::app);
    output << std::setprecision(12);
    for (const auto& circuit : circuits)
        output << m_epoch << ',' << Simulator::Now().GetSeconds() << ',' << status << ','
               << circuit.a << ',' << circuit.ap << ',' << circuit.b << ',' << circuit.bp
               << '\n';
}

void UbToeController::WriteReconfiguration(const char* status) const
{
    uint64_t total = 0, paused = 0;
    for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
    {
        auto controller = NodeList::GetNode(nodeId)->GetObject<UbController>();
        if (!controller) continue;
        for (const auto& [tpn, tp] : controller->GetTpnMap())
        {
            (void)tpn;
            if (!tp) continue;
            ++total;
            if (PauseNewBusinessData(nodeId, tp->GetSport(), tp->GetDest(), tp->GetDport()))
                ++paused;
        }
    }
    std::ofstream output(m_outputDir + "/toe_reconfiguration.csv", std::ios::app);
    output << std::setprecision(12) << m_epoch << ',' << Simulator::Now().GetSeconds()
           << ',' << status << ',' << (m_selective ? "selective" : "global") << ','
           << m_plan.changedPairs.size() << ',' << paused << ',' << total << ','
           << m_plan.retained.size() << ',' << m_plan.added.size() << ','
           << m_unpausedPayloadBytes << ','
           << (Simulator::Now() - m_pauseStart).GetSeconds() << '\n';
}

bool UbToeController::VerifyPhysicalTopology() const
{
    for (const auto& circuit : m_currentCircuits)
    {
        auto a = DynamicCast<UbPort>(NodeList::GetNode(circuit.a)->GetDevice(circuit.ap));
        auto b = DynamicCast<UbPort>(NodeList::GetNode(circuit.b)->GetDevice(circuit.bp));
        auto link = a ? DynamicCast<UbLink>(a->GetChannel()) : nullptr;
        if (!a || !b || !link || b->GetChannel() != link ||
            link->GetNDevices() != 2 || link->GetDestination(a) != b ||
            link->GetDestination(b) != a) return false;
    }
    return true;
}

void UbToeController::BeginReconfiguration(const UbToeSolver::Topology& target,
                                           const UbToeSolver::Result& result)
{
    m_plan = UbToeReconfiguration::Build(m_currentCircuits, target);
    NS_ABORT_MSG_IF(m_plan.removed.size() != result.changedCircuits ||
                        m_plan.added.size() != result.changedCircuits,
                    "ToE port plan differs from solver circuit change count");
    m_pendingTopology = target;
    m_pendingResult = result;
    m_unpausedPayloadBytes = 0;
    m_guardTxPackets.clear();
    m_paused = true;
    m_pauseStart = Simulator::Now();
    m_drainPolls = 0;
    WriteSummary(result, "draining");
    WriteReconfiguration("draining");
    WriteCircuits(m_plan.retained, "retained");
    WriteCircuits(m_plan.removed, "remove");
    WriteCircuits(m_plan.added, "add");
    if (m_recomputeEvent.IsPending()) Simulator::Cancel(m_recomputeEvent);
    if (m_mode == Mode::WITH_TE) UbTeController::Get().PauseForTopologyChange();
    m_timeoutEvent = Simulator::Schedule(Seconds(10),
                                         &UbToeController::AbortReconfiguration, this);
    m_drainEvent = Simulator::Schedule(MicroSeconds(100), &UbToeController::PollDrain, this);
}

bool UbToeController::IsDrained() const
{
    for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
    {
        Ptr<Node> node = NodeList::GetNode(nodeId);
        Ptr<UbController> controller = node->GetObject<UbController>();
        if (controller)
        {
            for (const auto& [tpn, tp] : controller->GetTpnMap())
            {
                (void)tpn;
                if (tp && PauseNewBusinessData(nodeId, tp->GetSport(),
                                                tp->GetDest(), tp->GetDport()) &&
                    (tp->GetPsnSndUna() < tp->GetPsnSndNxt() ||
                     tp->HasPendingControlForTopologyChange())) return false;
            }
        }
        auto sw = node->GetObject<UbSwitch>();
        if (m_selective)
        {
            const auto ports = m_plan.changedPorts.find(nodeId);
            if (ports == m_plan.changedPorts.end()) continue;
            for (uint16_t portId : ports->second)
            {
                auto port = DynamicCast<UbPort>(node->GetDevice(portId));
                if (port->IsBusy() || !port->GetUbQueue()->IsEmpty() ||
                    (sw && !sw->IsPortQuiescentForTopologyChange(portId))) return false;
            }
        }
        else
        {
            for (uint32_t i = 0; i < node->GetNDevices(); ++i)
            {
                auto port = DynamicCast<UbPort>(node->GetDevice(i));
                if (port && (port->IsBusy() || !port->GetUbQueue()->IsEmpty())) return false;
            }
            if (sw && !sw->IsQuiescentForTopologyChange()) return false;
        }
    }
    return true;
}

void UbToeController::PollDrain()
{
    if (!m_paused) return;
    if (!IsDrained()) m_drainPolls = 0;
    else ++m_drainPolls;
    if (m_drainPolls >= 2)
    {
        m_drainFinish = Simulator::Now();
        m_guardTxPackets.clear();
        for (const auto& [leaf, ports] : m_plan.changedPorts)
            for (uint16_t portId : ports)
                m_guardTxPackets[{leaf, portId}] =
                    DynamicCast<UbPort>(NodeList::GetNode(leaf)->GetDevice(portId))->GetTxPackets();
        // Keep the timeout armed throughout the propagation/switch guard.
        m_applyEvent = Simulator::Schedule(m_maxLinkDelay * 2 + NanoSeconds(1) + m_switchDelay,
                                            &UbToeController::ApplyReconfiguration, this);
        return;
    }
    m_drainEvent = Simulator::Schedule(MicroSeconds(100), &UbToeController::PollDrain, this);
}

void UbToeController::AbortReconfiguration()
{
    if (!m_paused) return;
    if (m_drainEvent.IsPending()) Simulator::Cancel(m_drainEvent);
    if (m_applyEvent.IsPending()) Simulator::Cancel(m_applyEvent);
    WriteSummary(m_pendingResult, "drain-timeout");
    WriteReconfiguration("drain-timeout");
    std::ofstream errors(m_outputDir + "/toe_errors.log", std::ios::app);
    errors << std::setprecision(12) << Simulator::Now().GetSeconds() << ",drain-timeout\n";
    if (m_mode == Mode::WITH_TE) UbTeController::Get().ResumeAfterAbortedTopologyChange();
    ResumeTraffic();
}

void UbToeController::ResumeTraffic()
{
    std::set<std::pair<uint32_t, uint16_t>> wakePorts;
    for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
    {
        auto node = NodeList::GetNode(nodeId);
        auto controller = node->GetObject<UbController>();
        if (controller)
            for (const auto& [tpn, tp] : controller->GetTpnMap())
            {
                (void)tpn;
                if (tp && PauseNewBusinessData(nodeId, tp->GetSport(),
                                                tp->GetDest(), tp->GetDport()))
                    wakePorts.insert({nodeId, tp->GetSport()});
            }
        if (!m_selective)
            for (uint32_t index = 0; index < node->GetNDevices(); ++index)
                wakePorts.insert({nodeId, static_cast<uint16_t>(index)});
    }
    for (const auto& [leaf, ports] : m_plan.changedPorts)
        for (uint16_t portId : ports) wakePorts.insert({leaf, portId});
    m_paused = false;
    m_pendingTopology.clear();
    m_plan = {};
    m_guardTxPackets.clear();
    for (const auto& [nodeId, portId] : wakePorts)
    {
        auto port = DynamicCast<UbPort>(NodeList::GetNode(nodeId)->GetDevice(portId));
        if (port) port->TriggerTransmit();
    }
    m_recomputeEvent = Simulator::Schedule(Seconds(m_intervalSeconds) + NanoSeconds(1),
                                            &UbToeController::Recompute, this);
}

void UbToeController::ApplyReconfiguration()
{
    if (!m_paused) return;
    bool quietGuard = true;
    for (const auto& [endpoint, packets] : m_guardTxPackets)
        if (DynamicCast<UbPort>(NodeList::GetNode(endpoint.first)->GetDevice(endpoint.second))
                ->GetTxPackets() != packets) quietGuard = false;
    if (!quietGuard || !IsDrained())
    {
        m_drainPolls = 0;
        m_drainEvent = Simulator::Schedule(MicroSeconds(100), &UbToeController::PollDrain, this);
        return;
    }
    for (const auto& pair : m_plan.added)
    {
        auto pa = DynamicCast<UbPort>(NodeList::GetNode(pair.a)->GetDevice(pair.ap));
        auto pb = DynamicCast<UbPort>(NodeList::GetNode(pair.b)->GetDevice(pair.bp));
        auto link = CreateObject<UbLink>();
        link->SetAttribute("Delay", TimeValue(m_linkDelay));
        pa->Attach(link);
        pb->Attach(link);
    }
    // Retained ports keep their UbLink objects and peer flow-control state.
    for (const auto& [leaf, ports] : m_plan.changedPorts)
    {
        auto sw = NodeList::GetNode(leaf)->GetObject<UbSwitch>();
        for (uint16_t portId : ports)
        {
            sw->InitPortFlowControl(portId);
            DynamicCast<UbPort>(NodeList::GetNode(leaf)->GetDevice(portId))->ResetCredits();
        }
    }
    m_currentCircuits = m_plan.retained;
    m_currentCircuits.insert(m_currentCircuits.end(), m_plan.added.begin(), m_plan.added.end());
    m_linkPorts.clear();
    for (const auto& circuit : m_currentCircuits)
    {
        m_linkPorts[{circuit.a, circuit.b}].push_back(circuit.ap);
        m_linkPorts[{circuit.b, circuit.a}].push_back(circuit.bp);
    }
    for (auto& [edge, ports] : m_linkPorts) std::sort(ports.begin(), ports.end());
    NS_ABORT_MSG_IF(CurrentTopology() != m_pendingTopology || !VerifyPhysicalTopology(),
                    "ToE physical port readback does not match target topology");
    if (m_mode == Mode::WITH_TE)
    {
        for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
        {
            auto sw = NodeList::GetNode(nodeId)->GetObject<UbSwitch>();
            if (sw && sw->GetRoutingProcess()) sw->GetRoutingProcess()->ClearTeRouteCache();
        }
        UbTeController::Get().RefreshTopology();
        UbTeController::Get().RecomputeForTopologyChange();
    }
    WriteTopology(CurrentTopology(), "applied");
    WriteCircuits(m_currentCircuits, "applied");
    WriteReconfiguration("applied");
    std::ofstream summary(m_outputDir + "/toe_summary.csv", std::ios::app);
    summary << std::setprecision(12)
        << m_epoch << ',' << Simulator::Now().GetSeconds() << ",applied,"
        << m_pendingResult.minMaxUtilization << ',' << m_pendingResult.appliedUtilizationBound
        << ',' << m_pendingResult.changedCircuits << ',' << m_pendingResult.optimal << ','
        << (m_drainFinish - m_pauseStart).GetSeconds() << ','
        << (Simulator::Now() - m_pauseStart).GetSeconds() << '\n';
    if (m_timeoutEvent.IsPending()) Simulator::Cancel(m_timeoutEvent);
    ResumeTraffic();
}

int UbToeController::SelectEcmpOutPort(uint32_t leaf,
                                       const RoutingKey& key,
                                       uint64_t flowHash,
                                       uint16_t inPort,
                                       bool& selectedShortestPath) const
{
    auto leafForIp = [&](uint32_t ip) -> uint32_t {
        const Ipv4Address address(ip);
        const uint32_t host = utils::IpToNodeId(address);
        const uint32_t low = ip & 0xff;
        if (low == 0) return UINT32_MAX;
        auto it = m_hostPortToLeaf.find({host, static_cast<uint16_t>(low - 1)});
        return it == m_hostPortToLeaf.end() ? UINT32_MAX : it->second;
    };
    const uint32_t src = leafForIp(key.sip), dst = leafForIp(key.dip);
    if (src == UINT32_MAX || dst == UINT32_MAX || src == dst || leaf == dst) return -2;
    std::vector<uint16_t> candidates;
    if (leaf == src)
    {
        candidates = PortsBetween(src, dst);
        if (candidates.empty())
        {
            for (const auto& [middle, group] : m_groups)
            {
                (void)group;
                if (middle == src || middle == dst || PortsBetween(middle, dst).empty()) continue;
                const auto firstHop = PortsBetween(src, middle);
                candidates.insert(candidates.end(), firstHop.begin(), firstHop.end());
            }
        }
    }
    else
    {
        candidates = PortsBetween(leaf, dst);
    }
    candidates.erase(std::remove(candidates.begin(), candidates.end(), inPort), candidates.end());
    if (candidates.empty()) return -1;
    selectedShortestPath = true;
    return candidates[flowHash % candidates.size()];
}

} // namespace ns3

