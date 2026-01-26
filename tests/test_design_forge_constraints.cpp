#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/procgen_design_forge.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "Assertion failed: " << #expr << " (" << __FILE__ << ":" << __LINE__ << ")" \
                << std::endl;                                                                       \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::ComponentDef make_component(std::string id, nebula4x::ComponentType type) {
  nebula4x::ComponentDef c;
  c.id = std::move(id);
  c.name = c.id;
  c.type = type;
  c.signature_multiplier = 1.0;
  return c;
}

void add_component(nebula4x::ContentDB& content, const nebula4x::ComponentDef& c) {
  content.components[c.id] = c;
}

double range_mkm(const nebula4x::ShipDesign& d) {
  if (d.fuel_use_per_mkm <= 0.0) {
    return 0.0;
  }
  return d.fuel_capacity_tons / d.fuel_use_per_mkm;
}

}  // namespace

int test_design_forge_constraints() {
  nebula4x::ContentDB content;

  // --- Minimal component set that can satisfy a constrained freighter spec ---
  {
    auto eng = make_component("eng_fast", nebula4x::ComponentType::Engine);
    eng.mass_tons = 5.0;
    eng.speed_km_s = 20.0;
    eng.fuel_use_per_mkm = 0.2;
    eng.power_use = 1.0;
    add_component(content, eng);
  }
  {
    auto fuel = make_component("fuel_std", nebula4x::ComponentType::FuelTank);
    fuel.mass_tons = 5.0;
    fuel.fuel_capacity_tons = 200.0;
    add_component(content, fuel);
  }
  {
    auto cargo = make_component("cargo100", nebula4x::ComponentType::Cargo);
    cargo.mass_tons = 5.0;
    cargo.cargo_tons = 100.0;
    add_component(content, cargo);
  }
  {
    auto reactor = make_component("react10", nebula4x::ComponentType::Reactor);
    reactor.mass_tons = 5.0;
    reactor.power_output = 10.0;
    add_component(content, reactor);
  }
  {
    auto sensor = make_component("sensor_long", nebula4x::ComponentType::Sensor);
    sensor.mass_tons = 5.0;
    sensor.sensor_range_mkm = 300.0;
    sensor.power_use = 1.0;
    add_component(content, sensor);
  }
  {
    auto sensor_ecm = make_component("sensor_ecm", nebula4x::ComponentType::Sensor);
    sensor_ecm.mass_tons = 5.0;
    sensor_ecm.sensor_range_mkm = 200.0;
    sensor_ecm.power_use = 1.0;
    sensor_ecm.ecm_strength = 5.0;
    add_component(content, sensor_ecm);
  }
  {
    auto sensor_eccm = make_component("sensor_eccm", nebula4x::ComponentType::Sensor);
    sensor_eccm.mass_tons = 5.0;
    sensor_eccm.sensor_range_mkm = 150.0;
    sensor_eccm.power_use = 1.0;
    sensor_eccm.eccm_strength = 4.0;
    add_component(content, sensor_eccm);
  }

  const std::vector<std::string> unlocked = {
      "eng_fast", "fuel_std", "cargo100", "react10", "sensor_long", "sensor_ecm", "sensor_eccm"};

  nebula4x::ShipDesign base;
  base.id = "base";
  base.name = "Base";
  base.role = nebula4x::ShipRole::Freighter;
  base.components = {"eng_fast", "fuel_std"};

  nebula4x::DesignForgeOptions opt;
  opt.role = nebula4x::ShipRole::Freighter;
  opt.desired_count = 12;
  opt.candidate_multiplier = 24;
  opt.mutations_per_candidate = 6;
  opt.max_components = 20;
  opt.prefer_shields = true;
  opt.include_ecm_eccm = true;
  opt.id_prefix = "t";
  opt.name_prefix = "Test";

  // Constraints (should be satisfiable with the component set above).
  opt.constraints.min_speed_km_s = 20.0;
  opt.constraints.min_range_mkm = 900.0;
  opt.constraints.min_cargo_tons = 300.0;
  opt.constraints.min_sensor_range_mkm = 250.0;
  opt.constraints.min_ecm_strength = 5.0;
  opt.constraints.min_eccm_strength = 4.0;
  opt.constraints.require_power_balance = true;
  opt.constraints.min_power_margin = 0.0;

  // Ask the forge to emit only candidates that satisfy the constraints.
  opt.only_meeting_constraints = true;

  std::string dbg;
  auto forged = nebula4x::forge_design_variants(content, unlocked, base, 12345ULL, opt, &dbg);

  N4X_ASSERT(!forged.empty());

  for (const auto& fd : forged) {
    N4X_ASSERT(fd.meets_constraints);

    const auto& d = fd.design;
    N4X_ASSERT(d.speed_km_s >= opt.constraints.min_speed_km_s);
    N4X_ASSERT(range_mkm(d) >= opt.constraints.min_range_mkm);
    N4X_ASSERT(d.cargo_tons >= opt.constraints.min_cargo_tons);
    N4X_ASSERT(d.sensor_range_mkm >= opt.constraints.min_sensor_range_mkm);
    N4X_ASSERT(d.ecm_strength >= opt.constraints.min_ecm_strength);
    N4X_ASSERT(d.eccm_strength >= opt.constraints.min_eccm_strength);

    // Power margin check.
    const double margin = d.power_generation - d.power_use_total;
    N4X_ASSERT(margin >= opt.constraints.min_power_margin - 1e-9);
  }

  // Determinism (same seed => same top result).
  std::string dbg2;
  auto forged2 = nebula4x::forge_design_variants(content, unlocked, base, 12345ULL, opt, &dbg2);
  N4X_ASSERT(!forged2.empty());
  N4X_ASSERT(forged2.front().design.components == forged.front().design.components);

  // Impossible hard constraint should yield no results when only_meeting_constraints is enabled.
  nebula4x::DesignForgeOptions opt_bad = opt;
  opt_bad.constraints.min_speed_km_s = 9999.0;
  std::string dbg3;
  auto bad = nebula4x::forge_design_variants(content, unlocked, base, 12345ULL, opt_bad, &dbg3);
  N4X_ASSERT(bad.empty());

  return 0;
}
