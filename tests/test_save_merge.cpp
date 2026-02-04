#include <iostream>
#include <string>

#include "nebula4x/util/json.h"
#include "nebula4x/util/save_merge.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_save_merge() {
  using nebula4x::json::parse;
  using nebula4x::json::stringify;

  // Non-conflicting object key changes should merge cleanly.
  {
    const auto base = parse(R"({"a":1,"b":1})");
    const auto local = parse(R"({"a":2,"b":1})");
    const auto remote = parse(R"({"a":1,"b":3})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());
    N4X_ASSERT(stringify(res.merged, 0) == R"({"a":2,"b":3})");
  }

  // Conflicting scalar change: with prefer_local, we should get local and record a conflict.
  {
    const auto base = parse(R"({"a":1})");
    const auto local = parse(R"({"a":2})");
    const auto remote = parse(R"({"a":3})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kPreferLocal;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.size() == 1);
    N4X_ASSERT(res.conflicts[0].path == "/a");
    N4X_ASSERT(stringify(res.merged, 0) == R"({"a":2})");
  }

  // Arrays: when lengths match, merge index-wise.
  {
    const auto base = parse(R"({"arr":[{"x":1},{"y":1}]})");
    const auto local = parse(R"({"arr":[{"x":2},{"y":1}]})");
    const auto remote = parse(R"({"arr":[{"x":1},{"y":2}]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());
    const auto& root = res.merged.object();
    N4X_ASSERT(root.at("arr").is_array());
    const auto& arr = root.at("arr").array();
    N4X_ASSERT(arr.size() == 2);
    N4X_ASSERT(arr[0].object().at("x").number_value() == 2.0);
    N4X_ASSERT(arr[1].object().at("y").number_value() == 2.0);
  }

  // Arrays (objects with id): when lengths differ, merge by key.
  {
    const auto base = parse(R"({"arr":[{"id":1,"x":1},{"id":2,"y":1}]})");
    const auto local = parse(R"({"arr":[{"id":1,"x":2},{"id":2,"y":1},{"id":3,"z":5}]})");
    const auto remote = parse(R"({"arr":[{"id":1,"x":1},{"id":2,"y":2}]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());

    const auto& root = res.merged.object();
    N4X_ASSERT(root.at("arr").is_array());
    const auto& arr = root.at("arr").array();
    N4X_ASSERT(arr.size() == 3);

    // Expected order: base order (id=1, id=2), then local-only additions.
    N4X_ASSERT(arr[0].object().at("id").number_value() == 1.0);
    N4X_ASSERT(arr[0].object().at("x").number_value() == 2.0);
    N4X_ASSERT(arr[1].object().at("id").number_value() == 2.0);
    N4X_ASSERT(arr[1].object().at("y").number_value() == 2.0);
    N4X_ASSERT(arr[2].object().at("id").number_value() == 3.0);
    N4X_ASSERT(arr[2].object().at("z").number_value() == 5.0);
  }

  // Arrays (objects with id): delete vs unchanged should delete cleanly.
  {
    const auto base = parse(R"({"arr":[{"id":1},{"id":2}]})");
    const auto local = parse(R"({"arr":[{"id":1}]})");
    const auto remote = parse(R"({"arr":[{"id":1},{"id":2}]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());
    const auto& arr = res.merged.object().at("arr").array();
    N4X_ASSERT(arr.size() == 1);
    N4X_ASSERT(arr[0].object().at("id").number_value() == 1.0);
  }

  // Arrays (objects with id): delete vs modify should conflict.
  {
    const auto base = parse(R"({"arr":[{"id":1,"x":1},{"id":2,"y":1}]})");
    const auto local = parse(R"({"arr":[{"id":1,"x":1}]})");
    const auto remote = parse(R"({"arr":[{"id":1,"x":1},{"id":2,"y":2}]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kPreferRemote;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.size() == 1);
    N4X_ASSERT(res.conflicts[0].path == "/arr/1");

    const auto& arr = res.merged.object().at("arr").array();
    N4X_ASSERT(arr.size() == 2);
    N4X_ASSERT(arr[1].object().at("id").number_value() == 2.0);
    N4X_ASSERT(arr[1].object().at("y").number_value() == 2.0);
  }

  // Arrays: insertion-wise merge can weave concurrent appends without a key.
  {
    const auto base = parse(R"({"arr":[1,2]})");
    const auto local = parse(R"({"arr":[1,2,3]})");
    const auto remote = parse(R"({"arr":[1,2,4]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;
    opt.merge_arrays_by_insertions = true;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());
    N4X_ASSERT(stringify(res.merged, 0) == R"({"arr":[1,2,3,4]})");
  }

  // Arrays: insertion-wise merge can weave concurrent inserts between anchors.
  {
    const auto base = parse(R"({"arr":[1,4]})");
    const auto local = parse(R"({"arr":[1,2,4]})");
    const auto remote = parse(R"({"arr":[1,3,4]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;
    opt.merge_arrays_by_insertions = true;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());
    N4X_ASSERT(stringify(res.merged, 0) == R"({"arr":[1,2,3,4]})");
  }

  // Arrays: length changes are treated atomically and should conflict when both sides diverge.
  {
    const auto base = parse(R"({"arr":[1,2]})");
    const auto local = parse(R"({"arr":[1,2,3]})");
    const auto remote = parse(R"({"arr":[1]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kPreferRemote;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.size() == 1);
    N4X_ASSERT(res.conflicts[0].path == "/arr");
    N4X_ASSERT(stringify(res.merged, 0) == R"({"arr":[1]})");
  }

  // Missing base key + two object additions should merge by union.
  {
    const auto base = parse(R"({})");
    const auto local = parse(R"({"x":{"a":1}})");
    const auto remote = parse(R"({"x":{"b":2}})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());
    N4X_ASSERT(stringify(res.merged, 0) == R"({"x":{"a":1,"b":2}})");
  }

  // Arrays (objects): auto-discover key-wise merge keys (e.g. "ID") when the
  // default candidates aren't present.
  {
    const auto base = parse(R"({"arr":[{"ID":1,"x":1},{"ID":2,"y":1}]})");
    const auto local = parse(R"({"arr":[{"ID":1,"x":2},{"ID":2,"y":1},{"ID":3,"z":5}]})");
    const auto remote = parse(R"({"arr":[{"ID":1,"x":1},{"ID":2,"y":2}]})");

    nebula4x::SaveMergeOptions opt;
    opt.on_conflict = nebula4x::MergeConflictResolution::kFail;
    opt.auto_discover_array_key = true;

    const auto res = nebula4x::merge_json_three_way(base, local, remote, opt);
    N4X_ASSERT(res.conflicts.empty());

    const auto& arr = res.merged.object().at("arr").array();
    N4X_ASSERT(arr.size() == 3);
    N4X_ASSERT(arr[0].object().at("ID").number_value() == 1.0);
    N4X_ASSERT(arr[0].object().at("x").number_value() == 2.0);
    N4X_ASSERT(arr[1].object().at("ID").number_value() == 2.0);
    N4X_ASSERT(arr[1].object().at("y").number_value() == 2.0);
    N4X_ASSERT(arr[2].object().at("ID").number_value() == 3.0);
    N4X_ASSERT(arr[2].object().at("z").number_value() == 5.0);
  }

  return 0;
}
