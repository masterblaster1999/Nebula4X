#include <iostream>
#include <string>

#include "nebula4x/core/content_validation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_content_validation() {
  // Validate the repo's default content + tech tree.
  auto content = nebula4x::load_content_db_from_file("data/blueprints/starting_blueprints.json");
  content.techs = nebula4x::load_tech_db_from_file("data/tech/tech_tree.json");

  {
    const auto errors = nebula4x::validate_content_db(content);
    if (!errors.empty()) {
      std::cerr << "Content validation failed:\n";
      for (const auto& e : errors) std::cerr << "  - " << e << "\n";
      return 1;
    }
  }

  // Sanity: validation should catch obvious errors.
  {
    auto bad = content;
    nebula4x::TechDef t;
    t.id = "bad_tech";
    t.name = "Bad Tech";
    t.cost = 1;
    t.effects.push_back({"unlock_component", "does_not_exist", 0.0});
    bad.techs[t.id] = t;

    const auto errors = nebula4x::validate_content_db(bad);
    bool has_unlock_error = false;
    for (const auto& e : errors) {
      if (e.find("unlocks unknown component") != std::string::npos) {
        has_unlock_error = true;
        break;
      }
    }
    N4X_ASSERT(has_unlock_error);
  }

  // Sanity: prereq cycles should be detected (can deadlock research).
  {
    auto bad = content;

    nebula4x::TechDef a;
    a.id = "cycle_a";
    a.name = "Cycle A";
    a.cost = 1;
    a.prereqs.push_back("cycle_b");
    a.effects.push_back({"unlock_component", "engine_chem_mk1", 0.0});

    nebula4x::TechDef b;
    b.id = "cycle_b";
    b.name = "Cycle B";
    b.cost = 1;
    b.prereqs.push_back("cycle_a");
    b.effects.push_back({"unlock_component", "cargo_mk1", 0.0});

    bad.techs[a.id] = a;
    bad.techs[b.id] = b;

    const auto errors = nebula4x::validate_content_db(bad);
    bool has_cycle = false;
    for (const auto& e : errors) {
      if (e.find("prerequisite cycle") != std::string::npos) {
        has_cycle = true;
        break;
      }
    }
    N4X_ASSERT(has_cycle);
  }

  return 0;
}
