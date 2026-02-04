#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {

// Three-way merge of JSON documents (intended for Nebula4X saves).
//
// Given:
//   base   - common ancestor
//   local  - one branch (e.g., your edited save)
//   remote - the other branch (e.g., a mod/tool output)
//
// The merge is structural:
//  - Objects merge by key recursively.
//  - Arrays can merge in three ways:
//      * Index-wise when base/local/remote have equal length (optional).
//      * Key-wise for arrays of objects with a unique id-like field (optional).
//      * Insertion-wise when base is a subsequence of both local+remote (optional).
//      * Otherwise, arrays are treated atomically.
//  - Primitive/type changes use standard three-way rules.

enum class MergeConflictResolution {
  // Record conflicts but do not automatically resolve them.
  // (Callers can decide whether to reject the merge.)
  kFail,

  // Prefer one side's value when a conflict is detected.
  kPreferLocal,
  kPreferRemote,
  kPreferBase,
};

struct SaveMergeOptions {
  // When true, try to merge arrays element-by-element, but ONLY when base/local/remote
  // are all arrays of the same length.
  //
  // This helps avoid conflicts for arrays that behave like fixed-index records.
  bool merge_arrays_by_index{true};

  // When true, attempt to merge arrays of objects by a stable identifier key
  // (e.g. 'id', 'guid', 'uuid') when index-wise merging is not possible.
  //
  // This is useful for arrays that behave like unordered sets of records
  // where element order is not semantically important.
  bool merge_arrays_by_key{true};

  // When true, attempt to auto-discover a suitable identifier key for key-wise
  // array merging when array_key_candidates are not present.
  //
  // Discovery looks for a key that:
  //  - exists in every object element across base/local/remote (when present)
  //  - has string/number values
  //  - is unique within each array
  //
  // This is a best-effort heuristic intended to reduce conflicts when a save
  // uses a different field name than the default candidates (e.g. "ID").
  bool auto_discover_array_key{true};

  // When true, attempt to merge arrays by weaving together insertions when the
  // base array is a subsequence of both local and remote.
  //
  // This is useful for arrays that behave like append-only logs or set-like
  // collections of primitives where concurrent additions are common.
  bool merge_arrays_by_insertions{true};

  // Candidate object keys for key-wise array merges, in priority order.
  //
  // If array_key_override is non-empty, only that key is considered.
  std::vector<std::string> array_key_candidates{"id", "guid", "uuid"};

  // If non-empty, override array_key_candidates and only attempt key-wise
  // merging using this field name.
  std::string array_key_override{};

  // Guardrail: arrays larger than this are not eligible for key-wise merging
  // (to avoid pathological memory use in tooling).
  std::size_t max_array_key_merge_elems{4096};

  // Guardrail: arrays larger than this are not eligible for insertion-wise
  // merging (to avoid pathological runtime/memory use in tooling).
  std::size_t max_array_insertion_merge_elems{4096};

  // When true, treat numbers that differ only by a tiny epsilon as equal when
  // deciding whether a value "changed" between base/local/remote.
  bool nearly_equal_numbers{true};

  // Conflict resolution policy.
  MergeConflictResolution on_conflict{MergeConflictResolution::kFail};

  // Indentation for JSON text output helper functions.
  int indent{2};
};

// A three-way merge conflict.
//
// Presence flags distinguish a missing object key from an explicit JSON null.
struct SaveMergeConflict {
  // JSON Pointer path to the conflicting value.
  // Root is "".
  std::string path;

  bool has_base{false};
  json::Value base;

  bool has_local{false};
  json::Value local;

  bool has_remote{false};
  json::Value remote;
};

struct SaveMergeResult {
  json::Value merged;
  std::vector<SaveMergeConflict> conflicts;
};

// Merge three JSON values.
SaveMergeResult merge_json_three_way(const json::Value& base, const json::Value& local, const json::Value& remote,
                                     SaveMergeOptions opt = {});

// Merge three JSON documents provided as text.
//
// If opt.on_conflict == kFail and the merge detects conflicts, this throws std::runtime_error.
// Otherwise it returns the merged document (even when conflicts are recorded).
std::string merge_saves_three_way(const std::string& base_json, const std::string& local_json,
                                 const std::string& remote_json, SaveMergeOptions opt = {});

// Emit a machine-readable JSON report for a three-way merge.
//
// Format:
// {
//   "conflicts_total": N,
//   "resolved": true|false,
//   "resolution": "fail|prefer_local|prefer_remote|prefer_base",
//   "conflicts": [ {"path":"...", "base":..., "local":..., "remote":...}, ... ]
// }
//
// The returned string is a single JSON document with a trailing newline.
std::string merge_saves_three_way_report(const std::string& base_json, const std::string& local_json,
                                        const std::string& remote_json, SaveMergeOptions opt = {});

} // namespace nebula4x
