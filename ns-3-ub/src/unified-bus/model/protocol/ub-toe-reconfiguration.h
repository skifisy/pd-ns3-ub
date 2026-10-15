// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TOE_RECONFIGURATION_H
#define UB_TOE_RECONFIGURATION_H

#include "ub-toe-solver.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace ns3 {

/** Pure port planner, shared by the runtime and standalone regression tests. */
class UbToeReconfiguration
{
  public:
    using Od = UbToeSolver::Od;
    using Endpoint = std::pair<uint32_t, uint16_t>;
    struct Circuit
    {
        uint32_t a;
        uint16_t ap;
        uint32_t b;
        uint16_t bp;
        bool operator==(const Circuit& other) const
        {
            return a == other.a && ap == other.ap && b == other.b && bp == other.bp;
        }
    };
    struct Plan
    {
        std::vector<Circuit> retained;
        std::vector<Circuit> removed;
        std::vector<Circuit> added;
        std::set<Od> changedPairs;
        std::map<uint32_t, std::set<uint16_t>> changedPorts;
    };

    static Plan Build(const std::vector<Circuit>& current,
                      const UbToeSolver::Topology& target)
    {
        Plan plan;
        std::map<Od, std::vector<Circuit>> bundles;
        std::set<Endpoint> endpoints;
        std::map<uint32_t, uint32_t> oldDegree, newDegree;
        for (const auto& circuit : current)
        {
            if (circuit.a >= circuit.b ||
                !endpoints.insert({circuit.a, circuit.ap}).second ||
                !endpoints.insert({circuit.b, circuit.bp}).second)
                throw std::invalid_argument("invalid/duplicate ToE physical circuit endpoint");
            bundles[{circuit.a, circuit.b}].push_back(circuit);
            ++oldDegree[circuit.a];
            ++oldDegree[circuit.b];
        }
        for (const auto& [pair, count] : target)
        {
            if (pair.first >= pair.second || count == 0 || !bundles.count(pair))
                throw std::invalid_argument("invalid ToE target circuit bundle");
            newDegree[pair.first] += count;
            newDegree[pair.second] += count;
        }
        if (bundles.size() != target.size() || oldDegree != newDegree)
            throw std::invalid_argument("ToE target must preserve pair coverage and leaf degree");

        std::map<uint32_t, std::vector<uint16_t>> freePorts;
        for (auto& [pair, circuits] : bundles)
        {
            std::sort(circuits.begin(), circuits.end(), [](const Circuit& a, const Circuit& b) {
                return std::make_pair(a.ap, a.bp) < std::make_pair(b.ap, b.bp);
            });
            const auto keep = std::min<size_t>(circuits.size(), target.at(pair));
            if (circuits.size() != target.at(pair)) plan.changedPairs.insert(pair);
            for (size_t index = 0; index < circuits.size(); ++index)
            {
                const auto& circuit = circuits[index];
                if (index < keep) plan.retained.push_back(circuit);
                else
                {
                    plan.removed.push_back(circuit);
                    freePorts[circuit.a].push_back(circuit.ap);
                    freePorts[circuit.b].push_back(circuit.bp);
                    plan.changedPorts[circuit.a].insert(circuit.ap);
                    plan.changedPorts[circuit.b].insert(circuit.bp);
                }
            }
        }
        for (auto& [leaf, ports] : freePorts) std::sort(ports.begin(), ports.end());
        std::map<uint32_t, size_t> used;
        for (const auto& [pair, count] : target)
        {
            const auto old = bundles.at(pair).size();
            for (size_t index = old; index < count; ++index)
            {
                auto& a = freePorts[pair.first];
                auto& b = freePorts[pair.second];
                if (used[pair.first] >= a.size() || used[pair.second] >= b.size())
                    throw std::logic_error("ToE port planner exhausted free endpoints");
                plan.added.push_back({pair.first, a[used[pair.first]++],
                                      pair.second, b[used[pair.second]++]});
            }
        }
        for (const auto& [leaf, ports] : freePorts)
            if (used[leaf] != ports.size())
                throw std::logic_error("ToE port planner left unused endpoints");
        return plan;
    }

    /** Both directions of changed direct bundles, plus all potential same-group transits. */
    static bool AffectsEcmpOd(uint32_t src, uint32_t dst,
                             const std::map<uint32_t, uint32_t>& groups,
                             const std::set<Od>& changedPairs)
    {
        if (changedPairs.empty() || src == dst) return false;
        if (!groups.count(src) || !groups.count(dst)) return true;
        if (groups.at(src) == groups.at(dst)) return true;
        return changedPairs.count(std::minmax(src, dst)) != 0;
    }
};

} // namespace ns3
#endif // UB_TOE_RECONFIGURATION_H
