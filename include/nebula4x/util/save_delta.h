#pragma once

#include <string>
#include <vector>

#include "nebula4x/util/json.h"

namespace nebula4x {

// Delta-save format strings.
inline constexpr const char kDeltaSaveFormatV1[] = "nebula4x.delta_save.v1";

// A lightweight container for storing a base Nebula4X save plus a sequence of
// RFC 7386 JSON Merge Patches.
//
// This is designed for:
//  - compact save history / journaling experiments
//  - repro files for bugs (base save + small patch chain)
//  - fast "what changed" workflows in tooling
//
// File format (JSON):
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
// Notes:
// - Patches are merge patches (RFC 7386): objects merge recursively; arrays and
//   primitives replace; setting a key to null deletes it.
// - If state digests are present, they refer to the reconstructed state *after*
//   applying that patch.

struct DeltaSavePatch {
  json::Value patch;
  std::string state_digest_hex;
};

struct DeltaSaveFile {
  std::string format{kDeltaSaveFormatV1};
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
DeltaSaveFile make_delta_save(const std::string& base_save_json, const std::string& target_save_json);

// Append a new "target" save to an existing delta-save.
//
// This computes a merge patch from the current *latest* reconstructed save to
// target_save_json and appends it.
void append_delta_save(DeltaSaveFile& f, const std::string& target_save_json);

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
