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
  assert(Toe::WithinBudget(old, result.topology, prefill, budget));
  for (const auto &[pair, count] : Toe::RemovedPairs(old, result.topology))
    if (prefill.count(pair.first) || prefill.count(pair.second))
      assert(count <= budget);
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
  assert(result.topology == target); // Two distinct pairs may each lose one.
  result =
      Toe::Select(old, target, prefill, 0, [](const auto &) { return 1.; });
  assert(result.topology == old);
  result = Toe::Select(old, target, prefill, 2,
                       [&](const auto &t) { return t == old ? 1. : 2.; });
  assert(result.topology == old); // Reject MLU regression.

  // A pair budget still forbids removing two circuits from the same pair,
  // including removals accumulated over separately accepted swaps.
  auto repeatedOld = old;
  for (auto &[pair, count] : repeatedOld) {
    (void)pair;
    count = 3;
  }
  auto repeatedTarget = repeatedOld;
  for (auto pair : {Toe::Od{1, 2}, {3, 4}})
    repeatedTarget[pair] -= 2;
  for (auto pair : {Toe::Od{1, 3}, {2, 4}})
    repeatedTarget[pair] += 2;
  const auto once = Toe::Select(repeatedOld, repeatedTarget, prefill, 1,
                                [](const auto &) { return 1.; });
  Check(repeatedOld, repeatedTarget, once, prefill, 1);
  assert(once.topology.at({1, 2}) == 2 && once.topology.at({3, 4}) == 2);
  assert(once.changedCircuits == 2);
  const auto twice = Toe::Select(once.topology, repeatedTarget, prefill, 1,
                                 [](const auto &) { return 1.; });
  assert(twice.topology == repeatedTarget); // Budget resets each epoch.
  const auto unrestricted = Toe::Select(repeatedOld, repeatedTarget, {5, 6}, 0,
                                        [](const auto &) { return 1.; });
  assert(unrestricted.topology == repeatedTarget); // Non-P pairs are uncapped.
  bool rejected = false;
  ++target[{1, 2}];
  try {
    Toe::Select(old, target, prefill, 2, [](const auto &) { return 1.; });
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  assert(rejected);
}

static void TestRemoveOnePerPair() {
  Toe::Topology old;
  std::map<unsigned, unsigned> groups;
  std::set<unsigned> prefill;
  std::vector<Planner::Circuit> circuits;
  std::map<unsigned, uint16_t> port;
  std::map<Toe::Od, double> demand;
  for (unsigned a = 37; a <= 56; ++a) {
    groups[a] = (a - 37) / 4;
    port[a] = 64;
    if (a <= 48)
      prefill.insert(a);
  }
  for (unsigned a = 37; a <= 56; ++a)
    for (unsigned b = a + 1; b <= 56; ++b)
      if (groups.at(a) != groups.at(b)) {
        old[{a, b}] = 4;
        for (unsigned lane = 0; lane < 4; ++lane)
          circuits.push_back({a, port[a]++, b, port[b]++});
      }
  auto target = old;
  for (auto &[pair, count] : target) {
    const auto [a, b] = pair;
    if (b <= 48)
      count = 5; // All 48 allowed inter-group P-P pairs gain one.
    else if (a <= 48)
      count = 3; // Every P-D and P-S pair loses one.
    else {
      count = 7; // All 16 D-S pairs gain three.
      demand[{b, a}] = 1600e9;
    }
  }
  // The former per-leaf remove1 cap would reject eight removals on each P.
  assert(Toe::WithinBudget(old, target, prefill, 1));
  assert(!Toe::WithinBudget(old, target, prefill, 0));
  for (unsigned p : prefill)
    assert(Toe::Removed(old, target).at(p) == 8);
  const auto evaluate = [&](const auto &t) {
    return Toe::EcmpMlu(t, groups, demand);
  };
  const auto result = Toe::Select(old, target, prefill, 1, evaluate);
  Check(old, target, result, prefill, 1);
  assert(result.topology == target);
  assert(result.evaluations == 1 && !result.searchLimited);
  Near(result.beforeMlu, 1);
  Near(result.plannedMlu, 4.0 / 7);
  assert(result.changedCircuits == 96);
  unsigned pp = 0, pd = 0, ps = 0, ds = 0;
  for (const auto &[pair, count] : result.topology) {
    if (pair.second <= 48)
      pp += count;
    else if (pair.first <= 48 && pair.second <= 52)
      pd += count;
    else if (pair.first <= 48)
      ps += count;
    else
      ds += count;
  }
  assert(pp == 240 && pd == 144 && ps == 144 && ds == 112);
  const auto plan = Planner::Build(circuits, result.topology);
  assert(plan.retained.size() == 544);
  assert(plan.removed.size() == 96 && plan.added.size() == 96);
  for (const auto &[leaf, changed] : plan.changedPorts)
    assert(changed.size() == (leaf <= 48 ? 8 : 12));
  Toe::Topology readback;
  std::map<unsigned, std::set<uint16_t>> endpoints;
  auto record = [&](const auto &circuit) {
    assert(endpoints[circuit.a].insert(circuit.ap).second);
    assert(endpoints[circuit.b].insert(circuit.bp).second);
    ++readback[{circuit.a, circuit.b}];
  };
  for (const auto &circuit : plan.retained)
    record(circuit);
  for (const auto &circuit : plan.added)
    record(circuit);
  assert(readback == target);
  for (const auto &[leaf, ports] : endpoints) {
    (void)leaf;
    assert(ports.size() == 64 && *ports.begin() == 64 && *ports.rbegin() == 127);
  }
  const auto again = Toe::Select(old, target, prefill, 1, evaluate);
  assert(again.topology == result.topology);
  const auto frozen = Toe::Select(old, target, prefill, 0, evaluate);
  assert(frozen.topology == old);
}

static void TestBoundedFallback() {
  // Keep the full target over budget so both long-trail shapes exercise the
  // local search rather than the complete-target fast path.
  for (bool sharedLeaf : {false, true}) {
    Toe::Topology old;
    std::set<unsigned> prefill;
    for (unsigned a = 1; a <= 10; ++a) {
      prefill.insert(a);
      for (unsigned b = a + 1; b <= 10; ++b)
        if ((a <= 6) == (b <= 6))
          old[{a, b}] = 3;
    }
    auto target = old;
    const std::vector<Toe::Od> removed = sharedLeaf
        ? std::vector<Toe::Od>{{1, 2}, {1, 3}, {4, 5}}
        : std::vector<Toe::Od>{{1, 2}, {3, 4}, {5, 6}};
    const std::vector<Toe::Od> added = sharedLeaf
        ? std::vector<Toe::Od>{{2, 3}, {1, 4}, {1, 5}}
        : std::vector<Toe::Od>{{2, 3}, {4, 5}, {1, 6}};
    for (auto pair : removed)
      --target[pair];
    for (auto pair : added)
      ++target[pair];
    for (auto pair : {Toe::Od{7, 8}, {9, 10}})
      target[pair] -= 2;
    for (auto pair : {Toe::Od{7, 9}, {8, 10}})
      target[pair] += 2;
    const auto result = Toe::Select(old, target, prefill, 1,
                                    [](const auto &) { return 1.; });
    Check(old, target, result, prefill, 1);
    for (auto pair : removed)
      assert(result.topology.at(pair) == target.at(pair));
    for (auto pair : added)
      assert(result.topology.at(pair) == target.at(pair));
    assert(result.topology.at({7, 8}) == 2 && result.topology.at({9, 10}) == 2);
    assert(result.changedCircuits == 5);

    // A legal full target with a worse preview must fall back to safe partial
    // moves; exhausting the evaluation budget never publishes the bad target.
    auto legalTarget = result.topology;
    const auto evaluate = [&](const auto &t) { return t == legalTarget ? 2. : 1.; };
    const auto partial = Toe::Select(old, legalTarget, prefill, 1, evaluate);
    Check(old, legalTarget, partial, prefill, 1);
    assert(partial.topology != legalTarget && partial.changedCircuits > 0);
    const auto limited = Toe::Select(old, legalTarget, prefill, 1, evaluate, 1);
    assert(limited.topology == old && limited.evaluations == 1 && limited.searchLimited);
  }
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
  TestRemoveOnePerPair();
  TestBoundedFallback();
  TestMooncake();
  std::cout
      << "PASS: TE probabilities, reversal/freeze/idle/fallback/cap/repair; "
         "TOE long trails, per-pair budgets, remove1 D-S 4-to-7, ECMP transit, deterministic Mooncake "
         "physical plans\n";
}
