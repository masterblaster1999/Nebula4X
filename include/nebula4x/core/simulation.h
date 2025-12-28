#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

struct SimConfig {
  double seconds_per_day{86400.0};

  // Colony population growth rate (fraction per year).
  //
  // Example: 0.01 = +1% per year. A negative value models population decline.
  // Applied once per simulated day in tick_colonies().
  double population_growth_rate_per_year{0.01};

  // When interacting with moving orbital bodies (colonies), ships need a tolerance
  // for being considered "in orbit / docked".
  //
  // If you set this too low, slower ships may endlessly chase a planet's updated
  // position and never reach an exact point for cargo transfers.
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
  // Each day, a docked ship repairs (repair_hp_per_day_per_shipyard * shipyard_count)
  // hitpoints, capped to the design max HP.
  //
  // 0.0 disables repairs.
  double repair_hp_per_day_per_shipyard{0.5};

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
  //
  // Minimum tons moved in a single auto-freight task (avoids tiny shipments).
  double auto_freight_min_transfer_tons{1.0};

  // When pulling minerals from a source colony, auto-freight will never take more than
  // this fraction of that colony's computed exportable surplus in a single task.
  // (0.0 = take nothing, 1.0 = take full surplus).
  double auto_freight_max_take_fraction_of_surplus{0.75};
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
  bool hit{false};
  SimEvent event;
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

  double distance_mkm{0.0};
  double eta_days{0.0};
};

class Simulation {
 public:
  Simulation(ContentDB content, SimConfig cfg);

  ContentDB& content() { return content_; }
  const ContentDB& content() const { return content_; }

  const SimConfig& cfg() const { return cfg_; }

  GameState& state() { return state_; }
  const GameState& state() const { return state_; }

  void new_game();
  void load_game(GameState loaded);

  // Advance simulation by N days.
  void advance_days(int days);

  // Advance simulation day-by-day up to max_days, stopping early if a newly
  // recorded persistent SimEvent matches the provided stop condition.
  //
  // This is primarily a UI/CLI convenience to "time warp" until something
  // interesting happens.
  AdvanceUntilEventResult advance_until_event(int max_days, const EventStopCondition& stop);

  // --- Order helpers ---
  // Clear all queued orders for a ship.
  // Returns false if the ship does not exist.
  bool clear_orders(Id ship_id);

  // Enable repeating the ship's current order queue.
  //
  // When enabled, once the order queue becomes empty it will be refilled from a saved
  // template (captured at enable time or via update).
  //
  // Returns false if the ship does not exist or has no queued orders.
  bool enable_order_repeat(Id ship_id);

  // Replace the saved repeat template with the ship's current queue.
  // Repeat remains enabled.
  //
  // Returns false if the ship does not exist or has no queued orders.
  bool update_order_repeat_template(Id ship_id);

  // Disable repeating and clear the saved template.
  // Returns false if the ship does not exist.
  bool disable_order_repeat(Id ship_id);

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
  bool issue_fleet_travel_to_system(Id fleet_id, Id target_system_id, bool restrict_to_discovered = false);
  bool issue_fleet_attack_ship(Id fleet_id, Id target_ship_id, bool restrict_to_discovered = false);

  bool issue_fleet_load_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons = 0.0,
                                bool restrict_to_discovered = false);
  bool issue_fleet_unload_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons = 0.0,
                                  bool restrict_to_discovered = false);
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
  
  // Station keep with a body for a duration (-1 for indefinite).
  // Unlike MoveToBody, this keeps updating the ship's position to match the body's orbit.
  bool issue_orbit_body(Id ship_id, Id body_id, int duration_days = -1, bool restrict_to_discovered = false);

  bool issue_travel_via_jump(Id ship_id, Id jump_point_id);
  // Pathfind through the jump network and enqueue TravelViaJump steps to reach a target system.
  //
  // When restrict_to_discovered is true, pathfinding will only traverse systems the ship's
  // faction has already discovered (useful for fog-of-war UI).
  //
  // Returns false if no route is known/available.
  bool issue_travel_to_system(Id ship_id, Id target_system_id, bool restrict_to_discovered = false);
  // Attack a hostile ship.
  //
  // If the target is in another system, the simulation will auto-enqueue TravelViaJump steps
  // to reach the target's current (if detected) or last-known (from contact memory) system,
  // then pursue it once in-system.
  //
  // When restrict_to_discovered is true, jump routing will only traverse systems discovered
  // by the attacker's faction.
  bool issue_attack_ship(Id attacker_ship_id, Id target_ship_id, bool restrict_to_discovered = false);

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

  // Transfer cargo directly to another ship in space.
  bool issue_transfer_cargo_to_ship(Id ship_id, Id target_ship_id, const std::string& mineral, double tons = 0.0,
                                    bool restrict_to_discovered = false);

  // Decommission a ship at a friendly colony, recovering some mineral cost.
  bool issue_scrap_ship(Id ship_id, Id colony_id, bool restrict_to_discovered = false);

  bool enqueue_build(Id colony_id, const std::string& design_id);

  // Refit an existing ship at a colony shipyard (prototype).
  // Enqueues a shipyard order that, when complete, updates the ship's design_id.
  bool enqueue_refit(Id colony_id, Id ship_id, const std::string& target_design_id, std::string* error = nullptr);

  // Estimate shipyard work (tons) required to refit a ship to a target design.
  double estimate_refit_tons(Id ship_id, const std::string& target_design_id) const;

  // Build installations at a colony using construction points + minerals.
  // Returns false if the colony/installation is invalid, quantity <= 0, or the
  // installation is not unlocked for that colony's faction.
  bool enqueue_installation_build(Id colony_id, const std::string& installation_id, int quantity = 1);

// --- Colony production queue editing (UI convenience) ---
// Shipyard queue (build orders)
bool delete_shipyard_order(Id colony_id, int index);
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

  // Logistics helpers (pure queries)
  // Compute per-colony mineral shortfalls that would stall shipyard/construction.
  std::vector<LogisticsNeed> logistics_needs_for_faction(Id faction_id) const;

  // Diplomacy / Rules-of-Engagement helpers.
  //
  // Stances are directed: A->B may differ from B->A.
  //
  // Backward compatibility: if no stance is defined, the relationship defaults to Hostile.
  DiplomacyStatus diplomatic_status(Id from_faction_id, Id to_faction_id) const;
  bool are_factions_hostile(Id from_faction_id, Id to_faction_id) const;

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

  // Exploration / map knowledge helpers.
  bool is_system_discovered_by_faction(Id viewer_faction_id, Id system_id) const;

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


  // Player design creation. Designs are stored in GameState::custom_designs and are saved.
  bool upsert_custom_design(ShipDesign design, std::string* error = nullptr);

  // Design lookup (includes custom designs).
  const ShipDesign* find_design(const std::string& design_id) const;

 private:
  void recompute_body_positions();
  void tick_one_day();
  void tick_colonies();
  void tick_research();
  void tick_shipyards();
  void tick_construction();
  void tick_ai();
  void tick_ships();
  void tick_contacts();
  void tick_combat();
  void tick_repairs();

  void discover_system_for_faction(Id faction_id, Id system_id);

  void apply_design_stats_to_ship(Ship& ship);
  void initialize_unlocks_for_faction(Faction& f);

  // Remove a ship reference from any fleets and prune empty fleets.
  void remove_ship_from_fleets(Id ship_id);
  // Prune invalid ship references from fleets (missing ships) and drop empty fleets.
  void prune_fleets();

  void push_event(EventLevel level, std::string message);
  void push_event(EventLevel level, EventCategory category, std::string message, EventContext ctx = {});

  ContentDB content_;
  SimConfig cfg_;
  GameState state_;
};

} // namespace nebula4x
