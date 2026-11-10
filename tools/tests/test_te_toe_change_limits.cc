// SPDX-License-Identifier: GPL-2.0-only
#include "../../ns-3-ub/src/unified-bus/model/protocol/ub-te-weight-step.h"
#include "../../ns-3-ub/src/unified-bus/model/protocol/ub-toe-bounded.h"
#include "../../ns-3-ub/src/unified-bus/model/protocol/ub-toe-reconfiguration.h"

#include <cassert>
#include <iostream>
#include <random>

using Te = ns3::UbTeWeightStep;
using Toe = ns3::UbToeBounded;
using Planner = ns3::UbToeReconfiguration;

static void Near(double a, double b) { assert(std::abs(a - b) < 1e-10); }

static void Check(const Toe::Topology &old, const Toe::Topology &target,
                  const Toe::Result &result, const std::set<uint32_t> &prefill,
                  unsigned budget) {
  std::map<uint32_t, unsigned> before, after;
  unsigned removed = 0;
  for (const auto &[pair, count] : old) {
    auto value = result.topology.at(pair);
    assert(value >= 1 && value >= std::min(count, target.at(pair)) &&
           value <= std::max(count, target.at(pair)));
    before[pair.first] += count;
    before[pair.second] += count;
    after[pair.first] += value;
    after[pair.second] += value;
    if (count > value)
      removed += count - value;
  }
  assert(before == after && removed == result.changedCircuits);
  const auto counts = Toe::Removed(old, result.topology);
  for (auto leaf : prefill)
    assert(!counts.count(leaf) || counts.at(leaf) <= budget);
  assert(result.plannedMlu <= result.beforeMlu + 1e-10);
}

static void TestWeights() {
  const Te::Weights old{{-1, 1}}, target{{-1, .4}, {41, .4}, {42, .2}};
  auto next = Te::Limit(old, target, .1);
  Near(next.at(-1), .9);
  Near(next.at(41), 1.0 / 15);
  Near(next.at(42), 1.0 / 30);
  Near(Te::Distance(old, next), .1);
  Near(Te::Distance(old, Te::Limit(old, target, 0)), 0);
  Near(Te::Distance(target, Te::Limit(old, target, 1)), 0);
  auto reverse = Te::Limit(next, old, .05);
  Near(reverse.at(-1), .95);
  std::mt19937 rng(20261110);
  for (unsigned i = 0; i < 10000; ++i) {
    Te::Weights a, b;
    for (int p = -1; p < 12; ++p) {
      if (rng() % 2)
        a[p] = 1 + rng() % 1000;
      if (rng() % 2)
        b[p] = 1 + rng() % 1000;
    }
    if (a.empty() || b.empty())
      continue;
    a = Te::Normalize(a);
    b = Te::Normalize(b);
    const double delta = static_cast<double>(rng() % 1001) / 1000;
    const auto limited = Te::Limit(a, b, delta);
    double sum = 0;
    for (const auto &[path, w] : limited) {
      (void)path;
      assert(w >= 0);
      sum += w;
    }
    Near(sum, 1);
    assert(Te::Distance(a, limited) <= delta + 1e-12);
    assert(Te::Distance(limited, b) <= Te::Distance(a, b) + 1e-12);
  }
  Te::Capacities caps{{{37, 53}, 4}, {{37, 41}, 2}, {{41, 53}, 2},
                      {{38, 54}, 4}, {{38, 41}, 2}, {{41, 54}, 2}};
  Te::Policy previous{{{38, 54}, {{-1, .7}, {41, .3}}}};
  Te::Policy goal{{{37, 53}, {{-1, .4}, {41, .6}}}};
  auto published = Te::Prepare(previous, goal, caps, .1);
  Near(published.at({37, 53}).at(-1),
       .9); // First epoch starts from actual fallback.
  assert(published.at({38, 54}) == previous.at({38, 54})); // Idle OD retained.
  assert(Te::Prepare(published, {}, caps, .1) ==
         published); // Empty history retained.
  Near(Te::TransitRatio(published, caps, {{{37, 53}, 1}, {{38, 54}, 1}}), .2);
  // Mandatory transit is excluded from the optional-transit denominator.
  caps.erase({37, 53});
  Near(Te::TransitRatio(published, caps, {{{37, 53}, 100}, {{38, 54}, 1}}), .3);
  auto repaired = Te::Prepare(published, {}, caps, 0);
  Near(repaired.at({37, 53}).at(41), 1);
  Near(Te::Distance(published.at({37, 53}), repaired.at({37, 53})), .9);
  Near(Te::Mlu(repaired, caps, {{{37, 53}, 4}}), 2);
  bool rejected = false;
  try {
    Te::Limit(old, target, std::numeric_limits<double>::quiet_NaN());
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);

  // Per-OD interpolation can violate an aggregate transit cap even when
  // both endpoint policies satisfy it. The controller must hold publication.
  Te::Policy a{{{37, 53}, {{-1, 1}}}, {{38, 54}, {{-1, .5}, {41, .5}}}};
  Te::Policy b{{{37, 53}, {{-1, .95}, {41, .05}}}, {{38, 54}, {{-1, 1}}}};
  caps[{37, 53}] = 4;
  const std::map<Te::Od, double> demand{{{37, 53}, 10}, {{38, 54}, 1}};
  Near(Te::TransitRatio(a, caps, demand), .5 / 11);
  Near(Te::TransitRatio(b, caps, demand), .5 / 11);
  assert(Te::TransitRatio(Te::Prepare(a, b, caps, .1), caps, demand) >
         .5 / 11 + .01);
}

static void TestLongCircuits() {
  Toe::Topology old;
  for (unsigned a = 1; a <= 6; ++a)
    for (unsigned b = a + 1; b <= 6; ++b)
      old[{a, b}] = 2;
  auto target = old;
  for (auto e : {Toe::Od{1, 2}, {3, 4}, {5, 6}})
    --target[e];
  for (auto e : {Toe::Od{2, 3}, {4, 5}, {1, 6}})
    ++target[e];
  const std::set<unsigned> prefill{1, 2, 3, 4, 5, 6};
  auto result =
      Toe::Select(old, target, prefill, 1, [](const auto &) { return 1.; });
  Check(old, target, result, prefill, 1);
  assert(result.topology == target);
  // Two odd cycles sharing a vertex require visiting that leaf in both colors.
  target = old;
  for (auto e : {Toe::Od{1, 2}, {1, 3}, {4, 5}})
    --target[e];
  for (auto e : {Toe::Od{2, 3}, {1, 4}, {1, 5}})
    ++target[e];
  result =
      Toe::Select(old, target, prefill, 2, [](const auto &) { return 1.; });
  Check(old, target, result, prefill, 2);
  assert(result.topology == target);
  result =
      Toe::Select(old, target, prefill, 1, [](const auto &) { return 1.; });
  assert(result.topology == old); // Shared leaf needs two removals atomically.
  result =
      Toe::Select(old, target, prefill, 0, [](const auto &) { return 1.; });
  assert(result.topology == old);
  result = Toe::Select(old, target, prefill, 2,
                       [&](const auto &t) { return t == old ? 1. : 2.; });
  assert(result.topology == old); // Reject MLU regression.
  bool rejected = false;
  ++target[{1, 2}];
  try {
    Toe::Select(old, target, prefill, 2, [](const auto &) { return 1.; });
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

static void TestMooncake() {
  Toe::Topology old;
  std::map<unsigned, unsigned> groups;
  std::set<unsigned> prefill;
  std::vector<Planner::Circuit> circuits;
  std::map<unsigned, uint16_t> port;
  for (unsigned a = 37; a <= 56; ++a) {
    groups[a] = (a - 37) / 4;
    if (a <= 48)
      prefill.insert(a);
    for (unsigned b = a + 1; b <= 56; ++b)
      if ((a - 37) / 4 != (b - 37) / 4) {
        old[{a, b}] = 4;
        for (unsigned c = 0; c < 4; ++c)
          circuits.push_back({a, port[a]++, b, port[b]++});
      }
  }
  // Same-group traffic hashes over physical first-hop ports, then transits.
  Near(Toe::EcmpMlu(old, groups, {{{37, 38}, 64 * 400e9}}), 1);
  Near(Toe::EcmpMlu(old, groups, {{{37, 53}, 1600e9}}), 1);
  std::mt19937 rng(123);
  for (unsigned trial = 0; trial < 30; ++trial) {
    auto target = old;
    for (unsigned step = 0; step < 40; ++step) {
      const unsigned a = 37 + rng() % 20, b = 37 + rng() % 20,
                     c = 37 + rng() % 20, d = 37 + rng() % 20;
      if (std::set<unsigned>{a, b, c, d}.size() != 4)
        continue;
      const Toe::Od ab = std::minmax(a, b), cd = std::minmax(c, d),
                    ac = std::minmax(a, c), bd = std::minmax(b, d);
      if (!target.count(ab) || !target.count(cd) || !target.count(ac) ||
          !target.count(bd) || target[ab] <= 1 || target[cd] <= 1)
        continue;
      --target[ab];
      --target[cd];
      ++target[ac];
      ++target[bd];
    }
    const std::map<Toe::Od, double> demand{
        {{53, 49}, 1600e9}, {{37, 38}, 800e9}, {{54, 42}, 800e9}};
    auto evaluate = [&](const auto &t) {
      return Toe::EcmpMlu(t, groups, demand);
    };
    const auto result = Toe::Select(old, target, prefill, 2, evaluate);
    Check(old, target, result, prefill, 2);
    const auto plan = Planner::Build(circuits, result.topology);
    assert(plan.removed.size() == result.changedCircuits &&
           plan.added.size() == result.changedCircuits);
    const auto repeated = Toe::Select(old, target, prefill, 2, evaluate);
    assert(repeated.topology == result.topology);
  }
}

int main() {
  TestWeights();
  TestLongCircuits();
  TestMooncake();
  std::cout
      << "PASS: TE probabilities, reversal/freeze/idle/fallback/cap/repair; "
         "TOE long trails, budgets, ECMP transit, deterministic Mooncake "
         "physical plans\n";
}
