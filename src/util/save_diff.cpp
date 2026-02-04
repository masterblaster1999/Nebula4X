#include "nebula4x/util/save_diff.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"

namespace nebula4x {
namespace {

struct Change {
  std::string op;   // add|remove|replace
  std::string path; // JSON-pointer-like
  json::Value before;
  json::Value after;
};

struct PatchOp {
  std::string op;   // add|remove|replace
  std::string path; // RFC 6901 JSON Pointer
  std::string from; // RFC 6901 JSON Pointer (for move/copy)
  json::Value value;
  bool has_value{false};
  bool has_from{false};
  int index{-1}; // For diagnostics when applying patches.
};

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
  if (base.empty() || base == "/") return "/" + esc;
  return base + "/" + esc;
}

std::string join_index(const std::string& base, std::size_t idx) {
  const std::string token = std::to_string(idx);
  if (base.empty() || base == "/") return "/" + token;
  return base + "/" + token;
}

std::string join_append(const std::string& base) {
  if (base.empty() || base == "/") return "/-";
  return base + "/-";
}

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

// Strict equality for JSON Patch 'test' operations.
//
// RFC 6902 defines this as a logical JSON comparison: same JSON types and
// recursively equal values (object member ordering is not significant).
bool values_equal_strict(const json::Value& a, const json::Value& b) {
  if (a.index() != b.index()) return false;
  if (a.is_null()) return true;
  if (a.is_bool()) return *a.as_bool() == *b.as_bool();
  if (a.is_number()) return *a.as_number() == *b.as_number();
  if (a.is_string()) return *a.as_string() == *b.as_string();
  if (a.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    if (aa.size() != bb.size()) return false;
    for (std::size_t i = 0; i < aa.size(); ++i) {
      if (!values_equal_strict(aa[i], bb[i])) return false;
    }
    return true;
  }
  // object
  const auto& ao = a.object();
  const auto& bo = b.object();
  if (ao.size() != bo.size()) return false;
  for (const auto& [k, av] : ao) {
    const auto itb = bo.find(k);
    if (itb == bo.end()) return false;
    if (!values_equal_strict(av, itb->second)) return false;
  }
  return true;
}



constexpr std::size_t kMaxArrayMoveDetect = 256;

// Determines whether moving element `from` to `to` would make the arrays match,
// allowing the moved element's destination (k == to) to differ (the moved value
// may be subsequently patched in-place). All other indices must match exactly
// under the move transformation.
bool array_move_matches_allow_modified_at_to(const json::Array& aa, std::size_t a_begin, const json::Array& bb,
                                             std::size_t b_begin, std::size_t n, std::size_t from, std::size_t to,
                                             bool& modified_at_to) {
  modified_at_to = false;
  if (from == to) return false;
  if (from >= n || to >= n) return false;

  const json::Value& moved = aa[a_begin + from];

  for (std::size_t k = 0; k < n; ++k) {
    const json::Value* exp = nullptr;

    if (from < to) {
      if (k < from) {
        exp = &aa[a_begin + k];
      } else if (k < to) {
        exp = &aa[a_begin + (k + 1)];
      } else if (k == to) {
        exp = &moved;
      } else {
        exp = &aa[a_begin + k];
      }
    } else { // from > to
      if (k < to) {
        exp = &aa[a_begin + k];
      } else if (k == to) {
        exp = &moved;
      } else if (k <= from) {
        exp = &aa[a_begin + (k - 1)];
      } else {
        exp = &aa[a_begin + k];
      }
    }

    if (k == to) {
      modified_at_to = !values_equal(*exp, bb[b_begin + k]);
      continue;
    }

    if (!values_equal(*exp, bb[b_begin + k])) return false;
  }

  return true;
}

bool detect_array_single_move(const json::Array& aa, std::size_t a_begin, const json::Array& bb, std::size_t b_begin,
                              std::size_t n, std::size_t& from_abs, std::size_t& to_abs, bool& modified_at_to) {
  if (n < 2) return false;
  if (n > kMaxArrayMoveDetect) return false;

  // Narrow the search region to the first/last mismatching indices.
  std::size_t first = 0;
  while (first < n && values_equal(aa[a_begin + first], bb[b_begin + first])) {
    ++first;
  }
  if (first == n) return false;

  std::size_t last = n - 1;
  while (last > first && values_equal(aa[a_begin + last], bb[b_begin + last])) {
    --last;
  }

  for (std::size_t from = first; from <= last; ++from) {
    for (std::size_t to = first; to <= last; ++to) {
      if (from == to) continue;
      bool mod = false;
      if (!array_move_matches_allow_modified_at_to(aa, a_begin, bb, b_begin, n, from, to, mod)) continue;
      from_abs = a_begin + from;
      to_abs = b_begin + to;
      modified_at_to = mod;
      return true;
    }
  }

  return false;
}

constexpr std::size_t kMaxArrayLcs = 128;

struct LcsPair {
  std::size_t a_rel{0};
  std::size_t b_rel{0};
};

std::vector<LcsPair> lcs_equal_pairs(const json::Array& aa, std::size_t a_begin, std::size_t a_end,
                                     const json::Array& bb, std::size_t b_begin, std::size_t b_end) {
  std::vector<LcsPair> out;

  const std::size_t n = (a_end > a_begin) ? (a_end - a_begin) : 0;
  const std::size_t m = (b_end > b_begin) ? (b_end - b_begin) : 0;
  if (n == 0 || m == 0) return out;

  // Guard against quadratic blowups.
  if (n > kMaxArrayLcs || m > kMaxArrayLcs) return out;

  const std::size_t stride = m + 1;
  std::vector<std::uint16_t> dp((n + 1) * (m + 1), 0);

  auto at = [&](std::size_t i, std::size_t j) -> std::uint16_t& {
    return dp[i * stride + j];
  };

  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      if (values_equal(aa[a_begin + i], bb[b_begin + j])) {
        at(i, j) = static_cast<std::uint16_t>(at(i + 1, j + 1) + 1);
      } else {
        at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
  }

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n && j < m) {
    if (values_equal(aa[a_begin + i], bb[b_begin + j]) &&
        at(i, j) == static_cast<std::uint16_t>(at(i + 1, j + 1) + 1)) {
      out.push_back(LcsPair{.a_rel = i, .b_rel = j});
      ++i;
      ++j;
      continue;
    }

    const std::uint16_t down = at(i + 1, j);
    const std::uint16_t right = at(i, j + 1);
    if (down >= right) {
      ++i;
    } else {
      ++j;
    }
  }

  return out;
}

void push_change(std::vector<Change>& out, bool& truncated, const SaveDiffOptions& opt, Change c) {
  if (static_cast<int>(out.size()) >= opt.max_changes) {
    truncated = true;
    return;
  }
  // Normalize root path.
  if (c.path.empty()) c.path = "/";
  out.push_back(std::move(c));
}

void push_patch_op(std::vector<PatchOp>& out, bool& truncated, const JsonPatchOptions& opt, PatchOp op) {
  if (opt.max_ops > 0 && static_cast<int>(out.size()) >= opt.max_ops) {
    truncated = true;
    return;
  }
  // RFC 6901 root is the empty string. For convenience, normalize "/" -> "".
  if (op.path == "/") op.path.clear();
  out.push_back(std::move(op));
}

void push_precondition_test(std::vector<PatchOp>& out, bool& truncated, const JsonPatchOptions& opt,
                            const std::string& path, const json::Value& expected) {
  if (!opt.emit_tests) return;
  PatchOp t;
  t.op = "test";
  t.path = path;
  t.value = expected;
  t.has_value = true;
  push_patch_op(out, truncated, opt, std::move(t));
}

void diff_patch_impl(const json::Value& a, const json::Value& b, const std::string& path,
                     std::vector<PatchOp>& out, bool& truncated, const JsonPatchOptions& opt) {
  if (truncated) return;
  if (values_equal(a, b)) return;

  const bool a_scalar = a.is_null() || a.is_bool() || a.is_number() || a.is_string();
  const bool b_scalar = b.is_null() || b.is_bool() || b.is_number() || b.is_string();

  if (a.index() != b.index() || (a_scalar && b_scalar)) {
    push_precondition_test(out, truncated, opt, path, a);
    if (truncated) return;
    PatchOp op;
    op.op = "replace";
    op.path = path;
    op.value = b;
    op.has_value = true;
    push_patch_op(out, truncated, opt, std::move(op));
    return;
  }

  if (a.is_array() && b.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();

    // Arrays are a common source of noisy patches when items are inserted or removed
    // near the front/middle. To keep patches smaller, trim identical prefixes/suffixes
    // first and only diff the minimal "middle" window.
    const std::size_t min_sz = std::min(aa.size(), bb.size());

    std::size_t pre = 0;
    while (pre < min_sz && values_equal(aa[pre], bb[pre])) {
      ++pre;
    }

    std::size_t suf = 0;
    while (suf < (aa.size() - pre) && suf < (bb.size() - pre) &&
           values_equal(aa[aa.size() - 1 - suf], bb[bb.size() - 1 - suf])) {
      ++suf;
    }

    const std::size_t aa_mid_begin = pre;
    const std::size_t bb_mid_begin = pre;
    const std::size_t aa_mid_end = aa.size() - suf;
    const std::size_t bb_mid_end = bb.size() - suf;
    const std::size_t aa_mid_sz = (aa_mid_end >= aa_mid_begin) ? (aa_mid_end - aa_mid_begin) : 0;
    const std::size_t bb_mid_sz = (bb_mid_end >= bb_mid_begin) ? (bb_mid_end - bb_mid_begin) : 0;



    // If the trimmed middle windows are the same size, see if the change is a pure
    // single-element relocation. In that case, emit a single RFC 6902 'move' operation.
    if (aa_mid_sz == bb_mid_sz && aa_mid_sz >= 2 && aa_mid_begin == bb_mid_begin) {
      std::size_t from_abs = 0;
      std::size_t to_abs = 0;
      bool modified_at_to = false;
      if (detect_array_single_move(aa, aa_mid_begin, bb, bb_mid_begin, aa_mid_sz, from_abs, to_abs, modified_at_to)) {
        // If the element differs at its destination, only treat this as a useful
        // "move+edit" pattern when we can express the edit as a nested diff.
        //
        // For scalars (or type changes), a move followed by a full replace is
        // usually just noise (e.g. shift-left with a new tail would otherwise
        // become move+replace). In those cases, fall through to the shift/LCS
        // heuristics below which emit clearer remove+add sequences.
        bool accept = true;
        if (modified_at_to) {
          const json::Value& moved = aa[from_abs];
          const json::Value& dest = bb[to_abs];
          const bool moved_container = moved.is_object() || moved.is_array();
          const bool dest_container = dest.is_object() || dest.is_array();
          accept = moved_container && dest_container && (moved.index() == dest.index());
        }

        if (!accept) {
          // Not a beneficial move+edit; continue with other array heuristics.
        } else {
        PatchOp op;
        op.op = "move";
        op.from = join_index(path, from_abs);
        op.has_from = true;
        op.path = join_index(path, to_abs);
        op.has_value = false;
        push_precondition_test(out, truncated, opt, op.from, aa[from_abs]);
        if (truncated) return;
        push_patch_op(out, truncated, opt, std::move(op));
        if (truncated) return;

        // If the moved element is also modified at its destination, emit a follow-up
        // diff at the destination path. This keeps patches small when a record is
        // re-ordered and then edited.
        if (modified_at_to) {
          diff_patch_impl(aa[from_abs], bb[to_abs], join_index(path, to_abs), out, truncated, opt);
        }
        return;
        }
      }
    }

    // Additional array heuristic: if the trimmed middle windows are the same size but represent a
    // simple one-step shift/rotation, emit a smaller patch (remove+add or move) instead of a cascade
    // of replaces.
    if (aa_mid_sz == bb_mid_sz && aa_mid_sz >= 2) {
      const std::size_t n = aa_mid_sz;

      bool left_shift = true;
      for (std::size_t i = 0; i + 1 < n; ++i) {
        if (!values_equal(aa[aa_mid_begin + i + 1], bb[bb_mid_begin + i])) {
          left_shift = false;
          break;
        }
      }

      if (left_shift) {
        // Rotate-left by one: [x, a, b] -> [a, b, x]
        if (values_equal(aa[aa_mid_begin], bb[bb_mid_begin + (n - 1)])) {
          PatchOp op;
          op.op = "move";
          op.from = join_index(path, aa_mid_begin);
          op.has_from = true;
          op.path = join_index(path, aa_mid_begin + (n - 1));
          op.has_value = false;
          push_precondition_test(out, truncated, opt, op.from, aa[aa_mid_begin]);
          if (truncated) return;
          push_patch_op(out, truncated, opt, std::move(op));
          return;
        }

        // Shift-left by one with a new tail: remove the first element, append the new last element.
        {
          PatchOp rem;
          rem.op = "remove";
          rem.path = join_index(path, aa_mid_begin);
          rem.has_value = false;
          push_precondition_test(out, truncated, opt, rem.path, aa[aa_mid_begin]);
          if (truncated) return;
          push_patch_op(out, truncated, opt, std::move(rem));
          if (truncated) return;

          PatchOp add;
          add.op = "add";
          const std::size_t insert_idx = aa_mid_begin + (n - 1);
          const std::size_t size_after_remove = aa.size() - 1;
          add.path = (insert_idx == size_after_remove) ? join_append(path) : join_index(path, insert_idx);
          add.value = bb[bb_mid_begin + (n - 1)];
          add.has_value = true;
          push_patch_op(out, truncated, opt, std::move(add));
          return;
        }
      }

      bool right_shift = true;
      for (std::size_t i = 0; i + 1 < n; ++i) {
        if (!values_equal(aa[aa_mid_begin + i], bb[bb_mid_begin + i + 1])) {
          right_shift = false;
          break;
        }
      }

      if (right_shift) {
        // Rotate-right by one: [a, b, x] -> [x, a, b]
        if (values_equal(aa[aa_mid_begin + (n - 1)], bb[bb_mid_begin])) {
          PatchOp op;
          op.op = "move";
          op.from = join_index(path, aa_mid_begin + (n - 1));
          op.has_from = true;
          op.path = join_index(path, aa_mid_begin);
          op.has_value = false;
          push_precondition_test(out, truncated, opt, op.from, aa[aa_mid_begin + (n - 1)]);
          if (truncated) return;
          push_patch_op(out, truncated, opt, std::move(op));
          return;
        }

        // Shift-right by one with a new head: remove the last element, prepend the new first element.
        {
          PatchOp rem;
          rem.op = "remove";
          rem.path = join_index(path, aa_mid_begin + (n - 1));
          rem.has_value = false;
          push_precondition_test(out, truncated, opt, rem.path, aa[aa_mid_begin + (n - 1)]);
          if (truncated) return;
          push_patch_op(out, truncated, opt, std::move(rem));
          if (truncated) return;

          PatchOp add;
          add.op = "add";
          add.path = join_index(path, aa_mid_begin);
          add.value = bb[bb_mid_begin];
          add.has_value = true;
          push_patch_op(out, truncated, opt, std::move(add));
          return;
        }
      }
    }

    // For small arrays where the trimmed middle windows differ in length, use an LCS alignment
    // to avoid cascades of replaces when items are inserted/removed and later items also change.
    if (aa_mid_sz != bb_mid_sz && aa_mid_sz > 0 && bb_mid_sz > 0 && aa_mid_sz <= kMaxArrayLcs &&
        bb_mid_sz <= kMaxArrayLcs) {
      const auto matches = lcs_equal_pairs(aa, aa_mid_begin, aa_mid_end, bb, bb_mid_begin, bb_mid_end);

      std::size_t a_pos = 0;
      std::size_t b_pos = 0;
      std::size_t cur_idx = aa_mid_begin; // absolute index in the live array while patch ops apply
      std::size_t cur_size = aa.size();   // live array size as ops apply

      auto process_segment = [&](std::size_t a_end_rel, std::size_t b_end_rel) -> bool {
        const std::size_t da = a_end_rel - a_pos;
        const std::size_t db = b_end_rel - b_pos;
        const std::size_t common = std::min(da, db);

        for (std::size_t k = 0; k < common; ++k) {
          diff_patch_impl(aa[aa_mid_begin + a_pos + k], bb[bb_mid_begin + b_pos + k],
                          join_index(path, cur_idx + k), out, truncated, opt);
          if (truncated) return false;
        }

        if (da > db) {
          // Remove extra elements from end->front (keeps indices valid).
          for (std::size_t k = da; k-- > db;) {
            PatchOp op;
            op.op = "remove";
            op.path = join_index(path, cur_idx + k);
            op.has_value = false;
            push_precondition_test(out, truncated, opt, op.path, aa[aa_mid_begin + a_pos + k]);
            if (truncated) return false;
            push_patch_op(out, truncated, opt, std::move(op));
            if (truncated) return false;
            if (cur_size > 0) cur_size -= 1;
          }
        } else if (db > da) {
          // Insert extra elements in forward order to preserve relative ordering.
          for (std::size_t k = da; k < db; ++k) {
            PatchOp op;
            op.op = "add";
            const std::size_t insert_idx = cur_idx + k;
            op.path = (insert_idx == cur_size) ? join_append(path) : join_index(path, insert_idx);
            op.value = bb[bb_mid_begin + b_pos + k];
            op.has_value = true;
            push_patch_op(out, truncated, opt, std::move(op));
            if (truncated) return false;
            cur_size += 1;
          }
        }

        a_pos = a_end_rel;
        b_pos = b_end_rel;
        cur_idx += db;
        return true;
      };

      for (const auto& m : matches) {
        if (!process_segment(m.a_rel, m.b_rel)) return;

        // Skip the matched element (identical in both arrays).
        ++a_pos;
        ++b_pos;
        ++cur_idx;
      }

      if (!process_segment(aa_mid_sz, bb_mid_sz)) return;
      return;
    }

    const std::size_t n_mid = std::min(aa_mid_sz, bb_mid_sz);
    for (std::size_t i = 0; i < n_mid; ++i) {
      diff_patch_impl(aa[aa_mid_begin + i], bb[bb_mid_begin + i], join_index(path, aa_mid_begin + i), out, truncated,
                      opt);
      if (truncated) return;
    }

    // Important: for sequentially-applied JSON Patches, array removals must be
    // emitted from the end towards the front to keep indices valid.
    if (aa_mid_sz > bb_mid_sz) {
      for (std::size_t i = aa_mid_sz; i-- > bb_mid_sz;) {
        PatchOp op;
        op.op = "remove";
        op.path = join_index(path, aa_mid_begin + i);
        op.has_value = false;
        push_precondition_test(out, truncated, opt, op.path, aa[aa_mid_begin + i]);
        if (truncated) return;
        push_patch_op(out, truncated, opt, std::move(op));
        if (truncated) return;
      }
    } else if (bb_mid_sz > aa_mid_sz) {
      // When inserting/appending elements, prefer RFC 6902 'copy' if the value already exists at a
      // stable index in this array. This keeps patches smaller when repeated subtrees occur.
      const std::size_t insertion_start = bb_mid_begin + aa_mid_sz;

      struct CopySrc {
        std::size_t idx;
        const json::Value* v;
      };
      std::vector<CopySrc> copy_srcs;
      copy_srcs.reserve(insertion_start + (bb_mid_sz - aa_mid_sz));

      // Indices before the first insertion point are not shifted by later insertions (we insert in
      // increasing index order), so they are safe 'from' sources.
      for (std::size_t si = 0; si < insertion_start && si < bb.size(); ++si) {
        copy_srcs.push_back(CopySrc{.idx = si, .v = &bb[si]});
      }

      std::size_t cur_size = aa.size();
      for (std::size_t i = aa_mid_sz; i < bb_mid_sz; ++i) {
        const std::size_t insert_idx = bb_mid_begin + i;
        const json::Value& tv = bb[bb_mid_begin + i];

        PatchOp op;
        op.path = (insert_idx == cur_size) ? join_append(path) : join_index(path, insert_idx);

        bool used_copy = false;
        for (const auto& src : copy_srcs) {
          if (!src.v) continue;
          if (!values_equal_strict(*src.v, tv)) continue;

          op.op = "copy";
          op.from = join_index(path, src.idx);
          op.has_from = true;
          op.has_value = false;
          used_copy = true;
          break;
        }

        if (!used_copy) {
          op.op = "add";
          op.value = tv;
          op.has_value = true;
        } else {
          push_precondition_test(out, truncated, opt, op.from, tv);
          if (truncated) return;
        }

        push_patch_op(out, truncated, opt, std::move(op));
        if (truncated) return;

        // After insertion, the new element exists at insert_idx and is stable for the rest of this
        // insertion run (later insertions happen at higher indices).
        copy_srcs.push_back(CopySrc{.idx = insert_idx, .v = &tv});
        cur_size += 1;
      }
    }

    return;
  }

  if (a.is_object() && b.is_object()) {
    const auto& ao = a.object();
    const auto& bo = b.object();

    // Object key renames are common in manual save edits and schema evolution.
    // If a key is removed and a different key is added with an identical value,
    // emit a single RFC 6902 'move' instead of separate remove+add ops.
    //
    // Notes:
    // - We only consider destinations that did not exist in the original object
    //   to avoid clobbering values.
    // - Pairing is deterministic but greedy; this is a heuristic, not an LCS.
    std::vector<std::string> removed_keys;
    std::vector<std::string> added_keys;
    removed_keys.reserve(ao.size());
    added_keys.reserve(bo.size());

    for (const auto& [k, _] : ao) {
      if (bo.find(k) == bo.end()) removed_keys.push_back(k);
    }
    for (const auto& [k, _] : bo) {
      if (ao.find(k) == ao.end()) added_keys.push_back(k);
    }
    std::sort(removed_keys.begin(), removed_keys.end());
    std::sort(added_keys.begin(), added_keys.end());

    std::vector<bool> added_used(added_keys.size(), false);
    std::vector<std::string> moved_from;
    std::vector<std::string> moved_to;
    moved_from.reserve(std::min(removed_keys.size(), added_keys.size()));
    moved_to.reserve(std::min(removed_keys.size(), added_keys.size()));

    for (const auto& rk : removed_keys) {
      const json::Value& rv = ao.at(rk);
      for (std::size_t j = 0; j < added_keys.size(); ++j) {
        if (added_used[j]) continue;
        const auto itb = bo.find(added_keys[j]);
        if (itb == bo.end()) continue;
        if (!values_equal(rv, itb->second)) continue;

        PatchOp op;
        op.op = "move";
        op.from = join_path(path, rk);
        op.has_from = true;
        op.path = join_path(path, added_keys[j]);
        op.has_value = false;
        push_precondition_test(out, truncated, opt, op.from, rv);
        if (truncated) return;
        push_patch_op(out, truncated, opt, std::move(op));

        added_used[j] = true;
        moved_from.push_back(rk);
        moved_to.push_back(added_keys[j]);
        break;
      }
      if (truncated) return;
    }

    // Additional rename heuristic: if there is exactly one removed key and one added key
    // remaining, and the values are both containers of the same type, treat it as a
    // rename + edit. Emit a move, then diff the moved value at the destination.
    //
    // This is conservative and helps avoid noisy remove+add of large objects when only
    // a few nested fields changed.
    {
      std::vector<std::string> rem_left;
      std::vector<std::string> add_left;
      rem_left.reserve(removed_keys.size());
      add_left.reserve(added_keys.size());

      for (const auto& rk : removed_keys) {
        if (std::find(moved_from.begin(), moved_from.end(), rk) == moved_from.end()) rem_left.push_back(rk);
      }
      for (std::size_t j = 0; j < added_keys.size(); ++j) {
        if (!added_used[j]) add_left.push_back(added_keys[j]);
      }

      if (rem_left.size() == 1 && add_left.size() == 1) {
        const std::string& rk = rem_left[0];
        const std::string& ak = add_left[0];
        const json::Value& rv = ao.at(rk);
        const json::Value& tv = bo.at(ak);

        const bool r_cont = rv.is_object() || rv.is_array();
        const bool t_cont = tv.is_object() || tv.is_array();
        if (r_cont && t_cont && rv.index() == tv.index()) {
          PatchOp op;
          op.op = "move";
          op.from = join_path(path, rk);
          op.has_from = true;
          op.path = join_path(path, ak);
          op.has_value = false;
          push_precondition_test(out, truncated, opt, op.from, rv);
          if (truncated) return;
          push_patch_op(out, truncated, opt, std::move(op));
          if (truncated) return;

          // Record this as a rename so we don't also emit remove+add for the keys.
          moved_from.push_back(rk);
          moved_to.push_back(ak);

          // Mark the added key as used.
          for (std::size_t j = 0; j < added_keys.size(); ++j) {
            if (added_keys[j] == ak) {
              added_used[j] = true;
              break;
            }
          }

          // Patch the moved value in place at its new location.
          diff_patch_impl(rv, tv, join_path(path, ak), out, truncated, opt);
          if (truncated) return;
        }
      }
    }
    std::sort(moved_from.begin(), moved_from.end());
    std::sort(moved_to.begin(), moved_to.end());

    // Collect stable keys that already contain their final value by the time subsequent ops apply.
    // This enables safe RFC 6902 'copy' ops for duplicate-value additions (keeps patches smaller).
    std::vector<std::string> copy_sources;
    copy_sources.reserve(ao.size() + moved_to.size());

    // Keys that exist in both a and b with identical values are stable.
    for (const auto& [k, av] : ao) {
      const auto itb2 = bo.find(k);
      if (itb2 == bo.end()) continue;
      if (values_equal_strict(av, itb2->second)) copy_sources.push_back(k);
    }

    // Keys moved into place by earlier rename heuristics are also available and already patched to
    // their final value before the rest of the object diff runs.
    for (const auto& k : moved_to) copy_sources.push_back(k);

    std::sort(copy_sources.begin(), copy_sources.end());
    copy_sources.erase(std::unique(copy_sources.begin(), copy_sources.end()), copy_sources.end());

    auto add_copy_source = [&](const std::string& key) {
      if (key.empty()) return;
      auto it = std::lower_bound(copy_sources.begin(), copy_sources.end(), key);
      if (it == copy_sources.end() || *it != key) {
        copy_sources.insert(it, key);
      }
    };

    std::vector<std::string> keys;
    keys.reserve(ao.size() + bo.size());
    for (const auto& [k, _] : ao) keys.push_back(k);
    for (const auto& [k, _] : bo) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    for (const auto& k : keys) {
      const auto ita = ao.find(k);
      const auto itb = bo.find(k);
      const bool in_a = (ita != ao.end());
      const bool in_b = (itb != bo.end());

      if (!in_a && in_b) {
        // If this was already accounted for via a rename move, skip it.
        if (std::binary_search(moved_to.begin(), moved_to.end(), k)) continue;

        // If the added value already exists elsewhere in this object and that source is stable
        // at apply-time, emit a 'copy' op instead of embedding the full value.
        const json::Value& tv = itb->second;
        bool used_copy = false;
        for (const auto& src_k : copy_sources) {
          if (src_k == k) continue;
          const auto its = bo.find(src_k);
          if (its == bo.end()) continue;
          if (!values_equal_strict(its->second, tv)) continue;

          PatchOp op;
          op.op = "copy";
          op.from = join_path(path, src_k);
          op.has_from = true;
          op.path = join_path(path, k);
          op.has_value = false;
          push_precondition_test(out, truncated, opt, op.from, tv);
          if (truncated) return;
          push_patch_op(out, truncated, opt, std::move(op));
          used_copy = true;
          break;
        }

        if (!used_copy) {
          PatchOp op;
          op.op = "add";
          op.path = join_path(path, k);
          op.value = tv;
          op.has_value = true;
          push_patch_op(out, truncated, opt, std::move(op));
        }

        // After the add/copy, this key holds its final value and can serve as a copy source
        // for later keys within the same object patch.
        add_copy_source(k);
      } else if (in_a && !in_b) {
        // If this key was moved away as part of a rename, skip it.
        if (std::binary_search(moved_from.begin(), moved_from.end(), k)) continue;
        push_precondition_test(out, truncated, opt, join_path(path, k), ita->second);
        if (truncated) return;
        PatchOp op;
        op.op = "remove";
        op.path = join_path(path, k);
        op.has_value = false;
        push_patch_op(out, truncated, opt, std::move(op));
      } else if (in_a && in_b) {
        const json::Value& av = ita->second;
        const json::Value& tv = itb->second;

        // If the new value already exists elsewhere in this object (on a key that is stable at
        // apply-time), prefer RFC 6902 'copy' over emitting a nested patch.
        bool used_copy = false;
        if (!values_equal(av, tv)) {
          for (const auto& src_k : copy_sources) {
            if (src_k == k) continue;
            const auto its = bo.find(src_k);
            if (its == bo.end()) continue;
            if (!values_equal_strict(its->second, tv)) continue;

            PatchOp op;
            op.op = "copy";
            op.from = join_path(path, src_k);
            op.has_from = true;
            op.path = join_path(path, k);
            op.has_value = false;
            push_precondition_test(out, truncated, opt, op.from, tv);
            if (truncated) return;
            push_patch_op(out, truncated, opt, std::move(op));
            used_copy = true;
            break;
          }
        }
        if (truncated) return;
        if (used_copy) {
          add_copy_source(k);
          continue;
        }

        diff_patch_impl(av, tv, join_path(path, k), out, truncated, opt);
        if (truncated) return;
        add_copy_source(k);
      }

      if (truncated) return;
    }
    return;
  }

  // Fallback: replace (should be rare).
  {
    push_precondition_test(out, truncated, opt, path, a);
    if (truncated) return;
    PatchOp op;
    op.op = "replace";
    op.path = path;
    op.value = b;
    op.has_value = true;
    push_patch_op(out, truncated, opt, std::move(op));
  }
}

bool parse_array_index(const std::string& tok, std::size_t& out) {
  return parse_json_pointer_index(tok, out);
}

json::Value* pointer_parent(json::Value& doc, const std::vector<std::string>& tokens) {
  json::Value* cur = &doc;
  if (tokens.size() <= 1) return cur;

  for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
    const std::string& t = tokens[i];

    if (cur->is_object()) {
      auto* o = cur->as_object();
      auto it = o->find(t);
      if (it == o->end()) throw std::runtime_error("JSON pointer path not found: /" + t);
      cur = &it->second;
      continue;
    }

    if (cur->is_array()) {
      auto* a = cur->as_array();
      std::size_t idx = 0;
      if (!parse_array_index(t, idx)) throw std::runtime_error("JSON pointer array index invalid: " + t);
      if (idx >= a->size()) throw std::runtime_error("JSON pointer array index out of range: " + t);
      cur = &(*a)[idx];
      continue;
    }

    throw std::runtime_error("JSON pointer traversed into scalar at token: " + t);
  }
  return cur;
}

bool is_proper_prefix(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.size() >= b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

void apply_single_op(json::Value& doc, const PatchOp& op, const JsonPatchApplyOptions& opt) {
  // RFC 6902: operations are applied sequentially. Some ops (test/copy/move)
  // need to resolve values before mutating the document.

  if (op.op == "test") {
    if (!op.has_value) throw std::runtime_error("JSON Patch: test missing value");
    std::string err;
    const json::Value* cur = resolve_json_pointer(doc, op.path, opt.accept_root_slash, &err);
    if (!cur) {
      throw std::runtime_error("JSON Patch: test path not found: " + op.path + (err.empty() ? "" : (" (" + err + ")")));
    }
    if (!values_equal_strict(*cur, op.value)) {
      throw std::runtime_error("JSON Patch: test failed at path: " + op.path);
    }
    return;
  }

  if (op.op == "copy") {
    if (!op.has_from) throw std::runtime_error("JSON Patch: copy missing from");
    std::string err;
    const json::Value* src = resolve_json_pointer(doc, op.from, opt.accept_root_slash, &err);
    if (!src) {
      throw std::runtime_error("JSON Patch: copy from path not found: " + op.from + (err.empty() ? "" : (" (" + err + ")")));
    }
    PatchOp add;
    add.op = "add";
    add.path = op.path;
    add.value = *src;
    add.has_value = true;
    apply_single_op(doc, add, opt);
    return;
  }

  if (op.op == "move") {
    if (!op.has_from) throw std::runtime_error("JSON Patch: move missing from");

    const auto from_tokens = split_json_pointer(op.from, opt.accept_root_slash);
    const auto path_tokens = split_json_pointer(op.path, opt.accept_root_slash);
    if (is_proper_prefix(from_tokens, path_tokens)) {
      throw std::runtime_error("JSON Patch: move from is a proper prefix of path");
    }

    std::string err;
    const json::Value* src = resolve_json_pointer(doc, op.from, opt.accept_root_slash, &err);
    if (!src) {
      throw std::runtime_error("JSON Patch: move from path not found: " + op.from + (err.empty() ? "" : (" (" + err + ")")));
    }
    json::Value moved = *src;

    PatchOp rem;
    rem.op = "remove";
    rem.path = op.from;
    apply_single_op(doc, rem, opt);

    PatchOp add;
    add.op = "add";
    add.path = op.path;
    add.value = std::move(moved);
    add.has_value = true;
    apply_single_op(doc, add, opt);
    return;
  }

  const auto tokens = split_json_pointer(op.path, opt.accept_root_slash);

  // Root operations.
  if (tokens.empty()) {
    if (op.op == "remove") {
      throw std::runtime_error("JSON Patch: remove at document root is not supported");
    }
    if (op.op == "add" || op.op == "replace") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: missing value for root op");
      doc = op.value;
      return;
    }
    throw std::runtime_error("JSON Patch: unknown op: " + op.op);
  }

  json::Value* parent = pointer_parent(doc, tokens);
  const std::string& last = tokens.back();

  if (parent->is_object()) {
    auto* o = parent->as_object();

    if (op.op == "add") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: add missing value");
      (*o)[last] = op.value;
      return;
    }
    if (op.op == "replace") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: replace missing value");
      auto it = o->find(last);
      if (it == o->end()) throw std::runtime_error("JSON Patch: replace path not found: " + op.path);
      it->second = op.value;
      return;
    }
    if (op.op == "remove") {
      auto it = o->find(last);
      if (it == o->end()) throw std::runtime_error("JSON Patch: remove path not found: " + op.path);
      o->erase(it);
      return;
    }

    throw std::runtime_error("JSON Patch: unknown op: " + op.op);
  }

  if (parent->is_array()) {
    auto* a = parent->as_array();

    if (op.op == "add") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: add missing value");
      if (last == "-") {
        a->push_back(op.value);
        return;
      }
      std::size_t idx = 0;
      if (!parse_array_index(last, idx)) throw std::runtime_error("JSON Patch: invalid array index: " + last);
      if (idx > a->size()) throw std::runtime_error("JSON Patch: add index out of range: " + last);
      a->insert(a->begin() + static_cast<std::ptrdiff_t>(idx), op.value);
      return;
    }

    std::size_t idx = 0;
    if (!parse_array_index(last, idx)) throw std::runtime_error("JSON Patch: invalid array index: " + last);
    if (idx >= a->size()) throw std::runtime_error("JSON Patch: index out of range: " + last);

    if (op.op == "replace") {
      if (!op.has_value) throw std::runtime_error("JSON Patch: replace missing value");
      (*a)[idx] = op.value;
      return;
    }
    if (op.op == "remove") {
      a->erase(a->begin() + static_cast<std::ptrdiff_t>(idx));
      return;
    }

    throw std::runtime_error("JSON Patch: unknown op: " + op.op);
  }

  throw std::runtime_error("JSON Patch: parent is not container for path: " + op.path);
}

void diff_impl(const json::Value& a, const json::Value& b, const std::string& path, std::vector<Change>& out,
               bool& truncated, const SaveDiffOptions& opt) {
  if (truncated) return;

  if (values_equal(a, b)) return;

  // Type mismatch or scalar replacement.
  const bool a_scalar = a.is_null() || a.is_bool() || a.is_number() || a.is_string();
  const bool b_scalar = b.is_null() || b.is_bool() || b.is_number() || b.is_string();

  if (a.index() != b.index() || (a_scalar && b_scalar)) {
    push_change(out, truncated, opt, Change{"replace", path, a, b});
    return;
  }

  if (a.is_array() && b.is_array()) {
    const auto& aa = a.array();
    const auto& bb = b.array();
    const std::size_t n = std::min(aa.size(), bb.size());

    for (std::size_t i = 0; i < n; ++i) {
      diff_impl(aa[i], bb[i], join_index(path, i), out, truncated, opt);
      if (truncated) return;
    }

    if (aa.size() > bb.size()) {
      for (std::size_t i = bb.size(); i < aa.size(); ++i) {
        push_change(out, truncated, opt, Change{"remove", join_index(path, i), aa[i], nullptr});
        if (truncated) return;
      }
    } else if (bb.size() > aa.size()) {
      for (std::size_t i = aa.size(); i < bb.size(); ++i) {
        push_change(out, truncated, opt, Change{"add", join_index(path, i), nullptr, bb[i]});
        if (truncated) return;
      }
    }
    return;
  }

  if (a.is_object() && b.is_object()) {
    const auto& ao = a.object();
    const auto& bo = b.object();

    std::vector<std::string> keys;
    keys.reserve(ao.size() + bo.size());
    for (const auto& [k, _] : ao) keys.push_back(k);
    for (const auto& [k, _] : bo) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    for (const auto& k : keys) {
      const auto ita = ao.find(k);
      const auto itb = bo.find(k);
      const bool in_a = (ita != ao.end());
      const bool in_b = (itb != bo.end());

      if (!in_a && in_b) {
        push_change(out, truncated, opt, Change{"add", join_path(path, k), nullptr, itb->second});
      } else if (in_a && !in_b) {
        push_change(out, truncated, opt, Change{"remove", join_path(path, k), ita->second, nullptr});
      } else if (in_a && in_b) {
        diff_impl(ita->second, itb->second, join_path(path, k), out, truncated, opt);
      }

      if (truncated) return;
    }
    return;
  }

  // Fallback: replace (should be rare).
  push_change(out, truncated, opt, Change{"replace", path, a, b});
}

std::string shorten_value(const std::string& s, int max_chars) {
  if (max_chars <= 0) return s;
  if (static_cast<int>(s.size()) <= max_chars) return s;
  if (max_chars <= 3) return s.substr(0, static_cast<std::size_t>(max_chars));
  return s.substr(0, static_cast<std::size_t>(max_chars - 3)) + "...";
}

std::string value_one_line(const json::Value& v, int max_chars) {
  std::string s = json::stringify(v, 0);
  // Compact spaces a bit (indent==0 already avoids newlines, but keeps ":" without space).
  return shorten_value(s, max_chars);
}

} // namespace

std::string diff_saves_to_text(const std::string& a_json, const std::string& b_json, SaveDiffOptions opt) {
  json::Value a = json::parse(a_json);
  json::Value b = json::parse(b_json);

  std::vector<Change> changes;
  changes.reserve(static_cast<std::size_t>(std::max(0, opt.max_changes)));
  bool truncated = false;

  diff_impl(a, b, "", changes, truncated, opt);

  std::ostringstream out;
  out << "Save diff: " << changes.size() << " change" << (changes.size() == 1 ? "" : "s");
  if (truncated) out << " (truncated to " << opt.max_changes << ")";
  out << "\n";

  for (const auto& c : changes) {
    if (c.op == "add") {
      out << "+ " << c.path << " = " << value_one_line(c.after, opt.max_value_chars) << "\n";
    } else if (c.op == "remove") {
      out << "- " << c.path << " = " << value_one_line(c.before, opt.max_value_chars) << "\n";
    } else {
      out << "~ " << c.path << ": " << value_one_line(c.before, opt.max_value_chars)
          << " -> " << value_one_line(c.after, opt.max_value_chars) << "\n";
    }
  }

  return out.str();
}

std::string diff_saves_to_json(const std::string& a_json, const std::string& b_json, SaveDiffOptions opt) {
  json::Value a = json::parse(a_json);
  json::Value b = json::parse(b_json);

  std::vector<Change> changes;
  changes.reserve(static_cast<std::size_t>(std::max(0, opt.max_changes)));
  bool truncated = false;

  diff_impl(a, b, "", changes, truncated, opt);

  json::Object report;
  report["changes_total"] = static_cast<double>(changes.size());
  report["changes_shown"] = static_cast<double>(changes.size());
  report["truncated"] = truncated;

  json::Array arr;
  arr.reserve(changes.size());
  for (const auto& c : changes) {
    json::Object o;
    o["op"] = c.op;
    o["path"] = c.path.empty() ? std::string("/") : c.path;
    o["before"] = c.before;
    o["after"] = c.after;
    arr.emplace_back(json::object(std::move(o)));
  }
  report["changes"] = json::array(std::move(arr));

  std::string s = json::stringify(json::object(std::move(report)), 2);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

std::string diff_saves_to_json_patch(const std::string& a_json, const std::string& b_json, JsonPatchOptions opt) {
  json::Value a = json::parse(a_json);
  json::Value b = json::parse(b_json);

  std::vector<PatchOp> ops;
  ops.reserve(256);
  bool truncated = false;

  diff_patch_impl(a, b, "", ops, truncated, opt);

  if (truncated) {
    throw std::runtime_error(
        "JSON Patch generation truncated (max_ops reached). Increase JsonPatchOptions::max_ops or set it to 0 for unlimited.");
  }

  json::Array arr;
  arr.reserve(ops.size());
  for (const auto& op : ops) {
    json::Object o;
    o["op"] = op.op;
    o["path"] = op.path;
    if (op.has_from) o["from"] = op.from;
    if (op.has_value) o["value"] = op.value;
    arr.emplace_back(json::object(std::move(o)));
  }

  std::string s = json::stringify(json::array(std::move(arr)), opt.indent);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

namespace {

std::vector<PatchOp> parse_patch_ops(const json::Value& p) {
  if (!p.is_array()) throw std::runtime_error("JSON Patch must be a JSON array");

  std::vector<PatchOp> ops;
  ops.reserve(p.array().size());

  const auto& arr = p.array();
  for (std::size_t i = 0; i < arr.size(); ++i) {
    const auto& v = arr[i];

    const std::string idx = std::to_string(i);
    const std::string prefix = "JSON Patch: op #" + idx + ": ";

    if (!v.is_object()) throw std::runtime_error(prefix + "op must be an object");
    const auto& o = v.object();

    auto it_op = o.find("op");
    auto it_path = o.find("path");
    if (it_op == o.end() || !it_op->second.is_string()) throw std::runtime_error(prefix + "missing string 'op'");
    if (it_path == o.end() || !it_path->second.is_string()) throw std::runtime_error(prefix + "missing string 'path'");

    PatchOp op;
    op.index = static_cast<int>(i);
    op.op = it_op->second.string_value();
    op.path = it_path->second.string_value();

    const bool known_op = (op.op == "add" || op.op == "remove" || op.op == "replace" || op.op == "move" ||
                           op.op == "copy" || op.op == "test");
    if (!known_op) throw std::runtime_error(prefix + "unknown op: " + op.op);

    if (op.op == "add" || op.op == "replace" || op.op == "test") {
      auto it_val = o.find("value");
      if (it_val == o.end()) throw std::runtime_error(prefix + "missing 'value' for op: " + op.op);
      op.value = it_val->second;
      op.has_value = true;
    }

    if (op.op == "move" || op.op == "copy") {
      auto it_from = o.find("from");
      if (it_from == o.end() || !it_from->second.is_string()) {
        throw std::runtime_error(prefix + "missing string 'from' for op: " + op.op);
      }
      op.from = it_from->second.string_value();
      op.has_from = true;
    }

    ops.push_back(std::move(op));
  }

  return ops;
}

} // namespace

void apply_json_patch(json::Value& doc, const json::Value& patch, JsonPatchApplyOptions opt) {
  const std::vector<PatchOp> ops = parse_patch_ops(patch);

  for (const auto& op : ops) {
    try {
      apply_single_op(doc, op, opt);
    } catch (const std::exception& e) {
      const std::string idx = (op.index >= 0) ? std::to_string(op.index) : std::string("?");
      std::string msg = "JSON Patch: op #" + idx + " failed (op=" + op.op + ", path=" + op.path;
      if (op.has_from) msg += ", from=" + op.from;
      msg += "): ";
      msg += e.what();
      throw std::runtime_error(msg);
    }
  }
}

std::string apply_json_patch(const std::string& doc_json, const std::string& patch_json, JsonPatchApplyOptions opt) {
  json::Value doc = json::parse(doc_json);
  json::Value p = json::parse(patch_json);

  apply_json_patch(doc, p, opt);

  std::string s = json::stringify(doc, opt.indent);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

} // namespace nebula4x
