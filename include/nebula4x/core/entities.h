#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/power.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

// --- world entities ---

enum class BodyType { Star, Planet, Moon, Asteroid, Comet, GasGiant };

enum class ShipRole { Freighter, Surveyor, Combatant, Unknown };

enum class ComponentType {
  Engine,
  FuelTank,
  Cargo,
  Sensor,
  Reactor,
  Weapon,
  Armor,
  Shield,
  ColonyModule,
  TroopBay,
  Unknown
};

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



// Sensor emissions control / operating mode.
//
// Sensor mode affects two things:
//  1) The *range* of this ship's sensors when acting as a sensor source.
//  2) The ship's *detectability* (signature multiplier) when it is the target.
//
// This is a lightweight "EMCON"-style mechanic meant to create scouting tradeoffs:
//  - Passive: harder to detect, but shorter sensor range.
//  - Normal: baseline.
//  - Active: longer sensor range, but easier to detect.
enum class SensorMode : std::uint8_t {
  Passive = 0,
  Normal = 1,
  Active = 2,
};


// Repair priority for shipyard repairs.
//
// When multiple damaged ships are docked at the same colony, shipyard repair
// capacity is allocated in priority order (High -> Normal -> Low).
enum class RepairPriority : std::uint8_t {
  Low = 0,
  Normal = 1,
  High = 2,
};




struct Body {
  Id id{kInvalidId};
  std::string name;
  BodyType type{BodyType::Planet};
  Id system_id{kInvalidId};

  // Mineral deposits on this body.
  //
  // Deposits are interpreted as remaining extractable tons per mineral type.
  // Mining installations (see InstallationDef::mining) will extract from these
  // deposits each day and transfer the mined resources to colony stockpiles on
  // this body.
  //
  // Back-compat / mods:
  // - If a mineral key is missing, extraction for that mineral is treated as
  //   "unlimited" (i.e., mines will produce as they did in older versions).
  // - If a mineral is present with value <= 0, that deposit is depleted.
  std::unordered_map<std::string, double> mineral_deposits;

  // Orbital mechanics (prototype).
  //
  // - If parent_body_id == kInvalidId, this body orbits the system origin.
  // - Otherwise, it orbits the referenced parent body (which must be in the same system).
  Id parent_body_id{kInvalidId};

  // Keplerian orbit (2D prototype).
  //
  // orbit_radius_mkm: semi-major axis "a" in million km (mkm). For circular orbits
  //   (orbit_eccentricity == 0) this is equivalent to the orbit radius.
  // orbit_period_days: orbital period in days (used to advance mean anomaly).
  // orbit_phase_radians: mean anomaly at epoch (radians). For circular orbits,
  //   this is also the true anomaly / polar angle.
  // orbit_eccentricity: 0=circular, 0<e<1 elliptical.
  // orbit_arg_periapsis_radians: orientation of periapsis in the orbital plane.
  double orbit_radius_mkm{0.0};            // semi-major axis a (mkm)
  double orbit_period_days{0.0};           // days
  double orbit_phase_radians{0.0};         // mean anomaly at epoch (rad)
  double orbit_eccentricity{0.0};          // e
  double orbit_arg_periapsis_radians{0.0}; // ω (rad)

  // Optional physical metadata (procedural generation / UI).
  //
  // These values are not currently used by core simulation mechanics.
  double mass_solar{0.0};        // stars: solar masses
  double luminosity_solar{0.0};  // stars: solar luminosities
  double mass_earths{0.0};       // planets/moons/asteroids: Earth masses
  double radius_km{0.0};         // approximate radius (km)
  double surface_temp_k{0.0};    // approximate equilibrium temperature (K)

  // Optional: atmospheric pressure (in atm) and terraforming targets.
  //
  // These values are primarily used by the terraforming prototype.
  // If terraforming_target_* are <= 0, terraforming is treated as "disabled".
  double atmosphere_atm{0.0};
  double terraforming_target_temp_k{0.0};
  double terraforming_target_atm{0.0};
  bool terraforming_complete{false};

  // Cached position for current sim date (absolute, after applying parent orbits).
  Vec2 position_mkm{0.0, 0.0};
};

// Static component definition loaded from content files.
struct ComponentDef {
  std::string id;
  std::string name;
  ComponentType type{ComponentType::Unknown};

  double mass_tons{0.0};

  // Visibility / sensor signature multiplier.
  //
  // Ship designs derive a signature multiplier from their components. Sensor detection then scales effective detection ranges using:
  //   effective_range = sensor_range_mkm * target_signature_multiplier
  //
  // 1.0 = normal visibility. Lower values are harder to detect.
  double signature_multiplier{1.0};


  // Type-specific stats (0 means "not applicable").
  double speed_km_s{0.0};          // engine
  double fuel_use_per_mkm{0.0};    // engine (tons per million km)
  double fuel_capacity_tons{0.0};  // fuel tank
  double cargo_tons{0.0};          // cargo
  double sensor_range_mkm{0.0};    // sensor
  double colony_capacity_millions{0.0}; // colony module
  // Power model (prototype):
  // - Reactors contribute positive power_output.
  // - Other components may draw power_use.
  // Units are arbitrary "power points"; the simulation uses them only for
  // simple load-shedding (offline sensors/weapons/shields/engines) when a
  // design's total power use exceeds its generation.
  double power_output{0.0};        // reactor
  double power_use{0.0};           // consumer
  double weapon_damage{0.0};       // weapon (damage per day)
  double weapon_range_mkm{0.0};    // weapon
  double hp_bonus{0.0};            // armor
  double shield_hp{0.0};           // shield (max shield points)
  double shield_regen_per_day{0.0}; // shield (regen per day)

  // Troop bay (abstract "strength" points).
  double troop_capacity{0.0};
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
  double fuel_capacity_tons{0.0};
  double fuel_use_per_mkm{0.0};
  double cargo_tons{0.0};
  double sensor_range_mkm{0.0};
  // Visibility / sensor signature multiplier for this design.
  // 1.0 = normal; lower values are harder to detect.
  double signature_multiplier{1.0};
  double colony_capacity_millions{0.0};

  // Power budgeting.
  double power_generation{0.0};
  double power_use_total{0.0};
  double power_use_engines{0.0};
  double power_use_sensors{0.0};
  double power_use_weapons{0.0};
  double power_use_shields{0.0};
  double max_hp{0.0};
  double max_shields{0.0};
  double shield_regen_per_day{0.0};
  double weapon_damage{0.0};
  double weapon_range_mkm{0.0};

  // Derived troop capacity (from troop bays).
  double troop_capacity{0.0};
};

struct InstallationDef {
  std::string id;
  std::string name;

  // If true, `produces_per_day` is interpreted as extraction from the colony's
  // underlying body mineral deposits (Body::mineral_deposits). Extraction is
  // capped by remaining deposits.
  //
  // If false, `produces_per_day` creates minerals out of thin air (prototype /
  // back-compat behavior).
  bool mining{false};

  // Simple mineral production.
  std::unordered_map<std::string, double> produces_per_day;


  // Optional: mineral input consumption per day.
  //
  // If non-empty, non-mining installations will consume these minerals each day
  // in tick_colonies() and will scale their output down if inputs are insufficient.
  // This enables simple "industry recipes" like refineries (e.g. minerals -> Fuel).
  std::unordered_map<std::string, double> consumes_per_day;

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

  // Optional: orbital / planetary weapon platform.
  //
  // If weapon_damage > 0 and weapon_range_mkm > 0, colonies that have this
  // installation will automatically fire at detected hostile ships that enter
  // range during Simulation::tick_combat(). Damage is applied as normal weapon
  // damage (shields absorb first, then hull).
  //
  // Damage is expressed in the same abstract units as ShipDesign::weapon_damage
  // and is applied once per day per colony as an aggregated battery across all
  // qualifying installations.
  double weapon_damage{0.0};
  double weapon_range_mkm{0.0};

  // Only used by research labs.
  double research_points_per_day{0.0};

  // Optional: terraforming (points per day).
  double terraforming_points_per_day{0.0};

  // Optional: troop training (points per day).
  double troop_training_points_per_day{0.0};

  // Optional: habitation / life support capacity.
  //
  // Expressed as population in *millions* that can be supported in fully hostile
  // conditions (habitability = 0.0). The simulation uses this in combination
  // with the computed body habitability to determine whether a colony has
  // sufficient housing / life support to sustain its population.
  //
  // See: SimConfig::enable_habitability.
  double habitation_capacity_millions{0.0};

  // Optional: fortifications (static defense value).
  double fortification_points{0.0};
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

  // Embarked ground troops (abstract "strength" points).
  // Interpreted relative to the ship design's troop_capacity.
  double troops{0.0};

  // Embarked colonists / passengers (population, in millions).
  // Interpreted relative to the ship design's colony_capacity_millions.
  double colonists_millions{0.0};

  // Automation: when enabled, the simulation will generate exploration orders
  // for this ship whenever it is idle (no queued orders).
  bool auto_explore{false};

  // Automation: when enabled, the simulation will generate freight (mineral hauling) orders
  // for this ship whenever it is idle (no queued orders).
  bool auto_freight{false};

  // Automation: when enabled, the simulation will route this ship to refuel when
  // it is low on fuel and idle.
  //
  // Notes:
  // - Refueling itself is handled by Simulation::tick_refuel(), but auto-refuel
  //   is responsible for generating movement orders to reach a friendly colony.
  // - This is intentionally compatible with Auto-explore/Auto-freight: when fuel
  //   is low, auto-refuel will queue a refuel trip first, then the ship can
  //   resume its other automation once refueled.
  bool auto_refuel{false};

  // Fraction of fuel capacity at which auto-refuel triggers.
  //
  // Example: 0.25 means "refuel when below 25%".
  double auto_refuel_threshold_fraction{0.25};

  // Automation: when enabled, the simulation will route this ship to a friendly
  // shipyard for repairs when it is damaged and idle.
  //
  // Notes:
  // - Repairs themselves are handled by Simulation::tick_repairs().
  // - Auto-refuel runs first, so ships will prefer to resolve low-fuel situations
  //   before attempting to seek repairs.
  bool auto_repair{false};

  // Fraction of max HP at which auto-repair triggers.
  //
  // Example: 0.75 means "seek repairs when below 75% HP".
  double auto_repair_threshold_fraction{0.75};

  // Repair scheduling priority when docked at a shipyard.
  // Higher priority ships are repaired first when shipyard capacity is limited.
  RepairPriority repair_priority{RepairPriority::Normal};


  // Runtime power policy (enabled subsystems + load shedding priority).
  //
  // This is independent of the ship design's static power generation/usage
  // numbers and allows the player/AI to, for example, disable weapons to keep
  // sensors online on an underpowered scout.
  ShipPowerPolicy power_policy{};

  // Sensor emissions control (EMCON).
  //
  // This setting modifies both this ship's sensor range (when acting as a sensor source)
  // and its detectability (signature multiplier) when targeted by others.
  SensorMode sensor_mode{SensorMode::Normal};


  // Combat state.
  double hp{0.0};

  // Fuel state (if the design defines fuel).
  //
  // A value < 0 indicates "uninitialized" (e.g. loaded from an older save) and
  // will be initialized to the design max when design stats are applied.
  double fuel_tons{-1.0};

  // Shield state (if the design has shields).
  //
  // A value < 0 indicates \"uninitialized\" (e.g. loaded from an older save) and
  // will be initialized to the design max when design stats are applied.
  double shields{-1.0};
};

// A destroyed ship may leave a salvageable wreck.
//
// Wrecks are intentionally lightweight (a position + a bag of minerals). The
// first implementation treats salvage as recoverable minerals rather than
// persistent component objects.
struct Wreck {
  Id id{kInvalidId};
  std::string name;
  Id system_id{kInvalidId};

  // Position in-system (million km).
  Vec2 position_mkm{0.0, 0.0};

  // Salvageable minerals stored in this wreck (tons keyed by mineral name).
  // Empty means "no salvage".
  std::unordered_map<std::string, double> minerals;

  // Optional metadata for UI / debugging.
  Id source_ship_id{kInvalidId};
  Id source_faction_id{kInvalidId};
  std::string source_design_id;

  // Creation day (Date::days_since_epoch) for optional decay / analytics.
  int created_day{0};
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

  // If true, this build order was auto-queued by faction ship design targets.
  // (Only meaningful for new builds; refit orders are always manual.)
  bool auto_queued{false};

  bool is_refit() const { return refit_ship_id != kInvalidId; }
};

// Installation construction order for a colony.
struct InstallationBuildOrder {
  std::string installation_id;
  int quantity_remaining{0};
  // If true, this order was auto-queued by colony installation targets.
  bool auto_queued{false};

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

  // Manual mineral reserve thresholds (UI/auto-freight).
  //
  // Auto-freight will not export minerals below these values.
  // Missing entries imply a reserve of 0.
  std::unordered_map<std::string, double> mineral_reserves;

  // Desired stockpile targets (UI/auto-freight).
  //
  // When non-zero, auto-freight will attempt to *import* minerals to reach these
  // amounts at the colony, and will also avoid exporting below this target.
  //
  // This is complementary to mineral_reserves:
  // - mineral_reserves: 'never export below X'
  // - mineral_targets:  'try to keep at least X on-hand (import if needed)'
  //
  // Missing entries imply a target of 0.
  std::unordered_map<std::string, double> mineral_targets;

  // Desired installation counts (auto-build).
  //
  // When targets are set, the simulation will automatically enqueue construction
  // orders (marked InstallationBuildOrder::auto_queued) to build up to the desired
  // counts without consuming or reordering manually-queued construction.
  //
  // Targets never demolish installations. Lowering a target will only prune
  // auto-queued *pending* units (and will not cancel a unit already in-progress).
  // Missing entries imply a target of 0.
  std::unordered_map<std::string, int> installation_targets;

  // Installation counts
  std::unordered_map<std::string, int> installations;

  // Ground forces stationed at this colony (abstract "strength" points).
  double ground_forces{0.0};

  // Training queue for new troops at this colony (strength points remaining).
  double troop_training_queue{0.0};

  // Shipyard queue (very simplified)
  std::vector<BuildOrder> shipyard_queue;

  // Colony construction queue (for building installations)
  std::vector<InstallationBuildOrder> construction_queue;
};

// A persistent ground battle at a colony.
//
// This is intentionally minimal: the simulation resolves battles day-by-day.
struct GroundBattle {
  Id colony_id{kInvalidId};
  Id system_id{kInvalidId};

  Id attacker_faction_id{kInvalidId};
  Id defender_faction_id{kInvalidId};

  double attacker_strength{0.0};
  double defender_strength{0.0};

  int days_fought{0};
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

  // Automation: desired counts of ship designs to maintain.
  //
  // When non-empty, the simulation will automatically manage *auto-queued*
  // shipyard build orders across this faction's colonies to reach these targets.
  // Manual build/refit orders are never modified.
  std::unordered_map<std::string, int> ship_design_targets;

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
  Diplomacy,
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
