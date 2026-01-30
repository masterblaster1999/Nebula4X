#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/core/entities.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/core/tech_tree.h"

namespace nebula4x {

// Static content loaded from JSON files.
struct ContentDB {
  // Optional resource catalog (minerals/materials).
  //
  // When empty, the simulation will still function with ad-hoc string keys
  // in stockpiles/cargo/deposits; the catalog is mainly used for UI grouping
  // and for validating content files.
  std::unordered_map<std::string, ResourceDef> resources;

  std::unordered_map<std::string, ComponentDef> components;
  std::unordered_map<std::string, ShipDesign> designs;
  std::unordered_map<std::string, InstallationDef> installations;
  std::unordered_map<std::string, TechDef> techs;

  // Root JSON files used to load this content bundle (for tooling / hot reload).
  //
  // These are *not* part of save games; they are runtime metadata that lets the
  // UI/CLI re-load the same bundle without needing external configuration.
  std::vector<std::string> content_source_paths;
  std::vector<std::string> tech_source_paths;
};

// --- Victory / scoring ---

// Why the game ended (if it ended).
enum class VictoryReason : std::uint8_t {
  None = 0,
  // A faction met or exceeded VictoryRules::score_threshold.
  ScoreThreshold = 1,
  // Only one eligible faction remained "alive" under the elimination rules.
  LastFactionStanding = 2,
};

// Configurable victory rules stored in save-games.
//
// These are stored in GameState (not SimConfig) so the player can tweak them
// in the UI and have them persist with the save.
struct VictoryRules {
  // Master enable.
  bool enabled{false};

  // If true, factions with FactionControl::AI_Pirate are excluded from victory
  // checks (they still appear on the scoreboard).
  bool exclude_pirates{true};

  // --- Elimination victory ---
  // If enabled, the game ends when only one eligible faction remains alive.
  bool elimination_enabled{true};

  // If true, a faction counts as "alive" only if it owns at least one colony.
  // If false, fleets/ships also keep a faction alive.
  bool elimination_requires_colony{true};

  // --- Score victory ---
  // If > 0, the game ends when an eligible faction reaches this score.
  // (If 0, score victory is disabled.)
  double score_threshold{0.0};

  // Optional lead margin over the runner-up when score_threshold is met.
  // 0 => no margin requirement.
  double score_lead_margin{0.0};

  // --- Scoring weights (points) ---
  // Colonies owned.
  double score_colony_points{100.0};

  // Per million population.
  double score_population_per_million{1.0};

  // Per unit of installation "construction_cost".
  double score_installation_cost_mult{0.1};

  // Per ton of ship mass.
  double score_ship_mass_ton_mult{0.05};

  // Per known technology.
  double score_known_tech_points{5.0};

  // Exploration points.
  double score_discovered_system_points{10.0};
  double score_discovered_anomaly_points{5.0};

  // --- Score history tracking (analytics / projection) ---
  // When enabled, the simulation records periodic score snapshots into
  // GameState::score_history. This powers trend graphs and simple
  // victory ETA estimates in the UI.
  bool score_history_enabled{false};

  // Capture cadence in days (1 = daily).
  int score_history_interval_days{7};

  // Maximum stored samples (older samples are dropped).
  int score_history_max_samples{520};
};

// The (persistent) game-over state.
struct VictoryState {
  bool game_over{false};
  Id winner_faction_id{kInvalidId};
  VictoryReason reason{VictoryReason::None};
  std::int64_t victory_day{0};
  double winner_score{0.0};
};

// --- Score history (victory analytics) ---

struct ScoreHistoryEntry {
  Id faction_id{kInvalidId};
  // Total score at the time of the snapshot (already weighted by VictoryRules).
  double total{0.0};
};

struct ScoreHistorySample {
  // Date::days_since_epoch() at capture time.
  std::int64_t day{0};
  // Hour-of-day (0..23). For now snapshots are recorded at day boundaries.
  int hour{0};

  // Scores for all factions (sorted by faction_id for stable diffs).
  std::vector<ScoreHistoryEntry> scores;
};

// A single save-game state.
struct GameState {
  // v50: nebula storms (temporary system-level environmental hazards).
  // v51: faction narrative journal entries.
  // v52: procedural contracts (mission board scaffolding).
  // v53: score history snapshots (victory analytics / projection).
  // v54: convoy escort contracts + escort neutral flag.
  // v55: colony conditions + colony stability/events.
  // Latest on-disk save version produced by this build.
  //
  // Serialization will still load older versions and backfill fields.
  int save_version{57};
  Date date;

  // Hour-of-day within the current Date (0..23).
  //
  // This enables sub-day turn ticks (e.g. 1h, 6h, 12h) while keeping most
  // simulation systems on a daily cadence.
  int hour_of_day{0};

  Id next_id{1};

  // Monotonic id for SimEvent::seq.
  // Persisted so that clearing/pruning the event log does not reset the sequence.
  std::uint64_t next_event_seq{1};

  // Monotonic id for JournalEntry::seq.
  std::uint64_t next_journal_seq{1};

  std::unordered_map<Id, StarSystem> systems;
  // Procedural galaxy regions/sectors (optional).
  std::unordered_map<Id, Region> regions;
  std::unordered_map<Id, Body> bodies;
  std::unordered_map<Id, JumpPoint> jump_points;
  std::unordered_map<Id, Ship> ships;

  // Salvageable wrecks created by ship destruction.
  std::unordered_map<Id, Wreck> wrecks;

  // Exploration anomalies / points of interest.
  std::unordered_map<Id, Anomaly> anomalies;

  // Procedural contracts / missions.
  std::unordered_map<Id, Contract> contracts;

  // In-flight missile salvos.
  std::unordered_map<Id, MissileSalvo> missile_salvos;

  std::unordered_map<Id, Colony> colonies;
  std::unordered_map<Id, Faction> factions;

  // Active diplomacy treaties (symmetric agreements between two factions).
  std::unordered_map<Id, Treaty> treaties;

  // Pending diplomatic offers / treaty proposals (directed: from -> to).
  std::unordered_map<Id, DiplomaticOffer> diplomatic_offers;

  // Optional win conditions & scoring.
  VictoryRules victory_rules;
  VictoryState victory_state;

  // Periodic score snapshots (Victory window projections).
  std::vector<ScoreHistorySample> score_history;

  // Fleets are lightweight groupings of ships for convenience.
  std::unordered_map<Id, Fleet> fleets;

  // Player-created designs persisted in saves.
  std::unordered_map<std::string, ShipDesign> custom_designs;

  // Player-defined order templates (UI convenience).
  //
  // Stored in saves so players can build a small library of common
  // routes/patrols/etc and apply them to ships or fleets.
  std::unordered_map<std::string, std::vector<Order>> order_templates;

  std::unordered_map<Id, ShipOrders> ship_orders;

  // Persistent simulation event log.
  // Events are appended during ticks and saved/loaded with the game.
  std::vector<SimEvent> events;

  // Persistent ground battles.
  // Key: colony id.
  std::unordered_map<Id, GroundBattle> ground_battles;

  // UI convenience: which system is selected.
  Id selected_system{kInvalidId};
};

Id allocate_id(GameState& s);

// Small helper for safe lookups.
template <typename Map>
auto* find_ptr(Map& m, const typename Map::key_type& k) {
  auto it = m.find(k);
  if (it == m.end()) return static_cast<decltype(&it->second)>(nullptr);
  return &it->second;
}

template <typename Map>
const auto* find_ptr(const Map& m, const typename Map::key_type& k) {
  auto it = m.find(k);
  if (it == m.end()) return static_cast<const decltype(&it->second)>(nullptr);
  return &it->second;
}

} // namespace nebula4x
