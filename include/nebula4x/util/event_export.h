#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/game_state.h"

namespace nebula4x {

// Format a list of persistent simulation events as CSV.
//
// - Events are exported in the order provided.
// - Output includes both ids and best-effort resolved names (faction/system/ship/colony)
//   using the provided GameState.
std::string events_to_csv(const GameState& state, const std::vector<const SimEvent*>& events);

// Format a list of persistent simulation events as structured JSON.
//
// The JSON output is an array of objects. Each object includes:
//   day, date, seq, level, category,
//   faction_id, faction,
//   faction_id2, faction2,
//   system_id, system,
//   ship_id, ship,
//   colony_id, colony,
//   message
//
// - Events are exported in the order provided.
// - Output ends with a trailing newline.
std::string events_to_json(const GameState& state, const std::vector<const SimEvent*>& events);

// Format a list of persistent simulation events as JSON Lines (NDJSON / JSONL).
//
// The JSONL output is one JSON object per line, with the same fields as
// events_to_json(). Output ends with a trailing newline.
//
// This format is convenient for streaming / grep / jq pipelines.
std::string events_to_jsonl(const GameState& state, const std::vector<const SimEvent*>& events);

// Format a summary of a list of persistent simulation events as JSON.
//
// The JSON output is an object with:
//   count
//   range: { day_min, day_max, date_min, date_max }  (nulls when count=0)
//   levels: { info, warn, error }
//   categories: { general, research, shipyard, construction, movement, combat, intel, exploration, diplomacy }
//
// Output ends with a trailing newline.
std::string events_summary_to_json(const std::vector<const SimEvent*>& events);

// Format a summary of a list of persistent simulation events as CSV.
//
// The CSV output is two lines:
//   - a header row
//   - a single data row
//
// Columns match the JSON summary schema:
//   count,
//   day_min, day_max, date_min, date_max,
//   info, warn, error,
//   general, research, shipyard, construction, movement, combat, intel, exploration, diplomacy
//
// Output ends with a trailing newline.
std::string events_summary_to_csv(const std::vector<const SimEvent*>& events);

} // namespace nebula4x
