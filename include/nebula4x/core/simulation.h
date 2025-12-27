#pragma once

#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

struct SimConfig {
  double seconds_per_day{86400.0};

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
  // When enabled, once the queue becomes empty it will be refilled from a saved
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

  bool enqueue_build(Id colony_id, const std::string& design_id);

  // Build installations at a colony using construction points + minerals.
  // Returns false if the colony/installation is invalid, quantity <= 0, or the
  // installation is not unlocked for that colony's faction.
  bool enqueue_installation_build(Id colony_id, const std::string& installation_id, int quantity = 1);

  // UI helpers (pure queries)
  bool is_design_buildable_for_faction(Id faction_id, const std::string& design_id) const;
  bool is_installation_buildable_for_faction(Id faction_id, const std::string& installation_id) const;
  double construction_points_per_day(const Colony& colony) const;

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
  void tick_ships();
  void tick_contacts();
  void tick_combat();

  void discover_system_for_faction(Id faction_id, Id system_id);

  void apply_design_stats_to_ship(Ship& ship);
  void initialize_unlocks_for_faction(Faction& f);

  void push_event(EventLevel level, std::string message);
  void push_event(EventLevel level, EventCategory category, std::string message, EventContext ctx = {});

  ContentDB content_;
  SimConfig cfg_;
  GameState state_;
};

} // namespace nebula4x
