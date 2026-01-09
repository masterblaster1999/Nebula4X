#include "test.h"

#include "nebula4x/core/simulation.h"

#include <algorithm>
#include <iostream>

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_reverse_engineering() {
  using namespace nebula4x;

  // Minimal content: one salvage-research resource + one "alien" component
  // contained in an enemy ship design.
  ContentDB content;
  {
    ResourceDef dur;
    dur.id = "Duranium";
    dur.name = "Duranium";
    dur.category = "metal";
    dur.mineable = true;
    dur.salvage_research_rp_per_ton = 0.1; // 0.1 RP per ton salvaged.
    content.resources[dur.id] = dur;
  }
  {
    ComponentDef alien;
    alien.id = "alien_component";
    alien.name = "Alien Component";
    alien.type = ComponentType::Weapon;
    alien.mass_tons = 10.0;
    content.components[alien.id] = alien;
  }
  {
    ShipDesign d;
    d.id = "enemy_ship";
    d.name = "Enemy Ship";
    d.role = ShipRole::Combatant;
    d.mass_tons = 100.0;
    d.max_hp = 100.0;
    d.components = {"alien_component"};
    content.designs[d.id] = d;
  }
  {
    ShipDesign d;
    d.id = "salvager";
    d.name = "Salvager";
    d.role = ShipRole::Freighter;
    d.mass_tons = 50.0;
    d.max_hp = 50.0;
    d.speed_km_s = 0.0;
    d.cargo_tons = 500.0;
    content.designs[d.id] = d;
  }

  SimConfig cfg;
  cfg.enable_salvage_research = true;
  cfg.salvage_research_rp_multiplier = 1.0;
  cfg.enable_reverse_engineering = true;
  cfg.reverse_engineering_points_per_salvaged_ton = 1.0;
  cfg.reverse_engineering_points_required_per_component_ton = 1.0;
  cfg.reverse_engineering_unlock_cap_per_tick = 8;

  Simulation sim(std::move(content), cfg);

  // Minimal state: one system, two factions, one salvager ship, one enemy wreck.
  GameState st;
  st.save_version = GameState{}.save_version;
  st.date = Date(0);

  const Id sys_id = 1;
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Sys";
    sys.galaxy_pos = Vec2{0.0, 0.0};
    st.systems[sys_id] = sys;
  }

  {
    Faction f1;
    f1.id = 1;
    f1.name = "Player";
    st.factions[f1.id] = f1;
  }
  {
    Faction f2;
    f2.id = 2;
    f2.name = "Enemy";
    st.factions[f2.id] = f2;
  }

  const Id wreck_id = 100;
  {
    Wreck w;
    w.id = wreck_id;
    w.name = "Enemy Wreck";
    w.system_id = sys_id;
    w.position_mkm = Vec2{0.0, 0.0};
    w.created_day = 0;
    w.minerals["Duranium"] = 100.0;
    w.source_design_id = "enemy_ship";
    w.source_faction_id = 2;
    st.wrecks[w.id] = w;
  }

  const Id salvager_id = 10;
  {
    Ship sh;
    sh.id = salvager_id;
    sh.faction_id = 1;
    sh.system_id = sys_id;
    sh.position_mkm = Vec2{0.0, 0.0};
    sh.velocity_mkm_per_day = Vec2{0.0, 0.0};
    sh.name = "Salvager";
    sh.design_id = "salvager";
    st.ships[sh.id] = sh;
  }

  // Register the ship in its system.
  st.systems[sys_id].ships.push_back(salvager_id);

  // Give the ship a salvage order.
  {
    ShipOrders so;
    so.queue.push_back(SalvageWreck{.wreck_id = wreck_id, .mineral = "", .tons = 0.0});
    st.ship_orders[salvager_id] = so;
  }

  sim.load_game(st);

  // Run one day: should salvage the wreck, gain RP, and reverse-engineer the alien component.
  sim.advance_days(1);

  const auto& s = sim.state();
  const auto* fac = find_ptr(s.factions, Id{1});
  N4X_ASSERT(fac);

  // Salvage research should add RP.
  N4X_ASSERT(fac->research_points >= 1.0);

  // Reverse engineering should unlock the component.
  const bool unlocked = std::find(fac->unlocked_components.begin(), fac->unlocked_components.end(),
                                 std::string{"alien_component"}) != fac->unlocked_components.end();
  N4X_ASSERT(unlocked);

  // The wreck should be gone (fully salvaged).
  N4X_ASSERT(!find_ptr(s.wrecks, wreck_id));

  return 0;
}
