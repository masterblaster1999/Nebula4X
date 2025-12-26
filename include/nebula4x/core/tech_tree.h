#pragma once

#include <string>
#include <vector>

namespace nebula4x {

struct TechEffect {
  // Examples: "unlock_component", "unlock_installation".
  std::string type;
  std::string value;

  // Optional numeric payload.
  double amount{0.0};
};

struct TechDef {
  std::string id;
  std::string name;
  double cost{0.0};
  std::vector<std::string> prereqs;
  std::vector<TechEffect> effects;
};

} // namespace nebula4x
