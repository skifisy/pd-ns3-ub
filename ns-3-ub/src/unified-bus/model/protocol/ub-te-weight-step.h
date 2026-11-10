// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TE_WEIGHT_STEP_H
#define UB_TE_WEIGHT_STEP_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace ns3
{

/** Pure probability operations shared by TE publication and TOE previews. */
class UbTeWeightStep
{
  public:
    using Od = std::pair<uint32_t, uint32_t>;
    using Weights = std::map<int32_t, double>;
    using Policy = std::map<Od, Weights>;
    using Capacities = std::map<Od, double>;

    static Weights Normalize(Weights weights)
    {
        double sum = 0;
        for (const auto& [path, weight] : weights)
        {
            (void)path;
            if (!std::isfinite(weight) || weight < 0)
            {
                throw std::invalid_argument("invalid WCMP probability");
            }
            sum += weight;
        }
        if (!std::isfinite(sum) || sum <= 0)
        {
            throw std::invalid_argument("empty WCMP probability distribution");
        }
        for (auto& [path, weight] : weights)
        {
            weight /= sum;
        }
        return weights;
    }

    static double Distance(const Weights& a, const Weights& b)
    {
        double distance = 0;
        for (const auto& [path, weight] : a)
        {
            distance += std::abs(weight - (b.count(path) ? b.at(path) : 0));
        }
        for (const auto& [path, weight] : b)
        {
            if (!a.count(path))
            {
                distance += std::abs(weight);
            }
        }
        return distance / 2;
    }

    static Weights Limit(const Weights& old, const Weights& target, double step)
    {
        if (!std::isfinite(step) || step < 0 || step > 1)
        {
            throw std::invalid_argument("WCMP step must be in [0,1]");
        }
        const double distance = Distance(old, target);
        const double alpha = distance <= step ? 1 : step / distance;
        Weights next;
        for (const auto& [path, weight] : old)
        {
            next[path] = (1 - alpha) * weight;
        }
        for (const auto& [path, weight] : target)
        {
            next[path] += alpha * weight;
        }
        return next; // Do not renormalize after limiting.
    }

    static bool Valid(const Od& od, int32_t path, const Capacities& capacities)
    {
        const auto exists = [&](const Od& edge) {
            auto it = capacities.find(edge);
            return it != capacities.end() && it->second > 0;
        };
        if (path == -1)
        {
            return exists(od);
        }
        return path >= 0 && static_cast<uint32_t>(path) != od.first &&
               static_cast<uint32_t>(path) != od.second &&
               exists({od.first, static_cast<uint32_t>(path)}) &&
               exists({static_cast<uint32_t>(path), od.second});
    }

    // Matches UbTeController::Fallback, including capacity-weighted mandatory transit.
    static Weights Fallback(const Od& od, const Capacities& capacities)
    {
        if (Valid(od, -1, capacities))
        {
            return {{-1, 1}};
        }
        Weights weights;
        for (const auto& [edge, capacity] : capacities)
        {
            const int32_t middle = static_cast<int32_t>(edge.second);
            if (edge.first == od.first && Valid(od, middle, capacities))
            {
                weights[middle] = std::min(capacity, capacities.at({edge.second, od.second}));
            }
        }
        return Normalize(std::move(weights));
    }

    static Weights Repair(const Od& od, const Weights& old, const Capacities& capacities)
    {
        Weights valid;
        double sum = 0;
        for (const auto& [path, weight] : old)
        {
            if (Valid(od, path, capacities))
            {
                valid[path] = weight;
                sum += weight;
            }
        }
        if (valid.size() == old.size() && std::abs(sum - 1) <= 1e-12)
        {
            return old;
        }
        return sum > 0 ? Normalize(std::move(valid)) : Fallback(od, capacities);
    }

    static Policy Prepare(const Policy& old,
                          const Policy& target,
                          const Capacities& capacities,
                          double step)
    {
        Policy next;
        // Inactive ODs keep their last actual distribution, subject to reachability.
        for (const auto& [od, weights] : old)
        {
            next[od] = Repair(od, weights, capacities);
        }
        for (const auto& [od, weights] : target)
        {
            const Weights base = next.count(od) ? next.at(od) : Fallback(od, capacities);
            const Weights normalized = Normalize(weights);
            for (const auto& [path, weight] : normalized)
            {
                if (weight > 0 && !Valid(od, path, capacities))
                {
                    throw std::invalid_argument("LP published an unreachable WCMP path");
                }
            }
            next[od] = Limit(base, normalized, step);
        }
        return next;
    }

    static double Mlu(const Policy& policy,
                      const Capacities& capacities,
                      const std::map<Od, double>& demands)
    {
        std::map<Od, double> loads;
        for (const auto& [od, demand] : demands)
        {
            if (demand <= 0 || od.first == od.second)
            {
                continue;
            }
            const Weights weights = policy.count(od) ? policy.at(od) : Fallback(od, capacities);
            for (const auto& [path, weight] : weights)
            {
                if (weight <= 0)
                {
                    continue;
                }
                if (!Valid(od, path, capacities))
                {
                    throw std::invalid_argument("cannot evaluate an unreachable WCMP path");
                }
                if (path == -1)
                {
                    loads[od] += demand * weight;
                }
                else
                {
                    loads[{od.first, static_cast<uint32_t>(path)}] += demand * weight;
                    loads[{static_cast<uint32_t>(path), od.second}] += demand * weight;
                }
            }
        }
        double mlu = 0;
        for (const auto& [edge, load] : loads)
        {
            mlu = std::max(mlu, load / capacities.at(edge));
        }
        return mlu;
    }

    // Same eligible-OD denominator as --te-max-transit-ratio in the LP.
    static double TransitRatio(const Policy& policy,
                               const Capacities& capacities,
                               const std::map<Od, double>& demands)
    {
        double transit = 0, total = 0;
        for (const auto& [od, demand] : demands)
        {
            if (demand <= 0 || !Valid(od, -1, capacities))
            {
                continue;
            }
            bool eligible = false;
            for (const auto& [edge, capacity] : capacities)
            {
                (void)capacity;
                if (edge.first == od.first &&
                    Valid(od, static_cast<int32_t>(edge.second), capacities))
                {
                    eligible = true;
                }
            }
            if (!eligible)
            {
                continue;
            }
            total += demand;
            const Weights weights = policy.count(od) ? policy.at(od) : Fallback(od, capacities);
            for (const auto& [path, weight] : weights)
            {
                if (path != -1)
                {
                    transit += demand * weight;
                }
            }
        }
        return total > 0 ? transit / total : 0;
    }
};

} // namespace ns3
#endif // UB_TE_WEIGHT_STEP_H
