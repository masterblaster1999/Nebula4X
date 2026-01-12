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
  Mining,
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


// Symmetric diplomacy agreements (treaties) between two factions.
//
// Treaties are layered on top of directed DiplomacyStatus stances:
//   - Alliance forces mutual friendliness.
//   - Ceasefire / NonAggressionPact forces at least Neutral (prevents auto-engagement).
//
// Treaties are stored in GameState::treaties and may have a duration.
// duration_days < 0 means "indefinite".
//
// NOTE: Treaties are intentionally lightweight; negotiation/AI acceptance can be
// built on top of this primitive later.
enum class TreatyType : std::uint8_t {
  Ceasefire = 0,
  NonAggressionPact = 1,
  Alliance = 2,
  TradeAgreement = 3,
};

struct Treaty {
  Id id{kInvalidId};
  // Always stored normalized as (min(faction_a, faction_b), max(...)) so treaties
  // are inherently symmetric.
  Id faction_a{kInvalidId};
  Id faction_b{kInvalidId};
  TreatyType type{TreatyType::Ceasefire};

  // Simulation day (Date::days_since_epoch) when the treaty was signed/renewed.
  std::int64_t start_day{0};

  // Duration in days. <0 => indefinite.
  int duration_days{-1};
};

// Directed diplomacy "offers" / proposals.
//
// Unlike Treaties, which are immediately active agreements, offers represent a
// pending proposal from one faction to another that must be accepted (or can
// be declined / expire).
//
// This is a lightweight negotiation layer intended primarily for AI->player
// interaction and future diplomacy expansion.
//
// treaty_duration_days < 0 means "indefinite" if accepted.
// expire_day < 0 means the offer never expires.
struct DiplomaticOffer {
  Id id{kInvalidId};

  // Directional: from -> to.
  Id from_faction_id{kInvalidId};
  Id to_faction_id{kInvalidId};

  // The treaty that will be created if the offer is accepted.
  TreatyType treaty_type{TreatyType::Ceasefire};

  // Treaty duration in days (<0 => indefinite).
  int treaty_duration_days{-1};

  // Day the offer was created (Date::days_since_epoch).
  int created_day{0};

  // Day the offer expires and is auto-removed (<0 => never).
  int expire_day{-1};

  // Optional free-form note / flavor text.
  std::string message;
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

// Ship tactical doctrine / engagement settings.
//
// This is a deliberately lightweight, deterministic knob-set that influences
// how ships execute AttackShip orders (movement/positioning, not targeting).
//
// Motivation:
// - Pure missile ships previously closed to point-blank range when attacking
//   because only beam weapon range was considered by movement AI.
// - Mixed-weapon ships benefit from explicit player control over whether to
//   stand off at missile range, close for beams, or hold a custom standoff.
//
// Range selection mode used to choose the base weapon range for standoff.
enum class EngagementRangeMode : std::uint8_t {
  // Prefer beam range if available, else missile range, else a small minimum.
  Auto = 0,

  // Use beam weapon range (ShipDesign::weapon_range_mkm).
  Beam = 1,

  // Use missile range (ShipDesign::missile_range_mkm).
  Missile = 2,

  // Use max(beam_range, missile_range).
  Max = 3,

  // Use min positive among beam/missile.
  Min = 4,

  // Use custom_range_mkm.
  Custom = 5,
};

struct ShipCombatDoctrine {
  EngagementRangeMode range_mode{EngagementRangeMode::Auto};

  // Fraction (0..1) of the selected base range to maintain as standoff.
  //
  // Example: 0.9 means "try to stay at 90% of range" so weapons are in range
  // without constantly entering/leaving the boundary due to small movements.
  double range_fraction{0.9};

  // Minimum engagement range (mkm) for ships with no usable ranged weapons,
  // or as a safety floor.
  double min_range_mkm{0.1};

  // Used when range_mode == Custom.
  double custom_range_mkm{0.0};

  // When enabled, ships will actively back off (kite) if the target closes
  // inside their desired standoff range.
  bool kite_if_too_close{false};

  // Hysteresis for kiting decisions as a fraction of desired_range.
  //
  // Example: 0.10 => start backing off when distance < 90% of desired_range.
  double kite_deadband_fraction{0.10};
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
  // - If mineral_deposits is empty, all deposits are treated as "unlimited" and
  //   mining behaves as it did in older versions (no depletion).
  // - If mineral_deposits is non-empty, missing keys mean "no deposit".
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

// Electronic warfare.
//
// ECM reduces the effective detection / tracking quality of opposing sensors.
// ECCM counteracts ECM.
//
// Values are aggregated per-ship-design and interpreted as a multiplier
// roughly proportional to (1 + eccm_strength) / (1 + ecm_strength).
double ecm_strength{0.0};
double eccm_strength{0.0};



  // Type-specific stats (0 means "not applicable").
  double speed_km_s{0.0};          // engine
  double fuel_use_per_mkm{0.0};    // engine (tons per million km)
  double fuel_capacity_tons{0.0};  // fuel tank
  double cargo_tons{0.0};          // cargo
  double mining_tons_per_day{0.0}; // mining (tons/day)
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

  // Missile weapons (prototype: discrete salvos with time-of-flight).
  //
  // - missile_damage is applied when the salvo reaches its target.
  // - missile_speed_mkm_per_day controls time-to-impact.
  // - missile_reload_days is the cooldown between launches (per-ship; see Ship::missile_cooldown_days).
  double missile_damage{0.0};
  double missile_range_mkm{0.0};
  double missile_speed_mkm_per_day{0.0};
  double missile_reload_days{0.0};

  // Optional magazine capacity per launcher (number of salvos).
  // 0 => unlimited ammo (legacy behavior).
  int missile_ammo{0};

  // Point defense (anti-missile interception).
  //
  // Interpreted as damage that can be applied to incoming missile damage at impact.
  double point_defense_damage{0.0};
  double point_defense_range_mkm{0.0};
  double hp_bonus{0.0};            // armor
  double shield_hp{0.0};           // shield (max shield points)
  double shield_regen_per_day{0.0}; // shield (regen per day)

  // Thermal / heat model (optional).
  // Content can assign heat generation, dissipation (cooling), and capacity
  // to components to model reactors/engines/weapons that run hot as well as
  // dedicated radiators/heat sinks.
  double heat_generation_per_day{0.0};
  double heat_dissipation_per_day{0.0};
  double heat_capacity{0.0};

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
  double mining_tons_per_day{0.0};
  double sensor_range_mkm{0.0};
  // Visibility / sensor signature multiplier for this design.
  // 1.0 = normal; lower values are harder to detect.
  double signature_multiplier{1.0};

// Electronic warfare (aggregated from components).
double ecm_strength{0.0};
double eccm_strength{0.0};

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

  // Thermal / heat model (optional).
  // These are additive bonuses that modify the Simulation's base thermal model
  // (which derives baseline heat behavior from mass and power budgets).
  double heat_capacity_bonus{0.0};
  double heat_generation_bonus_per_day{0.0};
  double heat_dissipation_bonus_per_day{0.0};

    double weapon_damage{0.0};
  double weapon_range_mkm{0.0};

  // Missile weapons (discrete salvos with time-of-flight).
  double missile_damage{0.0};
  double missile_range_mkm{0.0};
  double missile_speed_mkm_per_day{0.0};
  double missile_reload_days{0.0};

  // Derived missile launcher count (number of weapon components with missile_damage > 0).
  int missile_launcher_count{0};

  // Total missile ammo capacity across all launchers (salvos).
  // 0 => unlimited ammo (legacy behavior).
  int missile_ammo_capacity{0};

  // Point defense (anti-missile interception).
  double point_defense_damage{0.0};
  double point_defense_range_mkm{0.0};

  // Derived troop capacity (from troop bays).
  double troop_capacity{0.0};
};

// Resource definitions are content-driven metadata for mineral / material types.
//
// The core simulation generally treats resources as string-keyed quantities
// (in stockpiles, cargo holds, deposits, etc.). ResourceDef is an optional
// catalog used for UI grouping and for content validation (catching typos).
struct ResourceDef {
  std::string id;
  std::string name;

  // Free-form category tag used by UI (e.g. "metal", "volatile", "fuel").
  // If empty, defaults to "mineral".
  std::string category{"mineral"};

  // If true, this resource can appear in Body::mineral_deposits and be mined.
  bool mineable{true};

  // Optional research value (RP) gained per ton when salvaging this resource
  // from a wreck.
  //
  // This intentionally does *not* apply to mining or colony industry; it is
  // only used by the wreck-salvage mechanic to reward exploration and combat
  // recovery operations.
  double salvage_research_rp_per_ton{0.0};
};

struct InstallationDef {
  std::string id;
  std::string name;

  // If true, this installation extracts minerals from the underlying body's
  // mineral deposits (Body::mineral_deposits) when present.
  //
  // Mining model:
  //  - If mining_tons_per_day > 0 and the body has a non-empty mineral_deposits map,
  //    the installation provides a generic extraction capacity (tons/day) that is
  //    distributed across all non-depleted deposits on that body (weighted by
  //    remaining tons).
  //  - Otherwise, produces_per_day is interpreted as per-mineral extraction rates
  //    (legacy behavior) and is capped by remaining deposits when those deposits
  //    exist.
  //
  // Back-compat / mods:
  //  - If a body has an empty mineral_deposits map, mining behaves as "unlimited"
  //    (mines produce without depletion as in early versions).
  bool mining{false};

  // Generic mining capacity in tons per day (see mining model above).
  double mining_tons_per_day{0.0};

  // Mineral production per day.
  //
  // For non-mining installations, this creates minerals out of thin air (prototype
  // industry output) and may be input-limited by consumes_per_day.
  //
  // For mining installations with mining_tons_per_day == 0, this is interpreted as
  // fixed extraction rates per mineral (legacy mining model).
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

  // Optional: crew training (points per day).
  //
  // This represents on-planet training infrastructure for ship crews (simulated
  // as a colony-wide training pool distributed across docked ships).
  double crew_training_points_per_day{0.0};

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

  // Approximate in-system velocity computed from the most recent movement tick.
  //
  // Units: million-km per simulated day.
  //
  // This is used for combat tracking/evasion and for UI feedback, and is stored
  // in saves so combat behavior is consistent across save/load boundaries.
  Vec2 velocity_mkm_per_day{0.0, 0.0};

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
  // When enabled, this ship will, when idle, automatically transport ground troops
  // between owned colonies to satisfy garrison targets and (optionally) reinforce
  // ongoing defensive ground battles.
  bool auto_troop_transport{false};


  // Automation: when enabled, the simulation will generate salvage orders for
  // this ship whenever it is idle (no queued orders).
  //
  // Auto-salvage is intended for freighters and dedicated recovery craft. When
  // a ship returns with minerals in its cargo hold, it will attempt to deliver
  // them to a friendly colony before seeking more wrecks.
  bool auto_salvage{false};

  // Automation: when enabled, the simulation will generate mobile mining orders
  // for this ship whenever it is idle (no queued orders).
  //
  // Auto-mine is intended for dedicated mining ships that extract minerals
  // directly from asteroid/comet deposits into their cargo holds.
  bool auto_mine{false};

  // Optional: home colony for auto-mine deliveries.
  //
  // If set to a valid colony id owned by this ship's faction, the auto-mine
  // routine will prefer delivering mined cargo to this colony.
  // If unset/invalid, the ship will deliver to the nearest friendly colony.
  Id auto_mine_home_colony_id{kInvalidId};

  // Optional mineral filter for auto-mine.
  //
  // If empty, auto-mine will mine any available minerals on the chosen body.
  // Otherwise, the ship will target this specific mineral.
  std::string auto_mine_mineral;

  // Automation: when enabled, the simulation will generate colonization orders
  // for this ship whenever it is idle (no queued orders).
  //
  // Only ships whose design includes a colony module (design.colony_capacity_millions > 0)
  // can meaningfully use this flag.
  bool auto_colonize{false};

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

  // Automation: when enabled, this ship will act as a fuel tanker and will
  // automatically travel to friendly idle ships that are low on fuel,
  // transferring fuel ship-to-ship.
  //
  // Auto-tanker only triggers when the tanker itself is idle (no queued orders).
  // The tanker will never transfer fuel below its configured reserve fraction.
  bool auto_tanker{false};

  // Fraction of this ship's fuel capacity that is reserved and will not be
  // transferred away by auto-tanker.
  //
  // Example: 0.25 means keep at least 25% of capacity as a safety reserve.
  double auto_tanker_reserve_fraction{0.25};

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

  // Auto rearm (for finite-ammo missile ships).
  bool auto_rearm{false};

  // Fraction of magazine capacity at which auto-rearm triggers.
  double auto_rearm_threshold_fraction{0.25};

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

  // Tactical doctrine for AttackShip positioning.
  //
  // This does *not* affect weapon targeting selection; it only influences how
  // ships choose a desired standoff distance and whether they kite when
  // engaged.
  ShipCombatDoctrine combat_doctrine{};


  // Combat state.
  double hp{0.0};

  // Maintenance / readiness condition (0..1). 1 = fully maintained.
  // Only affects simulation if cfg.enable_ship_maintenance is true.
  double maintenance_condition{1.0};

  // Crew training / experience (grade points).
  //
  // Points are mapped to a combat effectiveness modifier (hit chance / reload
  // / boarding) via Simulation::crew_grade_bonus_for_points().
  //
  // A value < 0 indicates "uninitialized" (older saves) and will be
  // initialized to SimConfig::crew_initial_grade_points when design stats are applied.
  double crew_grade_points{-1.0};

  // Missile weapon cooldown (days until the ship can launch another salvo).
  // 0 = ready.
  double missile_cooldown_days{0.0};

  // Missile ammo remaining (salvos).
  //
  // - Only used when the ship's design has a finite missile_ammo_capacity.
  // - -1 is treated as "uninitialized" for legacy saves and will be
  //   initialized to full capacity when design stats are applied.
  int missile_ammo{-1};

  // Boarding attempt cooldown (days).
  //
  // Boarding is a discrete action intended to happen roughly once per day in
  // the prototype. With sub-day turn ticks, we track a per-ship cooldown so
  // boarding doesn't occur multiple times per day.
  double boarding_cooldown_days{0.0};

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

  // Subsystem integrity (0..1).
  //
  // Integrity always affects ship performance via the Simulation::ship_subsystem_*_multiplier
  // helpers (speed / weapon output / sensor range / shields). Values start at 1.0 and are
  // reduced by optional mechanics.
  //
  // When SimConfig::enable_ship_subsystem_damage is enabled, combat can inflict critical
  // hits that reduce subsystem integrity. Other systems (e.g. deterministic maintenance
  // failures) may also reduce integrity when enabled.
  //
  // These are intentionally lightweight approximations (not per-component) and are typically
  // repaired at shipyards (see tick_repairs).
  double engines_integrity{1.0};
  double weapons_integrity{1.0};
  double sensors_integrity{1.0};
  double shields_integrity{1.0};

  // Thermal / heat state.
  // Heat is integrated each tick when SimConfig::enable_ship_heat is enabled.
  // heat_state is a small runtime bucket used to throttle repeated warnings;
  // it is intentionally not serialized.
  double heat{0.0};
  std::uint8_t heat_state{0};
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
  // Use a 64-bit day counter to match Date (and avoid narrowing warnings on some compilers).
  std::int64_t created_day{0};
};




// Exploration anomalies.
//
// An anomaly is a persistent point of interest in a star system that can be
// investigated by ships for rewards (research points, optional unlocks, etc).
//
// This is intentionally lightweight and content/mod-friendly:
// - kind is an arbitrary tag ("signal", "ruins", "phenomenon", ...).
// - investigation_days is the time required on-station to resolve the anomaly.
// - research_reward is an amount of research points to award on completion.
// - unlock_component_id is an optional component id to unlock for the faction.
//
// Resolution metadata is stored to support event logs / analytics.
struct Anomaly {
  Id id{kInvalidId};
  std::string name;
  std::string kind;

  Id system_id{kInvalidId};
  Vec2 position_mkm{0.0, 0.0};

  int investigation_days{1};
  double research_reward{0.0};
  std::string unlock_component_id;

  // Optional mineral cache reward (tons keyed by mineral name).
  //
  // On resolution, investigating ships will load as much as possible into cargo.
  // Any overflow becomes a salvageable Wreck (if wrecks are enabled).
  std::unordered_map<std::string, double> mineral_reward;

  // Optional hazard applied when the anomaly is resolved.
  //
  // hazard_chance is a probability in [0,1] that a hazard triggers.
  // hazard_damage is applied as non-lethal damage (shields first, then hull),
  // with hull HP clamped to a minimum of 1.
  double hazard_chance{0.0};
  double hazard_damage{0.0};

  bool resolved{false};
  Id resolved_by_faction_id{kInvalidId};
  std::int64_t resolved_day{0};
};

// Missile salvos (prototype).
//
// A salvo is created when a ship launches missiles at a target ship. It travels for
// a number of days (eta_days_remaining) and applies its damage on arrival.
// Point defense can reduce/negate the damage at impact time.
struct MissileSalvo {
  Id id{kInvalidId};
  Id system_id{kInvalidId};

  Id attacker_ship_id{kInvalidId};
  Id attacker_faction_id{kInvalidId};

  Id target_ship_id{kInvalidId};
  Id target_faction_id{kInvalidId};

  // Remaining damage payload that will be applied on impact.
  // (Point defense can reduce this while the salvo is in flight.)
  double damage{0.0};

  // Initial damage payload at launch time.
  // This enables clearer combat messaging ("payload" vs "leaked") and
  // UI visualization without having to infer history.
  double damage_initial{0.0};

  // --- Flight model (homing + range limit) ---
  //
  // speed_mkm_per_day is the salvo's flight speed at launch.
  //
  // When enable_missile_homing is true, the salvo's position advances by this
  // speed each combat tick, steering toward a predicted intercept point.
  double speed_mkm_per_day{0.0};

  // Remaining range (mkm) the salvo can travel before self-destructing.
  //
  // When missile_range_limits_flight is disabled, this may be set to a very
  // large value.
  double range_remaining_mkm{0.0};

  // Current salvo position (for homing missiles and UI overlays).
  //
  // For legacy saves, this is derived from launch/target positions + ETA.
  Vec2 pos_mkm{0.0, 0.0};

  // Guidance snapshot at launch (used by hit chance / ECCM).
  // These values are best-effort and may be 0 in legacy saves.
  double attacker_eccm_strength{0.0};
  double attacker_sensor_mkm_raw{0.0};

  // Total flight time at launch (days).
  // Stored as a double so sub-day turn ticks can decrement by fractional days
  // (e.g. 1h = 1/24d).
  double eta_days_total{0.0};

  // Estimated days until impact remaining.
  //
  // When enable_missile_homing is enabled, this is recomputed from current
  // geometry (distance / speed) each tick for UI convenience.
  double eta_days_remaining{0.0};

  // For UI visualization (map overlay): the launch and target positions used
  // to build a straight-line "missile track".
  Vec2 launch_pos_mkm{0.0, 0.0};
  Vec2 target_pos_mkm{0.0, 0.0};
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

  // Optional garrison automation target.
  //
  // When > 0, the simulation will automatically keep enough (auto-queued)
  // troop training in the queue so that ground_forces + queued_training reaches
  // this target.
  //
  // This is a pure QoL feature: it doesn't change the training rules, it
  // simply keeps the training queue topped up.
  double garrison_target_strength{0.0};

  // Training queue for new troops at this colony (strength points remaining).
  double troop_training_queue{0.0};

  // Portion of troop_training_queue that was auto-queued by garrison_target_strength.
  //
  // This allows the simulation to prune only the auto-generated portion when
  // the target is reduced, while leaving manual training intact.
  double troop_training_auto_queued{0.0};

  // Shipyard queue (very simplified)
  std::vector<BuildOrder> shipyard_queue;

  // Colony construction queue (for building installations)
  std::vector<InstallationBuildOrder> construction_queue;
};



// A reusable set of colony automation knobs (targets/reserves).
//
// This is a pure QoL/presets feature: profiles can be applied to colonies to
// quickly configure auto-build/auto-freight + garrison targets.
struct ColonyAutomationProfile {
  // Desired installation counts (auto-build).
  std::unordered_map<std::string, int> installation_targets;

  // Stockpile reserve floors (auto-freight export).
  std::unordered_map<std::string, double> mineral_reserves;

  // Desired stockpile targets (auto-freight import).
  std::unordered_map<std::string, double> mineral_targets;

  // Optional garrison automation target.
  double garrison_target_strength{0.0};
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

  // Accumulated damage to the defender's fortifications (in fortification points).
  //
  // During an active ground battle, attackers can progressively degrade the
  // effective fortification points of the colony. This reduces the defender's
  // combat bonuses while the battle continues. When the battle resolves, the
  // accumulated damage is applied by destroying fortification installations
  // on the colony.
  double fortification_damage_points{0.0};

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

  // Estimated 1-sigma position uncertainty (radius) at last detection.
  //
  // This is used to render uncertainty rings for stale contacts and to guide
  // simple search behavior when pursuing a lost contact.
  double last_seen_position_uncertainty_mkm{0.0};

  // Previous snapshot (for simple velocity estimation).
  //
  // Only populated when we have at least two detections in the same system.
  int prev_seen_day{-1};
  Vec2 prev_seen_position_mkm{0.0, 0.0};
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

  // Reverse engineering progress accumulated from salvaging wrecks.
  //
  // Map: component_id -> accumulated reverse-engineering points.
  // When progress reaches the required threshold (see SimConfig), the component
  // is added to unlocked_components and the progress entry is removed.
  std::unordered_map<std::string, double> reverse_engineering_progress;

  // Automation: desired counts of ship designs to maintain.
  //
  // When non-empty, the simulation will automatically manage *auto-queued*
  // shipyard build orders across this faction's colonies to reach these targets.
  // Manual build/refit orders are never modified.
  std::unordered_map<std::string, int> ship_design_targets;

  // Player-defined colony automation profiles (presets).
  //
  // Profiles can be applied to colonies to quickly configure installation
  // targets, mineral reserves/targets, and garrison targets.
  std::unordered_map<std::string, ColonyAutomationProfile> colony_profiles;

  // Colony founding defaults (auto-applied to new colonies).
  //
  // When enabled, the simulation will apply this profile to newly established
  // colonies created via ColonizeBody (manual or AI).
  bool auto_apply_colony_founding_profile{false};

  // Profile values to apply when auto_apply_colony_founding_profile is enabled.
  ColonyAutomationProfile colony_founding_profile;

  // Optional UI label for the above profile (does not affect simulation).
  std::string colony_founding_profile_name;


  // Exploration / map knowledge.
  // Systems this faction has discovered. Seeded from starting ships/colonies and
  // updated when ships transit jump points into new systems.
  std::vector<Id> discovered_systems;

  // Anomalies this faction has discovered (points of interest).
  //
  // Unlike systems/jump links, anomalies are discovered via sensor coverage
  // (ships/colonies) and persist once found.
  std::vector<Id> discovered_anomalies;

  // Jump point surveys (fog-of-war route knowledge).
  // When fog-of-war is enabled, the UI + route planner will only consider
  // jump links that have been surveyed by the viewing faction.
  std::vector<Id> surveyed_jump_points;

  // Incremental jump point survey progress (survey points accumulated so far).
  // Key: jump point id. Value: progress in arbitrary 'survey points'.
  //
  // This enables time-based surveying: ships can contribute progress over
  // multiple ticks/days before a jump point becomes fully surveyed.
  std::unordered_map<Id, double> jump_survey_progress;

  // Simple per-faction ship contact memory.
  // Key: ship id.
  std::unordered_map<Id, Contact> ship_contacts;


  // Diplomatic offer cooldowns by target faction (anti-spam for AI proposals).
  //
  // Key: other faction id. Value: day (Date::days_since_epoch) until which this
  // faction should not send another diplomatic offer to that faction.
  std::unordered_map<Id, int> diplomacy_offer_cooldown_until_day;

  // Pirate hideout rebuild cooldowns by system.
  //
  // Key: system id. Value: day (Date::days_since_epoch) until which this faction
  // is not allowed to establish a new pirate hideout in that system.
  //
  // This is used to prevent immediately re-spawning a base the day after it is
  // destroyed, giving the player a meaningful suppression window.
  std::unordered_map<Id, int> pirate_hideout_cooldown_until_day;
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

// Persistent high-level fleet automation.
//
// This is a player-facing QoL layer: when enabled, the simulation will generate
// and maintain fleet orders (e.g. defending a colony, patrolling a system) and
// optionally handle sustainment (refuel/repair) at friendly colonies.
//
// NOTE: This is intentionally lightweight and best-effort; it should never be
// required for core simulation correctness.

enum class FleetMissionType : std::uint8_t {
  None = 0,
  DefendColony = 1,
  PatrolSystem = 2,
  HuntHostiles = 3,
  EscortFreighters = 4,
  Explore = 5,
  PatrolRegion = 6,
  AssaultColony = 7,
};

enum class FleetSustainmentMode : std::uint8_t {
  None = 0,
  Refuel = 1,
  Repair = 2,
  Rearm = 3,
  Maintenance = 4,
};

struct FleetMission {
  FleetMissionType type{FleetMissionType::None};

  // --- DefendColony ---
  // Colony (not body) id to defend.
  Id defend_colony_id{kInvalidId};

  // Response radius around the defended body's current position.
  // 0 => treat as "anywhere in-system".
  double defend_radius_mkm{0.0};

  // --- PatrolSystem ---
  Id patrol_system_id{kInvalidId};

  // How long to loiter at each patrol waypoint.
  int patrol_dwell_days{5};

  // Internal: current waypoint index.
  int patrol_leg_index{0};

  // --- PatrolRegion ---
  // Region/sector id to patrol.
  Id patrol_region_id{kInvalidId};

  // How long to loiter at each waypoint while patrolling.
  int patrol_region_dwell_days{5};

  // Internal: current system index within the region (deterministic order).
  int patrol_region_system_index{0};

  // Internal: current waypoint index within the current system.
  int patrol_region_waypoint_index{0};


  // --- HuntHostiles ---
  // Maximum age (in days) of a hostile contact to pursue.
  int hunt_max_contact_age_days{30};

  // --- EscortFreighters ---
  // If set, the fleet will escort this specific ship. If kInvalidId, the
  // simulation will auto-select a suitable friendly freighter to escort.
  Id escort_target_ship_id{kInvalidId};

  // Runtime: currently escorted ship id (may differ from escort_target_ship_id
  // when auto-selecting).
  Id escort_active_ship_id{kInvalidId};

  // Follow distance to maintain behind the escorted ship.
  double escort_follow_distance_mkm{1.0};

  // How far the fleet may range from the escorted ship to intercept detected
  // hostiles.
  // 0 => treat as "anywhere in-system".
  double escort_defense_radius_mkm{50.0};

  // When true, only ships with auto_freight enabled are eligible escort targets.
  bool escort_only_auto_freight{true};

  // To reduce thrashing, auto-selection will only retarget at most once per
  // interval (unless the current target becomes invalid).
  int escort_retarget_interval_days{5};

  // Runtime: day of last escort target selection.
  int escort_last_retarget_day{0};


  // --- Explore ---
  // If true, the fleet will survey unknown exits in the current system before
  // transiting any surveyed exits to undiscovered systems.
  bool explore_survey_first{true};

  // If true, the fleet may transit surveyed exits that lead to undiscovered
  // systems (expansion). If false, it will only survey exits in already-
  // discovered systems.
  bool explore_allow_transit{true};

  // --- AssaultColony ---
  // High-level automation for planet-taking operations.
  //
  // The fleet will (best-effort):
  //   1) Stage at a friendly colony to embark troops (optional)
  //   2) Bombard the target colony (optional)
  //   3) Land troops to invade
  //
  // Target colony (not body) id to assault.
  Id assault_colony_id{kInvalidId};

  // Optional staging colony (same-faction) to load troops from.
  // If kInvalidId and assault_auto_stage=true, the simulation will
  // auto-pick a good staging colony.
  Id assault_staging_colony_id{kInvalidId};

  // If true, attempt to stage at a friendly colony to load troops when the
  // fleet does not yet have sufficient embarked strength.
  bool assault_auto_stage{true};

  // Margin factor applied when estimating attacker strength requirements.
  // (1.0 = parity, >1.0 = safer).
  double assault_troop_margin_factor{1.10};

  // If true, bombard the target colony before attempting to invade.
  bool assault_use_bombardment{true};

  // How long to bombard before proceeding with invasion.
  // 0 disables bombardment (equivalent to assault_use_bombardment=false).
  // -1 means bombard indefinitely (mission will not auto-transition).
  int assault_bombard_days{7};

  // Runtime: set when the mission has already executed its initial bombardment
  // phase (if enabled) so it can transition to invasion.
  bool assault_bombard_executed{false};

  // --- Sustainment (all mission types) ---
  bool auto_refuel{true};
  double refuel_threshold_fraction{0.25};
  double refuel_resume_fraction{0.90};

  bool auto_repair{true};
  double repair_threshold_fraction{0.50};
  double repair_resume_fraction{0.95};

  bool auto_rearm{true};
  double rearm_threshold_fraction{0.25};
  double rearm_resume_fraction{0.90};

  bool auto_maintenance{true};
  double maintenance_threshold_fraction{0.70};
  double maintenance_resume_fraction{0.95};

  // Runtime state: active sustainment target.
  FleetSustainmentMode sustainment_mode{FleetSustainmentMode::None};
  Id sustainment_colony_id{kInvalidId};

  // Best-effort status/debug info.
  Id last_target_ship_id{kInvalidId};
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

  // Optional fleet automation mission / stance.
  FleetMission mission{};
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


// Procedural galaxy regions ("sectors").
//
// Regions are a lightweight way to group nearby star systems and attach
// environment / content generation modifiers (e.g. mineral richness,
// nebula bias, piracy risk). They are generated by the random scenario
// generator but can also be authored/edited in saves.
struct Region {
  Id id{kInvalidId};
  std::string name;

  // Representative center position in galaxy space (arbitrary units).
  // For Voronoi regions this is typically the seed point / site.
  Vec2 center{0.0, 0.0};

  // Theme tag for UI/flavor ("Core Worlds", "Nebula Expanse", ...).
  // This is intentionally a string to keep save compatibility flexible.
  std::string theme;

  // Content modifiers (multipliers / biases).
  double mineral_richness_mult{1.0};   // affects non-volatile mineral deposits
  double volatile_richness_mult{1.0};  // affects Sorium / Fuel-like volatiles
  double salvage_richness_mult{1.0};   // affects derelict salvage packages

  // Additive nebula bias applied to systems in this region (-1..+1).
  // Positive values increase nebula density, negative values decrease it.
  double nebula_bias{0.0};

  // 0..1: higher => pirates and hostile "activity" more likely.
  double pirate_risk{0.0};

  // 0..1: dynamic security / suppression applied to piracy in this region.
  //
  // Updated by the simulation based on patrol missions by non-pirate factions
  // (see Simulation::tick_piracy_suppression). Effective piracy risk is:
  //   pirate_risk * (1 - pirate_suppression)
  double pirate_suppression{0.0};

  // 0..1: higher => ancient ruins / anomalies more likely.
  double ruins_density{0.0};
};

struct StarSystem {
  Id id{kInvalidId};
  std::string name;

  // Procedural region/sector id (optional).
  // When kInvalidId, the system is not assigned to any region.
  Id region_id{kInvalidId};

  // Position in galaxy map (arbitrary units)
  Vec2 galaxy_pos{0.0, 0.0};

  // System-level nebula/dust density in [0,1].
  // Higher values reduce effective sensor ranges and add a nebula haze on maps.
  double nebula_density{0.0};

  // Temporary nebula storm (dynamic environmental hazard).
  //
  // Peak intensity is in [0,1]. Storms ramp up/down over their lifetime using
  // a smooth pulse; see Simulation::system_storm_intensity().
  double storm_peak_intensity{0.0};
  std::int64_t storm_start_day{0}; // days_since_epoch
  std::int64_t storm_end_day{0};   // exclusive (storm active when now in [start, end))

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

  // Hour-of-day at the time the event occurred (0..23).
  //
  // When sub-day ticks are enabled, many events can occur mid-day.
  // This is primarily used for UI/exports; simulation logic generally uses
  // day-level scheduling.
  int hour{0};

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
