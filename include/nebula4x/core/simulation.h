#pragma once

#include <string>

#include "nebula4x/core/game_state.h"

namespace nebula4x {

struct SimConfig {
  double seconds_per_day{86400.0};
};

class Simulation {
 public:
  Simulation(ContentDB content, SimConfig cfg);

  ContentDB& content() { return content_; }
  const ContentDB& content() const { return content_; }

  SimConfig cfg() const { return cfg_; }

  GameState& state() { return state_; }
  const GameState& state() const { return state_; }

  void new_game();
  void load_game(GameState loaded);

  // Advance simulation by N days.
  void advance_days(int days);

  // Gameplay actions
  bool issue_move_to_point(Id ship_id, Vec2 target_mkm);
  bool issue_move_to_body(Id ship_id, Id body_id);
  bool issue_travel_via_jump(Id ship_id, Id jump_point_id);
  bool issue_attack_ship(Id attacker_ship_id, Id target_ship_id);

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
  void tick_combat();

  void apply_design_stats_to_ship(Ship& ship);
  void initialize_unlocks_for_faction(Faction& f);

  ContentDB content_;
  SimConfig cfg_;
  GameState state_;
};

} // namespace nebula4x
