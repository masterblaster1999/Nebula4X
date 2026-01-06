#include "nebula4x/util/json_merge_patch.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace nebula4x {
namespace {

bool nearly_equal(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}

bool values_equal(const json::Value& a, const json::Value& b) {
  if (a.index() != b.index()) return false;
  if (a.is_null()) return true;
  if (a.is_bool()) return *a.as_bool() == *b.as_bool();
  if (a.is_number()) return nearly_equal(*a.as_number(), *b.as_number());
  if (a.is_string()) return *a.as_string() == *b.as_string();
  if (a.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    if (aa.size() != bb.size()) return false;
    for (std::size_t i = 0; i < aa.size(); ++i) {
      if (!values_equal(aa[i], bb[i])) return false;
    }
    return true;
  }
  // object
  const auto& ao = a.object();
  const auto& bo = b.object();
  if (ao.size() != bo.size()) return false;

  // Deterministic compare: sort keys.
  std::vector<std::string> keys;
  keys.reserve(ao.size());
  for (const auto& [k, _] : ao) keys.push_back(k);
  std::sort(keys.begin(), keys.end());

  for (const auto& k : keys) {
    const auto itb = bo.find(k);
    if (itb == bo.end()) return false;
    const auto ita = ao.find(k);
    if (ita == ao.end()) return false;
    if (!values_equal(ita->second, itb->second)) return false;
  }
  return true;
}

void apply_merge_patch_value(json::Value& base, const json::Value& patch) {
  // RFC 7386-style merge patch semantics (objects recursively patch objects; arrays/primitive replace).
  if (!patch.is_object()) {
    base = patch;
    return;
  }
  if (!base.is_object()) base = json::Object{};
  auto* bobj = base.as_object();
  const auto& pobj = patch.object();
  for (const auto& [k, pv] : pobj) {
    if (pv.is_null()) {
      bobj->erase(k);
      continue;
    }
    auto it = bobj->find(k);
    if (it == bobj->end()) {
      (*bobj)[k] = pv;
    } else {
      apply_merge_patch_value(it->second, pv);
    }
  }
}

std::optional<json::Value> diff_merge_patch_opt(const json::Value& from, const json::Value& to) {
  // No change.
  if (values_equal(from, to)) return std::nullopt;

  // If the target isn't an object, the patch is a full replacement.
  if (!to.is_object()) return to;

  // Target is an object. If the source isn't, also full replacement.
  if (!from.is_object()) return to;

  const auto& fo = from.object();
  const auto& tobj = to.object();

  json::Object patch;

  // Removals: keys in from but not in to => null.
  for (const auto& [k, _] : fo) {
    if (tobj.find(k) == tobj.end()) patch[k] = nullptr;
  }

  // Additions/changes.
  for (const auto& [k, tv] : tobj) {
    const auto itf = fo.find(k);
    if (itf == fo.end()) {
      patch[k] = tv;
      continue;
    }
    auto child = diff_merge_patch_opt(itf->second, tv);
    if (!child.has_value()) continue; // unchanged
    patch[k] = std::move(*child);
  }

  return json::object(std::move(patch));
}

} // namespace

void apply_json_merge_patch(json::Value& base, const json::Value& patch) {
  apply_merge_patch_value(base, patch);
}

json::Value diff_json_merge_patch(const json::Value& from, const json::Value& to) {
  auto opt = diff_merge_patch_opt(from, to);
  if (!opt.has_value()) {
    // For identical objects, `{}` is a correct no-op merge patch.
    if (to.is_object()) return json::Object{};

    // For identical non-objects, returning the target value is a safe (if slightly
    // verbose) no-op patch.
    return to;
  }
  return std::move(*opt);
}

std::string apply_json_merge_patch(const std::string& doc_json, const std::string& patch_json, int indent) {
  auto doc = json::parse(doc_json);
  const auto patch = json::parse(patch_json);
  apply_json_merge_patch(doc, patch);
  return json::stringify(doc, indent);
}

std::string diff_json_merge_patch(const std::string& from_json, const std::string& to_json, int indent) {
  const auto from = json::parse(from_json);
  const auto to = json::parse(to_json);
  const auto patch = diff_json_merge_patch(from, to);
  return json::stringify(patch, indent);
}

} // namespace nebula4x
