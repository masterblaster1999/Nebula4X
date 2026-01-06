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
    N4X_ASSERT(canon == base);
  }

  // Latest reconstruction (snap1).
  {
    const std::string recon = nebula4x::reconstruct_delta_save_json(ds, -1, 2);
    const auto st = nebula4x::deserialize_game_from_json(recon);
    const std::string canon = nebula4x::serialize_game_to_json(st);
    N4X_ASSERT(canon == snap1);
  }

  // Append another snapshot.
  nebula4x::append_delta_save(ds, snap2);
  N4X_ASSERT(ds.patches.size() == 2);

  {
    const std::string recon = nebula4x::reconstruct_delta_save_json(ds, -1, 2);
    const auto st = nebula4x::deserialize_game_from_json(recon);
    const std::string canon = nebula4x::serialize_game_to_json(st);
    N4X_ASSERT(canon == snap2);
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
    N4X_ASSERT(canon == snap2);
  }

  return 0;
}
