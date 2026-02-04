#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {

// Delta-save format strings.
inline constexpr const char kDeltaSaveFormatV1[] = "nebula4x.delta_save.v1";
inline constexpr const char kDeltaSaveFormatV2[] = "nebula4x.delta_save.v2";

// Supported patch encodings for delta-save files.
//
// - MergePatch: RFC 7396 JSON Merge Patch (compact for object edits, but arrays replace wholesale)
// - JsonPatch:  RFC 6902 JSON Patch (more verbose but can be much smaller for array edits)
enum class DeltaSavePatchKind {
  MergePatch = 0,
  JsonPatch = 1,
};

inline constexpr const char kDeltaSavePatchKindMergePatch[] = "merge_patch";
inline constexpr const char kDeltaSavePatchKindJsonPatch[] = "json_patch";

inline const char* delta_save_patch_kind_to_string(DeltaSavePatchKind kind) {
  switch (kind) {
    case DeltaSavePatchKind::MergePatch:
      return kDeltaSavePatchKindMergePatch;
    case DeltaSavePatchKind::JsonPatch:
      return kDeltaSavePatchKindJsonPatch;
  }
  return kDeltaSavePatchKindMergePatch;
}

inline bool parse_delta_save_patch_kind(std::string_view s, DeltaSavePatchKind* out) {
  if (s == kDeltaSavePatchKindMergePatch) {
    if (out) *out = DeltaSavePatchKind::MergePatch;
    return true;
  }
  if (s == kDeltaSavePatchKindJsonPatch) {
    if (out) *out = DeltaSavePatchKind::JsonPatch;
    return true;
  }
  return false;
}

// A lightweight container for storing a base Nebula4X save plus a sequence of
// patches.
//
// This is designed for:
//  - compact save history / journaling experiments
//  - repro files for bugs (base save + small patch chain)
//  - fast "what changed" workflows in tooling
//
// File format (JSON):
//
// V1 (merge patch only):
// {
//   "format": "nebula4x.delta_save.v1",
//   "base": <save JSON>,
//   "base_state_digest": "<hex>" (optional),
//   "patches": [
//     { "patch": <merge patch JSON>, "state_digest": "<hex>" (optional) },
//     ...
//   ]
// }
//
// V2 (merge patch or JSON patch):
// {
//   "format": "nebula4x.delta_save.v2",
//   "patch_kind": "merge_patch" | "json_patch",
//   "base": <save JSON>,
//   "base_state_digest": "<hex>" (optional),
//   "patches": [
//     { "patch": <patch JSON>, "state_digest": "<hex>" (optional) },
//     ...
//   ]
// }
//
// Notes:
// - Merge patches are RFC 7396: objects merge recursively; arrays and primitives replace;
//   setting a key to null deletes it.
// - JSON patches are RFC 6902: arrays of operation objects {op,path,...}.
// - If state digests are present, they refer to the reconstructed state *after*
//   applying that patch.

struct DeltaSavePatch {
  json::Value patch;
  std::string state_digest_hex;
};

struct DeltaSaveFile {
  std::string format{kDeltaSaveFormatV1};
  DeltaSavePatchKind patch_kind{DeltaSavePatchKind::MergePatch};
  json::Value base;
  std::string base_state_digest_hex;
  std::vector<DeltaSavePatch> patches;
};

// Parse a delta-save file from JSON text.
// Throws std::runtime_error on invalid format.
DeltaSaveFile parse_delta_save_file(const std::string& json_text);

// Encode a delta-save file as JSON.
std::string stringify_delta_save_file(const DeltaSaveFile& f, int indent = 2);

// Create a delta-save from two Nebula4X save JSON documents.
//
// The returned delta-save has `base` equal to base_save_json and one patch that
// transforms base -> target.
DeltaSaveFile make_delta_save(const std::string& base_save_json, const std::string& target_save_json,
                              DeltaSavePatchKind kind = DeltaSavePatchKind::MergePatch);

// Append a new "target" save to an existing delta-save.
//
// This computes a patch from the current *latest* reconstructed save to
// target_save_json and appends it (using f.patch_kind).
void append_delta_save(DeltaSaveFile& f, const std::string& target_save_json);

// Squash a delta-save's history into a single patch.
//
// base_index selects which reconstructed snapshot becomes the new base:
//   0 => original base
//   N => snapshot after applying the first N patches
//   patches.size() => final snapshot (result will have 0 patches)
//
// out_kind controls the patch encoding used for the squashed patch.
//
// Throws std::runtime_error on invalid base_index or if reconstructed snapshots
// are not valid Nebula4X saves.
DeltaSaveFile squash_delta_save_as(const DeltaSaveFile& f, int base_index, DeltaSavePatchKind out_kind);

inline DeltaSaveFile squash_delta_save(const DeltaSaveFile& f, int base_index = 0) {
  return squash_delta_save_as(f, base_index, f.patch_kind);
}

// Convert a delta-save to a different patch encoding while preserving the
// snapshot count (i.e. the number of patches).
//
// This reconstructs each snapshot and re-diffs consecutive pairs using the
// requested encoding.
//
// The output base and snapshots are canonicalized via Nebula4X
// deserialize/serialize to ensure stable ordering.
//
// Throws std::runtime_error if any reconstructed snapshot is not a valid save.
DeltaSaveFile convert_delta_save_patch_kind(const DeltaSaveFile& f, DeltaSavePatchKind out_kind);

// Reconstruct the save JSON value.
//
// patch_count:
//  -1 => apply all patches
//   0 => return base
//   N => apply first N patches
json::Value reconstruct_delta_save_value(const DeltaSaveFile& f, int patch_count = -1);

// Reconstruct the save as JSON text.
std::string reconstruct_delta_save_json(const DeltaSaveFile& f, int patch_count = -1, int indent = 2);

// (Re)compute and fill digest fields by deserializing reconstructed Nebula4X
// saves and hashing the in-memory GameState.
//
// Throws std::runtime_error if any reconstructed snapshot is not a valid save.
void compute_delta_save_digests(DeltaSaveFile& f);

} // namespace nebula4x
