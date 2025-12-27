#include <iostream>
#include <string>

#include "nebula4x/core/content_validation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";          \
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
    N4X_ASSERT(!errors.empty());
  }

  return 0;
}
