// SPDX-License-Identifier: GPL-2.0-only
#include "ub-toe-solver.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>

#ifdef NS3_UB_HAVE_HIGHS
#include <highs/interfaces/highs_c_api.h>
#endif

namespace ns3 {

#ifdef NS3_UB_HAVE_HIGHS
namespace {

using Od = UbToeSolver::Od;
using Topology = UbToeSolver::Topology;
constexpr double kInf = 1e30;
constexpr double kCircuitCapacityGbps = 400.0;
constexpr uint32_t kPortsPerLeaf = 64;

struct Path
{
    Od od;
    std::vector<Od> edges;
};

struct Model
{
    std::vector<double> cost, lower, upper, rowLower, rowUpper, value;
    std::vector<HighsInt> start, index, integrality;
};

struct BuiltModel
{
    Model model;
    std::vector<Od> pairs;
    std::map<Od, HighsInt> pairColumn;
    double utilization{0};
};

void AddCoefficient(std::map<HighsInt, double>& row, HighsInt col, double value)
{
    row[col] += value;
}

BuiltModel Build(const std::vector<uint32_t>& leaves,
                 const std::map<uint32_t, uint32_t>& groups,
                 const Topology& current,
                 const std::map<Od, double>& demands,
                 UbToeSolver::RoutingMode mode,
                 double utilization,
                 bool minimizeChanges,
                 bool minimizeTransit = false,
                 const Topology* fixedTopology = nullptr)
{
    BuiltModel built;
    built.utilization = utilization;
    auto& model = built.model;
    for (size_t i = 0; i < leaves.size(); ++i)
    {
        for (size_t j = i + 1; j < leaves.size(); ++j)
        {
            if (groups.at(leaves[i]) != groups.at(leaves[j]))
            {
                built.pairs.emplace_back(leaves[i], leaves[j]);
            }
        }
    }

    const HighsInt nPairs = static_cast<HighsInt>(built.pairs.size());
    for (HighsInt i = 0; i < nPairs; ++i)
    {
        built.pairColumn[built.pairs[i]] = i;
    }

    std::vector<Path> paths;
    if (mode == UbToeSolver::RoutingMode::WCMP)
    {
        for (const auto& [od, demand] : demands)
        {
            if (demand <= 0 || od.first == od.second) continue;
            const bool direct = groups.at(od.first) != groups.at(od.second);
            if (direct) paths.push_back({od, {od}});
            for (uint32_t middle : leaves)
            {
                if (middle == od.first || middle == od.second) continue;
                if (groups.at(od.first) == groups.at(middle) ||
                    groups.at(middle) == groups.at(od.second)) continue;
                paths.push_back({od, {{od.first, middle}, {middle, od.second}}});
            }
            if (paths.empty() || paths.back().od != od)
                throw std::runtime_error("ToE has no feasible shortest/one-transit path");
        }
    }

    const HighsInt pathBase = nPairs;
    const HighsInt changeBase = pathBase + static_cast<HighsInt>(paths.size());
    const HighsInt columnCount = changeBase + (minimizeChanges ? 2 * nPairs : 0);
    for (HighsInt col = 0; col < columnCount; ++col)
    {
        model.cost.push_back(0.0);
        model.lower.push_back(0.0);
        model.upper.push_back(kInf);
        model.integrality.push_back(col < nPairs ? kHighsVarTypeInteger : 0);
    }
    for (HighsInt i = 0; i < nPairs; ++i)
    {
        model.lower[i] = 1.0;
        model.upper[i] = static_cast<double>(kPortsPerLeaf - 15);
        if (fixedTopology)
        {
            const auto fixed = fixedTopology->find(built.pairs[i]);
            if (fixed == fixedTopology->end())
                throw std::runtime_error("fixed ToE topology is missing a cross-group pair");
            model.lower[i] = model.upper[i] = fixed->second;
        }
    }
    if (minimizeChanges)
    {
        for (HighsInt i = 0; i < 2 * nPairs; ++i)
        {
            model.cost[changeBase + i] = 1.0;
        }
    }
    for (size_t i = 0; i < paths.size(); ++i)
    {
        model.upper[pathBase + static_cast<HighsInt>(i)] =
            demands.at(paths[i].od) / 1e9;
        if (minimizeTransit && paths[i].edges.size() > 1)
            model.cost[pathBase + static_cast<HighsInt>(i)] = 1.0;
    }

    std::vector<std::map<HighsInt, double>> rows;
    auto addRow = [&](std::map<HighsInt, double> row, double lo, double hi) {
        rows.push_back(std::move(row));
        model.rowLower.push_back(lo);
        model.rowUpper.push_back(hi);
    };

    // Each leaf consumes all 64 OCS ports.
    for (uint32_t leaf : leaves)
    {
        std::map<HighsInt, double> row;
        for (const auto& pair : built.pairs)
            if (pair.first == leaf || pair.second == leaf)
                AddCoefficient(row, built.pairColumn.at(pair), 1.0);
        addRow(std::move(row), kPortsPerLeaf, kPortsPerLeaf);
    }

    // Each OD sends all of its predicted demand on the selected candidate paths.
    if (mode == UbToeSolver::RoutingMode::WCMP)
    {
        for (const auto& [od, demand] : demands)
        {
            if (demand <= 0 || od.first == od.second) continue;
            std::map<HighsInt, double> row;
            for (size_t i = 0; i < paths.size(); ++i)
                if (paths[i].od == od) AddCoefficient(row, pathBase + i, 1.0);
            addRow(std::move(row), demand / 1e9, demand / 1e9);
        }
    }

    // Directed link capacity constraints.
    for (const auto& pair : built.pairs)
    {
        for (const Od direction : {pair, Od{pair.second, pair.first}})
        {
            std::map<HighsInt, double> row;
            double fixedLoad = 0.0;
            if (mode == UbToeSolver::RoutingMode::WCMP)
            {
                for (size_t i = 0; i < paths.size(); ++i)
                    if (std::find(paths[i].edges.begin(), paths[i].edges.end(), direction) !=
                        paths[i].edges.end())
                        AddCoefficient(row, pathBase + i, 1.0);
            }
            else
            {
                for (const auto& [od, demand] : demands)
                {
                    if (demand <= 0 || od.first == od.second) continue;
                    if (groups.at(od.first) != groups.at(od.second))
                    {
                        if (od == direction) fixedLoad += demand / 1e9;
                        continue;
                    }
                    // ECMP hashes uniformly over the 64 physical first-hop ports.
                    // The fraction using each transit leaf is n(i,k)/64.
                    if (direction.first == od.first && direction.second != od.second &&
                        groups.at(direction.second) != groups.at(od.first))
                    {
                        const Od pairIk = std::minmax(od.first, direction.second);
                        if (built.pairColumn.count(pairIk))
                            AddCoefficient(row, built.pairColumn.at(pairIk), demand / 64.0 / 1e9);
                    }
                    if (direction.second == od.second && direction.first != od.first &&
                        groups.at(direction.first) != groups.at(od.second))
                    {
                        const Od pairIk = std::minmax(od.first, direction.first);
                        if (built.pairColumn.count(pairIk))
                            AddCoefficient(row, built.pairColumn.at(pairIk), demand / 64.0 / 1e9);
                    }
                }
            }
            AddCoefficient(row, built.pairColumn.at(pair), -utilization * kCircuitCapacityGbps);
            addRow(std::move(row), -kInf, -fixedLoad);
        }
    }

    if (minimizeChanges)
    {
        for (HighsInt i = 0; i < nPairs; ++i)
        {
            std::map<HighsInt, double> row{{i, 1.0}, {changeBase + i, -1.0},
                                           {changeBase + nPairs + i, 1.0}};
            const auto it = current.find(built.pairs[i]);
            const double old = it == current.end() ? 0.0 : it->second;
            addRow(std::move(row), old, old);
        }
    }

    model.start.reserve(rows.size() + 1);
    for (const auto& row : rows)
    {
        model.start.push_back(static_cast<HighsInt>(model.index.size()));
        for (const auto& [col, coefficient] : row)
        {
            if (std::abs(coefficient) > 1e-15)
            {
                model.index.push_back(col);
                model.value.push_back(coefficient);
            }
        }
    }
    model.start.push_back(static_cast<HighsInt>(model.index.size()));
    return built;
}

struct Solution
{
    bool feasible{false};
    bool optimal{false};
    std::vector<double> columns;
};

Solution Run(const Model& model, double timeLimitSeconds)
{
    using Handle = std::unique_ptr<void, decltype(&Highs_destroy)>;
    Handle highs(Highs_create(), &Highs_destroy);
    if (!highs) throw std::runtime_error("HiGHS allocation failed in ToE");
    Highs_setBoolOptionValue(highs.get(), "output_flag", 0);
    Highs_setIntOptionValue(highs.get(), "threads", 1);
    Highs_setDoubleOptionValue(highs.get(), "time_limit", timeLimitSeconds);
    const HighsInt ncol = model.cost.size();
    const HighsInt nrow = model.rowLower.size();
    const HighsInt status = Highs_passMip(highs.get(), ncol, nrow,
                                          static_cast<HighsInt>(model.value.size()),
                                          kHighsMatrixFormatRowwise, kHighsObjSenseMinimize, 0.0,
                                          model.cost.data(), model.lower.data(), model.upper.data(),
                                          model.rowLower.data(), model.rowUpper.data(),
                                          model.start.data(), model.index.data(),
                                          model.value.data(),
                                          model.integrality.data());
    if (status == kHighsStatusError || Highs_run(highs.get()) == kHighsStatusError)
        throw std::runtime_error("HiGHS execution failed in ToE");
    const HighsInt modelStatus = Highs_getModelStatus(highs.get());
    if (modelStatus == kHighsModelStatusInfeasible) return {};
    if (modelStatus == kHighsModelStatusTimeLimit)
    {
        HighsInt solutionStatus = kHighsSolutionStatusNone;
        if (Highs_getIntInfoValue(highs.get(), "primal_solution_status", &solutionStatus) !=
                kHighsStatusError && solutionStatus == kHighsSolutionStatusFeasible)
        {
            Solution solution;
            solution.columns.resize(ncol);
            std::vector<double> colDual(ncol, 0.0), rowValue(nrow, 0.0), rowDual(nrow, 0.0);
            if (Highs_getSolution(highs.get(), solution.columns.data(), colDual.data(),
                                  rowValue.data(), rowDual.data()) == kHighsStatusError)
                throw std::runtime_error("HiGHS incumbent retrieval failed in ToE");
            solution.feasible = true;
            return solution;
        }
        throw std::runtime_error("HiGHS ToE time limit reached without a feasible incumbent");
    }
    if (modelStatus != kHighsModelStatusOptimal)
        throw std::runtime_error("HiGHS returned unsupported ToE status " +
                                 std::to_string(modelStatus));
    Solution solution;
    solution.columns.resize(ncol);
    std::vector<double> colDual(ncol, 0.0), rowValue(nrow, 0.0), rowDual(nrow, 0.0);
    if (Highs_getSolution(highs.get(), solution.columns.data(), colDual.data(), rowValue.data(),
                          rowDual.data()) == kHighsStatusError)
        throw std::runtime_error("HiGHS solution retrieval failed in ToE");
    solution.feasible = true;
    solution.optimal = modelStatus == kHighsModelStatusOptimal;
    return solution;
}

} // namespace
#endif

bool UbToeSolver::IsAvailable()
{
#ifdef NS3_UB_HAVE_HIGHS
    return true;
#else
    return false;
#endif
}

UbToeSolver::Result
UbToeSolver::Solve(const std::vector<uint32_t>& leaves,
                   const std::map<uint32_t, uint32_t>& groups,
                   const Topology& current,
                   const std::map<Od, double>& demandsBps,
                   RoutingMode mode,
                   double relativeMluSlack,
                   double timeLimitSeconds)
{
#ifndef NS3_UB_HAVE_HIGHS
    (void)leaves; (void)groups; (void)current; (void)demandsBps; (void)mode;
    (void)relativeMluSlack; (void)timeLimitSeconds;
    throw std::runtime_error("ToE requires the HiGHS C++ library");
#else
    if (leaves.empty() || !std::isfinite(relativeMluSlack) || relativeMluSlack < 0 ||
        relativeMluSlack > 1 ||
        !std::isfinite(timeLimitSeconds) || timeLimitSeconds <= 0)
        throw std::invalid_argument("invalid ToE solver parameters");
    for (uint32_t leaf : leaves)
        if (!groups.count(leaf)) throw std::invalid_argument("missing ToE leaf group");
    for (const auto& [od, demand] : demandsBps)
        if (!std::isfinite(demand) || demand < 0 || !groups.count(od.first) ||
            !groups.count(od.second)) throw std::invalid_argument("invalid ToE demand");

    // Bound the MLU by placing all offered traffic on a single 400 Gbps
    // circuit. Every feasible route uses any directed circuit at most once,
    // so this is a feasible upper bound independent of the number of ODs.
    // A count-based bound (for example, 2 * number of ODs) is far too coarse
    // for short traces with low offered load and can make the change-minimizer
    // incorrectly retain the current topology.
    double totalDemandGbps = 0.0;
    for (const auto& [od, demand] : demandsBps)
    {
        (void)od;
        totalDemandGbps += demand / 1e9;
    }
    if (totalDemandGbps == 0.0)
    {
        Result result;
        result.topology = current;
        result.optimal = true;
        return result;
    }
    double high = 1.1 * totalDemandGbps / kCircuitCapacityGbps;
    double low = 0.0;
    constexpr uint32_t kMluBisectionIterations = 20;
    const double perRunLimit = std::max(0.25, timeLimitSeconds / 24.0);
    BuiltModel best;
    Solution bestSolution;
    for (uint32_t expand = 0; expand < 10; ++expand)
    {
        auto trial = Build(leaves, groups, current, demandsBps, mode, high, false);
        auto result = Run(trial.model, perRunLimit);
        if (result.feasible) { best = std::move(trial); bestSolution = std::move(result); break; }
        high *= 2.0;
    }
    if (!bestSolution.feasible) throw std::runtime_error("ToE found no feasible topology");
    for (uint32_t iter = 0; iter < kMluBisectionIterations; ++iter)
    {
        const double mid = (low + high) / 2.0;
        auto trial = Build(leaves, groups, current, demandsBps, mode, mid, false);
        auto result = Run(trial.model, perRunLimit);
        if (result.feasible)
        {
            high = mid;
            best = std::move(trial);
            bestSolution = std::move(result);
        }
        else low = mid;
    }

    const double relaxedBound = high * (1.0 + relativeMluSlack + 1e-8);
    auto changeModel = Build(leaves, groups, current, demandsBps, mode, relaxedBound, true);
    auto changed = Run(changeModel.model, perRunLimit);
    if (!changed.feasible)
        throw std::runtime_error("ToE change-minimization model failed within MLU slack");

    Result result;
    result.minMaxUtilization = high;
    result.appliedUtilizationBound = relaxedBound;
    result.optimal = bestSolution.optimal && changed.optimal;
    for (size_t i = 0; i < changeModel.pairs.size(); ++i)
    {
        const uint32_t count = static_cast<uint32_t>(std::llround(changed.columns[i]));
        result.topology[changeModel.pairs[i]] = count;
        const auto old = current.find(changeModel.pairs[i]);
        if (old != current.end()) result.changedCircuits += old->second > count
                                                                  ? old->second - count
                                                                  : count - old->second;
        else result.changedCircuits += count;
    }
    result.changedCircuits /= 2;
    if (mode == RoutingMode::WCMP)
    {
        const auto transitModel = Build(leaves, groups, current, demandsBps, mode,
                                        relaxedBound, false, true, &result.topology);
        const auto transit = Run(transitModel.model, perRunLimit);
        if (!transit.feasible)
            throw std::runtime_error("ToE transit-minimization model failed");
        result.optimal = result.optimal && transit.optimal;
    }
    return result;
#endif
}

} // namespace ns3
