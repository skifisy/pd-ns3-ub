// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TE_SOLVER_H
#define UB_TE_SOLVER_H

#include <cstdint>
#include <cstddef>
#include <map>
#include <utility>

namespace ns3 {

/** In-process two-stage LP solver for Jupiter traffic engineering. */
class UbTeSolver
{
  public:
    using Od = std::pair<uint32_t, uint32_t>;
    using PathWeights = std::map<int32_t, double>; // -1 direct; otherwise transit leaf.

    struct Result
    {
        std::map<Od, PathWeights> policy;
        double minMaxUtilization{0.0};
        std::size_t pathCount{0};
    };

    /** Return true when the unified-bus module was linked with HiGHS. */
    static bool IsAvailable();

    /**
     * Solve the same two-stage LP previously implemented by
     * the original offline reference implementation:
     *   1. minimize maximum directed-edge utilization;
     *   2. preserve that optimum and minimize transit traffic.
     *
     * Capacities and demands are expressed in bits per second.  Path
     * variables are per-OD fractions, which keeps every conservation row
     * normalized to one even for very small demands.
     */
    static Result Solve(const std::map<Od, double>& capacitiesBps,
                        const std::map<Od, double>& demandsBps,
                        double s);
};

} // namespace ns3

#endif // UB_TE_SOLVER_H
