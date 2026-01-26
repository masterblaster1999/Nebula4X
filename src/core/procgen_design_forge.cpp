#include "nebula4x/core/procgen_design_forge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/design_stats.h"
#include "nebula4x/core/procgen_obscure.h"

namespace nebula4x {

namespace {

using procgen_obscure::HashRng;
using procgen_obscure::splitmix64;
using procgen_obscure::hex_n;

// --- small helpers ---

const char* role_short(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "FRT";
    case ShipRole::Surveyor: return "SRV";
    case ShipRole::Combatant: return "COM";
    default: return "UNK";
  }
}

double safe_range_mkm(const ShipDesign& d) {
  if (d.fuel_use_per_mkm <= 0.0) return 0.0;
  return d.fuel_capacity_tons / d.fuel_use_per_mkm;
}

double score_for_role(const ShipDesign& d, ShipRole role) {
  const double range = safe_range_mkm(d);
  const double speed = std::max(0.0, d.speed_km_s);
  const double mass = std::max(1.0, d.mass_tons);

  // Light penalty for bloated designs.
  const double mass_pen = std::pow(mass / 50.0, 0.25);

  switch (role) {
    case ShipRole::Freighter: {
      // Cargo and range dominate; speed is a convenience factor.
      const double cargo = std::max(0.0, d.cargo_tons);
      double s = 0.0;
      s += cargo * 1.25;
      s += range * 0.35;
      s += speed * 2.0;
      // Small bonus if it can colonize (multi-role logistics hull).
      s += d.colony_capacity_millions * 8.0;
      return s / mass_pen;
    }
    case ShipRole::Surveyor: {
      // Sensors and range dominate; speed helps exploration.
      const double sensor = std::max(0.0, d.sensor_range_mkm);
      double s = 0.0;
      s += sensor * 2.0;
      s += range * 0.6;
      s += speed * 1.5;
      // Stealthy scouts are fun.
      s += (1.0 - std::clamp(d.signature_multiplier, 0.05, 1.0)) * 60.0;
      // E-warfare helps survey fleets survive.
      s += (d.ecm_strength + d.eccm_strength) * 6.0;
      return s / mass_pen;
    }
    case ShipRole::Combatant: {
      // Damage + survivability + speed.
      double s = 0.0;
      const double beam = std::max(0.0, d.weapon_damage);
      const double missile = std::max(0.0, d.missile_damage);
      const double pd = std::max(0.0, d.point_defense_damage);
      const double hp = std::max(0.0, d.max_hp);
      const double sh = std::max(0.0, d.max_shields);
      s += beam * 40.0;
      s += missile * 46.0;
      s += pd * 18.0;
      s += hp * 1.0;
      s += sh * 2.2;
      s += speed * 1.8;
      s += range * 0.10;
      s += d.sensor_range_mkm * 0.6;
      s += (d.ecm_strength + d.eccm_strength) * 12.0;
      return s / mass_pen;
    }
    default: {
      // Fallback: something vaguely sensible.
      double s = 0.0;
      s += d.cargo_tons * 0.5;
      s += d.sensor_range_mkm * 0.5;
      s += d.weapon_damage * 5.0;
      s += d.missile_damage * 5.0;
      s += d.max_hp * 0.2;
      s += speed * 0.5;
      s += range * 0.2;
      return s / mass_pen;
    }
  }
}

bool constraints_enabled(const DesignForgeConstraints& c) {
  return c.min_speed_km_s > 0.0 || c.min_range_mkm > 0.0 || c.max_mass_tons > 0.0 || c.min_cargo_tons > 0.0 ||
         c.min_mining_tons_per_day > 0.0 || c.min_colony_capacity_millions > 0.0 || c.min_troop_capacity > 0.0 ||
         c.min_sensor_range_mkm > 0.0 || c.max_signature_multiplier > 0.0 || c.min_ecm_strength > 0.0 ||
         c.min_eccm_strength > 0.0 || c.min_beam_damage > 0.0 || c.min_missile_damage > 0.0 ||
         c.min_point_defense_damage > 0.0 || c.min_shields > 0.0 || c.min_hp > 0.0 ||
         c.require_power_balance || c.min_power_margin > 0.0;
}

struct ConstraintEval {
  bool meets{true};
  double penalty{0.0};
};

ConstraintEval eval_constraints(const ShipDesign& d, const DesignForgeConstraints& c) {
  ConstraintEval e;
  const double range = safe_range_mkm(d);
  const double speed = std::max(0.0, d.speed_km_s);
  const double mass = std::max(0.0, d.mass_tons);

  auto need_min = [&](double val, double min, double weight) {
    if (!(min > 0.0)) return;
    if (val + 1e-9 >= min) return;
    e.meets = false;
    const double def = min - val;
    const double denom = std::max(1.0, std::fabs(min));
    e.penalty += (def / denom) * weight;
  };

  auto need_max = [&](double val, double max, double weight) {
    if (!(max > 0.0)) return;
    if (val <= max + 1e-9) return;
    e.meets = false;
    const double def = val - max;
    const double denom = std::max(1.0, std::fabs(max));
    e.penalty += (def / denom) * weight;
  };

  need_min(speed, c.min_speed_km_s, 2200.0);
  need_min(range, c.min_range_mkm, 2200.0);
  need_max(mass, c.max_mass_tons, 1600.0);

  need_min(std::max(0.0, d.cargo_tons), c.min_cargo_tons, 2200.0);
  need_min(std::max(0.0, d.mining_tons_per_day), c.min_mining_tons_per_day, 1800.0);
  need_min(std::max(0.0, d.colony_capacity_millions), c.min_colony_capacity_millions, 2200.0);
  need_min(std::max(0.0, d.troop_capacity), c.min_troop_capacity, 1800.0);

  need_min(std::max(0.0, d.sensor_range_mkm), c.min_sensor_range_mkm, 2200.0);
  if (c.max_signature_multiplier > 0.0) {
    // 0..1 where smaller is better.
    need_max(std::clamp(d.signature_multiplier, 0.0, 1.0), c.max_signature_multiplier, 1400.0);
  }
  need_min(std::max(0.0, d.ecm_strength), c.min_ecm_strength, 1200.0);
  need_min(std::max(0.0, d.eccm_strength), c.min_eccm_strength, 1200.0);

  need_min(std::max(0.0, d.weapon_damage), c.min_beam_damage, 2600.0);
  need_min(std::max(0.0, d.missile_damage), c.min_missile_damage, 2600.0);
  need_min(std::max(0.0, d.point_defense_damage), c.min_point_defense_damage, 2200.0);
  need_min(std::max(0.0, d.max_shields), c.min_shields, 1600.0);
  need_min(std::max(0.0, d.max_hp), c.min_hp, 1600.0);

  if (c.require_power_balance) {
    const double margin = d.power_generation - d.power_use_total;
    need_min(margin, c.min_power_margin, 2200.0);
  }
  return e;
}

struct Pools {
  std::vector<std::string> engines;
  std::vector<std::string> reactors;
  std::vector<std::string> fuel;
  std::vector<std::string> cargo;
  std::vector<std::string> sensors;
  std::vector<std::string> weapons;
  std::vector<std::string> armor;
  std::vector<std::string> shields;
  std::vector<std::string> colony;
  std::vector<std::string> troops;
  std::vector<std::string> mining;
};

bool component_exists(const ContentDB& content, const std::string& cid) {
  return content.components.find(cid) != content.components.end();
}

ComponentType component_type(const ContentDB& content, const std::string& cid) {
  auto it = content.components.find(cid);
  if (it == content.components.end()) return ComponentType::Unknown;
  return it->second.type;
}

Pools build_pools(const ContentDB& content, const std::vector<std::string>& unlocked) {
  Pools p;
  p.engines.reserve(16);
  p.reactors.reserve(16);
  p.fuel.reserve(16);
  p.cargo.reserve(16);
  p.sensors.reserve(32);
  p.weapons.reserve(32);

  for (const auto& cid : unlocked) {
    auto it = content.components.find(cid);
    if (it == content.components.end()) continue;
    const ComponentDef& c = it->second;
    switch (c.type) {
      case ComponentType::Engine: p.engines.push_back(cid); break;
      case ComponentType::Reactor: p.reactors.push_back(cid); break;
      case ComponentType::FuelTank: p.fuel.push_back(cid); break;
      case ComponentType::Cargo: p.cargo.push_back(cid); break;
      case ComponentType::Mining: p.mining.push_back(cid); break;
      case ComponentType::Sensor: p.sensors.push_back(cid); break;
      case ComponentType::Weapon: p.weapons.push_back(cid); break;
      case ComponentType::Armor: p.armor.push_back(cid); break;
      case ComponentType::Shield: p.shields.push_back(cid); break;
      case ComponentType::ColonyModule: p.colony.push_back(cid); break;
      case ComponentType::TroopBay: p.troops.push_back(cid); break;
      default: break;
    }
  }

  auto dedupe_sort = [](std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  };
  dedupe_sort(p.engines);
  dedupe_sort(p.reactors);
  dedupe_sort(p.fuel);
  dedupe_sort(p.cargo);
  dedupe_sort(p.sensors);
  dedupe_sort(p.weapons);
  dedupe_sort(p.armor);
  dedupe_sort(p.shields);
  dedupe_sort(p.colony);
  dedupe_sort(p.troops);
  dedupe_sort(p.mining);

  return p;
}

// Pick the component with the maximum value of f(cid). Returns empty if pool empty.
template <typename F>
std::string pick_best(const std::vector<std::string>& pool, const ContentDB& content, F f) {
  if (pool.empty()) return {};
  double best = -std::numeric_limits<double>::infinity();
  std::string best_id;
  for (const auto& cid : pool) {
    auto it = content.components.find(cid);
    if (it == content.components.end()) continue;
    const double val = f(it->second);
    if (val > best) {
      best = val;
      best_id = cid;
    }
  }
  return best_id;
}

std::string pick_random(HashRng& rng, const std::vector<std::string>& pool) {
  if (pool.empty()) return {};
  const int idx = rng.range_int(0, (int)pool.size() - 1);
  return pool[(size_t)idx];
}

int count_type(const ContentDB& content, const std::vector<std::string>& comps, ComponentType t) {
  int n = 0;
  for (const auto& cid : comps) {
    if (component_type(content, cid) == t) ++n;
  }
  return n;
}

void remove_one_of_type(const ContentDB& content, std::vector<std::string>& comps, ComponentType t) {
  for (auto it = comps.begin(); it != comps.end(); ++it) {
    if (component_type(content, *it) == t) {
      comps.erase(it);
      return;
    }
  }
}

void replace_one_of_type(const ContentDB& content,
                         std::vector<std::string>& comps,
                         ComponentType t,
                         const std::string& replacement) {
  if (replacement.empty()) return;
  for (auto& cid : comps) {
    if (component_type(content, cid) == t) {
      cid = replacement;
      return;
    }
  }
  // If none exist, add it.
  comps.push_back(replacement);
}

// Ensure at least one component of a type exists, using a fallback pick.
void ensure_type(const ContentDB& content,
                 std::vector<std::string>& comps,
                 ComponentType t,
                 const std::string& fallback) {
  if (count_type(content, comps, t) > 0) return;
  if (!fallback.empty()) comps.push_back(fallback);
}

// Quick power check: returns (generation - use).
double power_margin(const ContentDB& content, const std::vector<std::string>& comps) {
  double gen = 0.0;
  double use = 0.0;
  for (const auto& cid : comps) {
    auto it = content.components.find(cid);
    if (it == content.components.end()) continue;
    const auto& c = it->second;
    use += c.power_use;
    if (c.type == ComponentType::Reactor) gen += c.power_output;
  }
  return gen - use;
}

void try_balance_power(const ContentDB& content,
                       const Pools& pools,
                       HashRng& rng,
                       std::vector<std::string>& comps,
                       bool prefer_shields,
                       int max_components,
                       double required_margin) {
  const int cap = std::clamp(max_components, 6, 64);
  // Add reactors to cover deficits, up to a small cap.
  for (int i = 0; i < 4; ++i) {
    if (power_margin(content, comps) >= required_margin) return;
    // Pick the highest output reactor (power per mass isn't modeled yet).
    const std::string best = pick_best(pools.reactors, content, [&](const ComponentDef& c) {
      return c.power_output;
    });
    if (best.empty()) break;
    if ((int)comps.size() < cap) {
      comps.push_back(best);
    } else {
      break;
    }
  }

  // If still underpowered, shed optional power loads.
  for (int i = 0; i < 8; ++i) {
    if (power_margin(content, comps) >= required_margin) return;

    // Shields are expensive; drop them first unless explicitly preferred.
    if (!prefer_shields && count_type(content, comps, ComponentType::Shield) > 0) {
      remove_one_of_type(content, comps, ComponentType::Shield);
      continue;
    }

    // Drop one sensor (keep at least 1 if any).
    if (count_type(content, comps, ComponentType::Sensor) > 1) {
      remove_one_of_type(content, comps, ComponentType::Sensor);
      continue;
    }

    // Drop one weapon (keep at least 1 if any).
    if (count_type(content, comps, ComponentType::Weapon) > 1) {
      remove_one_of_type(content, comps, ComponentType::Weapon);
      continue;
    }

    // Last resort: replace the engine with a random other engine (maybe cheaper).
    if (!pools.engines.empty() && count_type(content, comps, ComponentType::Engine) > 0) {
      const std::string eng = pick_random(rng, pools.engines);
      replace_one_of_type(content, comps, ComponentType::Engine, eng);
      continue;
    }

    // Give up.
    break;
  }
}

std::vector<std::string> filtered_base_components(const ContentDB& content,
                                                  const std::vector<std::string>& unlocked,
                                                  const ShipDesign& base) {
  std::unordered_set<std::string> allow;
  allow.reserve(unlocked.size() * 2);
  for (const auto& cid : unlocked) allow.insert(cid);

  std::vector<std::string> out;
  out.reserve(base.components.size());
  for (const auto& cid : base.components) {
    if (!component_exists(content, cid)) continue;
    if (allow.find(cid) == allow.end()) continue;
    out.push_back(cid);
  }
  return out;
}

void mutate_components(const ContentDB& content,
                       const Pools& pools,
                       HashRng& rng,
                       std::vector<std::string>& comps,
                       ShipRole role,
                       const DesignForgeOptions& opt) {
  const int cap = std::clamp(opt.max_components, 6, 64);
  if (comps.empty()) return;

  // Mutation target types by role.
  std::vector<ComponentType> targets;
  switch (role) {
    case ShipRole::Freighter:
      targets = {ComponentType::Cargo, ComponentType::FuelTank, ComponentType::Engine, ComponentType::Sensor,
                 ComponentType::ColonyModule, ComponentType::TroopBay};
      break;
    case ShipRole::Surveyor:
      targets = {ComponentType::Sensor, ComponentType::Engine, ComponentType::FuelTank, ComponentType::Reactor,
                 ComponentType::Armor};
      break;
    case ShipRole::Combatant:
      targets = {ComponentType::Weapon, ComponentType::Shield, ComponentType::Armor, ComponentType::Engine,
                 ComponentType::Sensor, ComponentType::Reactor, ComponentType::FuelTank};
      break;
    default:
      targets = {ComponentType::Engine, ComponentType::FuelTank, ComponentType::Sensor, ComponentType::Weapon};
      break;
  }

  auto pool_for = [&](ComponentType t) -> const std::vector<std::string>& {
    switch (t) {
      case ComponentType::Engine: return pools.engines;
      case ComponentType::Reactor: return pools.reactors;
      case ComponentType::FuelTank: return pools.fuel;
      case ComponentType::Cargo: return pools.cargo;
      case ComponentType::Mining: return pools.mining;
      case ComponentType::Sensor: return pools.sensors;
      case ComponentType::Weapon: return pools.weapons;
      case ComponentType::Armor: return pools.armor;
      case ComponentType::Shield: return pools.shields;
      case ComponentType::ColonyModule: return pools.colony;
      case ComponentType::TroopBay: return pools.troops;
      default: {
        static const std::vector<std::string> kEmpty;
        return kEmpty;
      }
    }
  };

  for (int step = 0; step < std::max(0, opt.mutations_per_candidate); ++step) {
    if (targets.empty()) break;
    const ComponentType t = targets[(size_t)rng.range_int(0, (int)targets.size() - 1)];
    const auto& pool = pool_for(t);
    if (pool.empty()) continue;

    const int op = rng.range_int(0, 99);
    if (op < 65) {
      // Replace.
      std::string pick = pick_random(rng, pool);
      // Missiles preference for combatants.
      if (role == ShipRole::Combatant && t == ComponentType::Weapon && opt.prefer_missiles) {
        // Try a few times to find a missile launcher.
        for (int i = 0; i < 6; ++i) {
          std::string cand = pick_random(rng, pool);
          auto it = content.components.find(cand);
          if (it != content.components.end() && it->second.missile_damage > 0.0) {
            pick = std::move(cand);
            break;
          }
        }
      }
      replace_one_of_type(content, comps, t, pick);
    } else if (op < 85) {
      // Add (bounded).
      if ((int)comps.size() < cap) {
        comps.push_back(pick_random(rng, pool));
      }
    } else {
      // Remove (keep minimums for core types).
      const int cur = count_type(content, comps, t);
      int min_keep = 0;
      if (t == ComponentType::Engine) min_keep = 1;
      if (t == ComponentType::FuelTank) min_keep = 1;
      if (role == ShipRole::Surveyor && t == ComponentType::Sensor) min_keep = 1;
      if (role == ShipRole::Combatant && t == ComponentType::Weapon) min_keep = 1;
      if (cur > min_keep) remove_one_of_type(content, comps, t);
    }
  }
}

void ensure_role_minimums(const ContentDB& content,
                          const Pools& pools,
                          HashRng& rng,
                          std::vector<std::string>& comps,
                          ShipRole role,
                          const DesignForgeOptions& opt) {
  const int cap = std::clamp(opt.max_components, 6, 64);
  // Always: engine + fuel.
  const std::string best_engine = pick_best(pools.engines, content, [&](const ComponentDef& c) {
    return c.speed_km_s;
  });
  const std::string best_fuel = pick_best(pools.fuel, content, [&](const ComponentDef& c) {
    return c.fuel_capacity_tons;
  });
  ensure_type(content, comps, ComponentType::Engine, best_engine);
  ensure_type(content, comps, ComponentType::FuelTank, best_fuel);

  // Role-specific.
  if (role == ShipRole::Freighter) {
    const std::string best_cargo = pick_best(pools.cargo, content, [&](const ComponentDef& c) {
      return c.cargo_tons;
    });
    if (count_type(content, comps, ComponentType::Cargo) <= 0 && !best_cargo.empty()) {
      comps.push_back(best_cargo);
    }

    // If it already has cargo, allow a little bloat.
    if (!pools.cargo.empty() && count_type(content, comps, ComponentType::Cargo) < 3 && rng.next_u01() < 0.55) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.cargo));
    }

    if (!pools.colony.empty() && rng.next_u01() < 0.20) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.colony));
    }
    if (!pools.troops.empty() && rng.next_u01() < 0.15) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.troops));
    }
    if (!pools.sensors.empty() && count_type(content, comps, ComponentType::Sensor) <= 0 && rng.next_u01() < 0.45) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.sensors));
    }
  } else if (role == ShipRole::Surveyor) {
    const std::string best_sensor = pick_best(pools.sensors, content, [&](const ComponentDef& c) {
      return c.sensor_range_mkm;
    });
    ensure_type(content, comps, ComponentType::Sensor, best_sensor);

    // ECM/ECCM are sensor-typed in content; include occasionally.
    if (opt.include_ecm_eccm && !pools.sensors.empty() && rng.next_u01() < 0.30) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.sensors));
    }
  } else if (role == ShipRole::Combatant) {
    const std::string best_sensor = pick_best(pools.sensors, content, [&](const ComponentDef& c) {
      return c.sensor_range_mkm;
    });
    if (!best_sensor.empty()) ensure_type(content, comps, ComponentType::Sensor, best_sensor);

    if (count_type(content, comps, ComponentType::Weapon) <= 0) {
      const std::string best_weapon = pick_best(pools.weapons, content, [&](const ComponentDef& c) {
        // Prefer missiles if desired, else prefer raw damage.
        return opt.prefer_missiles ? c.missile_damage * 2.0 + c.weapon_damage : c.weapon_damage + c.missile_damage * 0.8;
      });
      if (!best_weapon.empty()) comps.push_back(best_weapon);
    }

    // Extra weapon sometimes.
    if (!pools.weapons.empty() && count_type(content, comps, ComponentType::Weapon) < 3 && rng.next_u01() < 0.55) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.weapons));
    }

    // Survivability.
    if (!pools.armor.empty() && count_type(content, comps, ComponentType::Armor) <= 0 && rng.next_u01() < 0.55) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.armor));
    }
    if (opt.prefer_shields && !pools.shields.empty() && count_type(content, comps, ComponentType::Shield) <= 0 && rng.next_u01() < 0.60) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.shields));
    }

    // E-warfare is useful for combatants too.
    if (opt.include_ecm_eccm && !pools.sensors.empty() && rng.next_u01() < 0.25) {
      if ((int)comps.size() < cap) comps.push_back(pick_random(rng, pools.sensors));
    }
  }

  // If we have power-hungry components and reactors exist, make sure we have at least one.
  if (!pools.reactors.empty()) {
    const double margin = power_margin(content, comps);
    if (margin < 0.0 && count_type(content, comps, ComponentType::Reactor) <= 0) {
      comps.push_back(pick_random(rng, pools.reactors));
    }
  }
}

void tune_to_constraints(const ContentDB& content,
                         const Pools& pools,
                         HashRng& rng,
                         std::vector<std::string>& comps,
                         ShipRole role,
                         const DesignForgeOptions& opt) {
  (void)rng;
  const auto& c = opt.constraints;
  if (!constraints_enabled(c)) return;

  const int cap = std::clamp(opt.max_components, 6, 64);

  // Targeted swaps for "single-choice" stats.
  if (c.min_speed_km_s > 0.0 && !pools.engines.empty()) {
    const std::string best_engine = pick_best(pools.engines, content, [&](const ComponentDef& cd) {
      return cd.speed_km_s;
    });
    if (!best_engine.empty()) ensure_type(content, comps, ComponentType::Engine, best_engine);
  }
  if (c.min_sensor_range_mkm > 0.0 && !pools.sensors.empty()) {
    const std::string best_sensor = pick_best(pools.sensors, content, [&](const ComponentDef& cd) {
      return cd.sensor_range_mkm;
    });
    if (!best_sensor.empty()) ensure_type(content, comps, ComponentType::Sensor, best_sensor);
  }

  // Rough local totals (cheap; final derived stats are computed later).
  double mass = 0.0;
  double hp_bonus = 0.0;
  double fuel_cap = 0.0;
  double fuel_use = 0.0;
  double cargo = 0.0;
  double mining = 0.0;
  double colony = 0.0;
  double troop = 0.0;
  double ecm = 0.0;
  double eccm = 0.0;
  double beam = 0.0;
  double missile = 0.0;
  double pd = 0.0;
  double shields = 0.0;
  for (const auto& cid : comps) {
    auto it = content.components.find(cid);
    if (it == content.components.end()) continue;
    const auto& cd = it->second;
    mass += cd.mass_tons;
    hp_bonus += cd.hp_bonus;
    fuel_cap += cd.fuel_capacity_tons;
    fuel_use += cd.fuel_use_per_mkm;
    cargo += cd.cargo_tons;
    mining += cd.mining_tons_per_day;
    colony += cd.colony_capacity_millions;
    troop += cd.troop_capacity;
    ecm += std::max(0.0, cd.ecm_strength);
    eccm += std::max(0.0, cd.eccm_strength);
    if (cd.type == ComponentType::Weapon) {
      beam += cd.weapon_damage;
      missile += std::max(0.0, cd.missile_damage);
      pd += std::max(0.0, cd.point_defense_damage);
    }
    if (cd.type == ComponentType::Shield) {
      shields += std::max(0.0, cd.shield_hp);
    }
  }

  auto can_add = [&](const std::string& cid) -> bool {
    if ((int)comps.size() >= cap) return false;
    auto it = content.components.find(cid);
    if (it == content.components.end()) return false;
    if (c.max_mass_tons > 0.0 && (mass + it->second.mass_tons) > c.max_mass_tons + 1e-6) return false;
    return true;
  };

  // Range.
  if (c.min_range_mkm > 0.0 && fuel_use > 0.0) {
    const std::string best_fuel = pick_best(pools.fuel, content, [&](const ComponentDef& cd) {
      return cd.fuel_capacity_tons;
    });
    while (!best_fuel.empty() && can_add(best_fuel) && (fuel_cap / fuel_use) + 1e-9 < c.min_range_mkm) {
      const auto& cd = content.components.at(best_fuel);
      comps.push_back(best_fuel);
      mass += cd.mass_tons;
      fuel_cap += cd.fuel_capacity_tons;
    }
  }

  // Cargo.
  if (c.min_cargo_tons > 0.0) {
    const std::string best_cargo = pick_best(pools.cargo, content, [&](const ComponentDef& cd) {
      return cd.cargo_tons;
    });
    while (!best_cargo.empty() && can_add(best_cargo) && cargo + 1e-9 < c.min_cargo_tons) {
      const auto& cd = content.components.at(best_cargo);
      comps.push_back(best_cargo);
      mass += cd.mass_tons;
      cargo += cd.cargo_tons;
    }
  }

  // Mining.
  if (c.min_mining_tons_per_day > 0.0) {
    const std::string best_mining = pick_best(pools.mining, content, [&](const ComponentDef& cd) {
      return cd.mining_tons_per_day;
    });
    while (!best_mining.empty() && can_add(best_mining) && mining + 1e-9 < c.min_mining_tons_per_day) {
      const auto& cd = content.components.at(best_mining);
      comps.push_back(best_mining);
      mass += cd.mass_tons;
      mining += cd.mining_tons_per_day;
    }
  }

  // Colonization / troops.
  if (c.min_colony_capacity_millions > 0.0) {
    const std::string best_colony = pick_best(pools.colony, content, [&](const ComponentDef& cd) {
      return cd.colony_capacity_millions;
    });
    while (!best_colony.empty() && can_add(best_colony) && colony + 1e-9 < c.min_colony_capacity_millions) {
      const auto& cd = content.components.at(best_colony);
      comps.push_back(best_colony);
      mass += cd.mass_tons;
      colony += cd.colony_capacity_millions;
    }
  }
  if (c.min_troop_capacity > 0.0) {
    const std::string best_troop = pick_best(pools.troops, content, [&](const ComponentDef& cd) {
      return cd.troop_capacity;
    });
    while (!best_troop.empty() && can_add(best_troop) && troop + 1e-9 < c.min_troop_capacity) {
      const auto& cd = content.components.at(best_troop);
      comps.push_back(best_troop);
      mass += cd.mass_tons;
      troop += cd.troop_capacity;
    }
  }

  // Combat.
  if (role == ShipRole::Combatant || c.min_beam_damage > 0.0 || c.min_missile_damage > 0.0 || c.min_point_defense_damage > 0.0) {
    const std::string best_beam = pick_best(pools.weapons, content, [&](const ComponentDef& cd) {
      return cd.weapon_damage;
    });
    const std::string best_missile = pick_best(pools.weapons, content, [&](const ComponentDef& cd) {
      return cd.missile_damage;
    });
    const std::string best_pd = pick_best(pools.weapons, content, [&](const ComponentDef& cd) {
      return cd.point_defense_damage;
    });

    while (!best_beam.empty() && can_add(best_beam) && beam + 1e-9 < c.min_beam_damage) {
      const auto& cd = content.components.at(best_beam);
      comps.push_back(best_beam);
      mass += cd.mass_tons;
      beam += cd.weapon_damage;
      missile += std::max(0.0, cd.missile_damage);
      pd += std::max(0.0, cd.point_defense_damage);
    }
    while (!best_missile.empty() && can_add(best_missile) && missile + 1e-9 < c.min_missile_damage) {
      const auto& cd = content.components.at(best_missile);
      comps.push_back(best_missile);
      mass += cd.mass_tons;
      beam += cd.weapon_damage;
      missile += std::max(0.0, cd.missile_damage);
      pd += std::max(0.0, cd.point_defense_damage);
    }
    while (!best_pd.empty() && can_add(best_pd) && pd + 1e-9 < c.min_point_defense_damage) {
      const auto& cd = content.components.at(best_pd);
      comps.push_back(best_pd);
      mass += cd.mass_tons;
      beam += cd.weapon_damage;
      missile += std::max(0.0, cd.missile_damage);
      pd += std::max(0.0, cd.point_defense_damage);
    }

    if (c.min_shields > 0.0) {
      const std::string best_shield = pick_best(pools.shields, content, [&](const ComponentDef& cd) {
        return cd.shield_hp;
      });
      while (!best_shield.empty() && can_add(best_shield) && shields + 1e-9 < c.min_shields) {
        const auto& cd = content.components.at(best_shield);
        comps.push_back(best_shield);
        mass += cd.mass_tons;
        shields += std::max(0.0, cd.shield_hp);
      }
    }
  }

  // HP: approximate with the same formula as design_stats (mass*2 + hp_bonus).
  if (c.min_hp > 0.0) {
    const std::string best_armor = pick_best(pools.armor, content, [&](const ComponentDef& cd) {
      return cd.hp_bonus;
    });
    while (!best_armor.empty() && can_add(best_armor) && (mass * 2.0 + hp_bonus) + 1e-9 < c.min_hp) {
      const auto& cd = content.components.at(best_armor);
      comps.push_back(best_armor);
      mass += cd.mass_tons;
      hp_bonus += cd.hp_bonus;
    }
  }

  // E-warfare.
  if (c.min_ecm_strength > 0.0) {
    const std::string best_ecm = pick_best(pools.sensors, content, [&](const ComponentDef& cd) {
      return cd.ecm_strength;
    });
    while (!best_ecm.empty() && can_add(best_ecm) && ecm + 1e-9 < c.min_ecm_strength) {
      const auto& cd = content.components.at(best_ecm);
      comps.push_back(best_ecm);
      mass += cd.mass_tons;
      ecm += std::max(0.0, cd.ecm_strength);
      eccm += std::max(0.0, cd.eccm_strength);
    }
  }
  if (c.min_eccm_strength > 0.0) {
    const std::string best_eccm = pick_best(pools.sensors, content, [&](const ComponentDef& cd) {
      return cd.eccm_strength;
    });
    while (!best_eccm.empty() && can_add(best_eccm) && eccm + 1e-9 < c.min_eccm_strength) {
      const auto& cd = content.components.at(best_eccm);
      comps.push_back(best_eccm);
      mass += cd.mass_tons;
      ecm += std::max(0.0, cd.ecm_strength);
      eccm += std::max(0.0, cd.eccm_strength);
    }
  }

  // Power balancing is handled by the main forge loop so constraints are
  // evaluated on the post-balance design.
}

std::string components_key(std::vector<std::string> comps) {
  std::sort(comps.begin(), comps.end());
  std::string k;
  for (const auto& c : comps) {
    k += c;
    k.push_back(';');
  }
  return k;
}

}  // namespace

std::vector<ForgedDesign> forge_design_variants(const ContentDB& content,
                                                const std::vector<std::string>& unlocked_components,
                                                const ShipDesign& base_design,
                                                std::uint64_t seed,
                                                const DesignForgeOptions& options,
                                                std::string* out_debug) {
  DesignForgeOptions opt = options;
  const ShipRole role = (opt.role == ShipRole::Unknown) ? base_design.role : opt.role;

  const Pools pools = build_pools(content, unlocked_components);
  if (pools.engines.empty() || pools.fuel.empty()) {
    if (out_debug) {
      *out_debug = "Not enough unlocked components to forge designs (need at least an engine and a fuel tank).";
    }
    return {};
  }

  const int desired = std::clamp(opt.desired_count, 1, 32);
  const int cand_mult = std::clamp(opt.candidate_multiplier, 1, 64);
  const int total_candidates = desired * cand_mult;

  std::vector<ForgedDesign> candidates;
  candidates.reserve((size_t)total_candidates);
  std::unordered_set<std::string> seen;
  seen.reserve((size_t)total_candidates * 2);

  const std::vector<std::string> base_comps = filtered_base_components(content, unlocked_components, base_design);

  for (int i = 0; i < total_candidates; ++i) {
    // Per-candidate deterministic seed.
    procgen_obscure::HashRng crng(procgen_obscure::splitmix64(seed ^ (std::uint64_t)i * 0x9e3779b97f4a7c15ULL));

    std::vector<std::string> comps = base_comps;
    if (comps.empty()) {
      // Start with a small baseline if the base design is not buildable for this component set.
      comps.reserve(8);
      comps.push_back(pick_best(pools.engines, content, [&](const ComponentDef& c) { return c.speed_km_s; }));
      comps.push_back(pick_best(pools.fuel, content, [&](const ComponentDef& c) { return c.fuel_capacity_tons; }));
    }

    ensure_role_minimums(content, pools, crng, comps, role, opt);
    mutate_components(content, pools, crng, comps, role, opt);
    ensure_role_minimums(content, pools, crng, comps, role, opt);
    tune_to_constraints(content, pools, crng, comps, role, opt);

    const double required_margin = opt.constraints.require_power_balance ? opt.constraints.min_power_margin : -0.25;
    try_balance_power(content, pools, crng, comps, opt.prefer_shields, opt.max_components, required_margin);

    // De-dup by component multiset.
    const std::string key = components_key(comps);
    if (!seen.insert(key).second) continue;

    ShipDesign d;
    d.role = role;
    d.components = std::move(comps);

    // Human-friendly (but stable) naming.
    const std::uint64_t suffix = procgen_obscure::splitmix64(seed ^ (std::uint64_t)i);
    d.id = opt.id_prefix + "_" + role_short(role) + "_" + procgen_obscure::hex_n(suffix, 8);
    d.name = opt.name_prefix + " " + role_short(role) + "-" + procgen_obscure::hex_n(suffix, 3);

    const auto res = derive_ship_design_stats(content, d);
    if (!res.ok) continue;

    const bool use_constraints = constraints_enabled(opt.constraints);
    const double base_score = score_for_role(d, role);
    const ConstraintEval ce = use_constraints ? eval_constraints(d, opt.constraints) : ConstraintEval{};
    const double s = base_score - ce.penalty;

    if (use_constraints && opt.only_meeting_constraints && !ce.meets) continue;

    ForgedDesign fd;
    fd.design = std::move(d);
    fd.score = s;
    fd.meets_constraints = use_constraints ? ce.meets : true;
    fd.constraint_penalty = use_constraints ? ce.penalty : 0.0;
    candidates.push_back(std::move(fd));
  }

  const bool use_constraints = constraints_enabled(opt.constraints);
  std::sort(candidates.begin(), candidates.end(), [&](const ForgedDesign& a, const ForgedDesign& b) {
    if (use_constraints && a.meets_constraints != b.meets_constraints) {
      return a.meets_constraints > b.meets_constraints;
    }
    return a.score > b.score;
  });

  if ((int)candidates.size() > desired) candidates.resize((size_t)desired);

  if (out_debug) {
    const int returned = (int)candidates.size();
    int meets = 0;
    for (const auto& cd : candidates) {
      if (cd.meets_constraints) meets += 1;
    }
    *out_debug = "Generated " + std::to_string(returned) + " / " + std::to_string(desired) + " designs from " +
                 std::to_string(total_candidates) + " candidates.";
    if (use_constraints) {
      *out_debug += " " + std::to_string(meets) + " meet constraints.";
    }
  }
  return candidates;
}

}  // namespace nebula4x
