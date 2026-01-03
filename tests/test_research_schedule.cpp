#include "test.h"

#include <iostream>

#include "nebula4x/core/research_schedule.h"
#include "nebula4x/core/simulation.h"

using namespace nebula4x;

#define N4X_ASSERT(cond)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << " expected " << #cond << "\n";                \
      ++failed;                                                                                                        \
    }                                                                                                                  \
  } while (0)

namespace {

TechDef mk_tech(const std::string& id, const std::string& name, double cost, std::vector<std::string> prereqs) {
  TechDef t;
  t.id = id;
  t.name = name;
  t.cost = cost;
  t.prereqs = std::move(prereqs);
  return t;
}

InstallationDef mk_lab(double rp_per_day) {
  InstallationDef d;
  d.id = "lab";
  d.name = "Lab";
  d.research_points_per_day = rp_per_day;
  return d;
}

Simulation make_sim(ContentDB content) {
  SimConfig cfg;
  Simulation sim(std::move(content), cfg);
  // Keep state minimal for these unit tests.
  sim.state().date = Date(0);
  return sim;
}

} // namespace

int test_research_schedule() {
  int failed = 0;

  // --- Case 1: basic queue with prerequisites ---
  {
    ContentDB content;
    content.installations["lab"] = mk_lab(10.0);
    content.techs["a"] = mk_tech("a", "A", 10, {});
    content.techs["b"] = mk_tech("b", "B", 20, {"a"});

    auto sim = make_sim(content);

    Faction f;
    f.id = 1;
    f.name = "F";
    f.research_points = 0.0;
    f.research_queue = {"a", "b"};
    sim.state().factions[f.id] = f;

    Colony c;
    c.id = 10;
    c.faction_id = f.id;
    c.installations["lab"] = 1;
    sim.state().colonies[c.id] = c;

    ResearchScheduleOptions opt;
    opt.max_days = 32;
    const auto sched = estimate_research_schedule(sim, f.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(!sched.truncated);
    N4X_ASSERT(sched.items.size() == 2);
    N4X_ASSERT(sched.items[0].tech_id == "a");
    N4X_ASSERT(sched.items[0].start_day == 1);
    N4X_ASSERT(sched.items[0].end_day == 1);
    N4X_ASSERT(sched.items[1].tech_id == "b");
    N4X_ASSERT(sched.items[1].start_day == 1);
    N4X_ASSERT(sched.items[1].end_day == 3);
  }

  // --- Case 2: large bank allows multiple completions the same day ---
  {
    ContentDB content;
    content.installations["lab"] = mk_lab(0.0);
    content.techs["a"] = mk_tech("a", "A", 10, {});
    content.techs["b"] = mk_tech("b", "B", 20, {});

    auto sim = make_sim(content);

    Faction f;
    f.id = 1;
    f.name = "F";
    f.research_points = 100.0;
    f.research_queue = {"a", "b"};
    sim.state().factions[f.id] = f;

    Colony c;
    c.id = 10;
    c.faction_id = f.id;
    c.installations["lab"] = 1;
    sim.state().colonies[c.id] = c;

    const auto sched = estimate_research_schedule(sim, f.id);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(sched.items.size() == 2);
    N4X_ASSERT(sched.items[0].end_day == 1);
    N4X_ASSERT(sched.items[1].end_day == 1);
  }

  // --- Case 3: research multiplier tech affects RP/day starting next day ---
  {
    ContentDB content;
    content.installations["lab"] = mk_lab(10.0);
    {
      TechDef bonus = mk_tech("bonus", "Bonus", 10, {});
      TechEffect eff;
      eff.type = "faction_output_bonus";
      eff.value = "research";
      eff.amount = 1.0; // +100% => x2
      bonus.effects.push_back(eff);
      content.techs["bonus"] = bonus;
    }
    content.techs["big"] = mk_tech("big", "Big", 40, {});

    auto sim = make_sim(content);

    Faction f;
    f.id = 1;
    f.name = "F";
    f.research_points = 0.0;
    f.research_queue = {"bonus", "big"};
    sim.state().factions[f.id] = f;

    Colony c;
    c.id = 10;
    c.faction_id = f.id;
    c.installations["lab"] = 1;
    sim.state().colonies[c.id] = c;

    ResearchScheduleOptions opt;
    opt.max_days = 16;
    const auto sched = estimate_research_schedule(sim, f.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(sched.items.size() == 2);
    N4X_ASSERT(sched.items[0].tech_id == "bonus");
    N4X_ASSERT(sched.items[0].end_day == 1);
    N4X_ASSERT(sched.items[1].tech_id == "big");
    N4X_ASSERT(sched.items[1].end_day == 3);
  }

  // --- Case 4: queue blocked by missing prerequisites (no researchable project) ---
  {
    ContentDB content;
    content.installations["lab"] = mk_lab(10.0);
    content.techs["a"] = mk_tech("a", "A", 10, {});
    content.techs["b"] = mk_tech("b", "B", 10, {"a"});

    auto sim = make_sim(content);

    Faction f;
    f.id = 1;
    f.name = "F";
    f.research_points = 0.0;
    f.research_queue = {"b"};
    sim.state().factions[f.id] = f;

    Colony c;
    c.id = 10;
    c.faction_id = f.id;
    c.installations["lab"] = 1;
    sim.state().colonies[c.id] = c;

    ResearchScheduleOptions opt;
    opt.max_days = 8;
    const auto sched = estimate_research_schedule(sim, f.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(sched.stalled);
    N4X_ASSERT(!sched.stall_reason.empty());
    N4X_ASSERT(sched.items.empty());
  }

  // --- Case 5: active project progress is preserved and flagged ---
  {
    ContentDB content;
    content.installations["lab"] = mk_lab(10.0);
    content.techs["a"] = mk_tech("a", "A", 10, {});

    auto sim = make_sim(content);

    Faction f;
    f.id = 1;
    f.name = "F";
    f.research_points = 0.0;
    f.active_research_id = "a";
    f.active_research_progress = 5.0;
    sim.state().factions[f.id] = f;

    Colony c;
    c.id = 10;
    c.faction_id = f.id;
    c.installations["lab"] = 1;
    sim.state().colonies[c.id] = c;

    ResearchScheduleOptions opt;
    opt.max_days = 8;
    const auto sched = estimate_research_schedule(sim, f.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(sched.items.size() == 1);
    N4X_ASSERT(sched.items[0].tech_id == "a");
    N4X_ASSERT(sched.items[0].was_active_at_start);
    N4X_ASSERT(sched.items[0].progress_at_start == 5.0);
    N4X_ASSERT(sched.items[0].start_day == 0);
    N4X_ASSERT(sched.items[0].end_day == 1);
  }

  return failed;
}
