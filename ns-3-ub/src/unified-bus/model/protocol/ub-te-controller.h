// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TE_CONTROLLER_H
#define UB_TE_CONTROLLER_H

#include "ns3/packet.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ns3 {

struct RoutingKey;

/** One process-wide controller for a static OCS case. In MPI mode each rank
 * would require a distributed reduction, so the example rejects MPI + TE. */
class UbTeController {
public:
    static UbTeController& Get();
    void Configure(const std::string& casePath, const std::string& solver,
                   double recomputeSeconds, uint32_t historyWindows, double s,
                   const std::string& outputDir);
    void Disable();
    void FlushFinalWindow();
    bool Enabled() const { return m_enabled; }
    void OnHostTransmit(uint32_t nodeId, uint16_t portId, Ptr<Packet> packet);

    // -2: not a TE-managed leaf flow; -1: no safe TE route; >=0: output port.
    int SelectLeafOutPort(uint32_t leaf, const RoutingKey& key, uint64_t flowHash,
                          uint16_t inPort, bool& selectedDirect);

private:
    using Od = std::pair<uint32_t, uint32_t>;
    using PathWeights = std::map<int32_t, double>; // -1 direct; otherwise transit leaf
    using FlowId = std::pair<uint32_t, uint64_t>;  // source leaf and existing ECMP hash

    UbTeController() = default;
    void Recompute();
    std::vector<uint16_t> PhysicalPorts(uint32_t src, uint32_t dst) const;
    std::vector<std::pair<int32_t, double>> Fallback(uint32_t src, uint32_t dst) const;
    static std::string Quote(const std::string& value);

    bool m_enabled = false;
    std::string m_casePath;
    std::string m_solver;
    std::string m_outputDir;
    double m_intervalSeconds = 30.0;
    uint32_t m_historyWindows = 120;
    double m_s = 0.0;
    uint64_t m_epoch = 0;
    std::map<std::pair<uint32_t, uint16_t>, uint32_t> m_hostPortToLeaf;
    std::map<Od, std::vector<uint16_t>> m_leafPorts;
    std::map<std::pair<uint32_t, uint16_t>, uint32_t> m_leafPortToPeer;
    std::map<uint64_t, std::map<Od, uint64_t>> m_observed; // floor(t / 30s)
    std::map<uint64_t, std::map<Od, uint64_t>> m_linkObserved; // OCS wire bytes
    std::map<uint64_t, std::map<Od, uint64_t>> m_history; // completed windows
    std::set<uint64_t> m_seenPsn; // host / source TPN / 24-bit packet sequence
    std::map<Od, PathWeights> m_policy;
    std::map<FlowId, int32_t> m_flowTransit; // pin active flows across epochs
    mutable std::mutex m_mutex;
};

} // namespace ns3
#endif
