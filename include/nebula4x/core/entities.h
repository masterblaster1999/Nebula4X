#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

// --- world entities ---

enum class BodyType { Star, Planet, Moon, Asteroid, GasGiant };

enum class ShipRole { Freighter, Surveyor, Combatant, Unknown };

enum class ComponentType { Engine, Cargo, Sensor, Reactor, Weapon, Armor, Shield, ColonyModule, Unknown };

// Prototype AI / control flags.
//
// The game is primarily player-driven, but some scenarios include non-player
// factions (e.g. pirates). This enum allows the simulation to optionally
// generate orders for those factions.
//
// NOTE: "Player" here means "no simulation AI"; it does not necessarily
// mean "human-controlled" in a future multiplayer sense.
enum class FactionControl : std::uint8_t {
  Player = 0,
  AI_Passive = 1,
  AI_Explorer = 2,
  AI_Pirate = 3,
};

// Diplomatic stance between factions.
//
// This is currently used as a simple Rules-of-Engagement / auto-targeting gate:
// ships will only auto-engage factions they consider Hostile. The map is directed
// (A's stance toward B can differ from B's stance toward A).
//
// Backwards compatibility: if no stance is defined, factions default to Hostile,
// which matches the earlier prototype behavior of "all non-self factions are enemies".
enum class DiplomacyStatus : std::uint8_t {
  Friendly = 0,
  Neutral = 1,
  Hostile = 2,
};



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
  double colony_capacity_millions{0.0}; // colony module
  double power{0.0};               // reactor
  double weapon_damage{0.0};       // weapon (damage per day)
  double weapon_range_mkm{0.0};    // weapon
  double hp_bonus{0.0};            // armor
  double shield_hp{0.0};           // shield (max shield points)
  double shield_regen_per_day{0.0}; // shield (regen per day)
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
  double colony_capacity_millions{0.0};
  double max_hp{0.0};
  double max_shields{0.0};
  double shield_regen_per_day{0.0};
  double weapon_damage{0.0};
  double weapon_range_mkm{0.0};
};

struct InstallationDef {
  std::string id;
  std::string name;

  // Simple mineral production.
  std::unordered_map<std::string, double> produces_per_day;

  // Colony construction points produced per day (used for building installations).
  // If 0, this installation does not contribute.
  double construction_points_per_day{0.0};

  // Construction points required to build one unit of this installation.
  // If 0, construction completes instantly after paying mineral build costs.
  double construction_cost{0.0};

  // Mineral costs paid up-front to start building one unit of this installation.
  // If empty, no minerals are required.
  std::unordered_map<std::string, double> build_costs;

  // Only used by shipyard.
  double build_rate_tons_per_day{0.0};

  // Optional: mineral input costs for shipbuilding.
  // Interpreted as "units of mineral required per ton built".
  // If empty, shipbuilding is free (prototype/back-compat default).
  std::unordered_map<std::string, double> build_costs_per_ton;

  // Optional: in-system sensor range (used by sensor stations / ground radar).
  double sensor_range_mkm{0.0};

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

  // Cargo carried by this ship (prototype: mineral tons keyed by mineral name).
  // This enables basic logistics between colonies.
  std::unordered_map<std::string, double> cargo;

  // Automation: when enabled, the simulation will generate exploration orders
  // for this ship whenever it is idle (no queued orders).
  bool auto_explore{false};

  // Automation: when enabled, the simulation will generate freight (mineral hauling) orders
  // for this ship whenever it is idle (no queued orders).
  bool auto_freight{false};

  // Combat state.
  double hp{0.0};
  // Shield state (if the design has shields).
  //
  // A value < 0 indicates \"uninitialized\" (e.g. loaded from an older save) and
  // will be initialized to the design max when design stats are applied.
  double shields{-1.0};
};

struct BuildOrder {
  // Shipyard queue entry.
  //
  // - If refit_ship_id == kInvalidId, this is a "build new ship" order for design_id.
  // - Otherwise, this is a "refit existing ship" order for refit_ship_id, targeting design_id.
  std::string design_id;
  double tons_remaining{0.0};

  // The ship being refitted (optional).
  Id refit_ship_id{kInvalidId};

  bool is_refit() const { return refit_ship_id != kInvalidId; }
};

// Installation construction order for a colony.
struct InstallationBuildOrder {
  std::string installation_id;
  int quantity_remaining{0};

  // Progress state for the current unit being built.
  bool minerals_paid{false};
  double cp_remaining{0.0};
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

  // Colony construction queue (for building installations)
  std::vector<InstallationBuildOrder> construction_queue;
};

// A simple intel record for a detected ship.
//
// Prototype design goals:
// - no global omniscience: you can only act on ships you've detected
// - memory: when contact is lost, keep a last-known snapshot for UI / orders
struct Contact {
  Id ship_id{kInvalidId};
  Id system_id{kInvalidId};

  // Last day (Date::days_since_epoch) this ship was detected.
  int last_seen_day{0};

  // Snapshot at last detection.
  Vec2 last_seen_position_mkm{0.0, 0.0};
  std::string last_seen_name;
  std::string last_seen_design_id;
  Id last_seen_faction_id{kInvalidId};
};

struct Faction {
  Id id{kInvalidId};
  std::string name;

  // Control type (player vs simulation AI).
  FactionControl control{FactionControl::Player};

  // Diplomatic stances toward other factions (directed).
  //
  // NOTE: Missing entries default to Hostile for backward compatibility with
  // older saves and tests.
  std::unordered_map<Id, DiplomacyStatus> relations;



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

  // Exploration / map knowledge.
  // Systems this faction has discovered. Seeded from starting ships/colonies and
  // updated when ships transit jump points into new systems.
  std::vector<Id> discovered_systems;

  // Simple per-faction ship contact memory.
  // Key: ship id.
  std::unordered_map<Id, Contact> ship_contacts;
};

// A lightweight grouping of ships for UI / order-issuing convenience.
//
// Design goals:
// - Fleets are *not* a heavyweight simulation entity (no combat modifiers).
// - Fleets are persisted in saves.
// - A ship may belong to at most one fleet at a time.
//
// Fleets may optionally specify a formation. Formations are applied as a
// small "cohesion" helper inside tick_ships() for some movement / attack
// cohorts (currently: move-to-point + attack) so that fleet-issued orders
// don't result in every ship piling onto the exact same coordinates.

enum class FleetFormation : std::uint8_t {
  None = 0,
  LineAbreast = 1,
  Column = 2,
  Wedge = 3,
  Ring = 4,
};
struct Fleet {
  Id id{kInvalidId};
  std::string name;
  Id faction_id{kInvalidId};

  // Designated leader ship.
  //
  // If leader_ship_id becomes invalid (ship destroyed / removed), the simulation
  // will automatically pick a new leader from ship_ids when possible.
  Id leader_ship_id{kInvalidId};

  // Member ships.
  std::vector<Id> ship_ids;

  // Optional formation settings.
  FleetFormation formation{FleetFormation::None};
  double formation_spacing_mkm{1.0};
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


// --- simulation event log (persisted in saves) ---

enum class EventLevel { Info, Warn, Error };

// High-level grouping for persistent simulation events.
//
// This is intentionally coarse. The goal is to support basic UI filtering
// and future structured event handling without committing to a huge taxonomy.
enum class EventCategory {
  General,
  Research,
  Shipyard,
  Construction,
  Movement,
  Combat,
  Intel,
  Exploration,
};

struct SimEvent {
  // Monotonic event sequence number within a save.
  // Assigned by the simulation when the event is recorded.
  std::uint64_t seq{0};

  // Date::days_since_epoch() at the time the event occurred.
  std::int64_t day{0};

  EventLevel level{EventLevel::Info};

  // Coarse category for filtering.
  EventCategory category{EventCategory::General};

  // Optional context for quick UI navigation and filtering.
  // 0 (kInvalidId) means "not set".
  Id faction_id{kInvalidId};
  Id faction_id2{kInvalidId};
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};

  std::string message;
};


} // namespace nebula4x
