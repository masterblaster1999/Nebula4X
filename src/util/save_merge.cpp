#include "nebula4x/util/save_merge.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nebula4x {
namespace {

bool nearly_equal(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}

bool values_equal_impl(const json::Value& a, const json::Value& b, bool nearly_equal_numbers) {
  if (a.index() != b.index()) return false;
  if (a.is_null()) return true;
  if (a.is_bool()) return *a.as_bool() == *b.as_bool();
  if (a.is_number()) {
    if (nearly_equal_numbers) return nearly_equal(*a.as_number(), *b.as_number());
    return *a.as_number() == *b.as_number();
  }
  if (a.is_string()) return *a.as_string() == *b.as_string();
  if (a.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    if (aa.size() != bb.size()) return false;
    for (std::size_t i = 0; i < aa.size(); ++i) {
      if (!values_equal_impl(aa[i], bb[i], nearly_equal_numbers)) return false;
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
    if (!values_equal_impl(ita->second, itb->second, nearly_equal_numbers)) return false;
  }
  return true;
}

bool present_equal(const json::Value* a, const json::Value* b, bool nearly_equal_numbers) {
  if (a == nullptr && b == nullptr) return true;
  if (a == nullptr || b == nullptr) return false;
  return values_equal_impl(*a, *b, nearly_equal_numbers);
}

std::string escape_path_token(const std::string& t) {
  // JSON Pointer escaping (RFC 6901):
  //  ~ -> ~0
  //  / -> ~1
  std::string out;
  out.reserve(t.size());
  for (char c : t) {
    if (c == '~') {
      out += "~0";
    } else if (c == '/') {
      out += "~1";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string join_path(const std::string& base, const std::string& token) {
  const std::string esc = escape_path_token(token);
  if (base.empty()) return "/" + esc;
  return base + "/" + esc;
}

std::string join_index(const std::string& base, std::size_t idx) {
  const std::string token = std::to_string(idx);
  if (base.empty()) return "/" + token;
  return base + "/" + token;
}

struct ArrayKeyIndex {
  std::vector<std::string> order;
  std::unordered_map<std::string, const json::Value*> by_key;
};

std::string lower_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::optional<std::string> key_id_string(const json::Value& v) {
  if (v.is_string()) return std::string("s:") + *v.as_string();
  if (v.is_number()) return std::string("n:") + json::stringify(v, 0);
  return std::nullopt;
}

std::optional<std::string> element_key_id(const json::Value& elem, const std::string& key_name) {
  if (!elem.is_object()) return std::nullopt;
  const auto& o = elem.object();
  const auto it = o.find(key_name);
  if (it == o.end()) return std::nullopt;
  return key_id_string(it->second);
}

std::optional<ArrayKeyIndex> build_array_key_index(const json::Array& arr, const std::string& key_name,
                                                   std::size_t max_elems) {
  if (arr.size() > max_elems) return std::nullopt;

  ArrayKeyIndex idx;
  idx.order.reserve(arr.size());
  idx.by_key.reserve(arr.size() * 2 + 1);

  for (const auto& el : arr) {
    const auto kid = element_key_id(el, key_name);
    if (!kid.has_value()) return std::nullopt;
    if (idx.by_key.find(*kid) != idx.by_key.end()) return std::nullopt;
    idx.by_key.emplace(*kid, &el);
    idx.order.push_back(*kid);
  }

  return idx;
}

std::optional<std::unordered_set<std::string>> keys_present_in_all_objects(const json::Array& arr) {
  if (arr.empty()) return std::unordered_set<std::string>{};

  std::unordered_map<std::string, std::size_t> counts;
  counts.reserve(arr.size() * 2 + 1);

  for (const auto& el : arr) {
    if (!el.is_object()) return std::nullopt;
    for (const auto& [k, _] : el.object()) {
      counts[k] += 1;
    }
  }

  std::unordered_set<std::string> out;
  out.reserve(counts.size());
  for (const auto& [k, c] : counts) {
    if (c == arr.size()) out.insert(k);
  }
  return out;
}

int array_key_priority(const std::string& key_name, const std::vector<std::string>& preferred_keys) {
  const std::string k = lower_ascii(key_name);
  for (std::size_t i = 0; i < preferred_keys.size(); ++i) {
    if (k == lower_ascii(preferred_keys[i])) {
      // Earlier keys are higher priority.
      const int base = 1000;
      return base - static_cast<int>(i);
    }
  }
  return 0;
}

// Best-effort: discover a key suitable for key-wise array merges by scanning arrays of objects.
std::optional<std::string> discover_array_merge_key(const json::Array& ba, const json::Array& la, const json::Array& ra,
                                                   const SaveMergeOptions& opt) {
  const std::size_t max_elems = opt.max_array_key_merge_elems;
  if (la.empty() || ra.empty()) return std::nullopt;
  if (la.size() > max_elems || ra.size() > max_elems) return std::nullopt;
  if (!ba.empty() && ba.size() > max_elems) return std::nullopt;

  const auto lk = keys_present_in_all_objects(la);
  const auto rk = keys_present_in_all_objects(ra);
  if (!lk.has_value() || !rk.has_value()) return std::nullopt;

  std::unordered_set<std::string> common = *lk;
  // Intersect with remote.
  for (auto it = common.begin(); it != common.end();) {
    if (rk->find(*it) == rk->end()) {
      it = common.erase(it);
    } else {
      ++it;
    }
  }

  // If base has elements, also intersect with base.
  if (!ba.empty()) {
    const auto bk = keys_present_in_all_objects(ba);
    if (!bk.has_value()) return std::nullopt;
    for (auto it = common.begin(); it != common.end();) {
      if (bk->find(*it) == bk->end()) {
        it = common.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (common.empty()) return std::nullopt;

  // Deterministic iteration.
  std::vector<std::string> keys(common.begin(), common.end());
  std::sort(keys.begin(), keys.end());

  std::optional<std::string> best;
  int best_pri = -1;
  std::size_t best_len = 0;

  for (const auto& key_name : keys) {
    const auto lidx = build_array_key_index(la, key_name, max_elems);
    const auto ridx = build_array_key_index(ra, key_name, max_elems);
    if (!lidx.has_value() || !ridx.has_value()) continue;

    if (!ba.empty()) {
      const auto bidx = build_array_key_index(ba, key_name, max_elems);
      if (!bidx.has_value()) continue;
    }

    const int pri = array_key_priority(key_name, opt.array_key_candidates);
    const std::size_t len = key_name.size();
    if (!best.has_value() || pri > best_pri || (pri == best_pri && (len < best_len || (len == best_len && key_name < *best)))) {
      best = key_name;
      best_pri = pri;
      best_len = len;
    }
  }

  return best;
}

bool subsequence_positions(const json::Array& base, const json::Array& seq, bool nearly_equal_numbers,
                           std::vector<std::size_t>& out_pos) {
  out_pos.clear();
  out_pos.reserve(base.size());
  if (base.empty()) return true;

  std::size_t j = 0;
  for (std::size_t i = 0; i < base.size(); ++i) {
    bool found = false;
    for (; j < seq.size(); ++j) {
      if (values_equal_impl(base[i], seq[j], nearly_equal_numbers)) {
        out_pos.push_back(j);
        ++j;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

bool segment_equal(const json::Array& a, std::size_t ab, std::size_t ae, const json::Array& b, std::size_t bb,
                   std::size_t be, bool nearly_equal_numbers) {
  if ((ae - ab) != (be - bb)) return false;
  for (std::size_t i = 0; i < (ae - ab); ++i) {
    if (!values_equal_impl(a[ab + i], b[bb + i], nearly_equal_numbers)) return false;
  }
  return true;
}

void append_merged_insert_segment(json::Array& out, const json::Array& la, std::size_t lb, std::size_t le,
                                  const json::Array& ra, std::size_t rb, std::size_t re, bool nearly_equal_numbers) {
  if (lb == le) {
    for (std::size_t i = rb; i < re; ++i) out.push_back(ra[i]);
    return;
  }
  if (rb == re) {
    for (std::size_t i = lb; i < le; ++i) out.push_back(la[i]);
    return;
  }
  if (segment_equal(la, lb, le, ra, rb, re, nearly_equal_numbers)) {
    for (std::size_t i = lb; i < le; ++i) out.push_back(la[i]);
    return;
  }

  // Deterministic union: local insertions first, then remote insertions that
  // are not already present in the insertion window.
  const std::size_t window_start = out.size();
  for (std::size_t i = lb; i < le; ++i) out.push_back(la[i]);
  for (std::size_t i = rb; i < re; ++i) {
    bool dup = false;
    for (std::size_t j = window_start; j < out.size(); ++j) {
      if (values_equal_impl(out[j], ra[i], nearly_equal_numbers)) {
        dup = true;
        break;
      }
    }
    if (!dup) out.push_back(ra[i]);
  }
}

std::optional<json::Array> merge_arrays_by_insertions(const json::Array& ba, const json::Array& la, const json::Array& ra,
                                                      bool nearly_equal_numbers) {
  std::vector<std::size_t> posL;
  std::vector<std::size_t> posR;
  if (!subsequence_positions(ba, la, nearly_equal_numbers, posL)) return std::nullopt;
  if (!subsequence_positions(ba, ra, nearly_equal_numbers, posR)) return std::nullopt;
  if (posL.size() != ba.size() || posR.size() != ba.size()) return std::nullopt;

  json::Array out;
  out.reserve(la.size() + ra.size());

  std::size_t prevL = 0;
  std::size_t prevR = 0;
  for (std::size_t i = 0; i < ba.size(); ++i) {
    const std::size_t li = posL[i];
    const std::size_t ri = posR[i];

    // Insertions before this base element.
    append_merged_insert_segment(out, la, prevL, li, ra, prevR, ri, nearly_equal_numbers);

    // Base element itself (unchanged on both sides).
    out.push_back(ba[i]);

    prevL = li + 1;
    prevR = ri + 1;
  }

  // Trailing insertions.
  append_merged_insert_segment(out, la, prevL, la.size(), ra, prevR, ra.size(), nearly_equal_numbers);
  return out;
}


void push_conflict(std::vector<SaveMergeConflict>& conflicts, const std::string& path, const json::Value* base,
                   const json::Value* local, const json::Value* remote) {
  SaveMergeConflict c;
  c.path = path;
  c.has_base = (base != nullptr);
  if (base) c.base = *base;
  c.has_local = (local != nullptr);
  if (local) c.local = *local;
  c.has_remote = (remote != nullptr);
  if (remote) c.remote = *remote;
  conflicts.push_back(std::move(c));
}

std::optional<json::Value> resolve_conflict(const json::Value* base, const json::Value* local, const json::Value* remote,
                                           MergeConflictResolution res) {
  switch (res) {
    case MergeConflictResolution::kPreferLocal:
      if (local) return *local;
      return std::nullopt;
    case MergeConflictResolution::kPreferRemote:
      if (remote) return *remote;
      return std::nullopt;
    case MergeConflictResolution::kPreferBase:
      if (base) return *base;
      return std::nullopt;
    case MergeConflictResolution::kFail:
    default:
      // Deterministic: return local when present, else remote, else base.
      if (local) return *local;
      if (remote) return *remote;
      if (base) return *base;
      return std::nullopt;
  }
}

std::optional<json::Value> merge3_impl(const std::string& path, const json::Value* base, const json::Value* local,
                                      const json::Value* remote, const SaveMergeOptions& opt,
                                      std::vector<SaveMergeConflict>& conflicts) {
  const bool ne = opt.nearly_equal_numbers;

  // Both deleted.
  if (local == nullptr && remote == nullptr) return std::nullopt;

  // Same result on both sides.
  if (present_equal(local, remote, ne)) {
    if (local == nullptr) return std::nullopt;
    return *local;
  }

  // If one side equals base, take the other.
  if (base != nullptr) {
    if (present_equal(local, base, ne)) {
      if (remote == nullptr) return std::nullopt;
      return *remote;
    }
    if (present_equal(remote, base, ne)) {
      if (local == nullptr) return std::nullopt;
      return *local;
    }
  } else {
    // No base value: treat as independent additions/removals.
    if (local == nullptr) {
      if (remote == nullptr) return std::nullopt;
      return *remote;
    }
    if (remote == nullptr) {
      return *local;
    }
  }

  // Structural merge: objects merge by key.
  if (local && remote && local->is_object() && remote->is_object()) {
    const json::Object empty;
    const json::Object& bo = (base && base->is_object()) ? base->object() : empty;
    const json::Object& lo = local->object();
    const json::Object& ro = remote->object();

    // Union of keys.
    std::vector<std::string> keys;
    keys.reserve(bo.size() + lo.size() + ro.size());
    for (const auto& [k, _] : bo) keys.push_back(k);
    for (const auto& [k, _] : lo) keys.push_back(k);
    for (const auto& [k, _] : ro) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    json::Object out;
    for (const auto& k : keys) {
      const json::Value* bc = nullptr;
      const json::Value* lc = nullptr;
      const json::Value* rc = nullptr;
      if (base && base->is_object()) {
        const auto it = bo.find(k);
        if (it != bo.end()) bc = &it->second;
      }
      {
        const auto it = lo.find(k);
        if (it != lo.end()) lc = &it->second;
      }
      {
        const auto it = ro.find(k);
        if (it != ro.end()) rc = &it->second;
      }

      auto child = merge3_impl(join_path(path, k), bc, lc, rc, opt, conflicts);
      if (!child.has_value()) continue; // missing
      out[k] = std::move(*child);
    }
    return json::object(std::move(out));
  }

  // Structural merge: arrays can merge index-wise when lengths match.
  if (opt.merge_arrays_by_index && base && local && remote && base->is_array() && local->is_array() && remote->is_array()) {
    const auto& ba = base->array();
    const auto& la = local->array();
    const auto& ra = remote->array();
    if (ba.size() == la.size() && ba.size() == ra.size()) {
      json::Array out;
      out.reserve(ba.size());
      for (std::size_t i = 0; i < ba.size(); ++i) {
        auto child = merge3_impl(join_index(path, i), &ba[i], &la[i], &ra[i], opt, conflicts);
        // Array elements are always present; if we ever produce a deletion here,
        // fall back to remote/local preference.
        if (!child.has_value()) {
          auto resolved = resolve_conflict(&ba[i], &la[i], &ra[i], opt.on_conflict);
          if (!resolved.has_value()) {
            // Should not happen.
            out.push_back(nullptr);
          } else {
            out.push_back(std::move(*resolved));
          }
        } else {
          out.push_back(std::move(*child));
        }
      }
      return json::array(std::move(out));
    }
  }



  // Structural merge: arrays can merge by key for arrays of objects with a unique
  // identifier field (id/guid/uuid/etc.).
  //
  // This is similar in spirit to Kubernetes "strategic merge patch" list merge keys,
  // but applied in a three-way merge context.
  if (opt.merge_arrays_by_key && local && remote && local->is_array() && remote->is_array() &&
      (!base || base->is_array())) {
    const json::Array empty;
    const json::Array& ba = (base && base->is_array()) ? base->array() : empty;
    const json::Array& la = local->array();
    const json::Array& ra = remote->array();

    const std::size_t max_elems = opt.max_array_key_merge_elems;
    if (ba.size() <= max_elems && la.size() <= max_elems && ra.size() <= max_elems) {
      std::vector<std::string> candidates;
      if (!opt.array_key_override.empty()) {
        candidates.push_back(opt.array_key_override);
      } else {
        candidates = opt.array_key_candidates;

        // Best-effort: if the preferred candidates fail, attempt to discover a
        // unique identifier key automatically.
        if (opt.auto_discover_array_key) {
          const auto discovered = discover_array_merge_key(ba, la, ra, opt);
          if (discovered.has_value() &&
              std::find(candidates.begin(), candidates.end(), *discovered) == candidates.end()) {
            candidates.push_back(*discovered);
          }
        }
      }

      for (const auto& key_name : candidates) {
        const auto lidx = build_array_key_index(la, key_name, max_elems);
        const auto ridx = build_array_key_index(ra, key_name, max_elems);
        if (!lidx.has_value() || !ridx.has_value()) continue;

        std::optional<ArrayKeyIndex> bidx;
        if (base && base->is_array()) {
          bidx = build_array_key_index(ba, key_name, max_elems);
          if (!bidx.has_value()) continue;
        } else {
          bidx = ArrayKeyIndex{};
        }

        std::unordered_set<std::string> seen;
        seen.reserve((bidx->order.size() + lidx->order.size() + ridx->order.size()) * 2 + 1);

        std::vector<std::string> order;
        order.reserve(bidx->order.size() + lidx->order.size() + ridx->order.size());

        const auto append_unique = [&](const std::vector<std::string>& ks) {
          for (const auto& k : ks) {
            if (seen.insert(k).second) order.push_back(k);
          }
        };

        // Preserve base order when possible; otherwise use local's order as the
        // initial baseline.
        if (base && base->is_array()) {
          append_unique(bidx->order);
        } else {
          append_unique(lidx->order);
        }
        append_unique(lidx->order);
        append_unique(ridx->order);

        json::Array out;
        out.reserve(order.size());
        for (const auto& kid : order) {
          const json::Value* bc = nullptr;
          const json::Value* lc = nullptr;
          const json::Value* rc = nullptr;

          if (base && base->is_array()) {
            const auto itb = bidx->by_key.find(kid);
            if (itb != bidx->by_key.end()) bc = itb->second;
          }
          {
            const auto itl = lidx->by_key.find(kid);
            if (itl != lidx->by_key.end()) lc = itl->second;
          }
          {
            const auto itr = ridx->by_key.find(kid);
            if (itr != ridx->by_key.end()) rc = itr->second;
          }

          // Use the merged array index for a valid JSON Pointer path.
          auto child = merge3_impl(join_index(path, out.size()), bc, lc, rc, opt, conflicts);
          if (!child.has_value()) continue; // deleted
          out.push_back(std::move(*child));
        }

        return json::array(std::move(out));
      }
    }
  }

  // Structural merge: arrays can merge by weaving insertions when the base array
  // is a subsequence of both local and remote.
  //
  // This helps common cases like concurrent appends to a log-like array.
  if (opt.merge_arrays_by_insertions && base && local && remote && base->is_array() && local->is_array() &&
      remote->is_array()) {
    const auto& ba = base->array();
    const auto& la = local->array();
    const auto& ra = remote->array();

    const std::size_t max_elems = opt.max_array_insertion_merge_elems;
    if (!ba.empty() && ba.size() <= max_elems && la.size() <= max_elems && ra.size() <= max_elems) {
      if (auto merged = merge_arrays_by_insertions(ba, la, ra, ne); merged.has_value()) {
        return json::array(std::move(*merged));
      }
    }
  }

  
  // Unresolvable divergent change: record conflict and resolve according to policy.
  push_conflict(conflicts, path, base, local, remote);
  return resolve_conflict(base, local, remote, opt.on_conflict);
}

std::string resolution_name(MergeConflictResolution r) {
  switch (r) {
    case MergeConflictResolution::kPreferLocal:
      return "prefer_local";
    case MergeConflictResolution::kPreferRemote:
      return "prefer_remote";
    case MergeConflictResolution::kPreferBase:
      return "prefer_base";
    case MergeConflictResolution::kFail:
    default:
      return "fail";
  }
}

} // namespace

SaveMergeResult merge_json_three_way(const json::Value& base, const json::Value& local, const json::Value& remote,
                                     SaveMergeOptions opt) {
  SaveMergeResult out;
  out.conflicts.clear();
  auto merged = merge3_impl(/*path=*/"", &base, &local, &remote, opt, out.conflicts);
  if (!merged.has_value()) {
    // Root deletion is not meaningful; fall back to local.
    out.merged = local;
  } else {
    out.merged = std::move(*merged);
  }
  return out;
}

std::string merge_saves_three_way(const std::string& base_json, const std::string& local_json, const std::string& remote_json,
                                 SaveMergeOptions opt) {
  const auto base = json::parse(base_json);
  const auto local = json::parse(local_json);
  const auto remote = json::parse(remote_json);

  auto res = merge_json_three_way(base, local, remote, opt);
  if (opt.on_conflict == MergeConflictResolution::kFail && !res.conflicts.empty()) {
    throw std::runtime_error("three-way merge produced conflicts (count=" + std::to_string(res.conflicts.size()) + ")");
  }
  return json::stringify(res.merged, opt.indent);
}

std::string merge_saves_three_way_report(const std::string& base_json, const std::string& local_json,
                                        const std::string& remote_json, SaveMergeOptions opt) {
  const auto base = json::parse(base_json);
  const auto local = json::parse(local_json);
  const auto remote = json::parse(remote_json);

  auto res = merge_json_three_way(base, local, remote, opt);

  json::Object root;
  root["conflicts_total"] = static_cast<double>(res.conflicts.size());
  root["resolved"] = (opt.on_conflict != MergeConflictResolution::kFail);
  root["resolution"] = resolution_name(opt.on_conflict);

  json::Array cs;
  cs.reserve(res.conflicts.size());
  for (const auto& c : res.conflicts) {
    json::Object e;
    e["path"] = c.path;
    if (c.has_base) e["base"] = c.base;
    if (c.has_local) e["local"] = c.local;
    if (c.has_remote) e["remote"] = c.remote;
    cs.push_back(std::move(e));
  }
  root["conflicts"] = std::move(cs);

  std::string out = json::stringify(root, opt.indent);
  out.push_back('\n');
  return out;
}

} // namespace nebula4x
