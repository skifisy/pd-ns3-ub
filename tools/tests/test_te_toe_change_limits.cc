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

struct Fixture {
  Toe::Topology current;
  std::map<unsigned, unsigned> groups;
  std::set<unsigned> prefill, decode, storage;
  std::vector<Planner::Circuit> circuits;
  Fixture() {
    std::map<unsigned, uint16_t> port;
    for (unsigned a = 37; a <= 56; ++a) {
      groups[a] = (a - 37) / 4;
      port[a] = 64;
      (a <= 48 ? prefill : a <= 52 ? decode : storage).insert(a);
    }
    for (unsigned a = 37; a <= 56; ++a)
      for (unsigned b = a + 1; b <= 56; ++b)
        if (groups.at(a) != groups.at(b)) {
          current[{a, b}] = 4;
          for (unsigned lane = 0; lane < 4; ++lane)
            circuits.push_back({a, port[a]++, b, port[b]++});
        }
  }
  Toe::Result Select(const Toe::Topology &target, unsigned budget = 1) const {
    return Toe::Select(current, target, prefill, decode, storage, budget);
  }
};

static void Check(const Fixture &f, const Toe::Topology &target,
                  const Toe::Result &result, unsigned budget = 1) {
  std::map<unsigned, unsigned> before, after;
  unsigned removed = 0;
  for (const auto &[pair, count] : f.current) {
    const auto value = result.topology.at(pair);
    assert(value >= 1);
    before[pair.first] += count;
    before[pair.second] += count;
    after[pair.first] += value;
    after[pair.second] += value;
    if (count > value) removed += count - value;
    if (pair.first <= 48 && pair.second >= 53) // P-S follows LP reductions only.
      assert(value <= count && value >= std::min(count, target.at(pair)));
    if (pair.first >= 49) // D-S grows towards, never beyond, the LP target.
      assert(value >= count && value <= std::max(count, target.at(pair)));
    if (pair.second <= 48) assert(value >= count); // P-P balances freed endpoints.
  }
  assert(before == after && removed == result.changedCircuits);
  assert(result.changedCircuits == 2 * result.allocations.size());
  assert(Toe::WithinBudget(f.current, result.topology, f.prefill, budget));
  auto replay = f.current;
  for (const auto &step : result.allocations) {
    assert(replay.at(step.ds) == step.before);
    assert(target.at(step.ds) == step.target && step.before < step.target);
    assert(f.prefill.count(step.pp.first) && f.prefill.count(step.pp.second));
    --replay.at(step.pd); --replay.at(step.ps);
    ++replay.at(step.ds); ++replay.at(step.pp);
    assert(Toe::WithinBudget(f.current,replay,f.prefill,budget));
  }
  assert(replay == result.topology); // The allocation audit reconstructs the plan.
  const auto plan = Planner::Build(f.circuits, result.topology);
  assert(plan.removed.size() == result.changedCircuits);
  assert(plan.added.size() == result.changedCircuits);
  Toe::Topology readback;
  std::map<unsigned, std::set<uint16_t>> endpoints;
  for (const auto *part : {&plan.retained, &plan.added})
    for (const auto &c : *part) {
      assert(endpoints[c.a].insert(c.ap).second);
      assert(endpoints[c.b].insert(c.bp).second);
      ++readback[{c.a, c.b}];
    }
  assert(readback == result.topology);
  for (const auto &[leaf, ports] : endpoints) {
    (void)leaf;
    assert(ports.size() == 64 && *ports.begin() == 64 && *ports.rbegin() == 127);
  }
}

// Exact epoch-4 target reconstructed from the user-supplied topology snapshot.
static Toe::Topology EpochFourTarget(const Fixture &f) {
  auto t = f.current;
  const unsigned pd[12][4] = {
      {1,4,1,4}, {1,1,1,1}, {1,2,4,1}, {2,4,4,4},
      {4,1,2,1}, {1,1,1,1}, {4,1,1,4}, {4,4,4,1},
      {1,1,4,2}, {1,4,1,1}, {4,4,4,4}, {4,1,1,4}};
  for (unsigned p = 37; p <= 48; ++p) {
    for (unsigned d = 49; d <= 52; ++d) t[{p,d}] = pd[p-37][d-49];
    for (unsigned s = 53; s <= 56; ++s) t[{p,s}] = p <= 40 ? 3 : 2;
  }
  for (auto [pair, delta] : std::map<Toe::Od, unsigned>{
      {{37,43},5}, {{37,47},5}, {{38,44},5}, {{38,48},11},
      {{39,43},9}, {{39,48},3}, {{40,44},6}, {{41,45},16},
      {{42,46},17}, {{42,47},3}}) t[pair] += delta;
  for (unsigned d : f.decode)
    for (unsigned s : f.storage) t[{d,s}] = 9;
  return t;
}

// Construct a port-balanced LP reference by explicit legal swaps. The final
// allocator may choose different P-D donors and P-P balancing links.
static Toe::Topology Target(const Fixture &f, const std::map<Toe::Od,unsigned> &ds) {
  auto t = f.current;
  unsigned index = 0;
  for (auto [pair, count] : ds)
    for (unsigned n = 4; n < count; ++n) {
      const unsigned pa = 37 + index % 12, pb = 37 + (index + 4) % 12;
      assert(t.at({pa,pair.first}) > 1 && t.at({pb,pair.second}) > 1);
      --t.at({pa,pair.first}); --t.at({pb,pair.second});
      ++t.at(pair); ++t.at(std::minmax(pa,pb)); ++index;
    }
  return t;
}

static void TestEpochFour() {
  Fixture f;
  const auto t = EpochFourTarget(f);
  const auto r = f.Select(t);
  Check(f,t,r);
  unsigned pp=0,pd=0,ps=0,ds=0;
  bool independentPd=false, independentPp=false;
  for (auto [pair,n] : r.topology) {
    if (pair.second <= 48) { pp+=n; independentPp |= n>t.at(pair); }
    else if (pair.first<=48 && pair.second<=52) { pd+=n; independentPd |= n<t.at(pair); }
    else if (pair.first<=48) ps+=n;
    else { ds+=n; assert(n==7); }
  }
  assert(pp==240 && pd==144 && ps==144 && ds==112);
  assert(independentPd && independentPp);
  for (auto p : f.prefill) assert(r.removed.at(p)==8);
  assert(f.Select(t).topology == r.topology);
  assert(f.Select(t,0).topology == f.current);
  std::map<Toe::Od,double> demand;
  for (auto d : f.decode) for (auto s : f.storage) demand[{s,d}]=1600e9;
  Near(Toe::EcmpMlu(f.current,f.groups,demand),1);
  Near(Toe::EcmpMlu(r.topology,f.groups,demand),4.0/7);
}

static void TestRatios() {
  Fixture f;
  const auto t=Target(f,{{{49,53},10},{{49,54},8},{{49,55},6},{{49,56},6}});
  const auto r=f.Select(t); Check(f,t,r);
  assert(r.topology.at({49,53})==9 && r.topology.at({49,54})==7);
  assert(r.topology.at({49,55})==6 && r.topology.at({49,56})==6);
  assert(r.allocations.front().ds == Toe::Od(49,53));
  auto hot=Target(f,{{{49,53},10}});
  const auto ten=f.Select(hot); Check(f,hot,ten);
  assert(ten.topology.at({49,53})==10); // No hard-coded 6/7/9 ceiling.
  assert(ten.allocations.size()==6);
  const auto done=f.Select(f.current); assert(done.topology==f.current);

  // A high-priority pair exhausts its P-S donors; other feasible pairs proceed.
  auto limited=f.current;
  for (unsigned pa : {37u,38u,39u}) {
    --limited.at({pa,49}); --limited.at({41,53});
    ++limited.at({49,53}); ++limited.at({pa,41});
  }
  for (unsigned i=0;i<2;++i) {
    --limited.at({37+i,49}); --limited.at({42+i,54});
    ++limited.at({49,54}); ++limited.at({37+i,42+i});
  }
  const auto skip=f.Select(limited); Check(f,limited,skip);
  assert(skip.topology.at({49,53})==5 && skip.topology.at({49,54})==6);
}

static void TestBudgetsAndRandom() {
  Fixture f;
  const auto t=EpochFourTarget(f);
  auto twice=f.Select(t,2); Check(f,t,twice,2);
  for(auto d:f.decode) for(auto s:f.storage) assert(twice.topology.at({d,s})==9);
  const auto first=f.Select(t);
  const auto next=Toe::Select(first.topology,t,f.prefill,f.decode,f.storage,1);
  assert(Toe::WithinBudget(first.topology,next.topology,f.prefill,1));
  assert(!next.allocations.empty()); // Budget resets against the next actual topology.
  auto floor=f.current;
  // No growth and no P-S reduction means no unsolicited changes.
  assert(f.Select(floor).allocations.empty());
  bool rejected=false;
  try { auto bad=t; ++bad.at({37,49}); f.Select(bad); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  rejected=false;
  try { auto bad=t; bad.erase({37,49}); f.Select(bad); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  rejected=false;
  try { auto bad=t; bad.at({37,49})=0; f.Select(bad); }
  catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);

  std::mt19937 rng(20261119);
  for(unsigned trial=0;trial<100;++trial) {
    auto random=f.current;
    for(unsigned step=0;step<50;++step) {
      const unsigned pa=37+rng()%12,pb=37+rng()%12,d=49+rng()%4,s=53+rng()%4;
      const Toe::Od pp=std::minmax(pa,pb);
      if(!random.count(pp) || random.at({pa,d})<=1 || random.at({pb,s})<=1) continue;
      --random.at({pa,d}); --random.at({pb,s}); ++random.at({d,s}); ++random.at(pp);
    }
    const unsigned budget=rng()%4;
    const auto r=f.Select(random,budget); Check(f,random,r,budget);
    assert(f.Select(random,budget).topology==r.topology);
  }
}

int main() {
  TestWeights(); TestEpochFour(); TestRatios(); TestBudgetsAndRandom();
  std::cout << "PASS: TE weight limits; LP-ratio D-S allocation, epoch4 64-to-112, "
               "nonuniform 9/7/6/6, target 10, donor budgets and physical readback\n";
}
