#include "nebula4x/util/save_delta.h"

#include <algorithm>
#include <stdexcept>

#include "nebula4x/core/serialization.h"

#include "nebula4x/util/digest.h"
#include "nebula4x/util/json_merge_patch.h"

namespace nebula4x {
namespace {


const json::Value& require_key(const json::Object& o, const std::string& key) {
  auto it = o.find(key);
  if (it == o.end()) throw std::runtime_error("delta-save missing key: " + key);
  return it->second;
}

std::string try_string(const json::Object& o, const std::string& key) {
  auto it = o.find(key);
  if (it == o.end()) return {};
  return it->second.string_value();
}

int clamp_patch_count(int requested, std::size_t total) {
  if (requested < 0) return static_cast<int>(total);
  if (requested > static_cast<int>(total)) return static_cast<int>(total);
  return requested;
}

std::string digest_state_hex_from_save_json(const std::string& save_json) {
  const auto st = nebula4x::deserialize_game_from_json(save_json);
  return nebula4x::digest64_to_hex(nebula4x::digest_game_state64(st));
}

} // namespace

DeltaSaveFile parse_delta_save_file(const std::string& json_text) {
  const json::Value root_v = json::parse(json_text);
  const auto* root = root_v.as_object();
  if (!root) throw std::runtime_error("delta-save root is not an object");

  DeltaSaveFile out;
  out.format = try_string(*root, "format");
  if (out.format.empty()) out.format = kDeltaSaveFormatV1;
  if (out.format != kDeltaSaveFormatV1) {
    throw std::runtime_error("unsupported delta-save format: " + out.format);
  }

  out.base = require_key(*root, "base");
  out.base_state_digest_hex = try_string(*root, "base_state_digest");

  const json::Value& patches_v = require_key(*root, "patches");
  const auto* patches = patches_v.as_array();
  if (!patches) throw std::runtime_error("delta-save 'patches' is not an array");

  out.patches.clear();
  out.patches.reserve(patches->size());
  for (const auto& pv : *patches) {
    DeltaSavePatch p;
    if (const auto* po = pv.as_object()) {
      // v1 structure: { "patch": <json>, "state_digest": "..." }
      auto itp = po->find("patch");
      if (itp == po->end()) throw std::runtime_error("delta-save patch entry missing 'patch'");
      p.patch = itp->second;
      p.state_digest_hex = try_string(*po, "state_digest");
    } else {
      // Backwards/handwritten: allow raw patch values.
      p.patch = pv;
    }
    out.patches.push_back(std::move(p));
  }

  return out;
}

std::string stringify_delta_save_file(const DeltaSaveFile& f, int indent) {
  json::Object root;
  root["format"] = f.format.empty() ? std::string(kDeltaSaveFormatV1) : f.format;
  root["base"] = f.base;
  if (!f.base_state_digest_hex.empty()) root["base_state_digest"] = f.base_state_digest_hex;

  json::Array patches;
  patches.reserve(f.patches.size());
  for (const auto& p : f.patches) {
    json::Object e;
    e["patch"] = p.patch;
    if (!p.state_digest_hex.empty()) e["state_digest"] = p.state_digest_hex;
    patches.push_back(std::move(e));
  }
  root["patches"] = patches;
  return json::stringify(root, indent);
}

DeltaSaveFile make_delta_save(const std::string& base_save_json, const std::string& target_save_json) {
  DeltaSaveFile out;
  out.format = kDeltaSaveFormatV1;
  out.base = json::parse(base_save_json);

  const json::Value target = json::parse(target_save_json);
  DeltaSavePatch p;
  p.patch = diff_json_merge_patch(out.base, target);
  out.patches = {p};

  // Fill digests for convenience/verification.
  compute_delta_save_digests(out);
  return out;
}

void append_delta_save(DeltaSaveFile& f, const std::string& target_save_json) {
  if (f.format.empty()) f.format = kDeltaSaveFormatV1;
  if (f.format != kDeltaSaveFormatV1) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }

  const json::Value latest = reconstruct_delta_save_value(f, -1);
  const json::Value target = json::parse(target_save_json);

  DeltaSavePatch p;
  p.patch = diff_json_merge_patch(latest, target);
  // Digest is for the *post*-patch state.
  p.state_digest_hex = digest_state_hex_from_save_json(target_save_json);
  f.patches.push_back(std::move(p));

  // Ensure base digest exists.
  if (f.base_state_digest_hex.empty()) {
    f.base_state_digest_hex = digest_state_hex_from_save_json(json::stringify(f.base, 2));
  }
}

json::Value reconstruct_delta_save_value(const DeltaSaveFile& f, int patch_count) {
  if (f.format.empty()) {
    // Allow in-memory construction without explicit format.
  } else if (f.format != kDeltaSaveFormatV1) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }

  json::Value st = f.base;
  const int n = clamp_patch_count(patch_count, f.patches.size());
  for (int i = 0; i < n; ++i) {
    apply_json_merge_patch(st, f.patches[static_cast<std::size_t>(i)].patch);
  }
  return st;
}

std::string reconstruct_delta_save_json(const DeltaSaveFile& f, int patch_count, int indent) {
  const json::Value st = reconstruct_delta_save_value(f, patch_count);
  return json::stringify(st, indent);
}

void compute_delta_save_digests(DeltaSaveFile& f) {
  if (f.format.empty()) f.format = kDeltaSaveFormatV1;
  if (f.format != kDeltaSaveFormatV1) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }

  // Base digest.
  {
    const std::string base_json = json::stringify(f.base, 2);
    f.base_state_digest_hex = digest_state_hex_from_save_json(base_json);
  }

  // Patch digests (post-patch snapshots).
  for (std::size_t i = 0; i < f.patches.size(); ++i) {
    const std::string snap_json = reconstruct_delta_save_json(f, static_cast<int>(i + 1), 2);
    f.patches[i].state_digest_hex = digest_state_hex_from_save_json(snap_json);
  }
}

} // namespace nebula4x
