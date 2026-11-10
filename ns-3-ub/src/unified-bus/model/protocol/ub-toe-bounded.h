// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TOE_BOUNDED_H
#define UB_TOE_BOUNDED_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ns3
{

/** Deterministic, bounded search over port-conserving alternating circuits.
 * This is a local postprocessor, not a replacement for the topology MIP. */
class UbToeBounded
{
  public:
    using Od = std::pair<uint32_t, uint32_t>;
    using Topology = std::map<Od, uint32_t>;
    using Move = std::map<Od, int32_t>;
    using Evaluator = std::function<double(const Topology&)>;

    struct Result
    {
        Topology topology;
        std::map<uint32_t, uint32_t> removed;
        uint64_t changedCircuits{0};
        double beforeMlu{0}, plannedMlu{0};
        uint32_t evaluations{0};
        bool searchLimited{false};
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
                         uint32_t budget,
                         const Evaluator& evaluate,
                         uint32_t maxEvaluations = 20000)
    {
        if (current.size() != target.size())
        {
            throw std::invalid_argument("TOE pair set changed");
        }
        std::map<uint32_t, int64_t> balance;
        for (const auto& [pair, count] : current)
        {
            if (pair.first >= pair.second || count < 1 || !target.count(pair) ||
                target.at(pair) < 1)
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
        result.beforeMlu = result.plannedMlu = evaluate(current);
        if (!std::isfinite(result.beforeMlu))
        {
            throw std::runtime_error("invalid current TOE MLU");
        }
        for (;;)
        {
            std::set<Move> moves;
            std::vector<Od> remove;
            for (const auto& [pair, count] : result.topology)
            {
                if (count > target.at(pair))
                {
                    remove.push_back(pair);
                }
            }
            // Enumerate all two-circuit swaps first.
            for (size_t i = 0; i < remove.size(); ++i)
            {
                for (size_t j = i + 1; j < remove.size(); ++j)
                {
                    const auto [a, b] = remove[i];
                    const auto [c, d] = remove[j];
                    if (a == c || a == d || b == c || b == d)
                    {
                        continue;
                    }
                    for (const auto& pairing :
                         {std::make_pair(Od{a, c}, Od{b, d}), std::make_pair(Od{a, d}, Od{b, c})})
                    {
                        const Od x = std::minmax(pairing.first.first, pairing.first.second);
                        const Od y = std::minmax(pairing.second.first, pairing.second.second);
                        if (!target.count(x) || !target.count(y) ||
                            result.topology.at(x) >= target.at(x) ||
                            result.topology.at(y) >= target.at(y))
                        {
                            continue;
                        }
                        moves.insert({{remove[i], -1}, {remove[j], -1}, {x, 1}, {y, 1}});
                    }
                }
            }
            // Also find longer alternating closed trails. A leaf may occur once
            // in each color, allowing odd-cycle pairs sharing a leaf. Edge use
            // is checked against the remaining multiplicity, including repeats.
            uint32_t expansions = 0;
            for (const auto& seed : remove)
            {
                if (expansions >= 100000)
                {
                    result.searchLimited = true;
                    break;
                }
                Move move;
                std::set<std::pair<uint32_t, int>> visited;
                const uint32_t start = seed.first;
                std::function<bool(uint32_t, int)> walk = [&](uint32_t leaf, int sign) {
                    if (++expansions > 100000)
                    {
                        return false;
                    }
                    if (!move.empty() && leaf == start && sign == -1)
                    {
                        if (move.size() > 4)
                        {
                            moves.insert(move);
                        }
                        return true;
                    }
                    if (!visited.insert({leaf, sign}).second)
                    {
                        return false;
                    }
                    for (const auto& [pair, count] : result.topology)
                    {
                        if (pair.first != leaf && pair.second != leaf)
                        {
                            continue;
                        }
                        const int64_t remaining = static_cast<int64_t>(target.at(pair)) - count;
                        const int64_t used = move.count(pair) ? move.at(pair) : 0;
                        if ((sign < 0 && remaining - used >= 0) ||
                            (sign > 0 && remaining - used <= 0))
                        {
                            continue;
                        }
                        if (move.empty() && pair != seed)
                        {
                            continue;
                        }
                        move[pair] += sign;
                        bool withinBudget = true;
                        if (sign < 0)
                        {
                            for (uint32_t p : {pair.first, pair.second})
                            {
                                if (!prefill.count(p))
                                {
                                    continue;
                                }
                                uint32_t removed = result.removed[p];
                                for (const auto& [edge, change] : move)
                                {
                                    if (change < 0 && (edge.first == p || edge.second == p))
                                    {
                                        removed += static_cast<uint32_t>(-change);
                                    }
                                }
                                if (removed > budget)
                                {
                                    withinBudget = false;
                                }
                            }
                        }
                        const uint32_t next = pair.first == leaf ? pair.second : pair.first;
                        if (withinBudget && walk(next, -sign))
                        {
                            return true;
                        }
                        move[pair] -= sign;
                        if (move[pair] == 0)
                        {
                            move.erase(pair);
                        }
                        if (expansions >= 100000)
                        {
                            break;
                        }
                    }
                    visited.erase({leaf, sign});
                    return false;
                };
                (void)walk(start, -1);
            }
            Topology best;
            double bestMlu = std::numeric_limits<double>::infinity();
            uint64_t bestChanges = std::numeric_limits<uint64_t>::max();
            for (const auto& move : moves)
            {
                Topology candidate = result.topology;
                for (const auto& [pair, delta] : move)
                {
                    candidate.at(pair) =
                        static_cast<uint32_t>(static_cast<int64_t>(candidate.at(pair)) + delta);
                }
                const auto removed = Removed(current, candidate);
                bool legal = true;
                for (uint32_t leaf : prefill)
                {
                    if (removed.count(leaf) && removed.at(leaf) > budget)
                    {
                        legal = false;
                    }
                }
                if (!legal)
                {
                    continue;
                }
                if (result.evaluations >= maxEvaluations)
                {
                    result.searchLimited = true;
                    break;
                }
                ++result.evaluations;
                const double mlu = evaluate(candidate);
                // Compare against both the initial and last accepted MLU to
                // avoid accumulating a numerical tolerance over many moves.
                const double tolerance = 1e-12 * std::max(1.0, result.beforeMlu);
                if (!std::isfinite(mlu) || mlu > result.beforeMlu + tolerance ||
                    mlu > result.plannedMlu + tolerance)
                {
                    continue;
                }
                uint64_t changes = 0;
                for (const auto& [leaf, count] : removed)
                {
                    (void)leaf;
                    changes += count;
                }
                changes /= 2;
                if (best.empty() || mlu < bestMlu - tolerance ||
                    (std::abs(mlu - bestMlu) <= tolerance && changes < bestChanges))
                {
                    best = std::move(candidate);
                    bestMlu = mlu;
                    bestChanges = changes;
                }
            }
            if (best.empty())
            {
                break;
            }
            result.topology = std::move(best);
            result.plannedMlu = bestMlu;
            result.changedCircuits = bestChanges;
            result.removed = Removed(current, result.topology);
            if (result.evaluations >= maxEvaluations)
            {
                result.searchLimited = true;
                break;
            }
        }
        return result;
    }
};

} // namespace ns3
#endif // UB_TOE_BOUNDED_H
