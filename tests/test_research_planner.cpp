#include "test.h"

#include <iostream>
#include <string>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/research_planner.h"

using namespace nebula4x;

#define N4X_ASSERT(cond)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << " expected " << #cond << "\n";                        \
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

} // namespace

int test_research_planner() {
  int failed = 0;

  ContentDB content;
  content.techs["a"] = mk_tech("a", "A", 10, {});
  content.techs["b"] = mk_tech("b", "B", 20, {"a"});
  content.techs["c"] = mk_tech("c", "C", 30, {"b"});

  {
    Faction f;
    f.known_techs = {"a"};
    const auto plan = compute_research_plan(content, f, "c");
    N4X_ASSERT(plan.ok());
    N4X_ASSERT(plan.plan.tech_ids.size() == 2);
    N4X_ASSERT(plan.plan.tech_ids[0] == "b");
    N4X_ASSERT(plan.plan.tech_ids[1] == "c");
    N4X_ASSERT(plan.plan.total_cost == 50);
  }

  {
    Faction f;
    const auto plan = compute_research_plan(content, f, "c");
    N4X_ASSERT(plan.ok());
    N4X_ASSERT(plan.plan.tech_ids.size() == 3);
    N4X_ASSERT(plan.plan.tech_ids[0] == "a");
    N4X_ASSERT(plan.plan.tech_ids[1] == "b");
    N4X_ASSERT(plan.plan.tech_ids[2] == "c");
    N4X_ASSERT(plan.plan.total_cost == 60);
  }

  {
    // Target already known -> empty plan.
    Faction f;
    f.known_techs = {"a"};
    const auto plan = compute_research_plan(content, f, "a");
    N4X_ASSERT(plan.ok());
    N4X_ASSERT(plan.plan.tech_ids.empty());
    N4X_ASSERT(plan.plan.total_cost == 0);
  }

  {
    // Missing prerequisite.
    ContentDB bad;
    bad.techs["d"] = mk_tech("d", "D", 1, {"missing"});
    Faction f;
    const auto plan = compute_research_plan(bad, f, "d");
    N4X_ASSERT(!plan.ok());
    N4X_ASSERT(!plan.errors.empty());
  }

  {
    // Cycle.
    ContentDB cyc;
    cyc.techs["a"] = mk_tech("a", "A", 1, {"b"});
    cyc.techs["b"] = mk_tech("b", "B", 1, {"a"});
    Faction f;
    const auto plan = compute_research_plan(cyc, f, "a");
    N4X_ASSERT(!plan.ok());
    N4X_ASSERT(!plan.errors.empty());
  }

  return failed;
}
