// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TOE_SOLVER_H
#define UB_TOE_SOLVER_H

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ns3 {

/** Integer topology optimization for the five-group Mooncake OCS. */
class UbToeSolver
{
  public:
    using Od = std::pair<uint32_t, uint32_t>;
    using Topology = std::map<Od, uint32_t>; // one canonical (i < j) entry per circuit bundle

    enum class RoutingMode
    {
        ECMP,
        WCMP,
    };

    struct Result
    {
        Topology topology;
        double minMaxUtilization{0.0};
        double appliedUtilizationBound{0.0};
        uint64_t changedCircuits{0};
        bool optimal{false};
    };

    static bool IsAvailable();

    /**
     * Solve a fixed-U integer feasibility model and then minimize circuit
     * changes within the requested relative MLU slack. Demands and capacity
     * are in bits/s. Each leaf has 64 ports and every cross-group pair has at
     * least one circuit.
     */
    static Result Solve(const std::vector<uint32_t>& leaves,
                        const std::map<uint32_t, uint32_t>& groups,
                        const Topology& current,
                        const std::map<Od, double>& demandsBps,
                        RoutingMode mode,
                        double relativeMluSlack = 0.01,
                        double timeLimitSeconds = 30.0);
};

} // namespace ns3

#endif // UB_TOE_SOLVER_H
