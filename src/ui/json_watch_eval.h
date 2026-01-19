#pragma once

#include <string>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x::ui {

struct JsonWatchConfig;
struct UIState;

// Controls optional/expensive outputs (sample previews) and parsing behavior.
struct JsonWatchEvalOptions {
  // When true, treat an empty string as the document root ("/") when resolving.
  bool accept_root_slash{true};

  // When true, gather sample match pointers + previews (useful for tooltips/navigation).
  bool collect_samples{false};
  int max_sample_matches{8};

  // Max characters for string previews (and sample previews).
  int max_preview_chars{120};
};

struct JsonWatchEvalResult {
  bool ok{false};
  bool numeric{false};
  double value{0.0};

  // Human-friendly display string (for tables/cards).
  std::string display;

  // Diagnostic string when ok==false (or when query traversal hit a hard error).
  std::string error;

  // Query-only diagnostics.
  bool is_query{false};
  int query_op{0};
  int match_count{0};
  int numeric_count{0};
  int nodes_visited{0};
  bool hit_match_limit{false};
  bool hit_node_limit{false};

  // Small sample of matched pointers (for tooltips/navigation).
  std::vector<std::string> sample_paths;
  std::vector<std::string> sample_previews;

  // Representative concrete JSON pointer for navigation/context actions.
  // - For strict pointer pins: the pin path (or "/" if empty).
  // - For query pins: first matched pointer if available; otherwise "/".
  std::string rep_ptr;
};

// Evaluate a Watchboard pin against a JSON document.
//
// This consolidates evaluation logic so Watchboard rows, HUD alerts, and future
// procedural panels stay consistent about coercions and query aggregation.
JsonWatchEvalResult eval_json_watch(const nebula4x::json::Value& root,
                                   const JsonWatchConfig& cfg,
                                   const UIState& ui,
                                   const JsonWatchEvalOptions& opts = {});

} // namespace nebula4x::ui
