#include "nebula4x/util/save_delta.h"

#include <algorithm>
#include <stdexcept>

#include "nebula4x/core/serialization.h"

#include "nebula4x/util/digest.h"
#include "nebula4x/util/json_merge_patch.h"
#include "nebula4x/util/save_diff.h"

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

DeltaSavePatchKind effective_patch_kind(const DeltaSaveFile& f) {
  if (f.format == kDeltaSaveFormatV1) return DeltaSavePatchKind::MergePatch;
  return f.patch_kind;
}

void apply_patch_inplace(json::Value& st, DeltaSavePatchKind kind, const json::Value& patch) {
  if (kind == DeltaSavePatchKind::MergePatch) {
    apply_json_merge_patch(st, patch);
    return;
  }
  apply_json_patch(st, patch, JsonPatchApplyOptions{});
}

void validate_patch_kind_vs_format(const DeltaSaveFile& f) {
  if (f.format == kDeltaSaveFormatV1) {
    if (f.patch_kind != DeltaSavePatchKind::MergePatch) {
      throw std::runtime_error("delta-save v1 only supports patch_kind=merge_patch");
    }
  }
}

DeltaSavePatchKind infer_patch_kind_from_parsed_patches(const std::vector<DeltaSavePatch>& patches) {
  bool any_array = false;
  bool any_non_array = false;
  for (const auto& p : patches) {
    if (p.patch.is_array()) {
      any_array = true;
    } else {
      any_non_array = true;
    }
  }
  if (any_array && !any_non_array) return DeltaSavePatchKind::JsonPatch;
  return DeltaSavePatchKind::MergePatch;
}

} // namespace

DeltaSaveFile parse_delta_save_file(const std::string& json_text) {
  const json::Value root_v = json::parse(json_text);
  const auto* root = root_v.as_object();
  if (!root) throw std::runtime_error("delta-save root is not an object");

  DeltaSaveFile out;
  out.format = try_string(*root, "format");
  if (out.format.empty()) out.format = kDeltaSaveFormatV1;

  if (out.format != kDeltaSaveFormatV1 && out.format != kDeltaSaveFormatV2) {
    throw std::runtime_error("unsupported delta-save format: " + out.format);
  }

  // Patch kind:
  // - V1 is implicitly merge_patch.
  // - V2 may specify patch_kind explicitly.
  out.patch_kind = DeltaSavePatchKind::MergePatch;
  std::string kind_str;
  if (out.format == kDeltaSaveFormatV2) {
    kind_str = try_string(*root, "patch_kind");
    if (!kind_str.empty()) {
      if (!parse_delta_save_patch_kind(kind_str, &out.patch_kind)) {
        throw std::runtime_error("delta-save: unknown patch_kind: " + kind_str);
      }
    }
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
      // v1/v2 structure: { "patch": <json>, "state_digest": "..." }
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

  // If V2 omitted patch_kind, try to infer it from the patch value shapes.
  if (out.format == kDeltaSaveFormatV2 && kind_str.empty()) {
    out.patch_kind = infer_patch_kind_from_parsed_patches(out.patches);
  }

  validate_patch_kind_vs_format(out);

  // Validate JSON Patch shape early so errors are clearer.
  if (out.patch_kind == DeltaSavePatchKind::JsonPatch) {
    for (std::size_t i = 0; i < out.patches.size(); ++i) {
      if (!out.patches[i].patch.is_array()) {
        throw std::runtime_error("delta-save: json_patch patch #" + std::to_string(i) + " is not an array");
      }
    }
  }

  return out;
}

std::string stringify_delta_save_file(const DeltaSaveFile& f, int indent) {
  validate_patch_kind_vs_format(f);

  json::Object root;

  const bool want_v2 = (f.format == kDeltaSaveFormatV2) || (f.patch_kind != DeltaSavePatchKind::MergePatch);
  const std::string fmt = want_v2 ? std::string(kDeltaSaveFormatV2) : (f.format.empty() ? std::string(kDeltaSaveFormatV1) : f.format);

  root["format"] = fmt;
  if (fmt == kDeltaSaveFormatV2) {
    root["patch_kind"] = std::string(delta_save_patch_kind_to_string(f.patch_kind));
  }

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

DeltaSaveFile make_delta_save(const std::string& base_save_json, const std::string& target_save_json, DeltaSavePatchKind kind) {
  DeltaSaveFile out;
  out.patch_kind = kind;
  out.format = (kind == DeltaSavePatchKind::JsonPatch) ? kDeltaSaveFormatV2 : kDeltaSaveFormatV1;
  out.base = json::parse(base_save_json);

  DeltaSavePatch p;
  if (kind == DeltaSavePatchKind::MergePatch) {
    const json::Value target = json::parse(target_save_json);
    p.patch = diff_json_merge_patch(out.base, target);
  } else {
    JsonPatchOptions jopt;
    jopt.indent = 0;
    const std::string patch_json = diff_saves_to_json_patch(base_save_json, target_save_json, jopt);
    p.patch = json::parse(patch_json);
  }
  out.patches = {p};

  // Fill digests for convenience/verification.
  compute_delta_save_digests(out);
  return out;
}

void append_delta_save(DeltaSaveFile& f, const std::string& target_save_json) {
  // Fill defaults for in-memory construction.
  if (f.format.empty()) {
    f.format = (f.patch_kind == DeltaSavePatchKind::JsonPatch) ? kDeltaSaveFormatV2 : kDeltaSaveFormatV1;
  }

  if (f.format != kDeltaSaveFormatV1 && f.format != kDeltaSaveFormatV2) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }
  if (f.format == kDeltaSaveFormatV1) f.patch_kind = DeltaSavePatchKind::MergePatch;

  validate_patch_kind_vs_format(f);

  const json::Value latest = reconstruct_delta_save_value(f, -1);

  DeltaSavePatch p;
  if (f.patch_kind == DeltaSavePatchKind::MergePatch) {
    const json::Value target = json::parse(target_save_json);
    p.patch = diff_json_merge_patch(latest, target);
  } else {
    JsonPatchOptions jopt;
    jopt.indent = 0;
    const std::string latest_json = json::stringify(latest, 2);
    const std::string patch_json = diff_saves_to_json_patch(latest_json, target_save_json, jopt);
    p.patch = json::parse(patch_json);
  }

  // Digest is for the *post*-patch state.
  p.state_digest_hex = digest_state_hex_from_save_json(target_save_json);
  f.patches.push_back(std::move(p));

  // Ensure base digest exists.
  if (f.base_state_digest_hex.empty()) {
    f.base_state_digest_hex = digest_state_hex_from_save_json(json::stringify(f.base, 2));
  }
}

DeltaSaveFile squash_delta_save_as(const DeltaSaveFile& f, int base_index, DeltaSavePatchKind out_kind) {
  if (base_index < 0 || base_index > static_cast<int>(f.patches.size())) {
    throw std::runtime_error("delta-save squash: base_index out of range");
  }

  // Reconstruct selected base snapshot and final snapshot.
  const std::string base_txt = reconstruct_delta_save_json(f, base_index, /*indent=*/2);
  const std::string final_txt = reconstruct_delta_save_json(f, -1, /*indent=*/2);

  // Canonicalize via Nebula4X serialization so diffs are stable and snapshots
  // are validated as real saves.
  const std::string base_canon = nebula4x::serialize_game_to_json(nebula4x::deserialize_game_from_json(base_txt));
  const std::string final_canon = nebula4x::serialize_game_to_json(nebula4x::deserialize_game_from_json(final_txt));

  DeltaSaveFile out;
  out.patch_kind = out_kind;
  if (out_kind == DeltaSavePatchKind::JsonPatch) {
    out.format = kDeltaSaveFormatV2;
  } else {
    // Preserve a v2 merge_patch input as v2 when possible.
    out.format = (f.format == kDeltaSaveFormatV2) ? kDeltaSaveFormatV2 : kDeltaSaveFormatV1;
  }
  out.base = json::parse(base_canon);
  out.patches.clear();

  if (base_canon != final_canon) {
    DeltaSavePatch p;
    if (out_kind == DeltaSavePatchKind::MergePatch) {
      p.patch = diff_json_merge_patch(out.base, json::parse(final_canon));
    } else {
      JsonPatchOptions jopt;
      jopt.indent = 0;
      const std::string patch_json = diff_saves_to_json_patch(base_canon, final_canon, jopt);
      p.patch = json::parse(patch_json);
    }
    out.patches.push_back(std::move(p));
  }

  compute_delta_save_digests(out);
  return out;
}

DeltaSaveFile convert_delta_save_patch_kind(const DeltaSaveFile& f, DeltaSavePatchKind out_kind) {
  if (!f.format.empty() && f.format != kDeltaSaveFormatV1 && f.format != kDeltaSaveFormatV2) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }

  const DeltaSavePatchKind in_kind = effective_patch_kind(f);

  struct CanonSnap {
    std::string json;
    json::Value val;
  };

  auto canonize = [](const json::Value& v) -> CanonSnap {
    // Use Nebula4X serialization to ensure stable ordering and to validate the snapshot.
    const std::string txt = json::stringify(v, 2);
    const auto st = nebula4x::deserialize_game_from_json(txt);
    CanonSnap out;
    out.json = nebula4x::serialize_game_to_json(st);
    out.val = json::parse(out.json);
    return out;
  };

  // Reconstruct + canonicalize every snapshot.
  std::vector<CanonSnap> snaps;
  snaps.reserve(f.patches.size() + 1);

  json::Value st = f.base;
  snaps.push_back(canonize(st));
  for (std::size_t i = 0; i < f.patches.size(); ++i) {
    apply_patch_inplace(st, in_kind, f.patches[i].patch);
    snaps.push_back(canonize(st));
  }

  DeltaSaveFile out;
  out.patch_kind = out_kind;
  if (out_kind == DeltaSavePatchKind::JsonPatch) {
    out.format = kDeltaSaveFormatV2;
  } else {
    // Preserve v2 merge_patch inputs as v2; otherwise default to v1.
    out.format = (f.format == kDeltaSaveFormatV2) ? kDeltaSaveFormatV2 : kDeltaSaveFormatV1;
  }
  out.base = snaps.front().val;
  out.patches.clear();
  out.patches.reserve(f.patches.size());

  for (std::size_t i = 0; i < f.patches.size(); ++i) {
    DeltaSavePatch p;
    if (out_kind == DeltaSavePatchKind::MergePatch) {
      p.patch = diff_json_merge_patch(snaps[i].val, snaps[i + 1].val);
    } else {
      JsonPatchOptions jopt;
      jopt.indent = 0;
      const std::string patch_json = diff_saves_to_json_patch(snaps[i].json, snaps[i + 1].json, jopt);
      p.patch = json::parse(patch_json);
    }
    out.patches.push_back(std::move(p));
  }

  compute_delta_save_digests(out);
  return out;
}

json::Value reconstruct_delta_save_value(const DeltaSaveFile& f, int patch_count) {
  DeltaSavePatchKind kind = effective_patch_kind(f);
  if (!f.format.empty() && f.format != kDeltaSaveFormatV1 && f.format != kDeltaSaveFormatV2) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }

  json::Value st = f.base;
  const int n = clamp_patch_count(patch_count, f.patches.size());
  for (int i = 0; i < n; ++i) {
    const auto& patch = f.patches[static_cast<std::size_t>(i)].patch;
    apply_patch_inplace(st, kind, patch);
  }
  return st;
}

std::string reconstruct_delta_save_json(const DeltaSaveFile& f, int patch_count, int indent) {
  const json::Value st = reconstruct_delta_save_value(f, patch_count);
  return json::stringify(st, indent);
}

void compute_delta_save_digests(DeltaSaveFile& f) {
  if (f.format.empty()) {
    f.format = (f.patch_kind == DeltaSavePatchKind::JsonPatch) ? kDeltaSaveFormatV2 : kDeltaSaveFormatV1;
  }

  if (f.format != kDeltaSaveFormatV1 && f.format != kDeltaSaveFormatV2) {
    throw std::runtime_error("unsupported delta-save format: " + f.format);
  }
  if (f.format == kDeltaSaveFormatV1) f.patch_kind = DeltaSavePatchKind::MergePatch;

  validate_patch_kind_vs_format(f);

  const DeltaSavePatchKind kind = effective_patch_kind(f);

  // Reconstruct incrementally (O(n)) instead of repeatedly replaying from the base (O(n^2)).
  json::Value st = f.base;
  f.base_state_digest_hex = digest_state_hex_from_save_json(json::stringify(st, 2));

  for (std::size_t i = 0; i < f.patches.size(); ++i) {
    apply_patch_inplace(st, kind, f.patches[i].patch);
    f.patches[i].state_digest_hex = digest_state_hex_from_save_json(json::stringify(st, 2));
  }
}

} // namespace nebula4x
