// SPDX-License-Identifier: GPL-2.0-only
#include "ub-te-solver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef NS3_UB_HAVE_HIGHS
#include <highs/interfaces/highs_c_api.h>
#endif

namespace ns3 {

#ifdef NS3_UB_HAVE_HIGHS
namespace {

using Od = UbTeSolver::Od;

constexpr double kInfinity = 1e30;
constexpr double kNegativeWeightTolerance = 1e-7;
constexpr double kWeightSumTolerance = 1e-12;

struct CandidatePath
{
    Od od;
    int32_t transit{-1};
    std::array<Od, 2> edges{};
    uint8_t edgeCount{0};
    double demandGbps{0.0};
    double upperBound{kInfinity};
};

struct SparseModel
{
    std::vector<double> cost;
    std::vector<double> colLower;
    std::vector<double> colUpper;
    std::vector<double> rowLower;
    std::vector<double> rowUpper;
    std::vector<HighsInt> start;
    std::vector<HighsInt> index;
    std::vector<double> value;
};

std::string
OdString(const Od& od)
{
    return std::to_string(od.first) + "->" + std::to_string(od.second);
}

std::vector<CandidatePath>
BuildCandidates(const std::map<Od, double>& capacitiesBps,
                const std::map<Od, double>& demandsBps,
                double s)
{
    std::set<uint32_t> leaves;
    for (const auto& [edge, capacity] : capacitiesBps)
    {
        if (!std::isfinite(capacity) || capacity <= 0.0)
        {
            throw std::runtime_error("invalid capacity for edge " + OdString(edge));
        }
        leaves.insert(edge.first);
        leaves.insert(edge.second);
    }

    std::vector<CandidatePath> paths;
    for (const auto& [od, demandBps] : demandsBps)
    {
        if (!std::isfinite(demandBps) || demandBps < 0.0)
        {
            throw std::runtime_error("invalid demand for OD " + OdString(od));
        }
        if (demandBps == 0.0 || od.first == od.second)
        {
            continue;
        }

        struct RawPath
        {
            int32_t transit;
            std::array<Od, 2> edges;
            uint8_t edgeCount;
            double capacityBps;
        };
        std::vector<RawPath> available;
        const auto direct = capacitiesBps.find(od);
        if (direct != capacitiesBps.end())
        {
            available.push_back({-1, {od, {}}, 1, direct->second});
        }
        for (uint32_t middle : leaves)
        {
            if (middle == od.first || middle == od.second)
            {
                continue;
            }
            const Od first{od.first, middle};
            const Od second{middle, od.second};
            const auto firstCapacity = capacitiesBps.find(first);
            const auto secondCapacity = capacitiesBps.find(second);
            if (firstCapacity == capacitiesBps.end() || secondCapacity == capacitiesBps.end())
            {
                continue;
            }
            available.push_back({static_cast<int32_t>(middle),
                                 {first, second},
                                 2,
                                 std::min(firstCapacity->second, secondCapacity->second)});
        }
        if (available.empty())
        {
            throw std::runtime_error("no direct or single-transit route for OD " +
                                     OdString(od));
        }

        double totalPathCapacity = 0.0;
        for (const auto& path : available)
        {
            totalPathCapacity += path.capacityBps;
        }
        for (const auto& path : available)
        {
            const double upper = s == 0.0
                                     ? kInfinity
                                     : path.capacityBps / (s * totalPathCapacity);
            paths.push_back({od,
                             path.transit,
                             path.edges,
                             path.edgeCount,
                             demandBps / 1e9,
                             upper});
        }
    }
    return paths;
}

SparseModel
BuildModel(const std::map<Od, double>& capacitiesBps,
           const std::vector<CandidatePath>& paths,
           bool minimizeTransit,
           double utilizationUpperBound)
{
    std::map<Od, HighsInt> odRows;
    for (const auto& path : paths)
    {
        odRows.try_emplace(path.od, static_cast<HighsInt>(odRows.size()));
    }
    std::map<Od, HighsInt> edgeRows;
    for (const auto& [edge, capacity] : capacitiesBps)
    {
        (void)capacity;
        edgeRows.emplace(edge, static_cast<HighsInt>(odRows.size() + edgeRows.size()));
    }

    const HighsInt pathCount = static_cast<HighsInt>(paths.size());
    const HighsInt columnCount = pathCount + 1; // Last column is U.
    const HighsInt rowCount = static_cast<HighsInt>(odRows.size() + edgeRows.size());
    std::vector<std::vector<std::pair<HighsInt, double>>> rows(rowCount);

    SparseModel model;
    model.cost.assign(columnCount, 0.0);
    model.colLower.assign(columnCount, 0.0);
    model.colUpper.assign(columnCount, kInfinity);
    model.rowLower.assign(rowCount, -kInfinity);
    model.rowUpper.assign(rowCount, 0.0);

    for (const auto& [od, row] : odRows)
    {
        (void)od;
        model.rowLower[row] = 1.0;
        model.rowUpper[row] = 1.0;
    }
    for (HighsInt column = 0; column < pathCount; ++column)
    {
        const auto& path = paths[column];
        model.colUpper[column] = path.upperBound;
        model.cost[column] = minimizeTransit && path.transit >= 0 ? path.demandGbps : 0.0;
        rows[odRows.at(path.od)].push_back({column, 1.0});
        for (uint8_t edgeIndex = 0; edgeIndex < path.edgeCount; ++edgeIndex)
        {
            rows[edgeRows.at(path.edges[edgeIndex])].push_back({column, path.demandGbps});
        }
    }
    for (const auto& [edge, row] : edgeRows)
    {
        rows[row].push_back({pathCount, -capacitiesBps.at(edge) / 1e9});
    }
    if (minimizeTransit)
    {
        model.colUpper[pathCount] = utilizationUpperBound;
    }
    else
    {
        model.cost[pathCount] = 1.0;
    }

    model.start.reserve(static_cast<std::size_t>(rowCount) + 1);
    for (const auto& row : rows)
    {
        model.start.push_back(static_cast<HighsInt>(model.index.size()));
        for (const auto& [column, coefficient] : row)
        {
            model.index.push_back(column);
            model.value.push_back(coefficient);
        }
    }
    model.start.push_back(static_cast<HighsInt>(model.index.size()));
    return model;
}

std::vector<double>
SolveModel(const SparseModel& model, const std::string& stage)
{
    using HighsHandle = std::unique_ptr<void, decltype(&Highs_destroy)>;
    HighsHandle highs(Highs_create(), &Highs_destroy);
    if (!highs)
    {
        throw std::runtime_error("HiGHS allocation failed during " + stage);
    }
    if (Highs_setBoolOptionValue(highs.get(), "output_flag", 0) == kHighsStatusError ||
        Highs_setIntOptionValue(highs.get(), "threads", 1) == kHighsStatusError)
    {
        throw std::runtime_error("HiGHS option setup failed during " + stage);
    }
    const HighsInt columnCount = static_cast<HighsInt>(model.cost.size());
    const HighsInt rowCount = static_cast<HighsInt>(model.rowLower.size());
    const HighsInt nonzeroCount = static_cast<HighsInt>(model.value.size());
    const HighsInt passStatus = Highs_passLp(highs.get(),
                                             columnCount,
                                             rowCount,
                                             nonzeroCount,
                                             kHighsMatrixFormatRowwise,
                                             kHighsObjSenseMinimize,
                                             0.0,
                                             model.cost.data(),
                                             model.colLower.data(),
                                             model.colUpper.data(),
                                             model.rowLower.data(),
                                             model.rowUpper.data(),
                                             model.start.data(),
                                             model.index.data(),
                                             model.value.data());
    if (passStatus == kHighsStatusError || Highs_run(highs.get()) == kHighsStatusError)
    {
        throw std::runtime_error("HiGHS execution failed during " + stage);
    }
    const HighsInt modelStatus = Highs_getModelStatus(highs.get());
    if (modelStatus != kHighsModelStatusOptimal)
    {
        throw std::runtime_error("HiGHS did not find an optimal " + stage +
                                 " solution; model_status=" + std::to_string(modelStatus));
    }
    std::vector<double> columns(columnCount, 0.0);
    std::vector<double> columnDual(columnCount, 0.0);
    std::vector<double> rowValue(rowCount, 0.0);
    std::vector<double> rowDual(rowCount, 0.0);
    if (Highs_getSolution(highs.get(),
                          columns.data(),
                          columnDual.data(),
                          rowValue.data(),
                          rowDual.data()) == kHighsStatusError)
    {
        throw std::runtime_error("HiGHS solution retrieval failed during " + stage);
    }
    return columns;
}

} // namespace
#endif // NS3_UB_HAVE_HIGHS

bool
UbTeSolver::IsAvailable()
{
#ifdef NS3_UB_HAVE_HIGHS
    return true;
#else
    return false;
#endif
}

UbTeSolver::Result
UbTeSolver::Solve(const std::map<Od, double>& capacitiesBps,
                  const std::map<Od, double>& demandsBps,
                  double s)
{
#ifndef NS3_UB_HAVE_HIGHS
    (void)capacitiesBps;
    (void)demandsBps;
    (void)s;
    throw std::runtime_error(
        "Jupiter TE requires the HiGHS C++ library; reconfigure ns-3 with highs_DIR");
#else
    if (!std::isfinite(s) || s < 0.0 || s > 1.0)
    {
        throw std::invalid_argument("Jupiter S must be in [0,1]");
    }
    const std::vector<CandidatePath> paths = BuildCandidates(capacitiesBps, demandsBps, s);
    if (paths.empty())
    {
        return {};
    }

    const SparseModel firstModel = BuildModel(capacitiesBps, paths, false, kInfinity);
    const std::vector<double> first = SolveModel(firstModel, "min-utilization stage");
    const double minimumUtilization = first.back();
    if (!std::isfinite(minimumUtilization) || minimumUtilization < 0.0)
    {
        throw std::runtime_error("HiGHS returned an invalid minimum utilization");
    }
    const double utilizationCap =
        minimumUtilization + std::max(1e-9, minimumUtilization * 1e-6);
    const SparseModel secondModel = BuildModel(capacitiesBps, paths, true, utilizationCap);
    const std::vector<double> second = SolveModel(secondModel, "minimum-transit stage");

    Result result;
    result.minMaxUtilization = minimumUtilization;
    result.pathCount = paths.size();
    std::map<Od, double> totals;
    for (std::size_t index = 0; index < paths.size(); ++index)
    {
        double weight = second[index];
        if (!std::isfinite(weight) || weight < -kNegativeWeightTolerance)
        {
            throw std::runtime_error("HiGHS returned an invalid path weight for OD " +
                                     OdString(paths[index].od));
        }
        weight = std::max(0.0, weight);
        result.policy[paths[index].od][paths[index].transit] = weight;
        totals[paths[index].od] += weight;
    }
    for (auto& [od, weights] : result.policy)
    {
        const double total = totals.at(od);
        if (!std::isfinite(total) || total <= kWeightSumTolerance)
        {
            throw std::runtime_error("HiGHS returned zero total path weight for OD " +
                                     OdString(od));
        }
        for (auto& [transit, weight] : weights)
        {
            (void)transit;
            weight /= total;
        }
    }
    return result;
#endif
}

} // namespace ns3
