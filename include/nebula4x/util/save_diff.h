#pragma once

#include <string>

#include "nebula4x/util/json.h"

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

// Options for RFC 6902 JSON Patch generation.
struct JsonPatchOptions {
  // Maximum number of operations to emit. 0 = unlimited.
  int max_ops{0};

  // Indentation for the returned patch JSON (0 = compact).
  int indent{2};

  // When true, emit RFC 6902 'test' operations as preconditions before
  // operations that depend on existing values (replace/remove/move/copy).
  // This helps patches fail fast when
  // applied to a drifting base document, at the cost of larger patches.
  bool emit_tests{false};
};

// Emit an RFC 6902 JSON Patch that transforms a_json -> b_json.
//
// Notes:
// - Emits "add", "remove", "replace", and occasionally "move"/"copy" operations.
//   * Arrays: simple shifts/rotations, single-element relocations, and duplicate-value insertions/appends (copy) when safe.
//   * Objects: simple key renames (move), plus duplicate-value additions/replacements (copy) when safe.
//     Copy sources are chosen from keys that already hold their final value at apply-time
//     (including keys patched earlier in the same object diff).
// - When JsonPatchOptions::emit_tests is true, emits "test" preconditions before operations that
//   depend on existing values (replace/remove/move/copy).
// - Paths use JSON Pointer (RFC 6901). The root is the empty string.
// - Array appends may use a "-" final segment (e.g., "/arr/-"), per RFC 6902.
// - For arrays, the generator trims identical prefix/suffix windows and detects simple one-step
//   shifts/rotations to keep patches smaller (avoids cascades of replaces).
// - This is intended for tooling/debugging and for save delta experiments.
//
// The returned string is a single JSON document with a trailing newline.
std::string diff_saves_to_json_patch(const std::string& a_json, const std::string& b_json, JsonPatchOptions opt = {});

// Options for applying a JSON Patch.
struct JsonPatchApplyOptions {
  // Indentation for the returned patched document (0 = compact).
  int indent{2};

  // Non-standard convenience: treat path "/" as the document root.
  // (RFC 6901 reserves "/" for the empty-string key, but Nebula4X's older
  // save diff reporting used "/" for the root in a few places.)
  bool accept_root_slash{true};
};

// Apply an RFC 6902 JSON Patch to a JSON document.
//
// Supported operations:
//   add, remove, replace, move, copy, test
//
// Note:
// - Removing the document root (path == "") is not supported (throws).
// - Unknown members on operation objects are ignored, per RFC 6902.
//
// Throws std::runtime_error on malformed patch operations or invalid paths.
// The returned string is a single JSON document with a trailing newline.
std::string apply_json_patch(const std::string& doc_json, const std::string& patch_json, JsonPatchApplyOptions opt = {});

// Apply an RFC 6902 JSON Patch (as a parsed JSON array) to an in-memory JSON value.
//
// This is the value-level equivalent of apply_json_patch(doc_json, patch_json, ...).
//
// Supported operations:
//   add, remove, replace, move, copy, test
//
// Throws std::runtime_error on malformed patch operations or invalid paths.
void apply_json_patch(json::Value& doc, const json::Value& patch, JsonPatchApplyOptions opt = {});

} // namespace nebula4x
