#include <iostream>
#include <string>

#include "nebula4x/core/advisor.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";        \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::GameState make_state() {
  using namespace nebula4x;
  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 1000;

  // Faction
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }

  // System
  {
    StarSystem sys;
    sys.id = 10;
    sys.name = "Sol";
    s.systems[sys.id] = sys;
    s.selected_system = sys.id;
  }

  // Body (hostile environment => requires habitation)
  {
    Body b;
    b.id = 100;
    b.name = "ColdRock";
    b.system_id = 10;
    b.surface_temp_k = 0.0;
    b.atmosphere_atm = 0.0;
    s.bodies[b.id] = b;
    s.systems[b.system_id].bodies.push_back(b.id);
  }

  // Colony
  {
    Colony c;
    c.id = 200;
    c.name = "Outpost";
    c.faction_id = 1;
    c.body_id = 100;
    c.population_millions = 50.0;

    // Construction order with unpaid minerals.
    InstallationBuildOrder ord;
    ord.installation_id = "test_install";
    ord.quantity_remaining = 1;
    ord.minerals_paid = false;
    c.construction_queue.push_back(ord);

    // Garrison target with no training capacity.
    c.garrison_target_strength = 10.0;
    c.ground_forces = 0.0;

    // No minerals on hand.
    c.minerals["Duranium"] = 0.0;

    s.colonies[c.id] = c;
  }

  // Ship
  {
    Ship sh;
    sh.id = 300;
    sh.name = "Scout";
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "scout";
    sh.fuel_tons = 10.0;
    sh.hp = 50.0;
    sh.missile_ammo = 0;
    sh.maintenance_condition = 0.30;
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
  }

  return s;
}

nebula4x::ContentDB make_content() {
  using namespace nebula4x;

  ContentDB c;

  // Minimal design for readiness checks.
  {
    ShipDesign d;
    d.id = "scout";
    d.name = "Scout";
    d.mass_tons = 1000.0;
    d.fuel_capacity_tons = 100.0;
    d.missile_ammo_capacity = 20;
    d.max_hp = 100.0;
    c.designs[d.id] = d;
  }

  // Installation used by the construction order.
  {
    InstallationDef inst;
    inst.id = "test_install";
    inst.name = "Test Install";
    inst.build_costs["Duranium"] = 100.0;
    c.installations[inst.id] = inst;
  }

  return c;
}

}  // namespace

int test_advisor() {
  using namespace nebula4x;

  SimConfig cfg;
  cfg.enable_ship_maintenance = true;
  Simulation sim(make_content(), cfg);
  sim.load_game(make_state());

  AdvisorIssueOptions opt;
  opt.low_fuel_fraction = 0.25;
  opt.low_hp_fraction = 0.75;

  const auto issues_a = advisor_issues_for_faction(sim, 1, opt);
  const auto issues_b = advisor_issues_for_faction(sim, 1, opt);

  // Deterministic size and ordering.
  N4X_ASSERT(issues_a.size() == issues_b.size());
  N4X_ASSERT(!issues_a.empty());

  bool saw_duranium_need = false;
  bool saw_fuel_need = false;
  bool saw_munitions_need = false;
  bool saw_metals_need = false;

  bool saw_low_fuel = false;
  bool saw_damaged = false;
  bool saw_low_ammo = false;
  bool saw_low_maint = false;

  bool saw_hab = false;
  bool saw_garrison = false;

  for (std::size_t i = 0; i < issues_a.size(); ++i) {
    const AdvisorIssue& ia = issues_a[i];
    const AdvisorIssue& ib = issues_b[i];

    N4X_ASSERT(ia.kind == ib.kind);
    N4X_ASSERT(ia.level == ib.level);
    N4X_ASSERT(ia.severity == ib.severity);
    N4X_ASSERT(ia.ship_id == ib.ship_id);
    N4X_ASSERT(ia.colony_id == ib.colony_id);
    N4X_ASSERT(ia.resource == ib.resource);

    switch (ia.kind) {
      case AdvisorIssueKind::LogisticsNeed:
        if (ia.resource == "Duranium") {
          N4X_ASSERT(ia.missing >= 99.9);
          saw_duranium_need = true;
        } else if (ia.resource == "Fuel") {
          N4X_ASSERT(ia.missing >= 89.9);
          saw_fuel_need = true;
        } else if (ia.resource == "Munitions") {
          N4X_ASSERT(ia.missing >= 19.9);
          saw_munitions_need = true;
        } else if (ia.resource == "Metals") {
          N4X_ASSERT(ia.missing >= 0.5);
          saw_metals_need = true;
        } else {
          N4X_ASSERT(false && "Unexpected logistics resource");
        }
        break;
      case AdvisorIssueKind::ShipLowFuel:
        saw_low_fuel = true;
        N4X_ASSERT(ia.ship_id == 300);
        break;
      case AdvisorIssueKind::ShipDamaged:
        saw_damaged = true;
        N4X_ASSERT(ia.ship_id == 300);
        break;
      case AdvisorIssueKind::ShipLowAmmo:
        saw_low_ammo = true;
        N4X_ASSERT(ia.ship_id == 300);
        break;
      case AdvisorIssueKind::ShipLowMaintenance:
        saw_low_maint = true;
        N4X_ASSERT(ia.ship_id == 300);
        break;
      case AdvisorIssueKind::ColonyHabitationShortfall:
        saw_hab = true;
        N4X_ASSERT(ia.colony_id == 200);
        break;
      case AdvisorIssueKind::ColonyGarrisonProblem:
        saw_garrison = true;
        N4X_ASSERT(ia.colony_id == 200);
        break;
    }
  }

    N4X_ASSERT(saw_duranium_need);
  N4X_ASSERT(saw_fuel_need);
  N4X_ASSERT(saw_munitions_need);
  N4X_ASSERT(saw_metals_need);

  N4X_ASSERT(saw_low_fuel);
  N4X_ASSERT(saw_damaged);
  N4X_ASSERT(saw_low_ammo);
  N4X_ASSERT(saw_low_maint);

  N4X_ASSERT(saw_hab);
  N4X_ASSERT(saw_garrison);

  return 0;
}
