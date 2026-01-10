#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

double run_day(double attacker_eccm, double target_ecm, double dist_mkm, bool move_target,
               double attacker_sensor_mkm, double attacker_weapon_range_mkm) {
  using namespace nebula4x;

  ContentDB content;

  ShipDesign attacker_d;
  attacker_d.id = "ew_attacker";
  attacker_d.name = "EW Attacker";
  attacker_d.mass_tons = 100.0;
  attacker_d.max_hp = 100.0;
  attacker_d.speed_km_s = move_target ? 0.0 : 0.0;
  attacker_d.sensor_range_mkm = attacker_sensor_mkm;
  attacker_d.signature_multiplier = 1.0;
  attacker_d.eccm_strength = attacker_eccm;

  attacker_d.weapon_damage = 10.0;
  attacker_d.weapon_range_mkm = attacker_weapon_range_mkm;

  content.designs[attacker_d.id] = attacker_d;

  ShipDesign target_d;
  target_d.id = "ew_target";
  target_d.name = "EW Target";
  target_d.mass_tons = 100.0;
  target_d.max_hp = 100.0;
  target_d.speed_km_s = move_target ? 200.0 : 0.0;
  target_d.sensor_range_mkm = 0.0;
  target_d.signature_multiplier = 1.0;
  target_d.ecm_strength = target_ecm;

  content.designs[target_d.id] = target_d;

  SimConfig cfg;
  cfg.max_events = 1000;
  cfg.enable_beam_hit_chance = true;

  Simulation sim(content, cfg);

  GameState s;
  s.save_version = 12;
  s.date = Date::from_ymd(2200, 1, 1);

  // Two hostile factions (default Hostile when relation missing).
  Id fac_a = 1;
  Id fac_b = 2;
  {
    Faction f;
    f.id = fac_a;
    f.name = "A";
    s.factions[f.id] = f;
  }
  {
    Faction f;
    f.id = fac_b;
    f.name = "B";
    s.factions[f.id] = f;
  }

  // Single system.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Test";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  s.systems[sys.id] = sys;

  // Ships.
  Ship attacker;
  attacker.id = 100;
  attacker.faction_id = fac_a;
  attacker.system_id = sys.id;
  attacker.design_id = attacker_d.id;
  attacker.name = "Attacker";
  attacker.position_mkm = Vec2{0.0, 0.0};
  attacker.hp = attacker_d.max_hp;

  Ship target;
  target.id = 101;
  target.faction_id = fac_b;
  target.system_id = sys.id;
  target.design_id = target_d.id;
  target.name = "Target";
  target.position_mkm = Vec2{dist_mkm, 0.0};
  target.hp = target_d.max_hp;

  if (move_target) {
    // Move perpendicular to the attacker->target line to generate angular velocity.
    s.ship_orders[target.id].queue.push_back(MoveToPoint{Vec2{dist_mkm, 100.0}});
  }

  s.ships[attacker.id] = attacker;
  s.ships[target.id] = target;

  sim.load_game(std::move(s));

  sim.advance_days(1);

  const Ship* after = find_ptr(sim.state().ships, target.id);
  N4X_ASSERT(after);
  return after->hp;
}

}  // namespace

int test_electronic_warfare() {
  // --- Detection gating: ECM should be able to prevent an engagement near the edge of sensor range,
  // and ECCM should counter it.
  {
    const double dist = 40.0;
    const double sensor = 50.0;
    const double wpn_range = 100.0;

    const double hp_no_ecm = run_day(/*attacker_eccm=*/0.0, /*target_ecm=*/0.0, dist, /*move_target=*/false,
                                    sensor, wpn_range);
    const double hp_ecm = run_day(/*attacker_eccm=*/0.0, /*target_ecm=*/1.0, dist, /*move_target=*/false,
                                 sensor, wpn_range);
    const double hp_eccm = run_day(/*attacker_eccm=*/1.0, /*target_ecm=*/1.0, dist, /*move_target=*/false,
                                  sensor, wpn_range);

    // Baseline: should take damage.
    N4X_ASSERT(hp_no_ecm < 100.0);

    // With ECM, detection should fail -> no damage taken.
    N4X_ASSERT(std::abs(hp_ecm - 100.0) < 1e-6);

    // ECCM should counter ECM -> damage resumes.
    N4X_ASSERT(hp_eccm < 100.0);
  }

  // --- Tracking degradation: with a moving target, ECM should reduce beam hit chance even if detected.
  {
    const double dist = 10.0;
    const double sensor = 1000.0;      // ensure detection regardless of EW
    const double wpn_range = 50.0;

    const double hp_no_ecm = run_day(/*attacker_eccm=*/0.0, /*target_ecm=*/0.0, dist, /*move_target=*/true,
                                    sensor, wpn_range);
    const double hp_ecm = run_day(/*attacker_eccm=*/0.0, /*target_ecm=*/3.0, dist, /*move_target=*/true,
                                 sensor, wpn_range);
    const double hp_eccm = run_day(/*attacker_eccm=*/3.0, /*target_ecm=*/3.0, dist, /*move_target=*/true,
                                  sensor, wpn_range);

    // EW should matter, but shouldn't eliminate all damage in this setup.
    N4X_ASSERT(hp_no_ecm < 100.0);
    N4X_ASSERT(hp_ecm <= 100.0);
    N4X_ASSERT(hp_ecm > hp_no_ecm);

    // ECCM should claw back some of the lost accuracy.
    N4X_ASSERT(hp_eccm < hp_ecm);
  }

  return 0;
}
