#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

// --- world entities ---

enum class BodyType { Star, Planet, Moon, Asteroid, GasGiant };

enum class ShipRole { Freighter, Surveyor, Combatant, Unknown };

enum class ComponentType { Engine, Cargo, Sensor, Reactor, Weapon, Armor, Unknown };

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

// Static component definition loaded from content files.
struct ComponentDef {
  std::string id;
  std::string name;
  ComponentType type{ComponentType::Unknown};

  double mass_tons{0.0};

  // Type-specific stats (0 means "not applicable").
  double speed_km_s{0.0};          // engine
  double cargo_tons{0.0};          // cargo
  double sensor_range_mkm{0.0};    // sensor
  double power{0.0};               // reactor
  double weapon_damage{0.0};       // weapon (damage per day)
  double weapon_range_mkm{0.0};    // weapon
  double hp_bonus{0.0};            // armor
};

// A ship design is essentially a named list of components + derived stats.
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
  double max_hp{0.0};
  double weapon_damage{0.0};
  double weapon_range_mkm{0.0};
};

struct InstallationDef {
  std::string id;
  std::string name;

  // Simple mineral production.
  std::unordered_map<std::string, double> produces_per_day;

  // Only used by shipyard.
  double build_rate_tons_per_day{0.0};

  // Only used by research labs.
  double research_points_per_day{0.0};
};

struct Ship {
  Id id{kInvalidId};
  std::string name;
  Id faction_id{kInvalidId};
  Id system_id{kInvalidId};

  // Position is in-system (million km).
  Vec2 position_mkm{0.0, 0.0};

  // Design reference.
  std::string design_id;

  // Cached design stats for fast ticking.
  double speed_km_s{0.0};

  // Combat state.
  double hp{0.0};
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

  // Banked research points waiting to be applied.
  double research_points{0.0};

  // Current research project.
  std::string active_research_id;
  double active_research_progress{0.0};
  std::vector<std::string> research_queue;

  // Known technologies.
  std::vector<std::string> known_techs;

  // Unlock lists (primarily for UI filtering / validation).
  std::vector<std::string> unlocked_components;
  std::vector<std::string> unlocked_installations;
};

// Jump points connect star systems.
struct JumpPoint {
  Id id{kInvalidId};
  std::string name;
  Id system_id{kInvalidId};

  // In-system position.
  Vec2 position_mkm{0.0, 0.0};

  // Bidirectional link (the jump point on the other side).
  Id linked_jump_id{kInvalidId};
};

struct StarSystem {
  Id id{kInvalidId};
  std::string name;

  // Position in galaxy map (arbitrary units)
  Vec2 galaxy_pos{0.0, 0.0};

  std::vector<Id> bodies;
  std::vector<Id> ships;
  std::vector<Id> jump_points;
};

} // namespace nebula4x
