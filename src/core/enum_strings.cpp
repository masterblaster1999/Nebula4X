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

} // namespace nebula4x