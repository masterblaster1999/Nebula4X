#pragma once

#include <string>

namespace nebula4x {

// Options for save diff output.
struct SaveDiffOptions {
  // Maximum number of changes to emit.
  int max_changes{200};

  // Maximum number of characters shown for an individual value in the text output.
  int max_value_chars{240};
};

// Compute a deterministic diff between two JSON documents (typically Nebula4X saves).
//
// The diff is computed structurally (object keys + array indices) and paths are reported
// using a JSON-Pointer-like syntax:
//   /rootKey/childKey/0/subKey
//
// Notes:
// - The caller is expected to pass *canonicalized* save JSON for best results.
//   (Nebula4X's JSON stringify already sorts object keys deterministically.)
// - Numeric comparisons treat values within a tiny epsilon as equal.
std::string diff_saves_to_text(const std::string& a_json, const std::string& b_json, SaveDiffOptions opt = {});

// JSON report:
// {
//   "changes_total": N,
//   "changes_shown": M,
//   "truncated": true|false,
//   "changes": [ { "op": "add|remove|replace", "path": "/...", "before": <json>, "after": <json> }, ... ]
// }
//
// The returned string is a single JSON document with a trailing newline.
std::string diff_saves_to_json(const std::string& a_json, const std::string& b_json, SaveDiffOptions opt = {});

} // namespace nebula4x
