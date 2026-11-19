// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TOE_BOUNDED_H
#define UB_TOE_BOUNDED_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace ns3
{

/** LP-guided integer D-S allocation using port-conserving two-circuit swaps.
 * This postprocessor never calls a solver or a topology evaluator. */
class UbToeBounded
{
  public:
    using Od = std::pair<uint32_t, uint32_t>;
    using Topology = std::map<Od, uint32_t>;
    struct Allocation
    {
        Od ds, pd, ps, pp;
        uint32_t before, target;
    };

    struct Result
    {
        Topology topology;
        std::map<uint32_t, uint32_t> removed;
        uint64_t changedCircuits{0};
        std::vector<Allocation> allocations;
    };

    static std::map<uint32_t, uint32_t> Removed(const Topology& old, const Topology& next)
    {
        std::map<uint32_t, uint32_t> removed;
        for (const auto& [pair, count] : old)
        {
            if (count > next.at(pair))
            {
                removed[pair.first] += count - next.at(pair);
                removed[pair.second] += count - next.at(pair);
            }
        }
        return removed;
    }

    // Budgets apply to each undirected pair incident to a Prefill leaf.
    // Leaf totals remain useful audit data, but are not removal limits.
    static Topology RemovedPairs(const Topology& old, const Topology& next)
    {
        Topology removed;
        for (const auto& [pair, count] : old)
        {
            if (count > next.at(pair))
            {
                removed[pair] = count - next.at(pair);
            }
        }
        return removed;
    }

    static bool WithinBudget(const Topology& old,
                             const Topology& next,
                             const std::set<uint32_t>& prefill,
                             uint32_t budget)
    {
        for (const auto& [pair, removed] : RemovedPairs(old, next))
        {
            if ((prefill.count(pair.first) || prefill.count(pair.second)) && removed > budget)
            {
                return false;
            }
        }
        return true;
    }

    static double EcmpMlu(const Topology& topology,
                          const std::map<uint32_t, uint32_t>& groups,
                          const std::map<Od, double>& demands)
    {
        std::map<Od, double> loads;
        std::map<uint32_t, uint32_t> degree;
        for (const auto& [pair, count] : topology)
        {
            degree[pair.first] += count;
            degree[pair.second] += count;
        }
        for (const auto& [od, demand] : demands)
        {
            if (demand <= 0 || od.first == od.second)
            {
                continue;
            }
            if (groups.at(od.first) != groups.at(od.second))
            {
                loads[od] += demand;
            }
            else
            {
                for (const auto& [middle, group] : groups)
                {
                    if (group == groups.at(od.first))
                    {
                        continue;
                    }
                    const double fraction =
                        static_cast<double>(topology.at(std::minmax(od.first, middle))) /
                        degree.at(od.first);
                    loads[{od.first, middle}] += demand * fraction;
                    loads[{middle, od.second}] += demand * fraction;
                }
            }
        }
        double mlu = 0;
        for (const auto& [edge, load] : loads)
        {
            mlu = std::max(mlu, load / (400e9 * topology.at(std::minmax(edge.first, edge.second))));
        }
        return mlu;
    }

    static Result Select(const Topology& current,
                         const Topology& target,
                         const std::set<uint32_t>& prefill,
                         const std::set<uint32_t>& decode,
                         const std::set<uint32_t>& storage,
                         uint32_t budget)
    {
        if (current.size() != target.size())
        {
            throw std::invalid_argument("TOE pair set changed");
        }
        std::set<uint32_t> roles = prefill;
        for (const auto* role : {&decode, &storage})
        {
            for (uint32_t leaf : *role)
            {
                if (!roles.insert(leaf).second)
                {
                    throw std::invalid_argument("overlapping TOE leaf roles");
                }
            }
        }
        std::map<uint32_t, int64_t> balance;
        for (const auto& [pair, count] : current)
        {
            if (pair.first >= pair.second || count < 1 || !target.count(pair) ||
                target.at(pair) < 1 || !roles.count(pair.first) || !roles.count(pair.second))
            {
                throw std::invalid_argument("invalid bounded TOE topology");
            }
            const int64_t delta = static_cast<int64_t>(target.at(pair)) - count;
            balance[pair.first] += delta;
            balance[pair.second] += delta;
        }
        for (const auto& [leaf, delta] : balance)
        {
            (void)leaf;
            if (delta != 0)
            {
                throw std::invalid_argument("TOE target changes leaf degree");
            }
        }
        Result result;
        result.topology = current;
        Topology remaining;
        for (uint32_t p : prefill)
        {
            for (uint32_t d : decode)
            {
                const Od pair = std::minmax(p, d);
                if (current.count(pair))
                {
                    // P-D donors are independent of the LP target.
                    remaining[pair] = std::min(budget, current.at(pair) - 1);
                }
            }
            for (uint32_t s : storage)
            {
                const Od pair = std::minmax(p, s);
                if (current.count(pair))
                {
                    // Only P-S reductions requested by the LP may donate.
                    const uint32_t reduction = current.at(pair) > target.at(pair)
                                                   ? current.at(pair) - target.at(pair) : 0;
                    remaining[pair] = std::min(budget, reduction);
                }
            }
        }
        for (;;)
        {
            std::vector<Od> candidates;
            for (uint32_t d : decode)
            {
                for (uint32_t s : storage)
                {
                    const Od pair = std::minmax(d, s);
                    if (current.count(pair) && result.topology.at(pair) < target.at(pair))
                    {
                        candidates.push_back(pair);
                    }
                }
            }
            // Compare target/current ratios exactly, without floating point.
            std::sort(candidates.begin(), candidates.end(), [&](const Od& a, const Od& b) {
                const uint64_t left = uint64_t{target.at(a)} * result.topology.at(b);
                const uint64_t right = uint64_t{target.at(b)} * result.topology.at(a);
                if (left != right) return left > right;
                const uint32_t gapA = target.at(a) - result.topology.at(a);
                const uint32_t gapB = target.at(b) - result.topology.at(b);
                if (gapA != gapB) return gapA > gapB;
                return a < b;
            });
            bool accepted = false;
            for (const Od& ds : candidates)
            {
                const uint32_t d = decode.count(ds.first) ? ds.first : ds.second;
                const uint32_t s = storage.count(ds.first) ? ds.first : ds.second;
                bool found = false;
                Od bestPd, bestPs, bestPp;
                std::tuple<uint32_t, uint32_t, Od, Od> bestScore;
                for (uint32_t pa : prefill)
                {
                    const Od pd = std::minmax(pa, d);
                    if (!remaining.count(pd) || remaining.at(pd) == 0) continue;
                    for (uint32_t pb : prefill)
                    {
                        const Od ps = std::minmax(pb, s);
                        const Od pp = std::minmax(pa, pb);
                        if (!remaining.count(ps) || remaining.at(ps) == 0 ||
                            !current.count(pp)) continue; // No self/intra-group P-P link.
                        // Spread balancing links; fixed IDs make ties reproducible.
                        const auto score = std::make_tuple(result.topology.at(pp),
                            current.at(pd) - result.topology.at(pd) +
                            current.at(ps) - result.topology.at(ps), pd, ps);
                        if (!found || score < bestScore)
                        {
                            found = true;
                            bestScore = score;
                            bestPd = pd;
                            bestPs = ps;
                            bestPp = pp;
                        }
                    }
                }
                if (!found) continue; // Try the next feasible D-S pair.
                result.allocations.push_back({ds, bestPd, bestPs, bestPp,
                                              result.topology.at(ds), target.at(ds)});
                --result.topology.at(bestPd);
                --result.topology.at(bestPs);
                ++result.topology.at(ds);
                ++result.topology.at(bestPp);
                --remaining.at(bestPd);
                --remaining.at(bestPs);
                result.changedCircuits += 2;
                accepted = true;
                break; // Re-rank after every allocated circuit.
            }
            if (!accepted) break;
        }
        result.removed = Removed(current, result.topology);
        return result;
    }
};

} // namespace ns3
#endif // UB_TOE_BOUNDED_H
