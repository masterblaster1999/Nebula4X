#include <iostream>
#include <string>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"

#include "nebula4x/util/digest.h"
#include "nebula4x/util/save_delta.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::ContentDB minimal_content() {
  nebula4x::ContentDB content;

  nebula4x::InstallationDef mine;
  mine.id = "automated_mine";
  mine.name = "Automated Mine";
  mine.produces_per_day = {{"Duranium", 1.0}};
  content.installations[mine.id] = mine;

  nebula4x::InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 50;
  content.installations[yard.id] = yard;

  nebula4x::ShipDesign d;
  d.id = "freighter_alpha";
  d.name = "Freighter Alpha";
  d.mass_tons = 100;
  d.speed_km_s = 10;
  content.designs[d.id] = d;

  return content;
}

std::string digest_hex(const std::string& save_json) {
  const auto st = nebula4x::deserialize_game_from_json(save_json);
  return nebula4x::digest64_to_hex(nebula4x::digest_game_state64(st));
}

} // namespace

int test_save_delta() {
  nebula4x::Simulation sim(minimal_content(), nebula4x::SimConfig{});

  const std::string base = nebula4x::serialize_game_to_json(sim.state());
  sim.advance_days(5);
  const std::string snap1 = nebula4x::serialize_game_to_json(sim.state());
  sim.advance_days(3);
  const std::string snap2 = nebula4x::serialize_game_to_json(sim.state());

  nebula4x::DeltaSaveFile ds = nebula4x::make_delta_save(base, snap1);
  N4X_ASSERT(ds.patches.size() == 1);

  // Base reconstruction.
  {
    const std::string recon_base = nebula4x::reconstruct_delta_save_json(ds, 0, 2);
    const auto st = nebula4x::deserialize_game_from_json(recon_base);
    const std::string canon = nebula4x::serialize_game_to_json(st);
    N4X_ASSERT(digest_hex(canon) == digest_hex(base));
  }

  // Latest reconstruction (snap1).
  {
    const std::string recon = nebula4x::reconstruct_delta_save_json(ds, -1, 2);
    const auto st = nebula4x::deserialize_game_from_json(recon);
    const std::string canon = nebula4x::serialize_game_to_json(st);
    N4X_ASSERT(digest_hex(canon) == digest_hex(snap1));
  }

  // Append another snapshot.
  nebula4x::append_delta_save(ds, snap2);
  N4X_ASSERT(ds.patches.size() == 2);

  {
    const std::string recon = nebula4x::reconstruct_delta_save_json(ds, -1, 2);
    const auto st = nebula4x::deserialize_game_from_json(recon);
    const std::string canon = nebula4x::serialize_game_to_json(st);
    N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
  }

  // Digests recorded in the delta-save should match reconstructed states.
  N4X_ASSERT(!ds.base_state_digest_hex.empty());
  N4X_ASSERT(!ds.patches[0].state_digest_hex.empty());
  N4X_ASSERT(!ds.patches[1].state_digest_hex.empty());
  N4X_ASSERT(ds.base_state_digest_hex == digest_hex(base));
  N4X_ASSERT(ds.patches[0].state_digest_hex == digest_hex(snap1));
  N4X_ASSERT(ds.patches[1].state_digest_hex == digest_hex(snap2));

  // Round-trip parse/stringify.
  {
    const std::string ds_json = nebula4x::stringify_delta_save_file(ds, 2);
    const nebula4x::DeltaSaveFile ds2 = nebula4x::parse_delta_save_file(ds_json);
    N4X_ASSERT(ds2.patches.size() == ds.patches.size());

    const std::string recon = nebula4x::reconstruct_delta_save_json(ds2, -1, 2);
    const auto st = nebula4x::deserialize_game_from_json(recon);
    const std::string canon = nebula4x::serialize_game_to_json(st);
    N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
  }

  // JSON Patch delta-save (v2).
  {
    nebula4x::DeltaSaveFile dsj = nebula4x::make_delta_save(base, snap1, nebula4x::DeltaSavePatchKind::JsonPatch);
    N4X_ASSERT(dsj.format == nebula4x::kDeltaSaveFormatV2);
    N4X_ASSERT(dsj.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch);
    N4X_ASSERT(dsj.patches.size() == 1);

    // Latest reconstruction (snap1).
    {
      const std::string recon = nebula4x::reconstruct_delta_save_json(dsj, -1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap1));
    }

    nebula4x::append_delta_save(dsj, snap2);
    N4X_ASSERT(dsj.patches.size() == 2);

    {
      const std::string recon = nebula4x::reconstruct_delta_save_json(dsj, -1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
    }

    // Digests recorded in the delta-save should match reconstructed states.
    N4X_ASSERT(!dsj.base_state_digest_hex.empty());
    N4X_ASSERT(!dsj.patches[0].state_digest_hex.empty());
    N4X_ASSERT(!dsj.patches[1].state_digest_hex.empty());
    N4X_ASSERT(dsj.base_state_digest_hex == digest_hex(base));
    N4X_ASSERT(dsj.patches[0].state_digest_hex == digest_hex(snap1));
    N4X_ASSERT(dsj.patches[1].state_digest_hex == digest_hex(snap2));

    // Round-trip parse/stringify.
    {
      const std::string dsj_json = nebula4x::stringify_delta_save_file(dsj, 2);
      const nebula4x::DeltaSaveFile dsj2 = nebula4x::parse_delta_save_file(dsj_json);
      N4X_ASSERT(dsj2.format == nebula4x::kDeltaSaveFormatV2);
      N4X_ASSERT(dsj2.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch);
      N4X_ASSERT(dsj2.patches.size() == dsj.patches.size());

      const std::string recon = nebula4x::reconstruct_delta_save_json(dsj2, -1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
    }
  }

  // Squash/compact delta-save history.
  {
    // ds currently holds base -> snap1 -> snap2 (merge patch).
    nebula4x::DeltaSaveFile squashed = nebula4x::squash_delta_save(ds, 0);
    N4X_ASSERT(squashed.patch_kind == nebula4x::DeltaSavePatchKind::MergePatch);
    N4X_ASSERT(squashed.patches.size() == 1);
    N4X_ASSERT(squashed.base_state_digest_hex == digest_hex(base));
    N4X_ASSERT(squashed.patches[0].state_digest_hex == digest_hex(snap2));

    {
      const std::string recon = nebula4x::reconstruct_delta_save_json(squashed, -1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
    }

    // Rebase at snap1 and squash the remainder.
    nebula4x::DeltaSaveFile squashed2 = nebula4x::squash_delta_save(ds, 1);
    N4X_ASSERT(squashed2.patch_kind == nebula4x::DeltaSavePatchKind::MergePatch);
    N4X_ASSERT(squashed2.patches.size() == 1);
    N4X_ASSERT(squashed2.base_state_digest_hex == digest_hex(snap1));
    N4X_ASSERT(squashed2.patches[0].state_digest_hex == digest_hex(snap2));

    // Convert to a JSON Patch delta-save while squashing.
    nebula4x::DeltaSaveFile squashedj =
        nebula4x::squash_delta_save_as(ds, 0, nebula4x::DeltaSavePatchKind::JsonPatch);
    N4X_ASSERT(squashedj.format == nebula4x::kDeltaSaveFormatV2);
    N4X_ASSERT(squashedj.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch);
    N4X_ASSERT(squashedj.patches.size() == 1);
    N4X_ASSERT(squashedj.base_state_digest_hex == digest_hex(base));
    N4X_ASSERT(squashedj.patches[0].state_digest_hex == digest_hex(snap2));

    // Squash to a snapshot: base_index == final => 0 patches.
    nebula4x::DeltaSaveFile squashed_final = nebula4x::squash_delta_save(ds, 2);
    N4X_ASSERT(squashed_final.patches.empty());
    N4X_ASSERT(squashed_final.base_state_digest_hex == digest_hex(snap2));
  }

  // Convert delta-save patch kind while preserving patch count.
  {
    // ds currently holds base -> snap1 -> snap2 (merge patch).
    nebula4x::DeltaSaveFile ds_jsonpatch =
        nebula4x::convert_delta_save_patch_kind(ds, nebula4x::DeltaSavePatchKind::JsonPatch);
    N4X_ASSERT(ds_jsonpatch.format == nebula4x::kDeltaSaveFormatV2);
    N4X_ASSERT(ds_jsonpatch.patch_kind == nebula4x::DeltaSavePatchKind::JsonPatch);
    N4X_ASSERT(ds_jsonpatch.patches.size() == ds.patches.size());

    // Reconstruct intermediate snapshot.
    {
      const std::string recon = nebula4x::reconstruct_delta_save_json(ds_jsonpatch, 1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap1));
    }

    // Final snapshot.
    {
      const std::string recon = nebula4x::reconstruct_delta_save_json(ds_jsonpatch, -1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
    }

    // Convert back to merge patch.
    nebula4x::DeltaSaveFile ds_merge =
        nebula4x::convert_delta_save_patch_kind(ds_jsonpatch, nebula4x::DeltaSavePatchKind::MergePatch);
    N4X_ASSERT(ds_merge.patch_kind == nebula4x::DeltaSavePatchKind::MergePatch);
    N4X_ASSERT(ds_merge.patches.size() == ds.patches.size());

    {
      const std::string recon = nebula4x::reconstruct_delta_save_json(ds_merge, -1, 2);
      const auto st = nebula4x::deserialize_game_from_json(recon);
      const std::string canon = nebula4x::serialize_game_to_json(st);
      N4X_ASSERT(digest_hex(canon) == digest_hex(snap2));
    }
  }

  return 0;
}
