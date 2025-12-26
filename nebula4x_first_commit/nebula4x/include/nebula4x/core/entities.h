#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

enum class BodyType { Star, Planet, Moon, Asteroid, GasGiant };

enum class ShipRole { Freighter, Surveyor, Combatant, Unknown };

struct Body {
  Id id{kInvalidId};
  std::string name;
  BodyType type{BodyType::Planet};
  Id system_id{kInvalidId};

  // Simple circular orbit around the system origin.
  double orbit_radius_mkm{0.0};     // million km
  double orbit_period_days{0.0};    // days
  double orbit_phase_radians{0.0};  // starting angle

  // Cached position for current sim date.
  Vec2 position_mkm{0.0, 0.0};
};

struct ShipDesign {
  std::string id;
  std::string name;
  ShipRole role{ShipRole::Unknown};
  std::vector<std::string> components;
  // Derived:
  double mass_tons{0.0};
  double speed_km_s{0.0};
  double cargo_tons{0.0};
  double sensor_range_mkm{0.0};
};

struct InstallationDef {
  std::string id;
  std::string name;
  std::unordered_map<std::string, double> produces_per_day;
  double build_rate_tons_per_day{0.0}; // only used by shipyard
};

struct Ship {
  Id id{kInvalidId};
  std::string name;
  Id faction_id{kInvalidId};
  Id system_id{kInvalidId};

  // position is in-system (million km).
  Vec2 position_mkm{0.0, 0.0};

  // Design stats
  std::string design_id;
  double speed_km_s{0.0};
};

struct BuildOrder {
  std::string design_id;
  double tons_remaining{0.0};
};

struct Colony {
  Id id{kInvalidId};
  std::string name;
  Id faction_id{kInvalidId};
  Id body_id{kInvalidId};

  double population_millions{100.0};

  // Stockpiles
  std::unordered_map<std::string, double> minerals;

  // Installation counts
  std::unordered_map<std::string, int> installations;

  // Shipyard queue (very simplified)
  std::vector<BuildOrder> shipyard_queue;
};

struct Faction {
  Id id{kInvalidId};
  std::string name;

  double research_points{0.0};
  std::vector<std::string> known_techs;
};

struct StarSystem {
  Id id{kInvalidId};
  std::string name;

  // Position in galaxy map (arbitrary units)
  Vec2 galaxy_pos{0.0, 0.0};

  std::vector<Id> bodies;
  std::vector<Id> ships;
};

} // namespace nebula4x
