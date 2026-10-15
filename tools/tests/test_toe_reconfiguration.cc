// SPDX-License-Identifier: GPL-2.0-only
// Standalone regression: no ns-3 build or HiGHS installation is required.
#include "../../ns-3-ub/src/unified-bus/model/protocol/ub-toe-reconfiguration.h"

#include <cassert>
#include <iostream>
#include <random>

using Planner = ns3::UbToeReconfiguration;
using Topology = ns3::UbToeSolver::Topology;

static std::vector<Planner::Circuit> InitialCircuits()
{
    std::vector<Planner::Circuit> circuits;
    std::map<uint32_t, uint16_t> next;
    for (uint32_t a = 37; a <= 56; ++a)
        for (uint32_t b = a + 1; b <= 56; ++b)
            if ((a - 37) / 4 != (b - 37) / 4)
                for (unsigned index = 0; index < 4; ++index)
                    circuits.push_back({a, next[a]++, b, next[b]++});
    return circuits;
}

static Topology Counts(const std::vector<Planner::Circuit>& circuits)
{
    Topology result;
    std::set<Planner::Endpoint> endpoints;
    std::map<uint32_t, unsigned> degree;
    for (const auto& circuit : circuits)
    {
        assert(circuit.a < circuit.b);
        assert(endpoints.insert({circuit.a, circuit.ap}).second);
        assert(endpoints.insert({circuit.b, circuit.bp}).second);
        ++result[{circuit.a, circuit.b}];
        ++degree[circuit.a];
        ++degree[circuit.b];
    }
    assert(circuits.size() == 640 && endpoints.size() == 1280 && result.size() == 160);
    for (const auto& [leaf, count] : degree) assert(count == 64);
    return result;
}

static void CheckPlan(const std::vector<Planner::Circuit>& current, const Topology& target)
{
    const auto plan = Planner::Build(current, target);
    const auto old = Counts(current);
    size_t retained = 0, removed = 0;
    std::set<Planner::Endpoint> freed, reused;
    for (const auto& [pair, count] : old)
    {
        retained += std::min(count, target.at(pair));
        removed += count > target.at(pair) ? count - target.at(pair) : 0;
    }
    assert(plan.retained.size() == retained && plan.removed.size() == removed);
    assert(plan.removed.size() == plan.added.size());
    for (const auto& circuit : plan.retained)
        assert(std::find(current.begin(), current.end(), circuit) != current.end());
    for (const auto& circuit : plan.removed)
    {
        freed.insert({circuit.a, circuit.ap});
        freed.insert({circuit.b, circuit.bp});
    }
    for (const auto& circuit : plan.added)
    {
        assert(reused.insert({circuit.a, circuit.ap}).second);
        assert(reused.insert({circuit.b, circuit.bp}).second);
    }
    assert(freed == reused);
    std::vector<Planner::Circuit> actual = plan.retained;
    actual.insert(actual.end(), plan.added.begin(), plan.added.end());
    assert(Counts(actual) == target);
}

int main()
{
    auto circuits = InitialCircuits();
    const auto original = Counts(circuits);
    auto noOp = Planner::Build(circuits, original);
    assert(noOp.retained.size() == 640 && noOp.added.empty() && noOp.changedPairs.empty());
    auto target = original;
    --target[{37, 41}];
    --target[{38, 42}];
    ++target[{37, 42}];
    ++target[{38, 41}];
    CheckPlan(circuits, target);
    const auto plan = Planner::Build(circuits, target);
    assert(plan.retained.size() == 638 && plan.added.size() == 2);
    std::map<uint32_t, uint32_t> groups;
    for (uint32_t leaf = 37; leaf <= 56; ++leaf) groups[leaf] = (leaf - 37) / 4;
    assert(Planner::AffectsEcmpOd(37, 41, groups, plan.changedPairs));
    assert(Planner::AffectsEcmpOd(41, 37, groups, plan.changedPairs));
    assert(Planner::AffectsEcmpOd(37, 42, groups, plan.changedPairs));
    assert(!Planner::AffectsEcmpOd(37, 45, groups, plan.changedPairs));
    assert(!Planner::AffectsEcmpOd(49, 53, groups, plan.changedPairs));
    assert(Planner::AffectsEcmpOd(49, 50, groups, plan.changedPairs));
    assert(!Planner::AffectsEcmpOd(37, 37, groups, plan.changedPairs));
    assert(Planner::AffectsEcmpOd(999, 41, groups, plan.changedPairs));
    assert(!Planner::AffectsEcmpOd(37, 38, groups, noOp.changedPairs));

    bool rejected = false;
    try { auto invalid = original; ++invalid[{37, 41}]; Planner::Build(circuits, invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { auto duplicate = circuits; duplicate.push_back(circuits.front());
          Planner::Build(duplicate, original); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    std::mt19937 random(20261009);
    for (unsigned iteration = 0; iteration < 1000; ++iteration)
    {
        auto next = Counts(circuits);
        for (unsigned change = 0; change < 20; ++change)
        {
            unsigned g = random() % 5, h = random() % 5;
            if (g == h) continue;
            unsigned ai = random() % 4, bi = (ai + 1 + random() % 3) % 4;
            unsigned ci = random() % 4, di = (ci + 1 + random() % 3) % 4;
            unsigned a = 37 + g * 4 + ai, b = 37 + g * 4 + bi;
            unsigned c = 37 + h * 4 + ci, d = 37 + h * 4 + di;
            const Planner::Od ac = std::minmax(a, c), bd = std::minmax(b, d);
            const Planner::Od ad = std::minmax(a, d), bc = std::minmax(b, c);
            if (next.at(ac) <= 1 || next.at(bd) <= 1) continue;
            --next[ac]; --next[bd]; ++next[ad]; ++next[bc];
        }
        CheckPlan(circuits, next);
        const auto update = Planner::Build(circuits, next);
        circuits = update.retained;
        circuits.insert(circuits.end(), update.added.begin(), update.added.end());
    }
    CheckPlan(circuits, original);
    std::cout << "PASS: preserve 638/640 circuits for a two-circuit move; pause scope, "
                 "invalid targets, and 1000 repeated port reconfigurations verified\n";
}
