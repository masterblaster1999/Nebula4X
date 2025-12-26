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
  bool enqueue_build(Id colony_id, const std::string& design_id);

 private:
  void recompute_body_positions();
  void tick_one_day();
  void tick_colonies();
  void tick_shipyards();
  void tick_ships();

  ContentDB content_;
  SimConfig cfg_;
  GameState state_;
};

} // namespace nebula4x
