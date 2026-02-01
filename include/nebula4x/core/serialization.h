#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/util/json.h"

namespace nebula4x {

// Serialize the game state into an in-memory JSON document.
//
// This is primarily used by UI/debug tooling that wants to query the live
// simulation via JSON Pointer patterns without paying an extra JSON text parse.
json::Value serialize_game_to_json_value(const GameState& state);

// Serialize the game state into a JSON text document (pretty-printed).
std::string serialize_game_to_json(const GameState& state);

// Parse a saved game from JSON text.
GameState deserialize_game_from_json(const std::string& json_text);

// --- Order template exchange (clipboard / tooling) ---
//
// This is a lightweight format for sharing order templates outside of the full
// save-game JSON. The format is intentionally compatible with the order objects
// used inside save files ("type" + fields) so templates can be pasted between
// games, saves, and players.
//
// Accepted input forms:
//   1) {"nebula4x_order_template_version": 1, "name": "...", "orders": [ ... ]}
//   2) {"name": "...", "orders": [ ... ]}  (version key optional)
//   3) [ ... ]  (raw array of order objects; name will be empty)
struct ParsedOrderTemplate {
  std::string name;
  std::vector<Order> orders;
};

// Build a JSON value for an order template.
json::Value serialize_order_template_to_json_value(const std::string& name,
                                                   const std::vector<Order>& orders,
                                                   int template_format_version = 1);

// Serialize an order template into JSON text (pretty-printed).
std::string serialize_order_template_to_json(const std::string& name,
                                             const std::vector<Order>& orders,
                                             int indent = 2,
                                             int template_format_version = 1);

// Parse an order template from JSON text.
// Returns false on parse/validation error.
bool deserialize_order_template_from_json(const std::string& json_text, ParsedOrderTemplate* out,
                                         std::string* error = nullptr);

} // namespace nebula4x
