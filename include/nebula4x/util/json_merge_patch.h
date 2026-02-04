#pragma once

#include <string>

#include "nebula4x/util/json.h"

namespace nebula4x {

// RFC 7396 JSON Merge Patch.
//
// Semantics:
// - If `patch` is NOT an object, `base` becomes `patch` (full replacement).
// - If `patch` IS an object and `base` is not, `base` is treated as an empty object.
// - For each key in the patch object:
//     - If the patch value is null, the key is removed from the base object.
//     - Otherwise the key is added/replaced, and if both base+patch values are objects,
//       the patch is applied recursively.
void apply_json_merge_patch(json::Value& base, const json::Value& patch);

// Compute a merge patch that transforms `from` -> `to`.
//
// Notes:
// - The returned patch is designed to be applied via apply_json_merge_patch().
// - Unchanged object keys are omitted from the patch when possible.
// - If `from` and `to` are identical objects, the returned patch is `{}`.
// - If `from` and `to` are identical non-objects, the returned patch is `to` (a no-op
//   that still round-trips correctly for all value types).
json::Value diff_json_merge_patch(const json::Value& from, const json::Value& to);

// Convenience wrappers operating on JSON text.
//
// These parse the inputs, run the value-level functions above, and stringify with `indent`.
// The returned strings do not include a trailing newline.
std::string apply_json_merge_patch(const std::string& doc_json, const std::string& patch_json, int indent = 2);
std::string diff_json_merge_patch(const std::string& from_json, const std::string& to_json, int indent = 2);

} // namespace nebula4x
