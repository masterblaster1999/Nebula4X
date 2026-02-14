#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

double get_mineral(const nebula4x::Colony& c, const std::string& mineral) {
  auto it = c.minerals.find(mineral);
  return (it == c.minerals.end()) ? 0.0 : it->second;
}

double get_map_tons(const std::unordered_map<std::string, double>& m, const std::string& key) {
  const auto it = m.find(key);
  return (it == m.end()) ? 0.0 : it->second;
}

template <typename Pred>
bool advance_until(nebula4x::Simulation& sim, int max_days, Pred&& pred) {
  if (pred()) return true;
  for (int d = 0; d < max_days; ++d) {
    sim.advance_days(1);
    if (pred()) return true;
  }
  return pred();
}

}  // namespace

int test_industry() {
  using namespace nebula4x;

  // 1) Industry conversion consumes inputs and produces outputs.
  {
    ContentDB content;

    InstallationDef ref;
    ref.id = "refinery";
    ref.name = "Refinery";
    ref.produces_per_day["Fuel"] = 200.0;
    ref.consumes_per_day["Duranium"] = 2.0;
    content.installations[ref.id] = ref;

    SimConfig cfg;
    cfg.enable_combat = false;
    cfg.enable_colony_stability_output_scaling = false;
    cfg.enable_colony_conditions = false;
    cfg.enable_trade_prosperity = false;

    Simulation sim(content, cfg);

    GameState st;
    st.save_version = GameState{}.save_version;

    Faction f;
    f.id = 1;
    f.name = "Player";
    f.control = FactionControl::Player;
    st.factions[f.id] = f;

    StarSystem sys;
    sys.id = 1;
    sys.name = "Sol";
    sys.galaxy_pos = Vec2{0.0, 0.0};
    st.systems[sys.id] = sys;

    Body body;
    body.id = 10;
    body.name = "Earth";
    body.system_id = sys.id;
    body.orbit_radius_mkm = 0.0;
    body.orbit_period_days = 1.0;
    body.orbit_phase_radians = 0.0;
    st.bodies[body.id] = body;

    Colony c;
    c.id = 20;
    c.name = "Colony";
    c.faction_id = f.id;
    c.body_id = body.id;
    c.population_millions = 100.0;
    c.installations[ref.id] = 1;
    c.minerals["Duranium"] = 10.0;
    st.colonies[c.id] = c;

    sim.load_game(st);

    sim.advance_days(1);
    const Colony& c1 = sim.state().colonies.at(c.id);
    N4X_ASSERT(std::abs(get_mineral(c1, "Duranium") - 8.0) < 1e-6, "refinery consumed 2 Duranium");
    N4X_ASSERT(std::abs(get_mineral(c1, "Fuel") - 200.0) < 1e-6, "refinery produced 200 Fuel");

    // Input-limited: only 1 Duranium available => 50% throughput.
    sim.state().colonies.at(c.id).minerals["Duranium"] = 1.0;
    sim.state().colonies.at(c.id).minerals["Fuel"] = 0.0;
    sim.advance_days(1);

    const Colony& c2 = sim.state().colonies.at(c.id);
    N4X_ASSERT(std::abs(get_mineral(c2, "Duranium") - 0.0) < 1e-6,
               "refinery consumed available Duranium when input-limited");
    N4X_ASSERT(std::abs(get_mineral(c2, "Fuel") - 100.0) < 1e-6,
               "refinery output scaled with available inputs");
  }

  // 2) Auto-freight supplies industry inputs (via logistics_needs_for_faction).
  {
    ContentDB content;

    InstallationDef ref;
    ref.id = "refinery";
    ref.name = "Refinery";
    ref.produces_per_day["Fuel"] = 200.0;
    ref.consumes_per_day["Duranium"] = 10.0;
    content.installations[ref.id] = ref;

    ShipDesign freighter;
    freighter.id = "freighter";
    freighter.name = "Freighter";
    freighter.role = ShipRole::Freighter;
    freighter.mass_tons = 100.0;
    freighter.max_hp = 100.0;
    freighter.speed_km_s = 100.0;
    freighter.cargo_tons = 500.0;
    content.designs[freighter.id] = freighter;

    SimConfig cfg;
    cfg.enable_combat = false;
    cfg.enable_colony_stability_output_scaling = false;
    cfg.enable_colony_conditions = false;
    cfg.enable_trade_prosperity = false;
    cfg.auto_freight_min_transfer_tons = 1.0;
    cfg.auto_freight_max_take_fraction_of_surplus = 1.0;
    cfg.auto_freight_industry_input_buffer_days = 1.0;

    Simulation sim(content, cfg);

    GameState st;
    st.save_version = GameState{}.save_version;

    Faction f;
    f.id = 1;
    f.name = "Player";
    f.control = FactionControl::Player;
    st.factions[f.id] = f;

    StarSystem sys;
    sys.id = 1;
    sys.name = "Sol";
    sys.galaxy_pos = Vec2{0.0, 0.0};
    st.systems[sys.id] = sys;

    Body src_body;
    src_body.id = 10;
    src_body.name = "Source";
    src_body.system_id = sys.id;
    src_body.orbit_radius_mkm = 0.0;
    src_body.orbit_period_days = 1.0;
    src_body.orbit_phase_radians = 0.0;
    st.bodies[src_body.id] = src_body;

    Body dst_body;
    dst_body.id = 11;
    dst_body.name = "Dest";
    dst_body.system_id = sys.id;
    dst_body.orbit_radius_mkm = 0.0;
    dst_body.orbit_period_days = 1.0;
    dst_body.orbit_phase_radians = 0.0;
    st.bodies[dst_body.id] = dst_body;

    Colony src;
    src.id = 20;
    src.name = "Src";
    src.faction_id = f.id;
    src.body_id = src_body.id;
    src.population_millions = 100.0;
    src.minerals["Duranium"] = 1000.0;
    st.colonies[src.id] = src;

    Colony dst;
    dst.id = 21;
    dst.name = "Dst";
    dst.faction_id = f.id;
    dst.body_id = dst_body.id;
    dst.population_millions = 100.0;
    dst.installations[ref.id] = 1;
    st.colonies[dst.id] = dst;

    Ship sh;
    sh.id = 100;
    sh.name = "Cargo-1";
    sh.faction_id = f.id;
    sh.design_id = freighter.id;
    sh.system_id = sys.id;
    sh.position_mkm = Vec2{0.0, 0.0};
    sh.auto_freight = true;
    st.ships[sh.id] = sh;

    sim.load_game(st);

    // Delivery may complete over one or more day ticks depending on order timing.
    const bool delivered_inputs = advance_until(sim, 3, [&]() {
      return get_mineral(sim.state().colonies.at(dst.id), "Duranium") >= 10.0 - 1e-6;
    });

    const double src_d1 = get_mineral(sim.state().colonies.at(src.id), "Duranium");
    const double dst_d1 = get_mineral(sim.state().colonies.at(dst.id), "Duranium");
    const double ship_d1 = get_map_tons(sim.state().ships.at(sh.id).cargo, "Duranium");

    N4X_ASSERT(delivered_inputs, "auto-freight delivered 1-day industry input buffer within three days");
    N4X_ASSERT(std::abs(dst_d1 - 10.0) < 1e-6, "destination received 10 Duranium industry buffer");
    N4X_ASSERT(std::abs((src_d1 + dst_d1 + ship_d1) - 1000.0) < 1e-6,
               "Duranium conserved in industry freight test (colonies + ship cargo)");

    // Disable further auto-freight and let industry run for a day on delivered inputs.
    sim.state().ships.at(sh.id).auto_freight = false;
    const bool processed_inputs = advance_until(sim, 2, [&]() {
      return get_mineral(sim.state().colonies.at(dst.id), "Fuel") >= 200.0 - 1e-6;
    });

    const Colony& dst_after = sim.state().colonies.at(dst.id);
    N4X_ASSERT(processed_inputs, "industry processed delivered inputs within two days");
    N4X_ASSERT(std::abs(get_mineral(dst_after, "Duranium") - 0.0) < 1e-6,
               "industry consumed delivered inputs");
    N4X_ASSERT(std::abs(get_mineral(dst_after, "Fuel") - 200.0) < 1e-6,
               "industry produced Fuel from inputs");
  }

  return 0;
}
