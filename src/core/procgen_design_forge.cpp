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
                       bool prefer_shields) {
  // Add reactors to cover deficits, up to a small cap.
  for (int i = 0; i < 4; ++i) {
    if (power_margin(content, comps) >= -0.25) return;
    // Pick the highest output reactor (power per mass isn't modeled yet).
    const std::string best = pick_best(pools.reactors, content, [&](const ComponentDef& c) {
      return c.power_output;
    });
    if (best.empty()) break;
    comps.push_back(best);
  }

  // If still underpowered, shed optional power loads.
  for (int i = 0; i < 8; ++i) {
    if (power_margin(content, comps) >= -0.25) return;

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
      if ((int)comps.size() < 14) {
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
      comps.push_back(pick_random(rng, pools.cargo));
    }

    if (!pools.colony.empty() && rng.next_u01() < 0.20) comps.push_back(pick_random(rng, pools.colony));
    if (!pools.troops.empty() && rng.next_u01() < 0.15) comps.push_back(pick_random(rng, pools.troops));
    if (!pools.sensors.empty() && count_type(content, comps, ComponentType::Sensor) <= 0 && rng.next_u01() < 0.45) {
      comps.push_back(pick_random(rng, pools.sensors));
    }
  } else if (role == ShipRole::Surveyor) {
    const std::string best_sensor = pick_best(pools.sensors, content, [&](const ComponentDef& c) {
      return c.sensor_range_mkm;
    });
    ensure_type(content, comps, ComponentType::Sensor, best_sensor);

    // ECM/ECCM are sensor-typed in content; include occasionally.
    if (opt.include_ecm_eccm && !pools.sensors.empty() && rng.next_u01() < 0.30) comps.push_back(pick_random(rng, pools.sensors));
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
      comps.push_back(pick_random(rng, pools.weapons));
    }

    // Survivability.
    if (!pools.armor.empty() && count_type(content, comps, ComponentType::Armor) <= 0 && rng.next_u01() < 0.55) {
      comps.push_back(pick_random(rng, pools.armor));
    }
    if (opt.prefer_shields && !pools.shields.empty() && count_type(content, comps, ComponentType::Shield) <= 0 && rng.next_u01() < 0.60) {
      comps.push_back(pick_random(rng, pools.shields));
    }

    // E-warfare is useful for combatants too.
    if (opt.include_ecm_eccm && !pools.sensors.empty() && rng.next_u01() < 0.25) comps.push_back(pick_random(rng, pools.sensors));
  }

  // If we have power-hungry components and reactors exist, make sure we have at least one.
  if (!pools.reactors.empty()) {
    const double margin = power_margin(content, comps);
    if (margin < 0.0 && count_type(content, comps, ComponentType::Reactor) <= 0) {
      comps.push_back(pick_random(rng, pools.reactors));
    }
  }
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
    try_balance_power(content, pools, crng, comps, opt.prefer_shields);

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

    const double s = score_for_role(d, role);
    candidates.push_back(ForgedDesign{std::move(d), s});
  }

  std::sort(candidates.begin(), candidates.end(), [](const ForgedDesign& a, const ForgedDesign& b) {
    return a.score > b.score;
  });

  if ((int)candidates.size() > desired) candidates.resize((size_t)desired);

  if (out_debug) {
    *out_debug = "Generated " + std::to_string(candidates.size()) + " / " + std::to_string(desired) +
                 " designs from " + std::to_string(total_candidates) + " candidates.";
  }
  return candidates;
}

}  // namespace nebula4x
