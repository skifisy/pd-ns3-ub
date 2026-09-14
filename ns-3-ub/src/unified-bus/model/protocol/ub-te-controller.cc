// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-te-controller.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-routing-process.h"
#include "ns3/simulator.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/fatal-error.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace ns3 {

UbTeController& UbTeController::Get()
{
    static UbTeController controller;
    return controller;
}

std::string UbTeController::Quote(const std::string& value)
{
    std::string quoted = "'";
    for (char c : value) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + "'";
}

void UbTeController::Disable()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = false;
    m_hostPortToLeaf.clear();
    m_leafPorts.clear();
    m_leafPortToPeer.clear();
    m_observed.clear();
    m_linkObserved.clear();
    m_history.clear();
    m_seenPsn.clear();
    m_policy.clear();
    m_flowTransit.clear();
}

void UbTeController::Configure(const std::string& casePath, const std::string& solver,
                               double recomputeSeconds, uint32_t historyWindows, double s,
                               const std::string& outputDir)
{
    NS_ABORT_MSG_IF(recomputeSeconds <= 0 || historyWindows == 0 || s < 0 || s > 1,
                    "TE requires positive interval, history and S in [0,1]");
    NS_ABORT_MSG_IF(!std::filesystem::exists(solver), "TE solver not found: " << solver);
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
            NS_ABORT_MSG_IF(!m_hostPortToLeaf.emplace(std::make_pair(host, static_cast<uint16_t>(hp)), leaf).second,
                            "duplicate TE host port in topology");
        }
    }
    for (auto& [edge, ports] : m_leafPorts) std::sort(ports.begin(), ports.end());
    NS_ABORT_MSG_IF(m_leafPorts.empty() || m_hostPortToLeaf.empty(), "TE topology has no OCS or host links");
    m_casePath = casePath;
    m_solver = solver;
    m_intervalSeconds = recomputeSeconds;
    m_historyWindows = historyWindows;
    m_s = s;
    m_outputDir = outputDir.empty() ? casePath + "/jupiter_te" : outputDir;
    std::filesystem::create_directories(m_outputDir);
    std::ofstream(m_outputDir + "/observed.csv") << "window,src_leaf,dst_leaf,bytes\n";
    std::ofstream(m_outputDir + "/prediction.csv") << "epoch,src_leaf,dst_leaf,bps\n";
    std::ofstream(m_outputDir + "/weights.csv") << "epoch,src_leaf,dst_leaf,transit_leaf,weight\n";
    std::ofstream(m_outputDir + "/link_bytes.csv") << "window,src_leaf,dst_leaf,wire_bytes\n";
    m_enabled = true;
    Simulator::Schedule(Seconds(m_intervalSeconds), &UbTeController::Recompute, this);
}

std::vector<uint16_t> UbTeController::PhysicalPorts(uint32_t src, uint32_t dst) const
{
    const auto it = m_leafPorts.find({src, dst});
    return it == m_leafPorts.end() ? std::vector<uint16_t>{} : it->second;
}

void UbTeController::OnHostTransmit(uint32_t nodeId, uint16_t portId, Ptr<Packet> packet)
{
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const uint64_t window = static_cast<uint64_t>(std::floor(Simulator::Now().GetSeconds() / 30.0));
    const auto edge = m_leafPortToPeer.find({nodeId, portId});
    if (edge != m_leafPortToPeer.end()) {
        m_linkObserved[window][{nodeId, edge->second}] += packet->GetSize();
        return;
    }
    auto source = m_hostPortToLeaf.find({nodeId, portId});
    if (source == m_hostPortToLeaf.end() || packet->GetSize() < 4) return;
    auto copy = packet->Copy();
    UbDatalinkPacketHeader dl;
    copy->RemoveHeader(dl);
    if (dl.GetConfig() != 0x03 || dl.GetPacketVL() == 0 || dl.GetCredit() || dl.GetACK()) return;
    UbIpBasedNetworkHeader nh;
    Ipv4Header ip;
    UdpHeader udp;
    UbTransportHeader tp;
    UbTransactionHeader ta;
    UbMAExtTah ma;
    if (copy->GetSize() < nh.GetSerializedSize() + ip.GetSerializedSize() +
                              udp.GetSerializedSize() + tp.GetSerializedSize() +
                              ta.GetSerializedSize() + ma.GetSerializedSize()) return;
    copy->RemoveHeader(nh);
    copy->RemoveHeader(ip);
    if (utils::IpToNodeId(ip.GetSource()) != nodeId) return;
    copy->RemoveHeader(udp);
    copy->RemoveHeader(tp);
    if (tp.GetTPOpcode() != 0x01) return; // only reliable TA data, never control/ACK
    copy->RemoveHeader(ta);
    copy->RemoveHeader(ma);
    const uint32_t bytes = copy->GetSize();
    if (bytes == 0) return; // ACK and zero-payload READ requests are not traffic demand.
    const auto destinationHost = utils::IpToNodeId(ip.GetDestination());
    const uint32_t low = ip.GetDestination().Get() & 255u;
    if (!low) return; // TE needs the precise destination host port, not its primary IP.
    const auto dest = m_hostPortToLeaf.find({destinationHost, static_cast<uint16_t>(low - 1)});
    if (dest == m_hostPortToLeaf.end() || dest->second == source->second) return;
    const uint64_t packetId = (static_cast<uint64_t>(nodeId) << 48) |
                              (static_cast<uint64_t>(tp.GetSrcTpn() & 0xffffffu) << 24) |
                              (tp.GetPsn() & 0xffffffu);
    if (!m_seenPsn.insert(packetId).second) return; // retransmission is not new demand
    m_observed[window][{source->second, dest->second}] += bytes;
}

void UbTeController::FlushFinalWindow()
{
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream observed(m_outputDir + "/observed.csv", std::ios::app);
    std::ofstream edges(m_outputDir + "/link_bytes.csv", std::ios::app);
    for (const auto& [window, traffic] : m_observed)
        for (const auto& [od, bytes] : traffic)
            observed << window << ',' << od.first << ',' << od.second << ',' << bytes << '\n';
    for (const auto& [window, traffic] : m_linkObserved)
        for (const auto& [edge, bytes] : traffic)
            edges << window << ',' << edge.first << ',' << edge.second << ',' << bytes << '\n';
    m_observed.clear();
    m_linkObserved.clear();
}

std::vector<std::pair<int32_t, double>> UbTeController::Fallback(uint32_t src, uint32_t dst) const
{
    if (!PhysicalPorts(src, dst).empty()) return {{-1, 1.0}};
    std::vector<std::pair<int32_t, double>> result;
    for (const auto& [edge, ports] : m_leafPorts) {
        if (edge.first != src || edge.second == dst) continue;
        auto second = PhysicalPorts(edge.second, dst);
        if (!second.empty()) result.emplace_back(static_cast<int32_t>(edge.second),
                                                  static_cast<double>(std::min(ports.size(), second.size())));
    }
    return result;
}

int UbTeController::SelectLeafOutPort(uint32_t leaf, const RoutingKey& key, uint64_t hash,
                                      uint16_t inPort, bool& selectedDirect)
{
    if (!m_enabled) return -2;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto firstEdge = m_leafPorts.lower_bound({leaf, 0});
    if (firstEdge == m_leafPorts.end() || firstEdge->first.first != leaf) return -2;
    const auto sourceHost = utils::IpToNodeId(Ipv4Address(key.sip));
    const auto destHost = utils::IpToNodeId(Ipv4Address(key.dip));
    const uint32_t sourceLow = key.sip & 255u, destLow = key.dip & 255u;
    if (!sourceLow || !destLow) return -2;
    auto srcIt = m_hostPortToLeaf.find({sourceHost, static_cast<uint16_t>(sourceLow - 1)});
    auto dstIt = m_hostPortToLeaf.find({destHost, static_cast<uint16_t>(destLow - 1)});
    if (srcIt == m_hostPortToLeaf.end() || dstIt == m_hostPortToLeaf.end()) return -2;
    const uint32_t sourceLeaf = srcIt->second, destLeaf = dstIt->second;
    if (sourceLeaf == destLeaf || leaf == destLeaf) return -2;
    NS_ABORT_MSG_IF(key.usePacketSpray, "Jupiter TE requires per-flow packet routing");
    if (leaf != sourceLeaf) {
        if (!key.useShortestPath) return -1;
        // A transit leaf always makes its final OCS hop directly to the destination.
        auto ports = PhysicalPorts(leaf, destLeaf);
        if (ports.empty()) return -1;
        selectedDirect = true;
        return ports[hash % ports.size()];
    }
    if (m_leafPortToPeer.count({leaf, inPort})) return -1; // no OCS-to-OCS re-selection at the source
    auto flow = FlowId{leaf, hash};
    int32_t transit = -1;
    auto pin = m_flowTransit.find(flow);
    if (pin != m_flowTransit.end()) {
        transit = pin->second;
    } else {
        std::vector<std::pair<int32_t, double>> paths;
        auto policy = m_policy.find({sourceLeaf, destLeaf});
        if (policy != m_policy.end()) {
            for (auto [mid, weight] : policy->second)
                if (weight > 0 && !PhysicalPorts(sourceLeaf, mid < 0 ? destLeaf : static_cast<uint32_t>(mid)).empty())
                    paths.emplace_back(mid, weight);
        }
        if (paths.empty()) paths = Fallback(sourceLeaf, destLeaf);
        if (paths.empty()) return -1;
        double sum = 0;
        for (auto [mid, weight] : paths) sum += weight;
        double draw = static_cast<double>(hash >> 11) / static_cast<double>(uint64_t{1} << 53) * sum;
        transit = paths.back().first;
        for (auto [mid, weight] : paths) {
            if (draw < weight) { transit = mid; break; }
            draw -= weight;
        }
        m_flowTransit.emplace(flow, transit);
    }
    const uint32_t nextLeaf = transit < 0 ? destLeaf : static_cast<uint32_t>(transit);
    auto ports = PhysicalPorts(leaf, nextLeaf);
    if (ports.empty()) return -1;
    selectedDirect = transit < 0;
    // Low hash bits reproduce the old per-flow ECMP choice across four circuits.
    const uint16_t chosen = ports[hash % ports.size()];
    if (chosen == inPort) return -1;
    return chosen;
}

void UbTeController::Recompute()
{
    if (!m_enabled) return;
    const auto now = Simulator::Now().GetSeconds();
    const uint64_t complete = static_cast<uint64_t>(std::floor((now + 1e-9) / 30.0));
    std::map<Od, uint64_t> maxima;
    std::map<uint64_t, std::map<Od, uint64_t>> completed;
    std::map<uint64_t, std::map<Od, uint64_t>> completedLinks;
    uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_epoch;
        for (auto it = m_observed.begin(); it != m_observed.end();) {
            if (it->first < complete) {
                completed.emplace(it->first, it->second);
                it = m_observed.erase(it);
            } else ++it;
        }
        for (auto it = m_linkObserved.begin(); it != m_linkObserved.end();) {
            if (it->first < complete) {
                completedLinks.emplace(it->first, it->second);
                it = m_linkObserved.erase(it);
            } else ++it;
        }
    }
    // Persist the history across recomputations even when the LP interval != 30s.
    m_history.insert(completed.begin(), completed.end());
    for (auto it = m_history.begin(); it != m_history.end();) {
        if (it->first + m_historyWindows < complete) it = m_history.erase(it);
        else ++it;
    }
    std::ofstream observed(m_outputDir + "/observed.csv", std::ios::app);
    std::ofstream edges(m_outputDir + "/link_bytes.csv", std::ios::app);
    for (const auto& [window, traffic] : completed)
        for (const auto& [od, bytes] : traffic)
            observed << window << ',' << od.first << ',' << od.second << ',' << bytes << '\n';
    for (const auto& [window, traffic] : completedLinks)
        for (const auto& [edge, bytes] : traffic)
            edges << window << ',' << edge.first << ',' << edge.second << ',' << bytes << '\n';
    for (const auto& [window, traffic] : m_history)
        if (window < complete && window + m_historyWindows >= complete)
            for (const auto& [od, bytes] : traffic)
                maxima[od] = std::max(maxima[od], bytes);
    const std::string matrixPath = m_outputDir + "/matrix-current.csv";
    const std::string policyPath = m_outputDir + "/policy-current.csv";
    std::ofstream prediction(m_outputDir + "/prediction.csv", std::ios::app);
    std::ofstream matrix(matrixPath);
    matrix << std::setprecision(17);
    prediction << std::setprecision(17);
    matrix << "src_leaf,dst_leaf,bps\n";
    for (const auto& [od, bytes] : maxima) {
        const double bps = 8.0 * static_cast<double>(bytes) / 30.0;
        matrix << od.first << ',' << od.second << ',' << bps << '\n';
        prediction << epoch << ',' << od.first << ',' << od.second << ',' << bps << '\n';
    }
    matrix.close();
    prediction.close();
    if (!maxima.empty()) {
        const auto command = "python3 " + Quote(m_solver) + " --topology " +
                             Quote(m_casePath + "/topology.csv") + " --matrix " + Quote(matrixPath) +
                             " --output " + Quote(policyPath) + " --s " + std::to_string(m_s);
        NS_ABORT_MSG_IF(std::system(command.c_str()) != 0, "Jupiter TE LP solver failed: " << command);
        std::ifstream policyInput(policyPath);
        NS_ABORT_MSG_IF(!policyInput, "Jupiter TE LP policy missing");
        std::map<Od, PathWeights> next;
        std::string line;
        std::getline(policyInput, line);
        std::ofstream weights(m_outputDir + "/weights.csv", std::ios::app);
        weights << std::setprecision(17);
        while (std::getline(policyInput, line)) {
            std::stringstream row(line);
            std::string part;
            std::getline(row, part, ','); const uint32_t src = std::stoul(part);
            std::getline(row, part, ','); const uint32_t dst = std::stoul(part);
            std::getline(row, part, ','); const int32_t mid = std::stoi(part);
            std::getline(row, part, ','); const double weight = std::stod(part);
            next[{src, dst}][mid] = weight;
            weights << epoch << ',' << src << ',' << dst << ',' << mid << ',' << weight << '\n';
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_policy.swap(next); // atomic policy publication; existing flows stay pinned.
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_policy.clear();
    }
    if (m_enabled) Simulator::Schedule(Seconds(m_intervalSeconds), &UbTeController::Recompute, this);
}

} // namespace ns3
