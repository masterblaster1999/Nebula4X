#include "nebula4x/core/enum_strings.h"

namespace nebula4x {

std::string body_type_to_string(BodyType t) {
  switch (t) {
    case BodyType::Star: return "star";
    case BodyType::Planet: return "planet";
    case BodyType::Moon: return "moon";
    case BodyType::Asteroid: return "asteroid";
    case BodyType::Comet: return "comet";
    case BodyType::GasGiant: return "gas_giant";
  }
  // Safe default for unknown/invalid values.
  return "planet";
}

BodyType body_type_from_string(const std::string& s) {
  if (s == "star") return BodyType::Star;
  if (s == "planet") return BodyType::Planet;
  if (s == "moon") return BodyType::Moon;
  if (s == "asteroid") return BodyType::Asteroid;
  if (s == "comet") return BodyType::Comet;
  if (s == "gas_giant") return BodyType::GasGiant;
  // Safe default for unknown strings.
  return BodyType::Planet;
}


std::string wreck_kind_to_string(WreckKind k) {
  switch (k) {
    case WreckKind::Ship: return "ship";
    case WreckKind::Cache: return "cache";
    case WreckKind::Debris: return "debris";
  }
  return "ship";
}

WreckKind wreck_kind_from_string(const std::string& s) {
  if (s == "ship" || s == "hull") return WreckKind::Ship;
  if (s == "cache" || s == "salvage_cache" || s == "mineral_cache") return WreckKind::Cache;
  if (s == "debris" || s == "field") return WreckKind::Debris;
  // Safe default for unknown strings.
  return WreckKind::Ship;
}

} // namespace nebula4x