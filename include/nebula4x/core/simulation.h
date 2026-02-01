#pragma once

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

struct SimConfig {
  double seconds_per_day{86400.0};

  // When enabled, economic systems (mining/industry, research, shipyards, construction,
  // terraforming, and repairs) tick proportionally on sub-day time steps (advance_hours),
  // instead of only at the midnight day boundary.
  //
  // This makes +1h/+6h/+12h time warp meaningful for non-combat progress while preserving
  // the classic "daily tick" feel when disabled.
  bool enable_subday_economy{false};

  // Colony population growth rate (fraction per year).
  //
  // Example: 0.01 = +1% per year. A negative value models population decline.
  // Applied once per simulated day in tick_colonies().
  double population_growth_rate_per_year{0.01};

  // --- Colony habitability / life support ---
  //
  // When enabled, colonies compute a simple "habitability" score for their body
  // based on the body's surface temperature and atmosphere. Colonies on hostile
  // worlds may require habitation / life support installations to sustain their
  // population.
  bool enable_habitability{true};

  // Ideal environment values used for habitability calculations when a body has
  // no terraforming targets set.
  double habitability_ideal_temp_k{288.0};
  double habitability_ideal_atm{1.0};

  // Linear tolerance bands for habitability scoring.
  // If abs(temp_k - ideal_temp_k) >= habitability_temp_tolerance_k => temp factor = 0.
  double habitability_temp_tolerance_k{50.0};
  // If abs(atm - ideal_atm) >= habitability_atm_tolerance => atmosphere factor = 0.
  double habitability_atm_tolerance{0.5};

  // Growth multiplier applied when a colony is living under artificial habitats
  // on a hostile world but has sufficient habitation capacity.
  //
  // The final growth rate is scaled by the computed habitability (so perfectly
  // hostile worlds may still have near-zero growth even when supported).
  double habitation_supported_growth_multiplier{0.25};

  // Population decline rate applied when a colony does not have sufficient
  // habitation capacity. The effective decline is scaled by the shortfall
  // fraction (0..1).
  double habitation_shortfall_decline_rate_per_year{0.25};

  // Only emit a warning event when the shortfall fraction is at or above this.
  double habitation_shortfall_event_min_fraction{0.10};

  // Global throttle for habitation shortfall warnings (days).
  int habitation_shortfall_event_interval_days{30};

  // When colonizing a body, automatically seed the new colony with enough
  // "Infrastructure" to support the initial population (if possible).
  bool seed_habitation_on_colonize{true};

  // When interacting with moving orbital bodies (colonies), ships need a tolerance
  // for being considered "in orbit / docked".
  //
  // If you set this too low, slower ships may endlessly chase a planet's updated
  // position and never reach an exact point for cargo transfers.
  // --- Docking / logistics transfer rates ---
  //
  // Cargo/fuel/troop transfers and wreck salvage are modeled with throughput limits
  // (per simulated day) and are scaled by dt_days each tick.
  //
  // This keeps gameplay consistent across different time-step sizes (1h/6h/24h),
  // preventing "instant" transfers when running high-resolution ticks.
  //
  // Cargo transfers are intentionally fast by default, while salvage is slower.
  double cargo_transfer_tons_per_day_per_cargo_ton{10.0};
  double cargo_transfer_tons_per_day_min{50.0};

  double fuel_transfer_tons_per_day_per_fuel_ton{5.0};
  double fuel_transfer_tons_per_day_min{50.0};

  double troop_transfer_strength_per_day_per_troop_cap{5.0};
  double troop_transfer_strength_per_day_min{10.0};

  // Colonist/passenger transfers (millions per simulated day).
  //
  // These are intentionally fairly fast so that moving population around the
  // empire isn't tedious on daily ticks, but they are still throughput-limited
  // to keep sub-day ticks (1h/6h/12h) consistent and to prevent instant moves.
  //
  // The transfer rate scales with the ship design's colony_capacity_millions.
  //
  // Example: colony_capacity_millions=50 and per_cap=1.0 => 50M/day (~2.08M/hour).
  double colonist_transfer_millions_per_day_per_colony_cap{1.0};
  double colonist_transfer_millions_per_day_min{5.0};

  double salvage_tons_per_day_per_cargo_ton{0.2};
  double salvage_tons_per_day_min{10.0};

  // --- Smart travel (fuel-aware routing) ---
  //
  // When enabled, the simulation exposes "smart" travel helpers that attempt to
  // automatically insert refuel stops at trade-partner colonies so ships/fleets
  // do not accidentally strand themselves while following long jump chains.
  //
  // The UI currently binds this to modifier-clicks on the galaxy map.
  bool enable_smart_travel_refuel_stops{true};

  // Number of days to remain in orbit at each refuel stop.
  //
  // While in orbit, tick_refuel()/tick_rearm()/maintenance can operate.
  int smart_travel_refuel_wait_days{3};

  // Minimum Fuel stockpile (tons) required for a colony to be considered as a
  // refuel stop candidate.
  double smart_travel_refuel_stop_min_fuel_tons{25.0};

  // Hard cap on number of intermediate refuel stops inserted by smart travel.
  int smart_travel_max_refuel_stops{4};

  // Safety factor applied to maximum leg range (0..1). Example: 0.90 leaves ~10%
  // reserve fuel.
  double smart_travel_range_safety_factor{0.90};

  // --- Salvage research / reverse engineering ---
  //
  // Salvaging wrecks can optionally award research points and/or reverse
  // engineer components from foreign designs. This is intended to make
  // exploration, battlefield recovery, and piracy more strategically
  // interesting.
  //
  // Salvage research is driven by ResourceDef::salvage_research_rp_per_ton.
  bool enable_salvage_research{true};
  double salvage_research_rp_multiplier{1.0};

  // Reverse engineering applies only to wrecks with a valid source_design_id
  // (typically ship wrecks), and is skipped when salvaging your own faction's
  // wrecks.
  bool enable_reverse_engineering{true};

  // Points gained per ton of salvage transferred from a wreck into cargo.
  // The points are distributed evenly across the source design's components
  // that are not yet unlocked by the salvaging faction.
  double reverse_engineering_points_per_salvaged_ton{0.05};

  // Required points per ton of a component's mass to unlock that component.
  // Required(component) = max(1, mass_tons * reverse_engineering_points_required_per_component_ton).
  double reverse_engineering_points_required_per_component_ton{1.0};

  // Optional cap on number of components that can be unlocked in a single tick.
  // 0 or negative => unlimited.
  int reverse_engineering_unlock_cap_per_tick{2};

  // --- Anomaly schematics (reverse engineering from exploration) ---
  //
  // When enabled, resolving anomalies can yield small amounts of reverse-engineering
  // progress toward otherwise-locked components. This is intended to make
  // exploration rewards feel more distinct from pure +RP.
  bool enable_anomaly_schematic_fragments{true};

  // Base points gained when resolving an anomaly (scaled by investigation time and RP reward).
  double anomaly_schematic_points_base{1.0};
  double anomaly_schematic_points_per_investigation_day{0.6};
  double anomaly_schematic_points_per_rp{0.02};

  // Kind multipliers (reward flavor).
  double anomaly_schematic_ruins_multiplier{1.3};
  double anomaly_schematic_signal_multiplier{1.0};
  double anomaly_schematic_distress_multiplier{0.9};
  double anomaly_schematic_phenomenon_multiplier{0.8};

  // Number of components to spread points across (1 = focused).
  int anomaly_schematic_components_per_anomaly{1};

  // If true, anomalies that already directly unlock a component can also grant schematic fragments.
  bool anomaly_schematic_allow_with_direct_unlock{false};

  // --- Obscure codex fragments (procedural ciphered lore) ---
  //
  // When enabled, anomalies expose a deterministic "codex fragment": a short
  // ciphered message plus a gradually-revealed translation that improves as
  // a faction resolves more anomalies in the same lead chain.
  //
  // Optionally, fully decoding a lead chain can reveal a special follow-up
  // "Codex Echo" site (and an optional contract offer).
  bool enable_obscure_codex_fragments{true};

  // Number of resolved anomalies in a lead chain required for full translation.
  // 1 => always fully translated.
  int codex_fragments_required{2};

  // When enabled, completing a codex translation (reaching codex_fragments_required)
  // spawns a special follow-up site (and, optionally, a contract).
  bool enable_codex_echo_reward{true};

  // Hop distance window for Codex Echo targets (computed over the jump network).
  // 0 disables hop filtering.
  int codex_echo_min_hops{2};
  int codex_echo_max_hops{6};

  // When enabled, Codex Echo rewards create an "Investigate" contract offer for
  // the discovering faction (if contracts are enabled).
  bool codex_echo_offer_contract{true};

  // Bonus research reward (RP) added to Codex Echo contracts.
  double codex_echo_contract_bonus_rp{20.0};

  // --- Procedural contracts / mission board ---
  //
  // When enabled, the simulation generates lightweight faction-scoped "contracts"
  // (missions) based on the existing world state:
  //   - discovered but unresolved anomalies
  //   - salvageable wrecks in discovered systems
  //   - unsurveyed jump points in discovered systems
  //
  // Contracts are persisted in saves and can be accepted/assigned via the UI.
  bool enable_contracts{true};

  // Maximum number of concurrently Offered contracts per faction.
  int contract_max_offers_per_faction{6};

  // Number of new Offered contracts generated per faction per day (up to max_offers).
  int contract_daily_new_offers_per_faction{2};

  // Offered contracts expire after this many days. <= 0 => never expire.
  int contract_offer_expiry_days{60};

  // Reward heuristic (research points) components.
  double contract_reward_base_rp{5.0};
  double contract_reward_rp_per_hop{2.0};
  double contract_reward_rp_per_risk{15.0};

  double docking_range_mkm{3.0};

  // Generic "arrived" epsilon used for fixed targets (move-to-point).
  double arrival_epsilon_mkm{1e-6};

  // Maximum number of persistent simulation events to keep in GameState::events.
  // 0 means "unlimited" (not recommended for very long runs).
  int max_events{1000};

  // Fraction of shipyard mineral costs refunded when scrapping a ship at a colony.
  //
  // 0.0 = no refund, 1.0 = full refund.
  double scrap_refund_fraction{0.5};

  // Ship refit efficiency (shipyard).
  //
  // Refit work is expressed in "tons of shipyard capacity", similar to building a new ship.
  // This multiplier scales the target design mass to determine how many tons of work a refit requires.
  //
  // Example: design mass 100t with multiplier 0.5 => 50 tons of work.
  double ship_refit_tons_multiplier{0.5};

  // Ship repair rate when docked at a friendly colony with a shipyard.
  //
  // A ship is considered docked if it is within docking_range_mkm of the colony's body.
  // Each day, the colony provides (repair_hp_per_day_per_shipyard * shipyard_count) total repair
  // capacity, shared across all docked damaged ships (allocated by RepairPriority). Repairs are
  // capped to each ship's design max HP.
  //
  // 0.0 disables repairs.
  double repair_hp_per_day_per_shipyard{0.5};

  // Mineral costs for repairs (tons per HP repaired).
  //
  // If non-zero, repairs will consume minerals from the colony stockpile. If a colony lacks the
  // required minerals, its repair throughput is reduced accordingly.
  //
  // 0.0 means "free" repairs for that mineral.
  double repair_duranium_per_hp{0.0};
  double repair_neutronium_per_hp{0.0};

  // --- Blockades (economic disruption) ---
  // When enabled, hostile armed ships loitering near a colony's body can impose a
  // "blockade" that reduces certain colony outputs (repairs, training, terraforming).
  //
  // This is intentionally lightweight: blockade pressure is computed from nearby
  // hostile/defender combat presence (plus static defenses) and turned into an
  // output multiplier.
  bool enable_blockades{true};

  // Radius around a colony's body within which hostile/defender ships contribute
  // to blockade pressure.
  double blockade_radius_mkm{10.0};

  // Baseline "resistance" power that reduces the impact of small raiding forces.
  // (Bigger values require larger hostile fleets to achieve the same pressure.)
  double blockade_base_resistance_power{20.0};

  // Maximum fractional output penalty at full pressure (0..1).
  // Example: 0.75 => at pressure=1, affected outputs run at 25%.
  double blockade_max_output_penalty{0.75};

  // --- Ship maintenance / spare parts (optional) ---
  //
  // When enabled, ships consume a chosen stockpiled resource ("spare parts") while
  // operating. If they cannot draw enough supply, their maintenance_condition decays,
  // applying speed and combat effectiveness penalties.
  //
  // This is intentionally lightweight and deterministic: readiness is tracked as a
  // continuous scalar. Optional deterministic malfunctions can be enabled via the
  // ship_maintenance_breakdown_* parameters.
  bool enable_ship_maintenance{false};

  // Resource id consumed as maintenance supplies. Default uses the generic
  // processed output "Metals" from the materials chain.
  std::string ship_maintenance_resource_id{"Metals"};

  // Tons of maintenance resource required per simulated day per ship mass ton.
  //
  // Example: 10,000t ship with 0.00002 consumes 0.2 tons/day.
  double ship_maintenance_tons_per_day_per_mass_ton{0.00002};

  // Condition recovery/decay rates per day.
  //
  // If supplied_fraction == 1.0, maintenance_condition increases by
  // ship_maintenance_recovery_per_day (capped at 1.0).
  //
  // Otherwise, maintenance_condition decreases by:
  //   ship_maintenance_decay_per_day * (1 - supplied_fraction).
  double ship_maintenance_recovery_per_day{0.02};
  double ship_maintenance_decay_per_day{0.01};

  // Minimum multipliers applied at maintenance_condition == 0.0 (linearly scaled
  // up to 1.0 at maintenance_condition == 1.0).
  double ship_maintenance_min_speed_multiplier{0.6};
  double ship_maintenance_min_combat_multiplier{0.7};


// Optional: deterministic maintenance breakdown / failures.
//
// When ship maintenance is enabled, ships that fall below
// ship_maintenance_breakdown_start_fraction have a per-day chance of suffering
// a subsystem malfunction (engines/weapons/sensors/shields). Malfunctions
// reduce subsystem integrity and generally require shipyard repairs to restore,
// creating a more meaningful sustainment loop beyond simple speed/combat
// multipliers.
//
// The hazard rate at condition==0 is ship_maintenance_breakdown_rate_per_day_at_zero.
// For condition in [0, start], the hazard scales as:
//   rate = rate0 * pow((start - condition) / start, exponent)
// and is converted into a Bernoulli probability via p = 1 - exp(-rate).
//
// Set rate0 to 0 to disable malfunctions (restoring the old behavior).
double ship_maintenance_breakdown_start_fraction{0.50};
double ship_maintenance_breakdown_rate_per_day_at_zero{0.03};
double ship_maintenance_breakdown_exponent{2.0};

// Subsystem integrity damage applied on a malfunction (fraction of integrity).
double ship_maintenance_breakdown_subsystem_damage_min{0.05};
double ship_maintenance_breakdown_subsystem_damage_max{0.20};


  // --- Crew training / experience (optional) ---
  //
  // Ships track a persistent crew_grade_points value (default 100) that
  // is improved by docked training and combat. This is mapped to a small
  // effectiveness modifier (accuracy/reload/boarding) via:
  //   bonus = (sqrt(points) - 10) / 100
  // which yields:
  //   points=0   -> -10%
  //   points=100 ->  0%
  //   points=400 -> +10%
  //   points=900 -> +20%
  //   points=1600-> +30%
  //   points=2500-> +40%
  //
  // This is inspired by classic 4X crew grade systems (e.g. Aurora-style
  // grade points), but intentionally simplified and deterministic.
  bool enable_crew_experience{true};

  // Starting crew grade points for newly created ships.
  double crew_initial_grade_points{100.0};

  // Global cap on crew grade points.
  double crew_grade_points_cap{2500.0};

  // Combat experience gain: grade points added per unit of combat "intensity".
  // Intensity is currently derived from damage dealt/received and missile intercepts.
  double crew_combat_grade_points_per_damage{0.25};

  // Global multiplier applied to colony crew_training_points_per_day.
  double crew_training_points_multiplier{1.0};

  // Crew casualties / complement (optional extension).
  //
  // When enabled, combat hull damage can reduce a ship's crew_complement (fraction of
  // required crew remaining). Under-crewed ships suffer a combat penalty even if their
  // surviving crew is experienced.
  bool enable_crew_casualties{true};

  // Fraction of full crew lost when the ship takes hull damage equal to its max HP.
  //
  // Example: 0.15 means that a hit that removes 20% of max HP causes ~3% crew loss.
  double crew_casualty_fraction_per_full_hull_damage{0.15};

  // Training points required to restore a ship from 0% -> 100% crew complement while docked
  // at a colony. Crew replacement draws from the same pool as crew training, so heavy
  // losses slow experience growth until crews are rebuilt.
  double crew_replacement_training_points_per_full_complement{1500.0};

  // Exponent applied to crew_complement to compute the combat performance multiplier.
  //
  // Effective multiplier = pow(crew_complement, exponent) * (1 + grade_bonus).
  // exponent < 1 softens the penalty (default sqrt).
  double crew_complement_exponent{0.5};

  // Only emit a crew-casualty warning event when a single tick reduces complement by
  // at least this fraction.
  double crew_casualty_event_min_loss_fraction{0.05};

  // --- Ship heat / thermal management (optional) ---
  //
  // When enabled, ships accumulate heat based on online power usage and
  // dissipate heat based on hull mass (radiator area proxy) plus optional
  // design bonuses from components. Excess heat reduces ship performance
  // (speed/sensors/weapons/shields), and extreme overheating can damage hull.
  bool enable_ship_heat{false};

  // Baseline heat capacity derived from ship mass.
  // Total capacity = mass_tons * ship_heat_base_capacity_per_mass_ton + design.heat_capacity_bonus.
  double ship_heat_base_capacity_per_mass_ton{1.0};

  // Baseline heat generation derived from *online* power usage.
  // Generation/day = online_power_use * ship_heat_generation_per_power_use_per_day + design.heat_generation_bonus_per_day.
  double ship_heat_generation_per_power_use_per_day{0.05};

  // Baseline heat dissipation derived from ship mass.
  // Dissipation/day = mass_tons * ship_heat_base_dissipation_per_mass_ton_per_day + design.heat_dissipation_bonus_per_day.
  double ship_heat_base_dissipation_per_mass_ton_per_day{0.02};

  // Performance penalties scale linearly from penalty_start_fraction to penalty_full_fraction
  // of heat capacity.
  double ship_heat_penalty_start_fraction{0.70};
  double ship_heat_penalty_full_fraction{1.00};

  // Minimum subsystem multipliers at/above ship_heat_penalty_full_fraction.
  double ship_heat_min_speed_multiplier{0.50};
  double ship_heat_min_sensor_range_multiplier{0.50};
  double ship_heat_min_weapon_output_multiplier{0.60};
  double ship_heat_min_shield_regen_multiplier{0.50};

  // Damage kicks in at severe overheating. Threshold is a fraction of capacity.
  double ship_heat_damage_threshold_fraction{1.20};

  // Hull damage rate at 200% of capacity (scaled linearly from threshold).
  // Example: 0.25 means 25% of max HP per day at 200% heat.
  double ship_heat_damage_fraction_per_day_at_200pct{0.25};

  // Thermal signature bloom: additional detectability multiplier derived from ship heat.
  //
  // When ship heat is enabled, a ship's effective signature multiplier is further
  // multiplied by:
  //   heat_sig = clamp(1 + ship_heat_signature_multiplier_per_fraction * heat_fraction,
  //                    1, ship_heat_signature_multiplier_max)
  // where heat_fraction is ship_heat / heat_capacity.
  //
  // Set ship_heat_signature_multiplier_per_fraction to 0 to disable this coupling.
  double ship_heat_signature_multiplier_per_fraction{0.50};

  // Upper bound for the thermal signature multiplier from heat (see above).
  double ship_heat_signature_multiplier_max{2.00};

  // --- Ship subsystem damage / critical hits (optional) ---
  //
  // When enabled, combat hull damage can inflict deterministic "critical hits"
  // that reduce key subsystem integrity (engines/weapons/sensors/shields).
  //
  // Integrity is modeled as a simple 0..1 scalar per subsystem and affects
  // performance until repaired at a shipyard. This is intentionally lightweight
  // (not per-component) but provides a meaningful mid-layer between "full HP"
  // and "destroyed".
  bool enable_ship_subsystem_damage{false};

  // Average number of subsystem crits inflicted when a ship takes hull damage
  // equal to 100% of its max HP.
  double ship_subsystem_crits_per_full_hull_damage{1.0};

  // Hard cap on crits applied to a single ship in one combat damage resolution.
  int ship_subsystem_max_crits_per_damage_instance{4};

  // Integrity loss per crit hit is sampled uniformly in [min, max].
  double ship_subsystem_integrity_loss_min{0.05};
  double ship_subsystem_integrity_loss_max{0.25};

  // Shipyard repairs can restore subsystem integrity using the same "repair
  // capacity" pool as hull HP. Subsystem repairs consume an HP-equivalent amount
  // of capacity:
  //   hp_equiv = deficit_integrity * max_hp * ship_subsystem_repair_hp_equiv_per_integrity
  // e.g. with 0.25, restoring a subsystem from 0 -> 1 costs 25% of max HP worth
  // of repair capacity.
  double ship_subsystem_repair_hp_equiv_per_integrity{0.25};


  // --- Sensor modes / EMCON ---
  //
  // Ships can operate their sensors in different modes (see SensorMode):
  //  - Passive: shorter sensor range, but harder to detect (lower signature).
  //  - Normal: baseline.
  //  - Active: longer sensor range, but easier to detect (higher signature).
  //
  // These multipliers are only applied when a ship's sensors are online
  // (subject to ShipPowerPolicy and available power).
  double sensor_mode_passive_range_multiplier{0.6};
  double sensor_mode_active_range_multiplier{1.5};
  double sensor_mode_passive_signature_multiplier{0.8};
  double sensor_mode_active_signature_multiplier{1.5};


  // --- Nebula storms / environmental hazards ---
  //
  // Star systems have a static nebula_density from the scenario generator, but
  // can also experience temporary "nebula storms" that intensify sensor
  // interference and slow movement.
  bool enable_nebula_storms{true};

  // Storms only occur in systems with at least this nebula_density.
  double nebula_storm_min_nebula_density{0.35};

  // At nebula_density = 1.0, this is the per-day chance of a storm starting.
  // Actual chance scales by pow(nebula_density, nebula_storm_start_chance_exponent).
  double nebula_storm_start_chance_per_day_at_max_density{0.02};
  double nebula_storm_start_chance_exponent{2.0};

  // Storm duration range in days.
  int nebula_storm_duration_days_min{5};
  int nebula_storm_duration_days_max{20};

  // Peak intensity range for a newly spawned storm.
  double nebula_storm_peak_intensity_min{0.4};
  double nebula_storm_peak_intensity_max{1.0};

  // Additional sensor attenuation at storm intensity 1.0.
  // Final sensor multiplier = base_nebula_multiplier * (1 - penalty * storm_intensity).
  double nebula_storm_sensor_penalty{0.50};

  // Movement speed penalty at storm intensity 1.0.
  // Final speed multiplier = base * (1 - penalty * storm_intensity).
  double nebula_storm_speed_penalty{0.40};

  // --- Spatial storm cells ---
  //
  // When enabled, active storms gain an additional position-dependent modulation
  // field ("storm cells"). The system-wide storm intensity remains the average
  // temporal pulse, but local intensity varies smoothly so ships can seek calmer
  // pockets or risk high-intensity cores.
  bool enable_nebula_storm_cells{true};

  // Strength of spatial variation (0..1).
  // 0 => local intensity == system intensity (uniform).
  // 1 => local intensity varies roughly in [0, 2x] around the base (clamped).
  double nebula_storm_cell_strength{0.65};

  // Typical size of storm cells (million-km). Smaller => more, smaller cells.
  double nebula_storm_cell_scale_mkm{1600.0};

  // Drift speed of the storm pattern (million-km per day).
  double nebula_storm_cell_drift_speed_mkm_per_day{220.0};

  // Contrast shaping for storm cells (>1 => sharper fronts, <1 => flatter).
  double nebula_storm_cell_sharpness{1.6};

  // Optional baseline movement drag from nebula density (even without storms).
  bool enable_nebula_drag{false};
  double nebula_drag_speed_penalty_at_max_density{0.25};

  // --- Nebula microfields (in-system "terrain") ---
  //
  // When enabled, StarSystem::nebula_density becomes the *baseline* density and
  // a deterministic local microfield creates pockets/filaments of denser and
  // clearer space. This affects both sensors and movement (via the
  // *_environment_multiplier_at() helpers).
  bool enable_nebula_microfields{true};

  // Typical size of microfield features (million-km). Smaller => finer filaments.
  double nebula_microfield_scale_mkm{900.0};
  // Size of the low-frequency warp field (million-km). Larger => gentler warps.
  double nebula_microfield_warp_scale_mkm{2600.0};
  // Strength of deviation around the system baseline density.
  double nebula_microfield_strength{0.28};
  // 0..1 blend between smooth clouds (0) and filamentary ridges (1).
  double nebula_microfield_filament_mix{0.65};
  // Contrast shaping (>1 => sharper filaments, <1 => flatter).
  double nebula_microfield_sharpness{1.25};

  // Targets deep inside a dense microfield should be somewhat harder to detect,
  // not just sources. We approximate this by lerping the target's signature
  // multiplier toward the local sensor environment multiplier.
  //
  // 0 => no target-side effect (legacy behavior)
  // 1 => target signature is fully multiplied by the local environment factor
  double nebula_target_signature_env_weight{0.50};

  // --- Sensor line-of-sight attenuation (experimental) ---
  //
  // In addition to local environmental multipliers at the sensor source and
  // target (nebula density, storms, microfields), optionally apply an extra
  // *line-of-sight* attenuation factor derived by ray-marching the environment
  // field between the two points.
  //
  // Implementation notes:
  //  - Uses an SDF-style distance estimate (f/|grad f|) on the local sensor
  //    environment multiplier field to take adaptive steps along the ray.
  //  - Uses deterministic, seeded jitter for stochastic sampling stability.
  //
  // The returned LOS multiplier is *relative* to the endpoints so uniform
  // environments produce ~1.0 (avoids double-counting source/target penalties).
  bool enable_sensor_los_attenuation{false};
  // Strength exponent for the LOS multiplier.
  // 0 => disabled (multiplier 1.0), 1 => linear, >1 => stronger occlusion.
  double sensor_los_strength{1.35};
  // Iso-threshold (in sensor environment multiplier space) for the implicit
  // surface used to estimate a signed distance bound along the ray.
  // Values closer to 1 treat mild haze as an occluder; values closer to 0
  // only treat the densest pockets as occluders.
  double sensor_los_iso_env{0.78};
  // Sphere-tracing safety scale on the distance estimate (<=1).
  double sensor_los_sdf_step_scale{0.90};
  // Adaptive step controls (million-km).
  double sensor_los_min_step_mkm{60.0};
  double sensor_los_max_step_mkm{900.0};
  // Finite-difference epsilon for gradient estimation (million-km).
  double sensor_los_grad_epsilon_mkm{40.0};
  // Maximum number of adaptive steps along a single LOS query.
  int sensor_los_max_steps{48};
  // Stratified sample jitter within each step (0..0.5 recommended).
  double sensor_los_sample_jitter{0.35};
  // Clamp bounds for the final relative LOS multiplier (before exponent).
  // Min keeps the game stable in extreme occlusion; max defaults to 1 (no boost).
  double sensor_los_min_multiplier{0.25};
  double sensor_los_max_multiplier{1.00};

  // --- Celestial body occlusion (experimental) ---
  //
  // When enabled, sensor detection and (optionally) direct-fire weapons
  // require a clear geometric line-of-sight that is not blocked by the
  // physical radii of non-stellar bodies (planets/moons/gas giants/etc.).
  //
  // Design notes:
  //  - This adds tactics: ships can hide behind planets, and beam weapons
  //    cannot shoot "through" a planet even if another sensor source
  //    provides target coordinates.
  //  - We intentionally ignore BodyType::Star for now because the simulation
  //    is 2D; treating the star as a hard occluder would create unrealistic
  //    "everything is blocked by the sun" artifacts.
  //
  // These flags default to false to preserve legacy behavior and existing
  // scenarios/tests.
  bool enable_body_occlusion_sensors{false};
  bool enable_body_occlusion_weapons{false};

  // Optional padding applied to body radii when testing occlusion (million-km).
  // This can be used to approximate atmospheres / "sensor horizon" fuzz.
  double body_occlusion_padding_mkm{0.0};


  // --- Terrain-aware navigation (experimental) ---
  //
  // When enabled, ships steering toward a target can optionally perform a small
  // deterministic "ray-probe" search around the straight-line direction to
  // find faster lanes through nebula microfields / storm cells.
  //
  // Implementation notes:
  //  - Evaluates candidate headings by integrating movement cost along short
  //    lookahead rays: cost ~= \int (ds / speed_multiplier(p)).
  //  - Uses an SDF-style distance estimate (f/|grad f|) on the local movement
  //    speed multiplier field for adaptive step sizes.
  //  - Uses deterministic jitter for stratified sampling stability.
  //
  // This is a lightweight receding-horizon controller (no persistent waypoints).
  bool enable_terrain_aware_navigation{false};

  // 0..1 blend between straight-line steering (0) and best-ray steering (1).
  double terrain_nav_strength{0.75};

  // How far ahead to evaluate candidate rays (million-km).
  double terrain_nav_lookahead_mkm{900.0};

  // Number of candidate rays to test (odd recommended).
  int terrain_nav_rays{9};

  // Max angular deviation from the direct-to-target heading (degrees).
  double terrain_nav_max_angle_deg{55.0};

  // Penalize large turns away from the goal (0..2 typical).
  double terrain_nav_turn_penalty{0.35};

  // --- Terrain nav ray integration knobs ---
  // Iso-threshold (in movement speed multiplier space) for the implicit surface
  // used to estimate an SDF-like distance bound.
  double terrain_nav_iso_speed{0.80};
  double terrain_nav_sdf_step_scale{0.85};
  double terrain_nav_min_step_mkm{80.0};
  double terrain_nav_max_step_mkm{900.0};
  double terrain_nav_grad_epsilon_mkm{50.0};
  int terrain_nav_max_steps{24};
  double terrain_nav_sample_jitter{0.35};


  // Shield drain per day at storm intensity 1.0 (applied when shields are online).
  double nebula_storm_shield_drain_per_day_at_intensity1{4.0};



  // --- Jump point surveying ---
  //
  // Under fog-of-war, factions must "survey" jump points before the jump network
  // can be used for route planning. Previously this was effectively instant once
  // a ship was in range.
  //
  // This models surveying as an incremental process that takes time, inspired by
  // classic space-4X workflows where specialized survey ships accumulate "survey
  // points" over time.
  //
  // Units are arbitrary "survey points". When a faction's accumulated progress for
  // a given jump point reaches jump_survey_points_required, the jump point becomes
  // surveyed for that faction.
  //
  // Set jump_survey_points_required <= 0 to revert to instant surveying.
  double jump_survey_points_required{1.0};

  // Normalization factor for sensor strength when accumulating survey points.
  // A ship with effective sensor range ~= jump_survey_reference_sensor_range_mkm in
  // Normal EMCON will contribute ~1.0 point/day (before role multipliers).
  double jump_survey_reference_sensor_range_mkm{400.0};

  // How close a ship must be to a jump point to contribute survey progress.
  // Non-surveyors always use docking_range_mkm. Surveyors can contribute at longer
  // range (scaled from their effective sensor range).
  double jump_survey_range_sensor_fraction{0.15};

  // Role multipliers to bias dedicated survey ships.
  double jump_survey_strength_multiplier_surveyor{1.0};
  double jump_survey_strength_multiplier_other{0.25};

  // Cap per-ship survey rate to avoid extreme values from modded sensor ranges.
  double jump_survey_points_per_day_cap{5.0};

  // --- Jump point phenomena (procedural difficulty fields) ---
  //
  // When enabled, each jump point is assigned a deterministic set of "phenomena"
  // values (stability/turbulence/shear) derived from its id/system/position.
  //
  // In this round we only integrate phenomena into *survey difficulty* (the
  // required survey points can vary by jump point). Other parameters are
  // reserved for future integration.
  bool enable_jump_point_phenomena{true};

  // Strength of the procedural survey difficulty multiplier.
  // 0 => no effect (all jumps use jump_survey_points_required).
  // 1 => full procedural multiplier.
  double jump_phenomena_survey_difficulty_strength{1.0};

  // Reserved tuning knobs for future jump-transit integration.
  double jump_phenomena_transit_hazard_strength{1.0};
  double jump_phenomena_hazard_surveyed_multiplier{0.35};
  double jump_phenomena_storm_hazard_bonus{0.35};
  double jump_phenomena_misjump_strength{1.0};
  double jump_phenomena_subsystem_glitch_strength{1.0};

  // --- Anomaly discovery (fog-of-war / exploration) ---
  //
  // Anomalies are discovered via sensor coverage. To avoid requiring ships to
  // pass extremely close to every point of interest, anomaly detection uses a
  // small multiplier relative to the normal sensor range used for ship contacts.
  //
  // 1.0 => anomalies are detected at normal sensor range.
  // >1.0 => anomalies are easier to discover (recommended for early-game).
  double anomaly_detection_range_multiplier{3.0};

  // Weight in [0,1] for applying *target-side* environmental attenuation to
  // anomaly discovery.
  //
  // Sensor sources already apply environment multipliers at the source
  // position (nebula density, storms, microfields). This additional factor
  // lets dense pockets/filaments also conceal anomalies at the *target*
  // location without making discovery impossible.
  //
  // 0.0 => ignore target environment (legacy behavior).
  // 1.0 => fully apply target environment (strong concealment).
  double anomaly_detection_target_env_weight{0.55};

  // --- Procedural exploration leads (anomaly chains) ---
  //
  // When enabled, resolving an anomaly can reveal a follow-up lead:
  //  - a star chart that reveals nearby systems/jump routes,
  //  - a new anomaly site spawned elsewhere, or
  //  - a hidden salvage cache.
  //
  // These leads are designed to create emergent exploration arcs without a
  // full quest/contract UI layer.
  bool enable_anomaly_leads{true};

  // Base probability in [0,1] that a resolved anomaly generates a lead.
  double anomaly_lead_base_chance{0.22};

  // Maximum depth of generated lead chains. Scenario/static anomalies start at 0.
  int anomaly_lead_max_depth{2};

  // Cap the total number of generated anomalies (lead_depth > 0) across the save.
  int anomaly_lead_max_total_generated{48};

  // When a lead points to another system, prefer targets within this hop window
  // (computed over the jump network). 0 disables hop filtering.
  int anomaly_lead_min_hops{1};
  int anomaly_lead_max_hops{4};

  // Lead type mix (when a lead triggers).
  // Remaining probability goes to a follow-up anomaly site.
  double anomaly_lead_star_chart_chance{0.30};
  double anomaly_lead_hidden_cache_chance{0.18};

  // --- Dynamic procedural points-of-interest (mid/late-game exploration) ---
  //
  // Random scenarios can feel "front-loaded": once initial anomalies are cleared,
  // exploration content can taper off. This system slowly injects new anomalies
  // and salvage caches over time, biased by procedural Region attributes
  // (ruins_density, pirate_risk, salvage_richness_mult) so the galaxy remains
  // uneven and flavorful.
  //
  // Spawned POIs are *not automatically revealed*; normal sensor discovery rules apply.
  bool enable_dynamic_poi_spawns{true};

  // Base per-system daily spawn chances (before region/nebula/colony modifiers).
  double dynamic_anomaly_spawn_chance_per_system_per_day{0.0025};
  double dynamic_cache_spawn_chance_per_system_per_day{0.0015};

  // Global caps. If <= 0, a size-scaled default is used.
  int dynamic_poi_max_unresolved_anomalies_total{0}; // default: ~2x num systems
  int dynamic_poi_max_active_caches_total{0};        // default: ~1x num systems

  // Per-system caps to avoid clumping.
  int dynamic_poi_max_unresolved_anomalies_per_system{3};
  int dynamic_poi_max_active_caches_per_system{2};


  // --- Intel / contact prediction ---
  //
  // When a contact is lost (fog-of-war), the simulation may extrapolate
  // a last-known position using a simple constant-velocity estimate derived
  // from the contact's two most recent detections in the same system.
  //
  // To avoid chasing stale tracks forever, extrapolation is clamped to
  // at most this many days after the last detection.
  int contact_prediction_max_days{30};

  // --- Intel / contact uncertainty ---
  //
  // When enabled, detected ship contacts remember an estimated position
  // uncertainty radius at last detection. As contacts age, their uncertainty
  // expands based on the estimated target speed.
  //
  // This supports:
  //  - UI: uncertainty rings on the system map and intel window.
  //  - Gameplay: simple deterministic "search wandering" when pursuing a lost
  //    contact (prevents perfect tail-chasing of stale tracks).
  bool enable_contact_uncertainty{true};

  // Measurement error at detection time is modeled as a fraction of the
  // effective detection range (after signature + EW). We interpolate between
  // these fractions based on distance/range.
  double contact_uncertainty_center_fraction_of_detect_range{0.01};
  double contact_uncertainty_edge_fraction_of_detect_range{0.08};

  // Absolute minimum measurement uncertainty (mkm) at the moment of detection.
  double contact_uncertainty_min_mkm{0.5};

  // Additional measurement error multiplier per point of target ECM strength.
  // error *= (1 + ecm * multiplier).
  double contact_uncertainty_ecm_strength_multiplier{0.25};

  // Uncertainty growth per day (mkm/day) as a fraction of estimated target
  // speed (mkm/day).
  double contact_uncertainty_growth_fraction_of_speed{0.25};

  // Optional minimum growth floor (mkm/day).
  double contact_uncertainty_growth_min_mkm_per_day{0.0};

  // Cap uncertainty radius to avoid runaway values on very old contacts.
  double contact_uncertainty_max_mkm{5000.0};

  // When pursuing a lost contact, wander within this fraction of the current
  // uncertainty radius around the predicted track.
  //
  // 0 disables the search offset (pure tail-chasing).
  double contact_search_offset_fraction{0.5};

  // Lost-contact search uses a deterministic low-discrepancy pattern (a
  // Fibonacci / golden-angle spiral) to pick successive search waypoints
  // within the uncertainty disk.
  //
  // This controls how many waypoint samples it takes to "fill" the disk from
  // center to edge before subsequent waypoints concentrate near the edge.
  int contact_search_pattern_points{64};

  // Safety cap: limit the search radius to what the pursuing ship can plausibly
  // traverse during the remaining contact-prediction budget.
  //
  // search_radius <= ship_speed_mkm_per_day * remaining_prediction_days * fraction
  //
  // Set to 0 to disable this cap.
  double contact_search_radius_speed_cap_fraction{1.0};

  // --- Intel / sensor fusion ---
  //
  // When enabled, simultaneous detections from multiple sensor sources are fused
  // into a single contact update per viewer. This reduces position uncertainty
  // in overlapping sensor networks in a deterministic way (no RNG).
  bool enable_contact_sensor_fusion{true};

  // --- Intel / contact identity fog-of-war ---
  //
  // Contacts are always tracked by internal ship_id, but the *revealed identity*
  // (design/name) can be gated by measurement uncertainty. This makes stealth/ECM
  // meaningfully delay identification without requiring a full track-id system.
  bool enable_contact_identity_fog{true};

  // Reveal thresholds based on the contact's uncertainty at detection time.
  // If the uncertainty is above these thresholds, the corresponding field may
  // remain unknown until a better detection is achieved.
  double contact_identity_reveal_design_uncertainty_mkm{30.0};
  double contact_identity_reveal_name_uncertainty_mkm{10.0};

  // Fallback assumed target speed (km/s) used for uncertainty growth when we have
  // neither a velocity estimate nor a known design for the contact.
  double contact_uncertainty_unknown_speed_km_s{100.0};

  // Master toggle for space combat.
  //
  // When disabled, tick_combat() is skipped. Ships will still move and can still
  // carry out non-combat orders; ground combat/invasions are unaffected.
  bool enable_combat{true};

  // --- Beam weapon accuracy / tracking ---
  //
  // When enabled, beam weapons (and planetary beam batteries) apply an accuracy
  // multiplier based on:
  //  - range (reduced accuracy near max range), and
  //  - relative angular velocity between attacker and target ("tracking"), and
  //  - the target's effective signature multiplier (stealth/EMCON makes tracking harder).
  //
  // This introduces maneuver + EMCON tradeoffs and makes sensor choices matter
  // in combat without adding per-weapon RNG (damage is scaled by expected hit chance).
  bool enable_beam_hit_chance{true};

  // Baseline hit chance at close range against a stationary, normal-signature target.
  double beam_base_hit_chance{0.95};

  // Minimum hit chance floor (applied after range + tracking multipliers).
  double beam_min_hit_chance{0.05};

  // Range penalty at maximum weapon range.
  // 0.0 => no range penalty, 0.5 => 50% accuracy at max range.
  double beam_range_penalty_at_max{0.40};

  // Reference angular velocity (radians/day) that a ship with reference sensors can
  // track with ~50% effectiveness (before signature scaling).
  double beam_tracking_ref_ang_per_day{0.60};

  // Reference sensor range (mkm) used to scale tracking with sensor quality.
  double beam_tracking_reference_sensor_range_mkm{400.0};

  // Minimum sensor range (mkm) used for tracking even if the attacker's sensors are offline.
  double beam_tracking_min_sensor_range_mkm{50.0};

  // Exponent applied to target signature multiplier when influencing tracking.
  // 0.5 means sqrt(sig): stealth helps, but does not dominate.
  double beam_signature_exponent{0.5};

  // Reference angular velocity for colony beam batteries (radians/day).
  // Colonies are assumed to have slightly better fire control than ships at equal sensor level.
  double colony_beam_tracking_ref_ang_per_day{0.80};


  // --- Beam line-of-fire attenuation / scattering (experimental) ---
  //
  // When enabled, beam weapons apply an additional transmission multiplier based
  // on the nebula/storm environment *along the line of fire* between attacker
  // and target.
  //
  // Implementation notes:
  //  - Uses an SDF-style distance estimate (f/|grad f|) on the local sensor
  //    environment multiplier field to take adaptive steps along the ray.
  //  - Integrates log(transmission) along the ray to estimate a geometric-mean
  //    transmittance (better matches multiplicative absorption).
  //  - Uses deterministic, seeded jitter for stochastic sampling stability.
  //
  // The resulting multiplier is clamped and exponentiated by beam_los_strength.
  bool enable_beam_los_attenuation{false};
  double beam_los_strength{1.00};
  double beam_los_iso_env{0.80};
  double beam_los_sdf_step_scale{0.90};
  double beam_los_min_step_mkm{60.0};
  double beam_los_max_step_mkm{900.0};
  double beam_los_grad_epsilon_mkm{40.0};
  int beam_los_max_steps{48};
  double beam_los_sample_jitter{0.35};
  double beam_los_min_multiplier{0.10};
  double beam_los_max_multiplier{1.00};
  bool beam_los_use_geometric_mean{true};

  // Optional: convert a portion of the energy "lost" to the medium into
  // low-intensity splash damage near the beam segment.
  //
  // This models turbulent scattering / bloom in dense nebula pockets and makes
  // fighting inside heavy terrain more chaotic (optionally including friendly fire).
  bool enable_beam_scatter_splash{false};
  // Fraction of (1 - beam_los_multiplier) damage that becomes splash.
  double beam_scatter_fraction_of_lost{0.35};
  // Splash radius around the beam segment (million-km).
  double beam_scatter_radius_mkm{0.35};
  // If true, splash can damage non-hostile ships too.
  bool beam_scatter_can_hit_friendly{false};


  // --- Missile guidance / homing ---
  //
  // Missiles are modeled as time-of-flight salvos. When homing is enabled,
  // in-flight salvos steer toward their moving target rather than following
  // a fixed straight-line track to the target's launch position.
  bool enable_missile_homing{true};

  // If true, a missile's configured missile_range_mkm also acts as an in-flight
  // fuel/range limit. Salvos that exhaust their remaining range self-destruct
  // without dealing damage (even if their ETA has not elapsed).
  bool missile_range_limits_flight{true};

  // Expected missile hit chance (no RNG) based on target maneuvering +
  // ECM/ECCM + signature (stealth/EMCON).
  //
  // When enabled, missile damage that "leaks" through point defense is scaled
  // by a computed hit chance (clamped to [missile_min_hit_chance, 1]).
  bool enable_missile_hit_chance{true};
  double missile_base_hit_chance{0.90};
  double missile_min_hit_chance{0.05};

  // Reference angular velocity (radians/day) for missile guidance/tracking.
  // Higher values make missiles less sensitive to fast target maneuvers.
  double missile_tracking_ref_ang_per_day{2.0};

  // Exponent applied to target signature multiplier when influencing missile
  // guidance/tracking. (See beam_signature_exponent for beams.)
  double missile_signature_exponent{0.35};

  // --- Boarding / capture (space combat) ---
  //
  // When enabled (and enable_combat is also true), ships carrying troops may
  // attempt to board and capture disabled hostile ships they are actively
  // attacking.
  bool enable_boarding{true};

  // Maximum range (mkm) at which boarding can be attempted.
  double boarding_range_mkm{0.1};

  // Target must be at or below this remaining HP fraction to be considered
  // "disabled" and boardable.
  double boarding_target_hp_fraction{0.25};

  // If true, a ship must have its shields fully down before it can be boarded.
  bool boarding_require_shields_down{true};

  // Minimum troop strength required on the attacker to attempt boarding.
  double boarding_min_attacker_troops{10.0};

  // Defender "crew resistance" modeled as (max_hp * boarding_defense_hp_factor),
  // added on top of any embarked troops.
  double boarding_defense_hp_factor{1.0};

  // Casualty scaling per boarding attempt (fractions of involved troops).
  double boarding_attacker_casualty_fraction{0.20};
  double boarding_defender_casualty_fraction{0.30};

  // Log failed boarding attempts as events (can be noisy in large combats).
  bool boarding_log_failures{false};

  // Combat event logging controls.
  //
  // Combat already logs ship destruction. These thresholds control whether we also
  // emit events for ships that take damage but survive (useful for debugging and
  // for "what just happened?" UX in the event log).
  //
  // An event is logged for a damaged ship when either:
  // - damage >= combat_damage_event_min_abs, OR
  // - damage/max_hp >= combat_damage_event_min_fraction.
  double combat_damage_event_min_abs{1.0};
  double combat_damage_event_min_fraction{0.10};

  // If a damaged ship's remaining HP fraction is <= this value, log the damage
  // event as Warn (otherwise Info).
  double combat_damage_event_warn_remaining_fraction{0.25};

  // --- Wrecks / salvage ---
  // When enabled, destroyed ships may leave salvageable wrecks.
  bool enable_wrecks{true};

  // Fraction of a destroyed ship's hull mass converted into salvage minerals.
  // Hull salvage uses shipyard build_costs_per_ton (if available) as a proxy
  // for mineral composition.
  double wreck_hull_salvage_fraction{0.25};

  // Fraction of a destroyed ship's carried cargo that remains recoverable.
  double wreck_cargo_salvage_fraction{0.75};

  // Optional decay: if > 0, wrecks older than this many days are removed.
  // 0 disables decay.
  int wreck_decay_days{0};

  // --- Orbital bombardment (ship -> colony) ---
  //
  // Ships executing BombardColony orders convert their weapon_damage into
  // daily "orbital strike" damage applied to the target colony.
  //
  // Damage is applied in the following order:
  //  1) Defender ground forces
  //  2) Installations (destroyed based on a derived "HP" value)
  //  3) Population
  //
  // Fraction of weapon range to hold at when bombarding (0..1).
  //
  // Example: 0.9 means "try to stay at 90% of max range".
  double bombard_standoff_range_fraction{0.9};

  // Ground force strength removed per 1 point of bombardment damage.
  double bombard_ground_strength_per_damage{1.0};

  // Installation HP derived from construction_cost.
  //
  // installation_hp = max(1.0, construction_cost * bombard_installation_hp_per_construction_cost)
  double bombard_installation_hp_per_construction_cost{0.02};

  // Population loss (in millions) per 1 point of *remaining* bombardment damage
  // after troops and installations.
  double bombard_population_millions_per_damage{0.05};

  // --- Ground combat / troops ---
  // How much ground force "strength" is produced per training point per day.
  //
  // Training converts Colony::troop_training_queue into Colony::ground_forces in
  // tick_ground_combat().
  double troop_strength_per_training_point{1.0};

  // Optional mineral costs per trained strength.
  //
  // Set to 0 to disable costs. When enabled, training is scaled down to the
  // maximum affordable amount for the day.
  double troop_training_duranium_per_strength{0.0};
  double troop_training_neutronium_per_strength{0.0};

  // Ground battle daily loss scaling.
  //
  // Each day, each side loses (ground_combat_loss_factor * opposing_strength),
  // modified by defender fortifications.
  double ground_combat_loss_factor{0.05};

  // Fortification effectiveness scale.
  //
  // Defender losses are divided by (1 + forts * fortification_defense_scale).
  double fortification_defense_scale{0.01};

  // Fortification offensive effectiveness scale.
  //
  // Attacker losses are multiplied by (1 + forts * fortification_attack_scale).
  //
  // This models the defender's prepared positions (trenches, bunkers, minefields)
  // translating into higher attacker attrition.
  double fortification_attack_scale{0.005};

  // Defender ground fire support from weapon installations.
  //
  // Installations with weapon_damage contribute additional attacker casualties
  // during ground combat:
  //   artillery_loss = weapon_damage_total * ground_combat_defender_artillery_strength_per_weapon_damage
  //
  // Set to 0 to disable the effect.
  double ground_combat_defender_artillery_strength_per_weapon_damage{0.15};

  // Fortification degradation during ground combat.
  //
  // Each day, attackers accumulate fortification damage:
  //   fortification_damage += attacker_strength * ground_combat_fortification_damage_per_attacker_strength_day
  //
  // Effective fortification points are reduced by this accumulated damage. At
  // battle resolution, the damage is applied by destroying fortification
  // installations on the colony.
  //
  // Set to 0 to disable fortification degradation.
  double ground_combat_fortification_damage_per_attacker_strength_day{0.005};

  // Ground combat fatigue / intensity.
  //
  // As a ground battle drags on, both sides tend to dig in and casualty rates
  // drop. This multiplier scales the daily loss factor (and fortification
  // damage rate) as a function of battle days fought:
  //   mult(day) = max(ground_combat_fatigue_min_multiplier,
  //                   1 / (1 + ground_combat_fatigue_per_day * day))
  //
  // Set ground_combat_fatigue_per_day to 0 to disable.
  double ground_combat_fatigue_per_day{0.03};
  double ground_combat_fatigue_min_multiplier{0.25};

  // Collateral damage from ground combat (installations + civilian population).
  //
  // Each day, collateral damage is derived from the total ground strength lost
  // that day (attacker_loss + defender_loss).
  //
  // Installations: casualties generate 'damage points' which are applied to
  // colony installations (excluding fortification installations). Installation
  // HP uses the same cost-derived model as orbital bombardment
  // (bombard_installation_hp_per_construction_cost).
  //
  // Population: casualties directly translate into civilian population loss.
  //
  // Set to 0 to disable either effect.
  double ground_combat_installation_damage_per_strength_lost{0.01};
  double ground_combat_population_millions_per_strength_lost{0.0002};

  // Safety cap on the number of non-fortification installations that can be
  // destroyed per colony per day by collateral damage.
  //
  // Set to -1 for unlimited.
  int ground_combat_collateral_max_installations_destroyed_per_day{20};

  // --- Terraforming ---
  // Temperature change (Kelvin) per terraforming point per day.
  double terraforming_temp_k_per_point_day{0.1};

  // Atmosphere change (atm) per terraforming point per day.
  double terraforming_atm_per_point_day{0.001};

  // Optional operational mineral costs per terraforming point.
  //
  // When enabled (>0), terraforming is scaled down to the maximum affordable
  // amount for the day, and the corresponding minerals are consumed from the
  // colony stockpile.
  //
  // This mirrors troop training costs and makes long-term terraforming projects
  // naturally interact with logistics/industry without hard-coding special-case
  // installations.
  double terraforming_duranium_per_point{0.0};
  double terraforming_neutronium_per_point{0.0};

  // Optional: scale terraforming effectiveness based on body mass.
  //
  // Small bodies are generally easier to terraform than large ones. When
  // enabled, the temperature/atmosphere deltas per point are multiplied by:
  //   scale = 1 / max(terraforming_min_mass_earths, mass_earths)^exponent
  //
  // Bodies without mass metadata (mass_earths <= 0) are treated as mass=1.
  bool terraforming_scale_with_body_mass{true};
  double terraforming_min_mass_earths{0.10};
  double terraforming_mass_scaling_exponent{1.0};

  // When true and both temperature+atmosphere targets are set, terraforming
  // points are treated as a shared budget that must be split between the two
  // axes.
  //
  // This avoids an implicit "double benefit" where a single point would
  // simultaneously advance both temperature and atmospheric pressure.
  bool terraforming_split_points_between_axes{true};

  // When within these tolerances of the target, terraforming is considered complete.
  double terraforming_temp_tolerance_k{0.5};
  double terraforming_atm_tolerance{0.01};

  // --- Fleet cohesion helpers ---
  //
  // These options are purely simulation-side quality-of-life features to make
  // fleet-issued orders behave more like a "real" group movement.
  //
  // When enabled, ships that belong to the same fleet and have the same *current*
  // movement order (front of queue) will match speed to the slowest ship in that
  // cohort. This keeps fleets from stringing out due to speed differences.
  bool fleet_speed_matching{true};

  // When enabled, if multiple ships in the same fleet are trying to transit the
  // same jump point in the same system, the simulation will hold the transit
  // until *all* of those ships have arrived at the jump point. This prevents
  // faster ships from jumping early and leaving slower ships behind.
  bool fleet_coordinated_jumps{true};

  // When enabled, fleets may apply simple formations as a cohesion helper.
  //
  // Formation settings live on Fleet and are applied for some cohorts
  // (currently: move-to-point and attack) by offsetting each ship's target.
  bool fleet_formations{true};

  // --- Auto-freight (mineral logistics) ---
  //
  // Ships with Ship::auto_freight enabled will, when idle, automatically haul minerals
  // between same-faction colonies to relieve shipyard/construction stalls.

  // If true, auto-freight will try to bundle multiple minerals into a single
  // trip when pulling from the same source colony and delivering to the same
  // destination colony. This greatly reduces "one mineral per trip" inefficiency
  // when a colony is missing several inputs at once (e.g. shipyards with multiple
  // mineral costs, construction orders, fuel top-ups).
  bool auto_freight_multi_mineral{true};
  //
  // Minimum tons moved in a single auto-freight task (avoids tiny shipments).
  double auto_freight_min_transfer_tons{1.0};

  // When pulling minerals from a source colony, auto-freight will never take more than
  // this fraction of that colony's computed exportable surplus in a single task.
  // (0.0 = take nothing, 1.0 = take full surplus).
  double auto_freight_max_take_fraction_of_surplus{0.75};

  // For non-mining "industry" installations that consume minerals (e.g. refineries),
  // auto-freight will try to keep this many *days* of input stockpiled at the colony.
  //
  // Example: an installation consumes 2 Duranium/day and the buffer is 30 days =>
  // desired stockpile is 60 Duranium.
  double auto_freight_industry_input_buffer_days{30.0};


  // --- Geological surveys (procedural mineral deposit discoveries) ---
  //
  // Colonies that build Geological Survey installations can occasionally discover
  // additional mineral deposits on their body over time. This helps keep the
  // mid/late-game economy from hard-stalling once initial deposits are exhausted,
  // while remaining deterministic (seeded by day+colony id).
  bool enable_geological_survey{true};

  // Baseline daily probability *per* geological_survey installation to discover a deposit.
  // (Scaled by region richness, mining tech multipliers, and depletion.)
  double geological_survey_discovery_chance_per_day_per_installation{0.001};

  // Hard cap to prevent extremely large colonies from rolling hundreds of times per day.
  int geological_survey_max_discoveries_per_colony_per_day{2};

  // Deposit yield range (tons) before modifiers.
  double geological_survey_min_deposit_tons{5000.0};
  double geological_survey_max_deposit_tons{150000.0};

  // When total remaining deposits on the body fall below this threshold (tons),
  // geological survey discovery odds are boosted linearly as deposits approach zero.
  double geological_survey_depletion_threshold_tons{500000.0};

  // Maximum multiplicative boost applied at full depletion (0 remaining).
  // Example: 4.0 -> up to 5x base chance.
  double geological_survey_depletion_chance_boost{4.0};

// --- Colony conditions / local events ---
//
// Colonies can accumulate temporary conditions (strikes, accidents, festivals)
// that modify local production for a limited duration.
//
// Conditions are deterministic and save-game persistent; random rolls are
// driven by a hashed RNG seeded by (day, colony id).
bool enable_colony_conditions{true};

// When enabled, colonies periodically roll for new conditions.
bool enable_colony_events{true};

// Roll cadence for colony events (days). The default is weekly.
int colony_event_roll_interval_days{7};

// Baseline probability for a negative colony event on a roll.
// (Further scaled by colony stability, population, and "event fatigue".)
double colony_event_negative_chance_per_roll{0.03};

// Baseline probability for a positive colony event on a roll.
double colony_event_positive_chance_per_roll{0.02};

// Hard cap on (negative + positive) probability per roll.
double colony_event_max_combined_chance_per_roll{0.20};

// Event fatigue: each existing condition multiplies event chances by this factor.
// (0.75 means 1 condition -> 75% chance, 2 conditions -> 56%, etc.)
double colony_event_existing_condition_chance_factor{0.75};

// Safety cap: maximum number of concurrent conditions stored on a colony.
int colony_condition_max_active{4};

// --- Colony stability output scaling ---
//
// When enabled, colony stability penalizes local production throughput when unstable.
// Stability is computed by colony_stability_status_for_colony() and normalized to [0,1].
//
// Colonies with stability >= colony_stability_neutral_threshold are unaffected (x1.0).
// Below that threshold, output is scaled down linearly toward
// colony_stability_min_output_multiplier at stability=0.
bool enable_colony_stability_output_scaling{true};

// Stability threshold above which colonies take no production penalty.
double colony_stability_neutral_threshold{0.80};

// Minimum production multiplier at stability=0.
double colony_stability_min_output_multiplier{0.50};





  // --- Auto-tanker (fuel logistics) ---
  //
  // Ships with Ship::auto_tanker enabled will, when idle, automatically seek out
  // friendly idle ships that are low on fuel and transfer fuel ship-to-ship.
  //
  // To avoid fighting other automation and player intent, auto-tanker only
  // targets ships that have auto_refuel disabled (i.e. ships that are not
  // already configured to route themselves to a colony for refueling).

  // Target ships below this fuel fraction are considered requesting fuel.
  // Example: 0.25 means below 25%.
  double auto_tanker_request_threshold_fraction{0.25};

  // Auto-tanker attempts to fill a target ship up to this fuel fraction.
  // Example: 0.9 means try to reach 90% full (subject to tanker reserves).
  double auto_tanker_fill_target_fraction{0.90};

  // Minimum fuel transfer size (tons) for an auto-tanker dispatch.
  // Prevents tiny 0.001t transfers due to floating point noise.
  double auto_tanker_min_transfer_tons{1.0};

  // --- Auto-troop transport (ground logistics) ---
  //
  // Ships with Ship::auto_troop_transport enabled will, when idle, automatically
  // ferry ground troops between owned colonies to satisfy garrison targets and
  // (optionally) reinforce ongoing defensive ground battles.

  // Minimum troop strength moved in a single auto-troop task (avoids tiny transfers).
  double auto_troop_min_transfer_strength{1.0};

  // When pulling troops from a source colony, auto-troop will never take more than
  // this fraction of that colony's computed surplus in a single task.
  // (0.0 = take nothing, 1.0 = take full surplus).
  double auto_troop_max_take_fraction_of_surplus{0.75};

  // When reinforcing an active defensive ground battle, auto-troop computes a
  // best-effort "hold the line" defender target using a square-law estimate and
  // this margin factor (>= 1.0 means safer).
  double auto_troop_defense_margin_factor{1.10};

  // If true, auto-troop considers ongoing defensive ground battles as urgent troop needs.
  bool auto_troop_consider_active_battles{true};

  // --- Auto-colonist transport (population logistics) ---
  //
  // Ships with Ship::auto_colonist_transport enabled will, when idle, automatically
  // ferry colonists between owned colonies to satisfy population targets.
  //
  // A destination colony is eligible when:
  //   colony.population_target_millions > colony.population_millions
  //
  // A source colony can export any population above:
  //   max(colony.population_reserve_millions, colony.population_target_millions)
  //
  // By default this export floor must be non-zero (auto_colonist_require_source_floor = true)
  // to avoid accidentally draining colonies when only destination targets are configured.

  // Minimum colonists moved in a single auto-colonist task (in millions).
  double auto_colonist_min_transfer_millions{1.0};

  // When pulling colonists from a source colony, auto-colonist will never take more than
  // this fraction of that colony's computed surplus in a single task.
  double auto_colonist_max_take_fraction_of_surplus{0.75};

  // If true, only export from colonies that have a non-zero export floor
  // (population_target_millions or population_reserve_millions).
  bool auto_colonist_require_source_floor{true};

  // --- Dynamic piracy / raids ---
  //
  // When enabled, AI pirate factions can spawn occasional "raid" groups in
  // systems with high piracy risk and valuable civilian activity (freighters,
  // colonies). This keeps the mid/late game from becoming permanently "safe"
  // after the initial pirate forces are defeated.
  bool enable_pirate_raids{true};

  // Baseline daily probability (per pirate faction) to attempt spawning a raid.
  // The final chance is scaled by target value, piracy risk, and a per-faction
  // cap on total pirate ships.
  double pirate_raid_base_chance_per_day{0.02};

  // Default piracy risk (0..1) used for systems that are not assigned to a
  // procedural Region (e.g. the handcrafted Sol scenario).
  double pirate_raid_default_system_risk{0.25};

  // When enabled, patrol missions by non-pirate factions dynamically suppress
  // piracy within Regions, reducing the effective pirate raid weighting.
  //
  // Suppression is a first-order filter toward a target value computed from the
  // total "patrol power" currently present in the region:
  //   target = 1 - exp(-patrol_power / pirate_suppression_power_scale)
  //   effective_risk = pirate_risk * (1 - pirate_suppression)
  bool enable_pirate_suppression{true};

  // Power scale for the suppression curve above. Smaller values mean fewer
  // ships are needed to significantly suppress piracy.
  double pirate_suppression_power_scale{50.0};

  // Fraction of the gap to target suppression applied per day (0..1).
  // Higher values make suppression ramp up/down faster.
  double pirate_suppression_adjust_fraction_per_day{0.15};

  // Risk exponent used when weighting candidate raid target systems.
  // Higher values concentrate raids more heavily in the highest-risk areas.
  double pirate_raid_risk_exponent{1.5};

  // Hard cap on the number of ships a single pirate faction is allowed to have
  // before raid spawning pauses.
  int pirate_raid_max_total_ships_per_faction{18};

  // Don't spawn new raids into a system that already has more than this many
  // pirate ships present (helps prevent stacking multiple raids in one place).
  int pirate_raid_max_existing_pirate_ships_in_target_system{2};

  // Min/max ships spawned for a single raid (clamped by remaining capacity
  // under pirate_raid_max_total_ships_per_faction).
  int pirate_raid_min_spawn_ships{1};
  int pirate_raid_max_spawn_ships{3};

  // If true, log a General event when a raid is spawned in a system discovered
  // by a player faction. (Contacts are still revealed via the existing
  // detection/intel event system.)
  bool pirate_raid_log_event{false};


  // --- Civilian trade convoys (procedural shipping traffic) ---
  //
  // This is an ambient simulation layer that makes the sector feel "alive":
  // neutral civilian freighters will travel along the busiest interstellar
  // trade lanes, creating organic piracy targets and visible traffic.
  //
  // Civilian convoys are owned by a dedicated AI_Passive faction, have their
  // movement/fuel abstracted (no fuel burn), and are excluded from victory.
  bool enable_civilian_trade_convoys{true};

  // Hard cap on the number of active civilian convoy ships.
  int civilian_trade_convoy_max_ships{18};

  // Minimum number of convoys to maintain once at least one trade lane exists.
  int civilian_trade_convoy_min_ships{2};

  // Target convoy count scales with sqrt(total trade volume):
  //   target = round(sqrt(total_volume) * civilian_trade_convoy_target_sqrt_mult)
  double civilian_trade_convoy_target_sqrt_mult{0.30};

  // Max number of new convoys spawned per day while approaching the target.
  int civilian_trade_convoy_max_spawn_per_day{2};

  // Consider only the top N trade lanes (by volume) when spawning convoys.
  int civilian_trade_convoy_consider_top_lanes{16};

  // Wait time at each endpoint before heading back (adds variation and reduces
  // "clumping" at jump points). Actual wait = base + [0, jitter].
  int civilian_trade_convoy_endpoint_wait_days_base{1};
  int civilian_trade_convoy_endpoint_wait_days_jitter{3};

  // How full civilian convoy cargo holds should be (0..1).
  //
  // - When enable_civilian_trade_convoy_cargo_transfers is false, cargo is purely
  //   cosmetic / salvageable and does not directly transfer colony minerals.
  // - When cargo transfers are enabled, this is the target fill fraction for
  //   each trade run (load/unload is throughput-limited, so runs may be partially
  //   filled if stockpiles are low or export reserves block transfers).
  double civilian_trade_convoy_cargo_fill_fraction{0.80};

  // If true, civilian trade convoys attempt to load and unload real minerals at
  // colony hubs (driven by TradeNetwork lanes).
  //
  // This creates a lightweight "civilian freight" layer that can smooth local
  // shortages and makes piracy/escorts matter economically.
  bool enable_civilian_trade_convoy_cargo_transfers{true};

  // Export safeguards for civilian trade convoys (Merchant Guild) when loading
  // minerals from colonies:
  //
  // Convoys will not export below a computed reserve floor derived from:
  // - colony mineral_reserves / mineral_targets, and
  // - the colony owner's current logistics needs (shipyards, industry input
  //   buffers, fuel/rearm requirements, etc.).
  //
  // This multiplier scales that reserve floor (>= 0). Values > 1 leave extra
  // safety stock behind.
  double civilian_trade_convoy_export_reserve_multiplier{1.25};

  // Additional hard floors (tons) for critical strategic goods even if the colony
  // has no explicit reserves/targets.
  double civilian_trade_convoy_export_min_fuel_reserve_tons{500.0};
  double civilian_trade_convoy_export_min_munitions_reserve_tons{250.0};


  // How strongly civilian convoys avoid high-risk endpoints (0..1).
  // Higher values shift traffic toward safer corridors, indirectly rewarding
  // suppression patrols.
  double civilian_trade_convoy_risk_aversion{0.65};

  // Lower bound on the convoy weight multiplier after risk aversion is applied.
  // Prevents all traffic from collapsing to a single lane when piracy is high.
  double civilian_trade_convoy_min_risk_weight{0.35};

  // Additional risk term from blockades.
  //
  // Civilian lane endpoint risk is approximated as:
  //   risk ~= piracy_risk + (civilian_trade_convoy_blockade_risk_weight * blockade_pressure)
  // where blockade_pressure is derived from hostile presence near colonies in the system.
  //
  // 0 disables this weighting.
  double civilian_trade_convoy_blockade_risk_weight{0.80};


  // --- Civilian shipping loss memory (economic disruption) ---
  //
  // Merchant shipping does not instantly recover after a raid. Instead, the
  // simulation can keep a short "memory" of recent civilian losses (based on
  // Merchant Guild ship wrecks). This pressure can then influence:
  //  - civilian convoy route selection
  //  - trade prosperity (representing insurance and disruption)
  //
  // memory_days controls how long losses remain relevant. 0 disables the
  // mechanic.
  int civilian_shipping_loss_memory_days{60};

  // Raw loss "score" (sum of decayed wreck weights) is mapped into a normalized
  // 0..1 pressure via:
  //   pressure = 1 - exp(-score / civilian_shipping_loss_pressure_scale)
  // Larger scale => losses need to be more frequent to produce the same
  // pressure.
  double civilian_shipping_loss_pressure_scale{2.0};

  // Additional risk term applied to civilian trade convoy lane weighting.
  //
  // Civilian endpoint risk becomes:
  //   risk ~= piracy_risk
  //        + (civilian_trade_convoy_blockade_risk_weight * blockade_pressure)
  //        + (civilian_trade_convoy_shipping_loss_risk_weight * shipping_loss_pressure)
  //
  // 0 disables this weighting.
  double civilian_trade_convoy_shipping_loss_risk_weight{0.90};


  // --- Civilian trade activity feedback (prosperity) ---
  //
  // When enabled, the simulation records a decayed per-system "trade activity"
  // score derived from real civilian cargo transfers (Merchant Guild convoys).
  //
  // This score can then boost Trade Prosperity in that system, providing an
  // emergent positive feedback loop:
  //   more safe traffic -> more prosperity -> more production -> more trade.
  //
  // This is intentionally macro/approximate; it does not model prices.
  bool enable_civilian_trade_activity_prosperity{true};

  // Memory window (days) for the trade activity score decay.
  // <= 0 disables the decay and clears the score.
  int civilian_trade_activity_memory_days{90};

  // Trade activity score is mapped into a 0..1 factor via:
  //   factor = 1 - exp(-score / civilian_trade_activity_score_scale_tons)
  // Larger scale => more volume required to reach the same factor.
  double civilian_trade_activity_score_scale_tons{20000.0};

  // Caps for the derived prosperity bonuses.
  //
  // - hub_score_bonus_cap is added to TradeNetwork hub_score (clamped to 1).
  // - market_size_bonus_cap is a multiplicative boost to TradeNetwork market_size.
  //
  // The actual applied bonus is cap * factor.
  double civilian_trade_activity_hub_score_bonus_cap{0.18};
  double civilian_trade_activity_market_size_bonus_cap{0.22};


  // --- Trade network diplomacy modifiers ---
  //
  // When enabled, TradeNetwork lane volumes are scaled based on the diplomatic
  // relationship between the dominant factions in the origin/destination
  // systems.
  //
  // This makes diplomacy feel economically meaningful (e.g. signing a trade
  // agreement can measurably increase interstellar commerce, while wars
  // suppress trade to a "smuggling" trickle).
  //
  // Note: this is intentionally approximate and meant to be a lightweight
  // macro-economic signal. It affects:
  //  - Trade lane overlays
  //  - Civilian trade convoy spawning (which is driven by lane volumes)
  bool enable_trade_network_diplomacy_multipliers{true};

  // Volume multiplier applied when the dominant factions are trade partners
  // (trade agreement / alliance / mutual friendly).
  double trade_network_volume_mult_trade_partner{1.15};

  // Volume multiplier applied when the factions are non-hostile but not trade
  // partners.
  double trade_network_volume_mult_neutral{0.90};

  // Volume multiplier applied when the factions are hostile.
  // Keep this > 0 if you want "smuggling" lanes to still exist.
  double trade_network_volume_mult_hostile{0.30};


  // --- Research agreements (diplomacy -> research cooperation) ---
  //
  // A Research Agreement treaty (and Alliances) can provide:
  //  - a per-partner research output bonus (applied to research point generation),
  //  - a symmetric collaboration bonus based on shared daily RP generation,
  //  - and a tech assistance bonus when researching a tech already known by a partner.
  bool enable_research_agreement_bonuses{true};
  double research_agreement_output_bonus_per_partner{0.05};
  double research_agreement_output_bonus_cap{0.25};
  double research_agreement_collaboration_bonus_fraction{0.10};
  double research_agreement_tech_help_bonus_per_partner{0.10};
  double research_agreement_tech_help_bonus_cap{0.50};


  // --- AI trade security patrols (procedural) ---
  //
  // When enabled, AI-controlled explorer empires will periodically retask their
  // Patrol Fleet to protect regions where their economic exposure (trade lane
  // volume involving their colonies) intersects with high piracy risk.
  //
  // This ties the procedural trade network into the piracy suppression system:
  // patrol presence increases Region::pirate_suppression, which in turn lowers
  // raid rates and attracts civilian traffic back to safe corridors.
  bool enable_ai_trade_security_patrols{true};

  // Re-evaluate patrol targets every N days (staggered per fleet).
  // <= 0 => every day.
  int ai_trade_security_patrol_retarget_interval_days{14};

  // Consider only the top N trade lanes (by volume) when scoring security needs.
  int ai_trade_security_patrol_consider_top_lanes{24};

  // Ignore lanes with total_volume below this threshold.
  double ai_trade_security_patrol_min_lane_volume{1.0};

  // Extra weight applied when a trade corridor passes through a system that
  // contains a colony owned by the patrolling faction.
  double ai_trade_security_patrol_own_colony_weight{1.5};

  // How strongly piracy risk amplifies patrol demand.
  // Need ~= volume_share * (0.20 + risk_weight * risk).
  double ai_trade_security_patrol_risk_weight{1.2};


  // --- Trade prosperity (economy bonus) ---
  //
  // When enabled, colonies receive a small output bonus derived from their
  // star system's trade market size and hub score (see TradeNetwork).
  // The bonus scales with colony population and is reduced by local piracy
  // risk and blockade pressure.
  //
  // This multiplier applies to:
  //  - non-mining industry production
  //  - research point generation
  //  - shipyard throughput
  //  - construction point generation
  bool enable_trade_prosperity{true};

  // Maximum additive output bonus (e.g. 0.20 => up to +20% output).
  double trade_prosperity_max_output_bonus{0.20};

  // Trade market size at which the market factor reaches 50% (half-saturation).
  double trade_prosperity_market_size_half_bonus{0.45};

  // Population (millions) at which the population factor reaches 50%.
  double trade_prosperity_pop_half_bonus_millions{2500.0};

  // Weight of hub score in the base bonus (0..1).
  // 0 => hub score ignored; 1 => hub score fully gates the base bonus.
  double trade_prosperity_hub_influence{0.35};

  // Optional diplomacy-driven market access boost.
  //
  // When enabled, a faction's active trade partners increase the *effective*
  // market size used when calculating TradeProsperityStatus::market_factor.
  //
  // This is intended to make trade treaties feel mechanically rewarding even
  // before deeper inter-faction logistics are simulated.
  bool enable_trade_prosperity_treaty_market_boost{true};

  // Effective market size multiplier added per trade partner.
  // Example: 0.10 with 3 trade partners => market_size *= (1 + 0.30).
  double trade_prosperity_treaty_market_boost_per_trade_partner{0.10};

  // Clamp on the total additive market size boost from treaties.
  // Example: 0.50 => market_size *= (1 + min(0.50, per_partner * N)).
  double trade_prosperity_treaty_market_boost_max{0.50};

  // Penalty applied from piracy risk (0..1). Higher => piracy more disruptive.
  double trade_prosperity_piracy_risk_penalty{0.85};

  // Penalty applied from blockade pressure (0..1). Higher => blockades more disruptive.
  double trade_prosperity_blockade_pressure_penalty{1.0};

  // Penalty applied from recent civilian shipping losses (0..1).
  //
  // This models a lightweight "insurance + confidence" effect: after merchant
  // losses, trade activity remains depressed for a while even if pirates leave.
  // It uses the shipping loss pressure derived from Merchant Guild wrecks in
  // the colony's system (see civilian_shipping_loss_memory_days).
  double trade_prosperity_shipping_loss_penalty{0.55};


  // --- Pirate hideouts (persistent pirate bases) ---
  //
  // When enabled, pirate factions can establish stealthy "hideout" bases inside
  // systems they raid. Hideouts make piracy more persistent by increasing the
  // likelihood of future raids in that system.
  bool enable_pirate_hideouts{true};

  // Chance to establish a hideout when a raid is spawned into a system that does
  // not already contain a hideout for that pirate faction (0..1).
  double pirate_hideout_establish_chance_per_raid{0.15};

  // Hard cap on the number of hideouts a single pirate faction can maintain.
  int pirate_hideout_max_total_per_faction{4};

  // Multiplier applied to a system's raid-selection weight when a hideout owned
  // by that pirate faction exists in the system.
  double pirate_hideout_system_weight_multiplier{2.5};

  // Cooldown applied when a pirate hideout is destroyed, preventing that pirate
  // faction from rebuilding a hideout in the same system for this many days.
  int pirate_hideout_rebuild_cooldown_days{180};

  // Optional reward for counter-piracy: when a pirate hideout is destroyed, the
  // owning Region's pirate_risk is reduced multiplicatively by:
  //   pirate_risk *= (1 - pirate_hideout_destroy_region_risk_reduction_fraction)
  //
  // 0 disables this effect.
  double pirate_hideout_destroy_region_risk_reduction_fraction{0.05};
};

// Optional context passed when recording a persistent simulation event.
//
// IDs use 0 (kInvalidId) to mean "unset".
struct EventContext {
  Id faction_id{kInvalidId};
  Id faction_id2{kInvalidId};
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};
};

// Criteria used by Simulation::advance_until_event.
//
// Fields are intentionally minimal; this is meant to be a convenient "time warp"
// helper for the UI/CLI, not a fully-fledged rules engine.
struct EventStopCondition {
  bool stop_on_info{true};
  bool stop_on_warn{true};
  bool stop_on_error{true};

  // If true, only stop when the event category matches.
  bool filter_category{false};
  EventCategory category{EventCategory::General};

  // If set (non-zero), only stop when the event references this faction
  // either as primary or secondary faction id.
  Id faction_id{kInvalidId};

  // Optional context filters.
  //
  // If set (non-zero), only stop when the event references this id.
  // These filters are useful when "time warping" until something happens in
  // a particular system, to a particular ship, etc.
  Id system_id{kInvalidId};
  Id ship_id{kInvalidId};
  Id colony_id{kInvalidId};

  // If non-empty, only stop when the event message contains this substring.
  // Matching is case-insensitive.
  std::string message_contains;
};

struct AdvanceUntilEventResult {
  int days_advanced{0};
  int hours_advanced{0};
  bool hit{false};
  SimEvent event;
};

// Result returned by Simulation::reload_content_db.

struct ReloadContentResult {
  bool ok{false};

  // Errors returned when loading/validating the new content bundle.
  // When non-empty, ok is false and the Simulation's existing content remains unchanged.
  std::vector<std::string> errors;

  // Non-fatal issues encountered while re-deriving cached/derived state after applying the new bundle.
  std::vector<std::string> warnings;

  int ships_updated{0};
  int factions_rebuilt{0};
  int custom_designs_updated{0};
  int custom_designs_failed{0};
};

// A query-only path plan through the jump network.
//
// This is primarily used by the UI to preview routes and estimated travel time
// before committing to issuing orders.
//
// --- Logistics helper types (UI/AI convenience) ---

// High-level reasons a colony is considered "in need" of minerals.
enum class LogisticsNeedKind {
  Shipyard,
  Construction,

  // Troop training inputs (currently Duranium/Neutronium per strength), expressed
  // as a desired buffer stockpile at the colony so training doesn't stall.
  TroopTraining,

  // Daily-running industry inputs (non-mining installations with consumes_per_day),
  // expressed as a desired buffer stockpile at the colony.
  IndustryInput,

  // User-defined mineral stockpile targets (see Colony::mineral_targets).
  StockpileTarget,

  // Fuel required to top up docked ships (and optionally maintain a buffer).
  Fuel,

  // Munitions required to rearm docked ships (finite missile magazines).
  Rearm,

  // Maintenance supplies required to service docked ships (when enabled).
  Maintenance,
};

// A computed mineral shortfall at a colony.
//
// desired_tons is a best-effort "target" amount to have on-hand to avoid stalling:
// - Shipyard: enough minerals to run the shipyard at full capacity for one day
// - Construction: enough minerals to pay for one unit of an installation build order
struct LogisticsNeed {
  Id colony_id{kInvalidId};
  LogisticsNeedKind kind{LogisticsNeedKind::Shipyard};

  std::string mineral;
  double desired_tons{0.0};
  double have_tons{0.0};
  double missing_tons{0.0};

  // Optional context identifier. For Construction needs this is typically the installation_id.
  // For Shipyard needs this is usually empty.
  std::string context_id;
};

// A lightweight summary of blockade pressure on a colony.
//
// Blockade pressure is driven by hostile combat presence near the colony's body
// relative to local defensive presence (friendly ships + static weapon
// installations) and a configurable baseline resistance.
//
// pressure is normalized to [0,1]. output_multiplier maps pressure into an
// efficiency scalar applied to certain colony outputs.
struct BlockadeStatus {
  Id colony_id{kInvalidId};

  double pressure{0.0};
  double output_multiplier{1.0};

  double hostile_power{0.0};
  double defender_power{0.0};

  int hostile_ships{0};
  int defender_ships{0};
};

// A lightweight summary of recent civilian shipping losses in a star system.
//
// This is a query-only metric derived from Merchant Guild ship wrecks (see
// Wreck::source_faction_id). It is intentionally "soft" and time-decayed,
// modeling the idea that merchants avoid systems that have seen recent losses
// even if the immediate pirate presence has moved on.
//
// pressure is normalized to [0,1]. score is a raw sum of decayed wreck weights
// (useful for debugging and tuning).
struct CivilianShippingLossStatus {
  Id system_id{kInvalidId};

  double pressure{0.0};
  double score{0.0};
  int recent_wrecks{0};
};

// A lightweight summary of recent civilian trade activity in a star system.
//
// The score is a decayed sum of real cargo transfers performed by Merchant
// Guild convoys. It can be used as a macro signal for "how busy" a system is.
//
// factor is normalized to [0,1] via a configurable scale.
struct CivilianTradeActivityStatus {
  Id system_id{kInvalidId};

  double score{0.0};
  double factor{0.0};
};

// Colony trade prosperity status.
//
// Trade prosperity is a lightweight economy modifier derived from the procedural
// trade network (market size + hub score), with disruption penalties for piracy
// risk, blockade pressure, and recent civilian shipping losses.
struct TradeProsperityStatus {
  Id colony_id{kInvalidId};

  // 1 + output_bonus
  double output_multiplier{1.0};

  // Additive bonus fraction (e.g. 0.05 => +5%).
  double output_bonus{0.0};

  // Diagnostics for UI/tooling.
  double market_size{0.0};

  // Number of other factions considered trade partners for the colony's owning
  // faction (trade agreements / alliances / mutual friendly).
  int trade_partner_count{0};

  // Multiplier applied to market_size from treaties (1.0 = no boost).
  double treaty_market_boost{1.0};

  // market_size * treaty_market_boost.
  double effective_market_size{0.0};
  double hub_score{0.0};
  double market_factor{0.0};
  double pop_factor{0.0};
  double piracy_risk{0.0};
  double blockade_pressure{0.0};
  double shipping_loss_pressure{0.0};
};


// Aggregated output multipliers derived from a colony's active conditions.
//
// All fields are multiplicative scalars (1.0 = no change).
struct ColonyConditionMultipliers {
  double mining{1.0};
  double industry{1.0};
  double research{1.0};
  double construction{1.0};
  double shipyard{1.0};
  double terraforming{1.0};
  double troop_training{1.0};
  double pop_growth{1.0};
};

// Colony stability is a pure query derived from environmental and economic
// factors (habitability, habitation shortfall, trade, piracy, blockade) and
// active conditions.
//
// stability is normalized to [0,1] and is primarily used to scale event odds.
struct ColonyStabilityStatus {
  Id colony_id{kInvalidId};

  double stability{1.0};

  // Diagnostics for UI/tooling.
  double habitability{1.0};
  double habitation_shortfall_frac{0.0};

  // Trade prosperity additive bonus (e.g. 0.05 => +5%).
  double trade_bonus{0.0};

  double piracy_risk{0.0};
  double blockade_pressure{0.0};
  double shipping_loss_pressure{0.0};

  // Net stability delta contributed by active conditions.
  double condition_delta{0.0};
};


// Notes:
// - jump_ids are the *source-side* jump points to traverse (i.e. the ids that
//   would be enqueued as TravelViaJump orders).
// - systems includes both start and destination systems.
// - distance/eta are best-effort estimates based on in-system straight-line
//   travel between jump point positions (jump transit itself is instantaneous
//   in the prototype).
struct JumpRoutePlan {
  std::vector<Id> systems;   // start -> ... -> destination
  std::vector<Id> jump_ids;  // one per hop (systems.size() - 1)

  // Jump-network travel only (straight-line in-system travel to each exit jump point).
  //
  // Jump transit itself is instantaneous in the prototype, so distance/eta ignore the
  // "jump" and account only for movement to reach the jump points.
  double distance_mkm{0.0};

  // Environment-adjusted distance used for ETA estimation.
  //
  // Ships slow down inside nebulae and storms (see system_movement_speed_multiplier()).
  // To keep planners (auto-routing, freight/fuel/salvage planners, etc.) consistent
  // with real movement, we also store an "effective" distance where each in-system
  // leg is scaled by 1 / environment_speed_multiplier.
  //
  // eta_days is derived from effective_distance_mkm, not distance_mkm.
  double effective_distance_mkm{0.0};

  double eta_days{0.0};

  // Optional goal position support:
  // When has_goal_pos is true, the planner prefers routes that also minimize the
  // final in-system travel needed after arriving in the destination system.
  //
  // arrival_pos_mkm is where the ship would appear in the destination system after
  // the final jump (or the start position for same-system plans).
  bool has_goal_pos{false};
  Vec2 goal_pos_mkm{0.0, 0.0};
  Vec2 arrival_pos_mkm{0.0, 0.0};

  // Additional in-system distance from arrival_pos_mkm to goal_pos_mkm.
  double final_leg_mkm{0.0};

  // Environment-adjusted final leg distance in the destination system.
  double effective_final_leg_mkm{0.0};

  // Convenience totals.
  double total_distance_mkm{0.0};
  double effective_total_distance_mkm{0.0};
  double total_eta_days{0.0};
};

// --- Scoring / victory helper types ---

// A breakdown of score contributions for a faction.
//
// The shows the *already weighted* point contributions (not raw values).
struct FactionScoreBreakdown {
  double colonies_points{0.0};
  double population_points{0.0};
  double installations_points{0.0};
  double ships_points{0.0};
  double tech_points{0.0};
  double exploration_points{0.0};

  double total_points() const {
    return colonies_points + population_points + installations_points + ships_points + tech_points + exploration_points;
  }
};

// A single scoreboard row.
struct ScoreboardEntry {
  Id faction_id{kInvalidId};
  std::string faction_name;
  FactionControl control{FactionControl::Player};

  // Eligibility & liveness for victory rule evaluation.
  bool eligible_for_victory{true};
  bool alive{true};

  // Score breakdown.
  FactionScoreBreakdown score;
};

class Simulation {
 public:
  Simulation(ContentDB content, SimConfig cfg);

  ContentDB& content() { return content_; }
  const ContentDB& content() const { return content_; }

  const SimConfig& cfg() const { return cfg_; }

  // Reverse engineering threshold (required points) for a component.
  // Returns 0 if the component is unknown to the content database.
  double reverse_engineering_points_required_for_component(const std::string& component_id) const;

  bool subday_economy_enabled() const { return cfg_.enable_subday_economy; }
  void set_subday_economy_enabled(bool enabled) { cfg_.enable_subday_economy = enabled; }

  bool ship_heat_enabled() const { return cfg_.enable_ship_heat; }
  void set_ship_heat_enabled(bool enabled) { cfg_.enable_ship_heat = enabled; }

  bool ship_subsystem_damage_enabled() const { return cfg_.enable_ship_subsystem_damage; }
  void set_ship_subsystem_damage_enabled(bool enabled) { cfg_.enable_ship_subsystem_damage = enabled; }

  bool sensor_los_attenuation_enabled() const { return cfg_.enable_sensor_los_attenuation; }
  void set_sensor_los_attenuation_enabled(bool enabled) { cfg_.enable_sensor_los_attenuation = enabled; }

  double sensor_los_strength() const { return cfg_.sensor_los_strength; }
  void set_sensor_los_strength(double strength) { cfg_.sensor_los_strength = strength; }


  bool body_occlusion_sensors_enabled() const { return cfg_.enable_body_occlusion_sensors; }
  void set_body_occlusion_sensors_enabled(bool enabled) { cfg_.enable_body_occlusion_sensors = enabled; }

  bool body_occlusion_weapons_enabled() const { return cfg_.enable_body_occlusion_weapons; }
  void set_body_occlusion_weapons_enabled(bool enabled) { cfg_.enable_body_occlusion_weapons = enabled; }

  double body_occlusion_padding_mkm() const { return cfg_.body_occlusion_padding_mkm; }
  void set_body_occlusion_padding_mkm(double v) { cfg_.body_occlusion_padding_mkm = v; }


  bool beam_los_attenuation_enabled() const { return cfg_.enable_beam_los_attenuation; }
  void set_beam_los_attenuation_enabled(bool enabled) { cfg_.enable_beam_los_attenuation = enabled; }

  double beam_los_strength() const { return cfg_.beam_los_strength; }
  void set_beam_los_strength(double strength) { cfg_.beam_los_strength = strength; }

  bool beam_scatter_splash_enabled() const { return cfg_.enable_beam_scatter_splash; }
  void set_beam_scatter_splash_enabled(bool enabled) { cfg_.enable_beam_scatter_splash = enabled; }

  double beam_scatter_fraction_of_lost() const { return cfg_.beam_scatter_fraction_of_lost; }
  void set_beam_scatter_fraction_of_lost(double v) { cfg_.beam_scatter_fraction_of_lost = v; }

  double beam_scatter_radius_mkm() const { return cfg_.beam_scatter_radius_mkm; }
  void set_beam_scatter_radius_mkm(double v) { cfg_.beam_scatter_radius_mkm = v; }

  bool beam_scatter_can_hit_friendly() const { return cfg_.beam_scatter_can_hit_friendly; }
  void set_beam_scatter_can_hit_friendly(bool enabled) { cfg_.beam_scatter_can_hit_friendly = enabled; }


  bool terrain_aware_navigation_enabled() const { return cfg_.enable_terrain_aware_navigation; }
  void set_terrain_aware_navigation_enabled(bool enabled) { cfg_.enable_terrain_aware_navigation = enabled; }

  double terrain_nav_strength() const { return cfg_.terrain_nav_strength; }
  void set_terrain_nav_strength(double strength) { cfg_.terrain_nav_strength = strength; }

  double terrain_nav_lookahead_mkm() const { return cfg_.terrain_nav_lookahead_mkm; }
  void set_terrain_nav_lookahead_mkm(double v) { cfg_.terrain_nav_lookahead_mkm = v; }

  int terrain_nav_rays() const { return cfg_.terrain_nav_rays; }
  void set_terrain_nav_rays(int v) { cfg_.terrain_nav_rays = v; }

  double terrain_nav_max_angle_deg() const { return cfg_.terrain_nav_max_angle_deg; }
  void set_terrain_nav_max_angle_deg(double deg) { cfg_.terrain_nav_max_angle_deg = deg; }

  double terrain_nav_turn_penalty() const { return cfg_.terrain_nav_turn_penalty; }
  void set_terrain_nav_turn_penalty(double v) { cfg_.terrain_nav_turn_penalty = v; }

  // --- Ship heat query helpers ---
  // Returns current heat as a fraction of the ship's heat capacity. If the
  // ship has no capacity or heat is disabled, returns 0.
  double ship_heat_fraction(const Ship& ship) const;

  // Subsystem performance multipliers based on current ship heat.
  double ship_heat_speed_multiplier(const Ship& ship) const;
  double ship_heat_sensor_range_multiplier(const Ship& ship) const;
  double ship_heat_weapon_output_multiplier(const Ship& ship) const;
  double ship_heat_shield_regen_multiplier(const Ship& ship) const;

  // Additional signature multiplier derived from heat (thermal bloom).
  // Returns 1.0 when ship heat is disabled.
  double ship_heat_signature_multiplier(const Ship& ship) const;

  // Effective signature multiplier for detection (design stealth * EMCON * thermal bloom).
  // If design is null, it will be looked up by ship.design_id.
  double ship_effective_signature_multiplier(const Ship& ship, const ShipDesign* design = nullptr) const;

  // --- Ship subsystem integrity query helpers ---
  // Returns 1.0 when subsystem damage is disabled.
  double ship_subsystem_engine_multiplier(const Ship& ship) const;
  double ship_subsystem_weapon_output_multiplier(const Ship& ship) const;
  double ship_subsystem_sensor_range_multiplier(const Ship& ship) const;
  double ship_subsystem_shield_multiplier(const Ship& ship) const;

  GameState& state() { return state_; }
  const GameState& state() const { return state_; }

  // --- Victory / scoring (query helpers) ---
  // Compute a weighted score breakdown for a faction using the current save's
  // VictoryRules weights.
  FactionScoreBreakdown compute_faction_score(Id faction_id) const;

  // Same as above, but with explicit rules (useful for UI previews).
  FactionScoreBreakdown compute_faction_score(Id faction_id, const VictoryRules& rules) const;

  // Build a scoreboard for all factions, sorted by total score descending.
  std::vector<ScoreboardEntry> compute_scoreboard() const;

  // Same as above, but with explicit rules.
  std::vector<ScoreboardEntry> compute_scoreboard(const VictoryRules& rules) const;

  // Monotonic counter that increments whenever the simulation's GameState is replaced
  // (new_game/load_game). UI code can use this to clear stale selections / caches.
  std::uint64_t state_generation() const { return state_generation_; }

  // Monotonic counter that increments whenever the simulation's ContentDB is replaced
  // (reload_content_db). UI code can use this to clear caches of content-derived lists.
  std::uint64_t content_generation() const { return content_generation_; }

  void new_game();

  // Create a new procedurally-generated scenario.
  //
  // This is a convenience wrapper around make_random_scenario(seed, num_systems).
  void new_game_random(std::uint32_t seed, int num_systems = 12);

  void load_game(GameState loaded);

  // Hot reload the simulation's content bundle (blueprints + tech tree).
  //
  // The caller is responsible for loading new_content (typically via
  // load_content_db_from_files + load_tech_db_from_files).
  //
  // On success, the simulation updates cached ship stats, rebuilds faction unlock lists,
  // and refreshes contacts/sensor coverage.
  //
  // Save schema is unchanged.
  ReloadContentResult reload_content_db(ContentDB new_content, bool validate_state = true);

  // Advance simulation by N days.
  void advance_days(int days);

  // Advance simulation by N hours.
  //
  // This enables sub-day turn ticks (e.g. 1h / 6h / 12h).
  //
  // By default, most economy systems still tick at the midnight day boundary.
  // If SimConfig::enable_subday_economy is true, economy systems are integrated
  // proportionally each step (dt in days), so advancing in hours will also
  // progress mining/industry, research, shipyards, construction, terraforming,
  // and docked repairs.
  void advance_hours(int hours);

  // Advance simulation day-by-day up to max_days, stopping early if a newly
  // recorded persistent SimEvent matches the provided stop condition.
  //
  // This is primarily a UI/CLI convenience to "time warp" until something
  // interesting happens.
  AdvanceUntilEventResult advance_until_event(int max_days, const EventStopCondition& stop);

  // Advance simulation in sub-day steps up to max_hours, stopping early if a newly
  // recorded persistent SimEvent matches the provided stop condition.
  //
  // step_hours controls the time-warp granularity (e.g. 1/6/12/24). The
  // implementation will not cross midnight within a single step.
  AdvanceUntilEventResult advance_until_event_hours(int max_hours, const EventStopCondition& stop, int step_hours = 1);

  // --- Debug / test hooks ---
  // Run AI planning (automation + mission planning) for the current state without advancing time.
  // This is useful for deterministic unit tests and bug hunting.
  void run_ai_planning();

  // --- Order helpers ---
  // Clear all queued orders for a ship.
  // Returns false if the ship does not exist.
  bool clear_orders(Id ship_id);

  // Enable repeating the ship's current order queue.
  //
  // When enabled, once the order queue becomes empty it will be refilled from a saved
  // template (captured at enable time or via update).
  //
  // repeat_count_remaining controls how many times the template will be re-enqueued:
  //   -1 => infinite repeats
  //    0 => do not refill again (repeat stops once the current queue finishes)
  //   >0 => remaining number of refills allowed
  //
  // Returns false if the ship does not exist or has no queued orders.
  bool enable_order_repeat(Id ship_id, int repeat_count_remaining = -1);

  // Replace the saved repeat template with the ship's current queue.
  // Repeat remains enabled.
  //
  // Returns false if the ship does not exist or has no queued orders.
  bool update_order_repeat_template(Id ship_id);

  // Disable repeating and clear the saved template.
  // Returns false if the ship does not exist.
  bool disable_order_repeat(Id ship_id);

  // Stop repeating but keep the saved template (so it can be restarted later).
  // Returns false if the ship does not exist.
  bool stop_order_repeat_keep_template(Id ship_id);

  // Set how many times the repeat template may be re-enqueued.
  //
  // Returns false if the ship does not exist.
  bool set_order_repeat_count(Id ship_id, int repeat_count_remaining);

  // Enable repeating using a previously saved repeat_template.
  //
  // If the ship's active queue is empty, it will be immediately populated from the template.
  // Returns false if the ship does not exist or has no saved template.
  bool enable_order_repeat_from_template(Id ship_id, int repeat_count_remaining = -1);

  // Cancel only the current (front) order.
  // Returns false if the ship does not exist or has no queued orders.
  bool cancel_current_order(Id ship_id);

  // --- Order queue editing (UI convenience) ---
  // Delete a queued order at a specific index.
  // Returns false if the ship/order index is invalid.
  bool delete_queued_order(Id ship_id, int index);

  // Duplicate a queued order at a specific index (inserts copy after index).
  // Returns false if the ship/order index is invalid.
  bool duplicate_queued_order(Id ship_id, int index);

  // Move a queued order from one index to another.
  // to_index may be == queue.size() to move to end.
  // Returns false if ship/from_index is invalid.
  bool move_queued_order(Id ship_id, int from_index, int to_index);

  // Replace the ship's queued orders with the provided queue.
  //
  // This is a UI convenience helper used for bulk editing (multi-delete, paste, undo/redo).
  // It intentionally does NOT modify repeat settings/templates (so editing the active queue
  // while repeat is on behaves consistently with the per-row edit helpers).
  //
  // Clears any emergency suspension state.
  bool set_queued_orders(Id ship_id, const std::vector<Order>& queue);

  // --- Order template library (persisted in saves) ---
  // Store a named order template.
  //
  // If overwrite is false and the template already exists, this fails.
  bool save_order_template(const std::string& name, const std::vector<Order>& orders,
                           bool overwrite = false, std::string* error = nullptr);
  bool delete_order_template(const std::string& name);
  bool rename_order_template(const std::string& old_name, const std::string& new_name,
                             std::string* error = nullptr);

  const std::vector<Order>* find_order_template(const std::string& name) const;
  std::vector<std::string> order_template_names() const;

  // Apply a saved template to a ship/fleet order queue.
  // If append is false, existing orders are cleared first.
  bool apply_order_template_to_ship(Id ship_id, const std::string& name, bool append = true);
  bool apply_order_template_to_fleet(Id fleet_id, const std::string& name, bool append = true);

  // Smart template application: make templates portable across starting systems.
  //
  // This recompiles the template at apply time, inserting any necessary TravelViaJump
  // orders *between* template steps based on the ship's predicted system after any
  // queued jumps (when append=true). This avoids the common failure mode where
  // TravelViaJump / colony-body orders are invalid when the template is applied to
  // a ship in a different system.
  //
  // When restrict_to_discovered is true, any auto-routing performed during
  // compilation will only traverse systems discovered by the ship's faction.
  bool apply_order_template_to_ship_smart(Id ship_id, const std::string& name, bool append = true,
                                         bool restrict_to_discovered = false,
                                         std::string* error = nullptr);
  bool apply_order_template_to_fleet_smart(Id fleet_id, const std::string& name, bool append = true,
                                          bool restrict_to_discovered = false,
                                          std::string* error = nullptr);

  // Smart compilation of an arbitrary order queue (without applying it).
  //
  // This is the core "route injector" used by smart template application.
  // It recompiles the provided orders, inserting any necessary TravelViaJump
  // orders *between* steps based on the ship's predicted system after any queued
  // jumps (when append=true).
  //
  // When restrict_to_discovered is true, any auto-routing performed during
  // compilation will only traverse systems discovered by the ship's faction.
  //
  // Returns false on failure and fills error.
  bool compile_orders_smart(Id ship_id, const std::vector<Order>& orders, bool append = true,
                            bool restrict_to_discovered = false, std::vector<Order>* out_compiled = nullptr,
                            std::string* error = nullptr) const;

  // Apply an arbitrary order queue to a ship/fleet (without using a named template).
  // If append is false, existing orders are cleared first.
  bool apply_orders_to_ship(Id ship_id, const std::vector<Order>& orders, bool append = true);
  bool apply_orders_to_fleet(Id fleet_id, const std::vector<Order>& orders, bool append = true);

  // Smart apply for arbitrary orders (routes between systems like smart template apply).
  bool apply_orders_to_ship_smart(Id ship_id, const std::vector<Order>& orders, bool append = true,
                                  bool restrict_to_discovered = false, std::string* error = nullptr);
  bool apply_orders_to_fleet_smart(Id fleet_id, const std::vector<Order>& orders, bool append = true,
                                   bool restrict_to_discovered = false, std::string* error = nullptr);


  // --- Fleet helpers ---
  // Fleets are lightweight groupings of ships (same faction) to make it easier
  // to issue orders in bulk.
  //
  // Fleets are persisted in GameState::fleets.
  // Invariants enforced by the Simulation helpers:
  // - All ships in a fleet must exist and belong to the fleet faction_id.
  // - A ship may belong to at most one fleet at a time.
  // - leader_ship_id, if set, must be a member of the fleet.
  Id create_fleet(Id faction_id, const std::string& name, const std::vector<Id>& ship_ids,
                  std::string* error = nullptr);
  bool disband_fleet(Id fleet_id);
  bool add_ship_to_fleet(Id fleet_id, Id ship_id, std::string* error = nullptr);
  bool remove_ship_from_fleet(Id fleet_id, Id ship_id);
  bool set_fleet_leader(Id fleet_id, Id ship_id);
  bool rename_fleet(Id fleet_id, const std::string& name);

  // Fleet formation configuration.
  //
  // Formation settings are persisted in saves and (optionally) used during
  // tick_ships() as a small cohesion helper.
  bool configure_fleet_formation(Id fleet_id, FleetFormation formation, double spacing_mkm);

  // Returns the fleet id containing ship_id, or kInvalidId if none.
  Id fleet_for_ship(Id ship_id) const;

  // Convenience to clear orders for every ship in a fleet.
  bool clear_fleet_orders(Id fleet_id);

  // Fleet order helpers (issue the same order to every member ship).
  bool issue_fleet_wait_days(Id fleet_id, int days);
  bool issue_fleet_move_to_point(Id fleet_id, Vec2 target_mkm);
  bool issue_fleet_move_to_body(Id fleet_id, Id body_id, bool restrict_to_discovered = false);
  bool issue_fleet_orbit_body(Id fleet_id, Id body_id, int duration_days = -1,
                              bool restrict_to_discovered = false);
  bool issue_fleet_travel_via_jump(Id fleet_id, Id jump_point_id);
  bool issue_fleet_survey_jump_point(Id fleet_id, Id jump_point_id, bool transit_when_done = false,
                                    bool restrict_to_discovered = false);
  bool issue_fleet_travel_to_system(Id fleet_id, Id target_system_id, bool restrict_to_discovered = false);
  // Fuel-aware travel helper that attempts to insert refuel stops at
  // trade-partner colonies along the route.
  bool issue_fleet_travel_to_system_smart(Id fleet_id, Id target_system_id,
                                         bool restrict_to_discovered = false);
  bool issue_fleet_attack_ship(Id fleet_id, Id target_ship_id, bool restrict_to_discovered = false);
  bool issue_fleet_escort_ship(Id fleet_id, Id target_ship_id, double follow_distance_mkm = 1.0,
                               bool restrict_to_discovered = false);

  // Bombard a colony from orbit using every fleet ship that has weapons.
  bool issue_fleet_bombard_colony(Id fleet_id, Id colony_id, int duration_days = -1,
                                  bool restrict_to_discovered = false);

  bool issue_fleet_load_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons = 0.0,
                                bool restrict_to_discovered = false);
  bool issue_fleet_unload_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons = 0.0,
                                  bool restrict_to_discovered = false);
  bool issue_fleet_salvage_wreck(Id fleet_id, Id wreck_id, const std::string& mineral, double tons = 0.0,
                                 bool restrict_to_discovered = false);
  bool issue_fleet_investigate_anomaly(Id fleet_id, Id anomaly_id, bool restrict_to_discovered = false);
  bool issue_fleet_transfer_cargo_to_ship(Id fleet_id, Id target_ship_id, const std::string& mineral, double tons = 0.0,
                                          bool restrict_to_discovered = false);
  bool issue_fleet_scrap_ship(Id fleet_id, Id colony_id, bool restrict_to_discovered = false);

  // Gameplay actions
  // Insert a delay into the ship's order queue.
  //
  // The ship will do nothing for the requested number of simulation days.
  // Returns false if ship does not exist or days <= 0.
  bool issue_wait_days(Id ship_id, int days);
  bool issue_move_to_point(Id ship_id, Vec2 target_mkm);
  // Move to a body. If the body is in another system, the simulation will
  // automatically enqueue TravelViaJump steps (using the jump network) and
  // then complete the move in the destination system.
  //
  // When restrict_to_discovered is true, jump routing will only traverse
  // systems discovered by the ship's faction.
  bool issue_move_to_body(Id ship_id, Id body_id, bool restrict_to_discovered = false);

  // Establish a new colony on the target body.
  //
  // If the body is in another system, the simulation will enqueue the necessary
  // TravelViaJump orders automatically (same behavior as issue_move_to_body).
  //
  // colony_name is optional; if empty, a default name will be derived from the
  // body name at execution time.
  bool issue_colonize_body(Id ship_id, Id body_id, const std::string& colony_name = {},
                           bool restrict_to_discovered = false);
  
  // Station keep with a body for a duration (-1 for indefinite).
  // Unlike MoveToBody, this keeps updating the ship's position to match the body's orbit.
  bool issue_orbit_body(Id ship_id, Id body_id, int duration_days = -1, bool restrict_to_discovered = false);

  bool issue_travel_via_jump(Id ship_id, Id jump_point_id);
  bool issue_survey_jump_point(Id ship_id, Id jump_point_id, bool transit_when_done = false,
                              bool restrict_to_discovered = false);
  // Pathfind through the jump network and enqueue TravelViaJump steps to reach a target system.
  //
  // When restrict_to_discovered is true, pathfinding will only traverse systems the ship's
  // faction has already discovered (useful for fog-of-war UI).
  //
  // Returns false if no route is known/available.
  //
  // If goal_pos_mkm is provided, the planner will prefer routes that also minimize
  // the additional in-system travel needed after arriving in the destination system
  // (useful when your next queued order is to move to a specific body/colony/ship).
  bool issue_travel_to_system(Id ship_id, Id target_system_id, bool restrict_to_discovered = false,
                            std::optional<Vec2> goal_pos_mkm = std::nullopt);
  // Fuel-aware travel helper that attempts to insert refuel stops at
  // trade-partner colonies along the route.
  //
  // When smart travel is disabled or the ship does not use fuel, this behaves
  // like issue_travel_to_system().
  bool issue_travel_to_system_smart(Id ship_id, Id target_system_id,
                                   bool restrict_to_discovered = false,
                                   std::optional<Vec2> goal_pos_mkm = std::nullopt);
  // Attack a hostile ship.
  //
  // If the target is in another system, the simulation will auto-enqueue TravelViaJump steps
  // to reach the target's current (if detected) or last-known (from contact memory) system,
  // then pursue it once in-system.
  //
  // When restrict_to_discovered is true, jump routing will only traverse systems discovered
  // by the attacker's faction.
  bool issue_attack_ship(Id attacker_ship_id, Id target_ship_id, bool restrict_to_discovered = false);

  // Escort a friendly ship. The escort will track across the jump network if needed.
  //
  // follow_distance_mkm:
  //  Desired separation in-system. If <= 0, the sim uses docking_range_mkm.
  bool issue_escort_ship(Id escort_ship_id, Id target_ship_id, double follow_distance_mkm = 1.0,
                         bool restrict_to_discovered = false, bool allow_neutral = false);

  // Cargo / logistics (prototype).
  // Load/unload colony minerals into a ship's cargo hold.
  // - mineral == "" means "all minerals".
  // - tons <= 0 means "as much as possible".
  //
  // If the colony is in another system, the simulation will automatically
  // enqueue TravelViaJump steps before the load/unload order.
  //
  // When restrict_to_discovered is true, jump routing will only traverse
  // systems discovered by the ship's faction.
  bool issue_load_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons = 0.0,
                          bool restrict_to_discovered = false);
  bool issue_unload_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons = 0.0,
                            bool restrict_to_discovered = false);

  // Salvage / wrecks.
  // Load minerals from a wreck into a ship's cargo hold.
  // - mineral == "" means "all minerals".
  // - tons <= 0 means "as much as possible".
  //
  // If the wreck is in another system, the simulation will automatically
  // enqueue TravelViaJump steps before the salvage order.
  bool issue_salvage_wreck(Id ship_id, Id wreck_id, const std::string& mineral, double tons = 0.0,
                           bool restrict_to_discovered = false);

  // Salvage a wreck to completion by shuttling minerals to a friendly colony.
  //
  // If dropoff_colony_id is invalid, the simulation will pick the nearest
  // reachable friendly colony (same faction) when unloading is required.
  bool issue_salvage_wreck_loop(Id ship_id, Id wreck_id, Id dropoff_colony_id = kInvalidId,
                               bool restrict_to_discovered = false);

  // Exploration anomalies.
  // Move to an anomaly and (once implemented) investigate it for rewards.
  //
  // If the anomaly is in another system, the simulation will automatically
  // enqueue TravelViaJump steps before the investigation order.
  bool issue_investigate_anomaly(Id ship_id, Id anomaly_id, bool restrict_to_discovered = false);

    // Mobile mining.
  // Mine minerals directly from a body's deposits into the ship's cargo hold.
  //
  // - mineral == "" means "mine all minerals present on the body".
  // - If the body is in another system, the simulation will automatically
  //   enqueue TravelViaJump steps before the mining order.
  // - When stop_when_cargo_full is true, the order completes once the ship has
  //   no free cargo capacity.
  //
  // When restrict_to_discovered is true, jump routing will only traverse
  // systems discovered by the ship's faction.
  bool issue_mine_body(Id ship_id, Id body_id, const std::string& mineral = "",
                       bool stop_when_cargo_full = true, bool restrict_to_discovered = false);

// Troops / invasion (prototype).
  // Load/unload colony ground forces into a ship's troop bays.
  // - strength <= 0 means "as much as possible".
  //
  // If the colony is in another system, the simulation will automatically
  // enqueue TravelViaJump steps before the load/unload/invade order.
  //
  // When restrict_to_discovered is true, jump routing will only traverse
  // systems discovered by the ship's faction.
  bool issue_load_troops(Id ship_id, Id colony_id, double strength = 0.0,
                         bool restrict_to_discovered = false);
  bool issue_unload_troops(Id ship_id, Id colony_id, double strength = 0.0,
                           bool restrict_to_discovered = false);

  // Population / colonists (prototype).
  // Load/unload colony population into a ship with colony module capacity.
  // - millions <= 0 means "as much as possible".
  //
  // If the colony is in another system, the simulation will automatically
  // enqueue TravelViaJump steps before the load/unload order.
  //
  // When restrict_to_discovered is true, jump routing will only traverse
  // systems discovered by the ship's faction.
  bool issue_load_colonists(Id ship_id, Id colony_id, double millions = 0.0,
                            bool restrict_to_discovered = false);
  bool issue_unload_colonists(Id ship_id, Id colony_id, double millions = 0.0,
                              bool restrict_to_discovered = false);

  // Invade a hostile colony (combat is resolved on the ground).
  bool issue_invade_colony(Id ship_id, Id colony_id, bool restrict_to_discovered = false);

  // Bombard a colony from orbit (space-to-ground fire).
  //
  // duration_days:
  //  -1 => bombard indefinitely (until cancelled)
  //  >0 => decrement once per day while the ship is in range and successfully fires
  bool issue_bombard_colony(Id ship_id, Id colony_id, int duration_days = -1,
                            bool restrict_to_discovered = false);

  // Colony troop training (prototype).
  // Adds strength to the colony's training queue; training consumes time
  // (training points/day) and converts queued strength into colony ground forces
  // during tick_ground_combat().
  bool enqueue_troop_training(Id colony_id, double strength);
  bool clear_troop_training_queue(Id colony_id);

  // Terraforming targets for a body (prototype).
  // target_* <= 0 means "do not target that parameter".
  bool set_terraforming_target(Id body_id, double target_temp_k, double target_atm);
  bool clear_terraforming_target(Id body_id);

  // Ground ops query helpers (pure queries).
  double terraforming_points_per_day(const Colony& colony) const;
  double troop_training_points_per_day(const Colony& colony) const;
  double crew_training_points_per_day(const Colony& colony) const;
  double fortification_points(const Colony& colony) const;

  // Piracy risk query helpers.
  //
  // Ambient piracy risk is region-based (after suppression). Effective piracy
  // risk also incorporates the combat presence of active pirate ships and
  // hideouts in the system.
  //
  // All values are normalized to [0,1].
  double ambient_piracy_risk_for_system(Id system_id) const;
  double pirate_presence_risk_for_system(Id system_id) const;
  double piracy_risk_for_system(Id system_id) const;

  // Blockade query helpers.
  //
  // These are pure queries (no save-game persistence). The implementation caches
  // per-colony pressure for the current day/hour to avoid repeated scans.
  BlockadeStatus blockade_status_for_colony(Id colony_id) const;
  double blockade_output_multiplier_for_colony(Id colony_id) const;

  // Civilian shipping loss query helpers.
  //
  // These are pure queries derived from Merchant Guild ship wrecks. The
  // implementation caches per-system pressure for the current day to avoid
  // repeated scans.
  CivilianShippingLossStatus civilian_shipping_loss_status_for_system(Id system_id) const;
  double civilian_shipping_loss_pressure_for_system(Id system_id) const;

  // Civilian trade activity query helpers.
  //
  // These are pure queries derived from the per-system activity score.
  CivilianTradeActivityStatus civilian_trade_activity_status_for_system(Id system_id) const;
  double civilian_trade_activity_factor_for_system(Id system_id) const;

  // Trade prosperity query helpers.
  //
  // Trade prosperity is a lightweight economy bonus derived from the procedural
  // interstellar trade network (market size + hub score). The bonus scales with
  // colony population and is reduced by piracy risk, blockade pressure, and
  // a short "shipping loss" memory derived from recent merchant wrecks.
  TradeProsperityStatus trade_prosperity_status_for_colony(Id colony_id) const;
  double trade_prosperity_output_multiplier_for_colony(Id colony_id) const;

// Colony condition query helpers.
//
// These are pure queries; conditions themselves are stored on Colony.
ColonyConditionMultipliers colony_condition_multipliers(const Colony& colony) const;
ColonyConditionMultipliers colony_condition_multipliers_for_condition(const ColonyCondition& condition) const;

std::string colony_condition_display_name(const std::string& condition_id) const;
std::string colony_condition_description(const std::string& condition_id) const;
bool colony_condition_is_positive(const std::string& condition_id) const;

// Returns the mineral cost to resolve a given condition immediately.
// If the condition is not resolvable, returns an empty map.
std::unordered_map<std::string, double> colony_condition_resolve_cost(Id colony_id,
                                                                      const ColonyCondition& condition) const;

// Resolve a condition immediately by paying its mineral cost (if any) and
// removing it from the colony.
bool resolve_colony_condition(Id colony_id, const std::string& condition_id, std::string* error = nullptr);

// Colony stability (0..1) derived from environment/economy + conditions.
ColonyStabilityStatus colony_stability_status_for_colony(Id colony_id) const;
ColonyStabilityStatus colony_stability_status_for_colony(const Colony& colony) const;

// Output scaling factor derived from colony stability (1.0 when scaling is disabled).
double colony_stability_output_multiplier_for_colony(Id colony_id) const;
double colony_stability_output_multiplier_for_colony(const Colony& colony) const;


  // Crew grade helpers.
  // Returns a signed bonus fraction (e.g. +0.10 = +10%).
  double crew_grade_bonus_for_points(double grade_points) const;
  double crew_grade_bonus(const Ship& ship) const;

  // Transfer cargo directly to another ship in space.
  bool issue_transfer_cargo_to_ship(Id ship_id, Id target_ship_id, const std::string& mineral, double tons = 0.0,
                                    bool restrict_to_discovered = false);

  // Transfer fuel directly to another ship in space.
  //
  // This enables ship-to-ship refueling (tanker operations). Fuel is moved from
  // the source ship's fuel tanks into the target ship's fuel tanks.
  //
  // - tons <= 0 means "as much as possible" (up to target free fuel capacity).
  // - Both ships must belong to the same faction.
  bool issue_transfer_fuel_to_ship(Id ship_id, Id target_ship_id, double tons = 0.0,
                                   bool restrict_to_discovered = false);

  // Transfer embarked troops directly to another ship in space.
  //
  // This enables ship-to-ship troop movement between friendly transports.
  // Troops are moved from the source ship's troop bays into the target ship's
  // troop bays.
  //
  // - strength <= 0 means "as much as possible" (up to target free troop capacity).
  // - Both ships must belong to the same faction.
  // - Both ships must have non-zero troop capacity.
  bool issue_transfer_troops_to_ship(Id ship_id, Id target_ship_id, double strength = 0.0,
                                     bool restrict_to_discovered = false);

  // Transfer embarked colonists directly to another ship in space.
  //
  // This enables ship-to-ship passenger movement between friendly colony ships/transports.
  // Colonists are moved from the source ship's colony capacity into the target ship's
  // colony capacity.
  //
  // - millions <= 0 means "as much as possible" (up to target free colony capacity).
  // - Both ships must belong to the same faction.
  // - Both ships must have non-zero colony capacity.
  bool issue_transfer_colonists_to_ship(Id ship_id, Id target_ship_id, double millions = 0.0,
                                       bool restrict_to_discovered = false);

  // Decommission a ship at a friendly colony, recovering some mineral cost.
  bool issue_scrap_ship(Id ship_id, Id colony_id, bool restrict_to_discovered = false);

  bool enqueue_build(Id colony_id, const std::string& design_id);

  // Refit an existing ship at a colony shipyard (prototype).
  //
  // This can be queued even if the ship is not currently docked at the colony.
  // The shipyard will simply skip the order until the ship arrives (and will
  // continue working on other orders behind it).
  //
  // When the shipyard order completes, the ship's design_id is updated.
  bool enqueue_refit(Id colony_id, Id ship_id, const std::string& target_design_id, std::string* error = nullptr);

  // Estimate shipyard work (tons) required to refit a ship to a target design.
  double estimate_refit_tons(Id ship_id, const std::string& target_design_id) const;

  // Build installations at a colony using construction points + minerals.
  // Returns false if the colony/installation is invalid, quantity <= 0, or the
  // installation is not unlocked for that colony's faction.
  bool enqueue_installation_build(Id colony_id, const std::string& installation_id, int quantity = 1);

// --- Colony production queue editing (UI convenience) ---
// Shipyard queue (build orders)
bool delete_shipyard_order(Id colony_id, int index, bool refund_minerals = true);
bool move_shipyard_order(Id colony_id, int from_index, int to_index);

// Construction queue (installation build orders)
// If refund_minerals is true, deleting an order refunds the mineral costs that were
// already paid for the currently in-progress unit (if any).
bool delete_construction_order(Id colony_id, int index, bool refund_minerals = true);
bool move_construction_order(Id colony_id, int from_index, int to_index);


  // UI helpers (pure queries)
  bool is_ship_docked_at_colony(Id ship_id, Id colony_id) const;
  bool is_design_buildable_for_faction(Id faction_id, const std::string& design_id) const;
  bool is_installation_buildable_for_faction(Id faction_id, const std::string& installation_id) const;
  double construction_points_per_day(const Colony& colony) const;

  // Colony habitability / life support helpers (pure queries)
  double body_habitability(Id body_id) const;
  double habitation_capacity_millions(const Colony& colony) const;
  double required_habitation_capacity_millions(const Colony& colony) const;

  // Logistics helpers (pure queries)
  // Compute per-colony mineral shortfalls that would stall shipyard/construction.
  std::vector<LogisticsNeed> logistics_needs_for_faction(Id faction_id) const;

  // Diplomacy / Rules-of-Engagement helpers.
  //
  // Stances are directed: A->B may differ from B->A.
  //
  // Backward compatibility: if no stance is defined, the relationship defaults to Hostile.
  DiplomacyStatus diplomatic_status(Id from_faction_id, Id to_faction_id) const;

  // Raw directed stance without treaty overlays.
  //
  // This is useful for UI: treaties can temporarily force at least Neutral (Ceasefire/NAP/Trade)
  // or Friendly (Alliance) even if the underlying stance is Hostile.
  DiplomacyStatus diplomatic_status_base(Id from_faction_id, Id to_faction_id) const;
  bool are_factions_hostile(Id from_faction_id, Id to_faction_id) const;
  // True if both factions consider each other Friendly (mutual friendliness).
  // Self is always friendly.
  bool are_factions_mutual_friendly(Id a_faction_id, Id b_faction_id) const;

  // True if the factions have an active relationship that permits trade/logistics
  // interactions at each other's colonies.
  //
  // This is granted by either:
  //  - Mutual Friendly stances (allied access), OR
  //  - An active TradeAgreement treaty (port access without full alliance).
  //
  // Self is always a trade partner.
  bool are_factions_trade_partners(Id a_faction_id, Id b_faction_id) const;

  // Treaties / diplomacy agreements.
  //
  // Treaties are symmetric agreements between two factions and are layered on top
  // of directed DiplomacyStatus stances.
  //
  // duration_days < 0 means "indefinite".
  //
  // Returns the treaty id (or kInvalidId on failure).
  Id create_treaty(Id faction_a, Id faction_b, TreatyType type, int duration_days = -1,
                   bool push_event = true, std::string* error = nullptr);

  // Cancel / break an existing treaty. Returns false on failure.
  bool cancel_treaty(Id treaty_id, bool push_event = true, std::string* error = nullptr);

  // List currently active treaties between the two factions (order-insensitive).
  std::vector<Treaty> treaties_between(Id faction_a, Id faction_b) const;

  // --- Diplomatic offers / proposals ---
  //
  // Offers are directed (from -> to). Accepting an offer creates a Treaty.
  //
  // treaty_duration_days < 0 => indefinite if accepted.
  // offer_expires_in_days <= 0 => never expires.
  //
  // Returns the offer id (or kInvalidId on failure).
  Id create_diplomatic_offer(Id from_faction_id, Id to_faction_id, TreatyType treaty_type,
                             int treaty_duration_days = -1, int offer_expires_in_days = 30,
                             bool push_event = true, std::string* error = nullptr,
                             const std::string& message = "");

  // Accept / decline a pending offer. On accept, a treaty is created and the offer is removed.
  bool accept_diplomatic_offer(Id offer_id, bool push_event = true, std::string* error = nullptr);
  bool decline_diplomatic_offer(Id offer_id, bool push_event = true, std::string* error = nullptr);

  // Query helpers.
  std::vector<DiplomaticOffer> diplomatic_offers_between(Id faction_a, Id faction_b) const;
  std::vector<DiplomaticOffer> incoming_diplomatic_offers(Id to_faction_id) const;

  // --- Procedural contracts / mission board ---
  //
  // Contracts are faction-scoped tasks generated by tick_contracts().
  // These APIs are convenience helpers for UI and automation.
  bool accept_contract(Id contract_id, bool push_event = true, std::string* error = nullptr);
  bool abandon_contract(Id contract_id, bool push_event = true, std::string* error = nullptr);
  bool clear_contract_assignment(Id contract_id, std::string* error = nullptr);

  // Assign a contract to a ship and optionally clear the ship's existing orders.
  // Assigning an Offered contract implicitly accepts it.
  bool assign_contract_to_ship(Id contract_id, Id ship_id, bool clear_existing_orders = false,
                               bool restrict_to_discovered = true, bool push_event = true,
                               std::string* error = nullptr);


  // Assign a contract to a fleet.
  //
  // The contract is assigned to a single primary ship for UI focus, but the fleet
  // will move and operate as a group (escorts will follow the primary ship).
  //
  // Assigning an Offered contract implicitly accepts it.
  bool assign_contract_to_fleet(Id contract_id, Id fleet_id, bool clear_existing_orders = false,
                                bool restrict_to_discovered = true, bool push_event = true,
                                std::string* error = nullptr);



  // Set a diplomatic stance. If reciprocal is true, also sets the inverse (B->A).
  //
  // If push_event is true, records a General event when the stance changes.
  bool set_diplomatic_status(Id from_faction_id, Id to_faction_id, DiplomacyStatus status,
                             bool reciprocal = true, bool push_event = true);

  // Sensor / intel helpers (simple in-system detection).
  // A ship is detected if it is within sensor range of any friendly ship or colony sensor in the same system.
  bool is_ship_detected_by_faction(Id viewer_faction_id, Id target_ship_id) const;
  std::vector<Id> detected_hostile_ships_in_system(Id viewer_faction_id, Id system_id) const;

  // Contact memory helpers.
  // Returns recently seen (last known) hostile ship contacts in the given system.
  // Contacts are updated automatically during simulation ticks.
  std::vector<Contact> recent_contacts_in_system(Id viewer_faction_id, Id system_id, int max_age_days = 30) const;

  // Compute the current uncertainty radius (mkm) for a stale contact.
  //
  // The returned value can be used by the UI to draw uncertainty rings and by
  // orders to perform lightweight "search" behavior around a predicted track.
  double contact_uncertainty_radius_mkm(const Contact& c, int now_day) const;

  // Exploration / map knowledge helpers.
  bool is_system_discovered_by_faction(Id viewer_faction_id, Id system_id) const;
  bool is_jump_point_surveyed_by_faction(Id viewer_faction_id, Id jump_point_id) const;

  // Required survey points for a specific jump point, accounting for
  // optional procedural jump-point phenomena (difficulty fields).
  double jump_survey_required_points_for_jump(Id jump_point_id) const;

  bool is_anomaly_discovered_by_faction(Id viewer_faction_id, Id anomaly_id) const;


  // --- Environmental helpers (nebula + storms) ---
  //
  // These functions expose system-level environmental multipliers used by both
  // the simulation and UI.
  bool system_has_storm(Id system_id) const;
  // Current storm intensity in [0,1], accounting for ramp up/down over time.
  double system_storm_intensity(Id system_id) const;
  // Local storm intensity (includes optional spatial storm cells).
  // When storm cells are disabled, this equals system_storm_intensity().
  double system_storm_intensity_at(Id system_id, const Vec2& pos_mkm) const;
  // Sensor attenuation multiplier in [0,1]. Includes nebula density + storm intensity.
  double system_sensor_environment_multiplier(Id system_id) const;
  // Movement speed multiplier in [0,1]. Includes optional nebula drag + storms.
  double system_movement_speed_multiplier(Id system_id) const;

  // Local, position-dependent variants (used for nebula microfields).
  // When microfields are disabled, these reduce to the system-wide versions.
  double system_nebula_density_at(Id system_id, const Vec2& pos_mkm) const;
  double system_sensor_environment_multiplier_at(Id system_id, const Vec2& pos_mkm) const;
  // Relative line-of-sight sensor multiplier between two points in the same system.
  //
  // This is an *additional* multiplier intended to model occlusion from dense
  // nebula pockets/filaments between the endpoints. It is computed by adaptive
  // ray-marching through the local sensor environment field using an SDF-style
  // distance estimate (f/|grad f|) and deterministic stochastic jitter.
  //
  // Important: The result is normalized to the endpoints so a uniform
  // environment produces ~1.0 (avoids double-counting source/target penalties).
  //
  // extra_seed can be used by callers to decorrelate jitter for different
  // queries with the same endpoints while remaining deterministic.
  double system_sensor_environment_multiplier_los(Id system_id, const Vec2& from_mkm, const Vec2& to_mkm,
                                                  std::uint64_t extra_seed = 0) const;


  // Absolute line-of-fire beam transmission multiplier between two points in the same system.
  //
  // Uses an SDF-style adaptive ray-march on the local sensor environment multiplier field
  // (interpreted as a beam transmission field) with deterministic jitter.
  //
  // Unlike system_sensor_environment_multiplier_los(), this returns an *absolute*
  // transmission multiplier (uniform environments yield that environment multiplier, not ~1).
  double system_beam_environment_multiplier_los(Id system_id, const Vec2& from_mkm, const Vec2& to_mkm,
                                                std::uint64_t extra_seed = 0) const;

  double system_movement_speed_multiplier_at(Id system_id, const Vec2& pos_mkm) const;

  // Environment-adjusted movement cost (million-km) along the segment from -> to.
  //
  // This approximates the travel-time distance integral:
  //   cost ~= \int (ds / speed_multiplier(p))
  // so cost is >= Euclidean distance when speed_multiplier <= 1.
  //
  // Uses an SDF-style adaptive ray-march (f/|grad f|) with deterministic
  // jitter, similar to system_sensor_environment_multiplier_los().
  double system_movement_environment_cost_los(Id system_id, const Vec2& from_mkm, const Vec2& to_mkm,
                                             std::uint64_t extra_seed = 0) const;

  // --- Jump route planning (query-only) ---
  // Plan a path through the jump network without mutating ship orders.
  //
  // If include_queued_jumps is true, the plan starts from the system/position
  // the ship/fleet leader would be at after executing already-queued TravelViaJump
  // orders (useful for Shift-queue previews in the UI).
  std::optional<JumpRoutePlan> plan_jump_route_for_ship(Id ship_id, Id target_system_id,
                                                       bool restrict_to_discovered = false,
                                                       bool include_queued_jumps = false) const;
  std::optional<JumpRoutePlan> plan_jump_route_for_fleet(Id fleet_id, Id target_system_id,
                                                        bool restrict_to_discovered = false,
                                                        bool include_queued_jumps = false) const;

  // Goal-aware jump route planning:
  // Like plan_jump_route_for_ship/fleet, but also considers the final in-system leg
  // inside the destination system when selecting the entry jump point.
  //
  // The returned plan includes final_leg_mkm and total_eta_days.
  std::optional<JumpRoutePlan> plan_jump_route_for_ship_to_pos(Id ship_id, Id target_system_id, Vec2 goal_pos_mkm,
                                                              bool restrict_to_discovered = false,
                                                              bool include_queued_jumps = false) const;
  std::optional<JumpRoutePlan> plan_jump_route_for_fleet_to_pos(Id fleet_id, Id target_system_id, Vec2 goal_pos_mkm,
                                                               bool restrict_to_discovered = false,
                                                               bool include_queued_jumps = false) const;

  // Low-level route planning for tools/planners that already have a start position.
  // This is a query-only helper and does not mutate game state.
  std::optional<JumpRoutePlan> plan_jump_route_from_pos(Id start_system_id, Vec2 start_pos_mkm, Id faction_id,
                                                       double speed_km_s, Id target_system_id,
                                                       bool restrict_to_discovered = false,
                                                       std::optional<Vec2> goal_pos_mkm = std::nullopt) const {
    return plan_jump_route_cached(start_system_id, start_pos_mkm, faction_id, speed_km_s, target_system_id,
                                 restrict_to_discovered, goal_pos_mkm);
  }




  // --- Intel route reveal (state mutation) ---
  //
  // Adds the given systems/jump-points to a faction's discovered/surveyed lists
  // without emitting per-item events/journal entries.
  //
  // Intended for "intel" rewards (e.g. recovered star charts / anomaly clue chains)
  // where spamming a large number of discovery events would be noisy.
  //
  // NOTE: This invalidates the jump-route cache.
  void reveal_route_intel_for_faction(Id faction_id,
                                     const std::vector<Id>& systems,
                                     const std::vector<Id>& jump_points);

  // Player design creation. Designs are stored in GameState::custom_designs and are saved.
  bool upsert_custom_design(ShipDesign design, std::string* error = nullptr);

  // Design lookup (includes custom designs).
  const ShipDesign* find_design(const std::string& design_id) const;

  // Append a narrative journal entry to a faction.
  //
  // This is a curated story layer over the raw event log and is safe for UI use.
  void add_journal_entry(Id faction_id, JournalEntry entry) {
    push_journal_entry(faction_id, std::move(entry));
  }

 private:
  void recompute_body_positions();
  void tick_one_day();
  void tick_one_tick_hours(int hours);
  void tick_colonies(double dt_days, bool emit_daily_events);
  void tick_colony_conditions(double dt_days, bool day_advanced);
  void tick_research(double dt_days);
  void tick_shipyards(double dt_days);
  void tick_construction(double dt_days);
  void tick_ai();
  void tick_civilian_trade_convoys();
  void tick_piracy_suppression();
  void tick_pirate_raids();
  void tick_nebula_storms();
  void tick_treaties();
  void tick_diplomatic_offers();
  void tick_contracts();
  void tick_dynamic_points_of_interest();
  void tick_score_history(bool force);
  void tick_victory();
  void tick_refuel();
  void tick_rearm();
  void tick_ship_maintenance(double dt_days);
  void tick_ship_maintenance_failures();
  void tick_crew_training(double dt_days);
  void tick_heat(double dt_days);
  void tick_ships(double dt_days);
  void tick_contacts(double dt_days, bool emit_contact_lost_events);
  void tick_shields(double dt_days);
  void tick_combat(double dt_days);
  void tick_ground_combat();
  void tick_terraforming(double dt_days);
  void tick_repairs(double dt_days);

  void discover_system_for_faction(Id faction_id, Id system_id);

  void discover_anomaly_for_faction(Id faction_id, Id anomaly_id, Id discovered_by_ship_id = kInvalidId);
  void survey_jump_point_for_faction(Id faction_id, Id jump_point_id);

  void apply_design_stats_to_ship(Ship& ship);
  void initialize_unlocks_for_faction(Faction& f);

  // Remove a ship reference from any fleets and prune empty fleets.
  void remove_ship_from_fleets(Id ship_id);
  // Prune invalid ship references from fleets (missing ships) and drop empty fleets.
  void prune_fleets();

  void push_event(EventLevel level, std::string message);
  void push_event(EventLevel level, EventCategory category, std::string message, EventContext ctx = {});

  // Append a narrative journal entry to a faction.
  //
  // This is a curated story layer over the raw event log.
  void push_journal_entry(Id faction_id, JournalEntry entry);

  // --- Piracy presence cache (performance) ---
  //
  // Computes a per-system pirate combat presence signal used by convoy routing,
  // trade prosperity disruption, and AI/security planning. Cached per hour.
  void ensure_piracy_presence_cache_current() const;

  struct PiracyPresenceInfo {
    double pirate_threat{0.0};
    double presence_risk{0.0};
    int pirate_ships{0};
    int pirate_hideouts{0};
  };

  mutable bool piracy_presence_cache_valid_{false};
  mutable std::int64_t piracy_presence_cache_day_{0};
  mutable int piracy_presence_cache_hour_{0};
  mutable std::uint64_t piracy_presence_cache_state_generation_{0};
  mutable std::uint64_t piracy_presence_cache_content_generation_{0};
  mutable std::unordered_map<Id, PiracyPresenceInfo> piracy_presence_cache_;

  // --- Blockade cache (performance) ---
  // Blockade pressure queries may be called frequently (UI + multiple tick
  // systems). Cache per-colony pressure for the current day/hour.
  void ensure_blockade_cache_current() const;
  void invalidate_blockade_cache() const;

  // --- Civilian shipping loss cache (performance) ---
  // Civilian shipping loss queries may be called frequently (UI + trade
  // prosperity + convoy routing). Cache per-system loss pressure for the
  // current simulation day.
  void ensure_civilian_shipping_loss_cache_current() const;
  void invalidate_civilian_shipping_loss_cache() const;

  // --- Trade prosperity cache (performance) ---
  // Trade prosperity queries may be called frequently (UI + economy ticks). Cache
  // per-system market size + hub score for the current simulation day.
  void ensure_trade_prosperity_cache_current() const;
  void invalidate_trade_prosperity_cache() const;

// --- Jump route planning cache (performance) ---
// Route planning can be called frequently from the UI (hover previews) and from AI logistics.
// Cache successful plans for the current simulation day to avoid repeated Dijkstra runs.
struct JumpRouteCacheKey {
  Id start_system_id{kInvalidId};
  std::uint64_t start_pos_x_bits{0};
  std::uint64_t start_pos_y_bits{0};
  Id faction_id{kInvalidId};
  Id target_system_id{kInvalidId};
  bool restrict_to_discovered{false};

  // Optional goal position included in the cache key (used for goal-aware routing).
  bool has_goal_pos{false};
  std::uint64_t goal_pos_x_bits{0};
  std::uint64_t goal_pos_y_bits{0};

  bool operator==(const JumpRouteCacheKey& o) const {
    return start_system_id == o.start_system_id &&
           start_pos_x_bits == o.start_pos_x_bits &&
           start_pos_y_bits == o.start_pos_y_bits &&
           faction_id == o.faction_id &&
           target_system_id == o.target_system_id &&
           restrict_to_discovered == o.restrict_to_discovered &&
           has_goal_pos == o.has_goal_pos &&
           goal_pos_x_bits == o.goal_pos_x_bits &&
           goal_pos_y_bits == o.goal_pos_y_bits;
  }
};

struct JumpRouteCacheKeyHash {
  std::size_t operator()(const JumpRouteCacheKey& k) const {
    std::size_t h = 0;
    auto mix = [](std::size_t& seed, std::size_t v) {
      seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };
    mix(h, std::hash<Id>()(k.start_system_id));
    mix(h, std::hash<std::uint64_t>()(k.start_pos_x_bits));
    mix(h, std::hash<std::uint64_t>()(k.start_pos_y_bits));
    mix(h, std::hash<Id>()(k.faction_id));
    mix(h, std::hash<Id>()(k.target_system_id));
    mix(h, std::hash<bool>()(k.restrict_to_discovered));
    mix(h, std::hash<bool>()(k.has_goal_pos));
    mix(h, std::hash<std::uint64_t>()(k.goal_pos_x_bits));
    mix(h, std::hash<std::uint64_t>()(k.goal_pos_y_bits));
    return h;
  }
};

struct JumpRouteCacheEntry {
  JumpRoutePlan plan;
  std::list<JumpRouteCacheKey>::iterator lru_it;
};

using JumpRouteCacheMap = std::unordered_map<JumpRouteCacheKey, JumpRouteCacheEntry, JumpRouteCacheKeyHash>;

std::optional<JumpRoutePlan> plan_jump_route_cached(Id start_system_id, Vec2 start_pos_mkm, Id faction_id,
                                                   double speed_km_s, Id target_system_id,
                                                   bool restrict_to_discovered,
                                                   std::optional<Vec2> goal_pos_mkm = std::nullopt) const;

void ensure_jump_route_cache_current() const;
void invalidate_jump_route_cache() const;
void touch_jump_route_cache_entry(JumpRouteCacheMap::iterator it) const;

static constexpr std::size_t kJumpRouteCacheCapacity = 256;
mutable bool jump_route_cache_day_valid_{false};
mutable std::int64_t jump_route_cache_day_{0};
mutable int jump_route_cache_hour_{0};

mutable std::list<JumpRouteCacheKey> jump_route_cache_lru_;
mutable JumpRouteCacheMap jump_route_cache_;
mutable std::uint64_t jump_route_cache_hits_{0};
mutable std::uint64_t jump_route_cache_misses_{0};

// --- Blockade cache ---
mutable bool blockade_cache_valid_{false};
mutable std::int64_t blockade_cache_day_{0};
mutable int blockade_cache_hour_{0};
mutable std::uint64_t blockade_cache_state_generation_{0};
mutable std::uint64_t blockade_cache_content_generation_{0};
mutable std::unordered_map<Id, BlockadeStatus> blockade_cache_;


// --- Civilian shipping loss cache ---
mutable bool civilian_shipping_loss_cache_valid_{false};
mutable std::int64_t civilian_shipping_loss_cache_day_{0};
mutable std::uint64_t civilian_shipping_loss_cache_state_generation_{0};
mutable std::uint64_t civilian_shipping_loss_cache_content_generation_{0};
mutable std::unordered_map<Id, CivilianShippingLossStatus> civilian_shipping_loss_cache_;


// --- Trade prosperity cache ---
mutable bool trade_prosperity_cache_valid_{false};
mutable std::int64_t trade_prosperity_cache_day_{0};
mutable std::uint64_t trade_prosperity_cache_state_generation_{0};
mutable std::uint64_t trade_prosperity_cache_content_generation_{0};

struct TradeProsperitySystemInfo {
  double market_size{0.0};
  double hub_score{0.0};
};
mutable std::unordered_map<Id, TradeProsperitySystemInfo> trade_prosperity_system_cache_;



  ContentDB content_;
  SimConfig cfg_;
  GameState state_;

  // Incremented when state_ is replaced (new_game/load_game).
  std::uint64_t state_generation_{0};


  // Incremented when content_ is replaced (reload_content_db).
  std::uint64_t content_generation_{0};
};

} // namespace nebula4x
