#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/core/content_validation.h"
#include "nebula4x/core/design_stats.h"
#include "nebula4x/core/enum_strings.h"
#include "nebula4x/core/state_validation.h"
#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::compute_power_allocation;
} // namespace

void Simulation::apply_design_stats_to_ship(Ship& ship) {
  const ShipDesign* d = find_design(ship.design_id);
  if (!d) {
    ship.speed_km_s = 0.0;
    if (ship.hp <= 0.0) ship.hp = 1.0;
    ship.fuel_tons = 0.0;
    ship.shields = 0.0;
    return;
  }

  ship.speed_km_s = d->speed_km_s;
  if (ship.hp <= 0.0) ship.hp = d->max_hp;
  ship.hp = std::clamp(ship.hp, 0.0, d->max_hp);

  const double fuel_cap = std::max(0.0, d->fuel_capacity_tons);
  if (fuel_cap <= 1e-9) {
    ship.fuel_tons = 0.0;
  } else {
    // Initialize fuel for older saves / newly created ships.
    if (ship.fuel_tons < 0.0) ship.fuel_tons = fuel_cap;
    ship.fuel_tons = std::clamp(ship.fuel_tons, 0.0, fuel_cap);
  }

  const double max_sh = std::max(0.0, d->max_shields);
  if (max_sh <= 1e-9) {
    ship.shields = 0.0;
  } else {
    // Initialize shields for older saves / newly created ships.
    if (ship.shields < 0.0) ship.shields = max_sh;
    ship.shields = std::clamp(ship.shields, 0.0, max_sh);
  }

  // Missile ammo initialization / clamping (finite-ammo missile designs).
  const int ammo_cap = std::max(0, d->missile_ammo_capacity);
  if (ammo_cap <= 0) {
    // Unlimited ammo (legacy behavior) or no missile launchers.
    if (ship.missile_ammo < 0) ship.missile_ammo = 0;
    ship.missile_ammo = std::max(0, ship.missile_ammo);
  } else {
    if (ship.missile_ammo < 0) ship.missile_ammo = ammo_cap;
    ship.missile_ammo = std::clamp(ship.missile_ammo, 0, ammo_cap);
  }

  const double troop_cap = std::max(0.0, d->troop_capacity);
  if (troop_cap <= 1e-9) {
    ship.troops = 0.0;
  } else {
    ship.troops = std::clamp(ship.troops, 0.0, troop_cap);
  }

  const double colonist_cap = std::max(0.0, d->colony_capacity_millions);
  if (colonist_cap <= 1e-9) {
    ship.colonists_millions = 0.0;
  } else {
    ship.colonists_millions = std::clamp(ship.colonists_millions, 0.0, colonist_cap);
  }

  if (!std::isfinite(ship.maintenance_condition)) ship.maintenance_condition = 1.0;
  ship.maintenance_condition = std::clamp(ship.maintenance_condition, 0.0, 1.0);

  // Crew grade points initialization / clamping (older saves / newly created ships).
  if (!std::isfinite(ship.crew_grade_points) || ship.crew_grade_points < 0.0) {
    ship.crew_grade_points = cfg_.crew_initial_grade_points;
  }
  const double crew_cap = std::max(0.0, cfg_.crew_grade_points_cap);
  if (crew_cap > 0.0) ship.crew_grade_points = std::clamp(ship.crew_grade_points, 0.0, crew_cap);
  else ship.crew_grade_points = std::max(0.0, ship.crew_grade_points);

  // Crew complement initialization / clamping (older saves / newly created ships).
  if (!std::isfinite(ship.crew_complement) || ship.crew_complement < 0.0) {
    ship.crew_complement = 1.0;
  }
  ship.crew_complement = std::clamp(ship.crew_complement, 0.0, 1.0);
}

bool Simulation::upsert_custom_design(ShipDesign design, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (design.id.empty()) return fail("Design id is empty");
  if (content_.designs.find(design.id) != content_.designs.end()) {
    return fail("Design id conflicts with built-in design: " + design.id);
  }
  if (design.name.empty()) design.name = design.id;

  // Derive computed stats (mass/speed/range/power/weapons/etc) from components.
  const auto res = derive_ship_design_stats(content_, design);
  if (!res.ok) return fail(res.error);

  state_.custom_designs[design.id] = std::move(design);
  return true;
}

void Simulation::initialize_unlocks_for_faction(Faction& f) {
  for (Id cid : sorted_keys(state_.colonies)) {
    const auto& col = state_.colonies.at(cid);
    if (col.faction_id != f.id) continue;

    if (const auto* body = find_ptr(state_.bodies, col.body_id)) {
      push_unique(f.discovered_systems, body->system_id);
    }

    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      push_unique(f.unlocked_installations, inst_id);
    }
  }

  for (Id sid : sorted_keys(state_.ships)) {
    const auto& ship = state_.ships.at(sid);
    if (ship.faction_id != f.id) continue;

    push_unique(f.discovered_systems, ship.system_id);

    if (const auto* d = find_design(ship.design_id)) {
      for (const auto& cid : d->components) push_unique(f.unlocked_components, cid);
    }
  }

  for (const auto& tech_id : f.known_techs) {
    auto tit = content_.techs.find(tech_id);
    if (tit == content_.techs.end()) continue;
    for (const auto& eff : tit->second.effects) {
      if (eff.type == "unlock_component") push_unique(f.unlocked_components, eff.value);
      if (eff.type == "unlock_installation") push_unique(f.unlocked_installations, eff.value);
    }
  }

  // Backfill jump-point surveys for legacy saves (pre-save_version 40).
  // If the field is empty, assume all jump points in currently discovered systems
  // are known/surveyed to preserve existing behaviour.
  if (f.surveyed_jump_points.empty()) {
    for (Id sys_id : f.discovered_systems) {
      const auto* sys = find_ptr(state_.systems, sys_id);
      if (!sys) continue;
      for (Id jid : sys->jump_points) {
        if (jid == kInvalidId) continue;
        if (state_.jump_points.find(jid) == state_.jump_points.end()) continue;
        push_unique(f.surveyed_jump_points, jid);
      }
    }
  }
}

void Simulation::remove_ship_from_fleets(Id ship_id) {
  if (ship_id == kInvalidId) return;
  if (state_.fleets.empty()) return;

  bool changed = false;
  for (auto& [_, fl] : state_.fleets) {
    const auto it = std::remove(fl.ship_ids.begin(), fl.ship_ids.end(), ship_id);
    if (it != fl.ship_ids.end()) {
      fl.ship_ids.erase(it, fl.ship_ids.end());
      changed = true;
    }
    if (fl.leader_ship_id == ship_id) {
      fl.leader_ship_id = kInvalidId;
      changed = true;
    }
  }

  if (changed) prune_fleets();
}

void Simulation::prune_fleets() {
  if (state_.fleets.empty()) return;

  // Deterministic pruning.
  const auto fleet_ids = sorted_keys(state_.fleets);

  // Enforce the invariant that a ship may belong to at most one fleet.
  std::unordered_set<Id> claimed;
  claimed.reserve(state_.ships.size() * 2);

  for (Id fleet_id : fleet_ids) {
    auto* fl = find_ptr(state_.fleets, fleet_id);
    if (!fl) continue;

    std::vector<Id> members;
    members.reserve(fl->ship_ids.size());
    for (Id sid : fl->ship_ids) {
      if (sid == kInvalidId) continue;
      const auto* sh = find_ptr(state_.ships, sid);
      if (!sh) continue;
      if (fl->faction_id != kInvalidId && sh->faction_id != fl->faction_id) continue;
      members.push_back(sid);
    }

    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());

    std::vector<Id> unique_members;
    unique_members.reserve(members.size());
    for (Id sid : members) {
      if (claimed.insert(sid).second) unique_members.push_back(sid);
    }

    fl->ship_ids = std::move(unique_members);

    if (!fl->ship_ids.empty()) {
      if (fl->leader_ship_id == kInvalidId ||
          std::find(fl->ship_ids.begin(), fl->ship_ids.end(), fl->leader_ship_id) == fl->ship_ids.end()) {
        fl->leader_ship_id = fl->ship_ids.front();
      }
    } else {
      fl->leader_ship_id = kInvalidId;
    }
  }

  for (auto it = state_.fleets.begin(); it != state_.fleets.end();) {
    if (it->second.ship_ids.empty()) {
      it = state_.fleets.erase(it);
    } else {
      ++it;
    }
  }
}

void Simulation::discover_system_for_faction(Id faction_id, Id system_id) {
  if (system_id == kInvalidId) return;
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;

  if (std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), system_id) !=
      fac->discovered_systems.end()) {
    return;
  }

  fac->discovered_systems.push_back(system_id);
  invalidate_jump_route_cache();

  const auto* sys = find_ptr(state_.systems, system_id);
  const std::string sys_name = sys ? sys->name : std::string("(unknown)");

  EventContext ctx;
  ctx.faction_id = faction_id;
  ctx.system_id = system_id;

  const std::string msg = fac->name + " discovered system " + sys_name;
  push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);

  // Journal entry (curated narrative layer).
  {
    JournalEntry je;
    je.category = EventCategory::Exploration;
    je.system_id = system_id;
    je.title = "System Discovered: " + sys_name;

    std::ostringstream ss;
    if (sys) {
      const int bodies = (int)sys->bodies.size();
      const int jumps = (int)sys->jump_points.size();
      ss << "Initial survey complete.";
      ss << "\nBodies: " << bodies << "   Jump points: " << jumps;
      ss.setf(std::ios::fixed);
      ss.precision(2);
      ss << "\nNebula density: " << std::clamp(sys->nebula_density, 0.0, 1.0);

      if (sys->region_id != kInvalidId) {
        if (const auto* reg = find_ptr(state_.regions, sys->region_id)) {
          if (!reg->name.empty()) ss << "\nRegion: " << reg->name;
          if (!reg->theme.empty()) ss << " (" << reg->theme << ")";
        }
      }

      const int now = (int)state_.date.days_since_epoch();
      if (sys->storm_peak_intensity > 1e-9 && sys->storm_start_day <= now && now < sys->storm_end_day) {
        ss << "\nNebula storm active.";
      }
    } else {
      ss << "Initial survey complete.";
    }
    je.text = ss.str();
    push_journal_entry(faction_id, std::move(je));
  }

  // Share the discovery with mutual-friendly factions.
  const auto faction_ids = sorted_keys(state_.factions);
  for (Id other_id : faction_ids) {
    if (other_id == faction_id) continue;
    if (!are_factions_mutual_friendly(faction_id, other_id)) continue;
    auto* other = find_ptr(state_.factions, other_id);
    if (!other) continue;

    if (std::find(other->discovered_systems.begin(), other->discovered_systems.end(), system_id) !=
        other->discovered_systems.end()) {
      continue;
    }

    other->discovered_systems.push_back(system_id);

    EventContext ctx2;
    ctx2.faction_id = other_id;
    ctx2.faction_id2 = faction_id;
    ctx2.system_id = system_id;

    const std::string msg2 = "Intel: " + fac->name + " shared discovery of system " + sys_name;
    push_event(EventLevel::Info, EventCategory::Intel, msg2, ctx2);

    // Journal entry for the receiving faction.
    {
      JournalEntry je;
      je.category = EventCategory::Intel;
      je.system_id = system_id;
      je.title = "Intel: New System " + sys_name;
      je.text = "Shared by " + fac->name + ".";
      push_journal_entry(other_id, std::move(je));
    }
  }

}

void Simulation::reveal_route_intel_for_faction(Id faction_id,
                                               const std::vector<Id>& systems,
                                               const std::vector<Id>& jump_points) {
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;

  bool changed = false;

  for (Id sid : systems) {
    if (sid == kInvalidId) continue;
    if (!find_ptr(state_.systems, sid)) continue;
    if (std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), sid) != fac->discovered_systems.end())
      continue;
    fac->discovered_systems.push_back(sid);
    changed = true;
  }

  for (Id jid : jump_points) {
    if (jid == kInvalidId) continue;
    if (!find_ptr(state_.jump_points, jid)) continue;
    if (std::find(fac->surveyed_jump_points.begin(), fac->surveyed_jump_points.end(), jid) !=
        fac->surveyed_jump_points.end())
      continue;
    fac->surveyed_jump_points.push_back(jid);
    // If we were mid-survey, the intel overrides it.
    fac->jump_survey_progress.erase(jid);
    changed = true;
  }

  if (changed) invalidate_jump_route_cache();
}



void Simulation::discover_anomaly_for_faction(Id faction_id, Id anomaly_id, Id discovered_by_ship_id) {
  if (anomaly_id == kInvalidId) return;
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;

  const auto* anom = find_ptr(state_.anomalies, anomaly_id);
  if (!anom) return;

  if (std::find(fac->discovered_anomalies.begin(), fac->discovered_anomalies.end(), anomaly_id) !=
      fac->discovered_anomalies.end()) {
    return;
  }

  fac->discovered_anomalies.push_back(anomaly_id);

  const auto* sys = find_ptr(state_.systems, anom->system_id);
  const std::string sys_name = sys ? sys->name : std::string("(unknown)");
  const std::string anom_name = !anom->name.empty() ? anom->name : std::string("(unnamed anomaly)");

  EventContext ctx;
  ctx.faction_id = faction_id;
  ctx.system_id = anom->system_id;
  ctx.ship_id = discovered_by_ship_id;

  const std::string msg = fac->name + " detected anomaly " + anom_name + " in " + sys_name;
  push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);

  // Journal entry.
  {
    JournalEntry je;
    je.category = EventCategory::Exploration;
    je.system_id = anom->system_id;
    je.ship_id = discovered_by_ship_id;
    je.anomaly_id = anomaly_id;
    je.title = "Anomaly Detected: " + anom_name;

    std::ostringstream ss;
    ss << "System: " << sys_name;
    if (anom->kind != AnomalyKind::Unknown) ss << "\nKind: " << anomaly_kind_label(anom->kind);
    ss << "\nInvestigation: " << std::max(1, anom->investigation_days) << " day(s) on-station";
    if (anom->research_reward > 1e-9) {
      ss.setf(std::ios::fixed);
      ss.precision(1);
      ss << "\nPotential reward: +" << anom->research_reward << " RP";
    }
    if (!anom->unlock_component_id.empty()) {
      ss << "\nPotential unlock: " << anom->unlock_component_id;
    }
    if (!anom->mineral_reward.empty()) {
      double total = 0.0;
      for (const auto& [_, t] : anom->mineral_reward) total += std::max(0.0, t);
      if (total > 1e-3) {
        ss.setf(std::ios::fixed);
        ss.precision(1);
        ss << "\nPotential cache: " << total << "t minerals";
      }
    }
    if (anom->hazard_chance > 1e-9 && anom->hazard_damage > 1e-9) {
      ss.setf(std::ios::fixed);
      ss.precision(0);
      ss << "\nHazard risk: " << std::clamp(anom->hazard_chance, 0.0, 1.0) * 100.0 << "%";
    }

    // Procedural "fingerprint" + flavor line for uniqueness.
    {
      const auto* reg = (sys && sys->region_id != kInvalidId) ? find_ptr(state_.regions, sys->region_id) : nullptr;
      const double neb = sys ? std::clamp(sys->nebula_density, 0.0, 1.0) : 0.0;
      const double ruins = reg ? std::clamp(reg->ruins_density, 0.0, 1.0) : 0.0;
      const double pir = reg ? std::clamp(reg->pirate_risk * (1.0 - reg->pirate_suppression), 0.0, 1.0) : 0.0;

      const std::string sig = procgen_obscure::anomaly_signature_code(*anom);
      ss << "\nSignature: " << sig;
      ss << "\n" << procgen_obscure::anomaly_signature_glyph(*anom);
      ss << "\n\n" << procgen_obscure::anomaly_lore_line(*anom, neb, ruins, pir);
    }

    je.text = ss.str();
    push_journal_entry(faction_id, std::move(je));
  }

  // Share anomaly intel with mutual-friendly factions.
  const auto faction_ids = sorted_keys(state_.factions);
  for (Id other_id : faction_ids) {
    if (other_id == faction_id) continue;
    if (!are_factions_mutual_friendly(faction_id, other_id)) continue;
    auto* other = find_ptr(state_.factions, other_id);
    if (!other) continue;

    if (std::find(other->discovered_anomalies.begin(), other->discovered_anomalies.end(), anomaly_id) !=
        other->discovered_anomalies.end()) {
      continue;
    }

    other->discovered_anomalies.push_back(anomaly_id);

    EventContext ctx2;
    ctx2.faction_id = other_id;
    ctx2.faction_id2 = faction_id;
    ctx2.system_id = anom->system_id;

    const std::string msg2 = "Intel: " + fac->name + " shared anomaly location " + anom_name + " in " + sys_name;
    push_event(EventLevel::Info, EventCategory::Intel, msg2, ctx2);

    {
      JournalEntry je;
      je.category = EventCategory::Intel;
      je.system_id = anom->system_id;
      je.anomaly_id = anomaly_id;
      je.title = "Intel: Anomaly Located";
      je.text = fac->name + " reported anomaly '" + anom_name + "' in " + sys_name + ".";
      push_journal_entry(other_id, std::move(je));
    }
  }
}

void Simulation::survey_jump_point_for_faction(Id faction_id, Id jump_point_id) {
  if (jump_point_id == kInvalidId) return;
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;

  const auto* jp = find_ptr(state_.jump_points, jump_point_id);
  if (!jp) return;

  auto add_survey = [&](Faction& f, Id jid) -> bool {
    if (jid == kInvalidId) return false;
    if (std::find(f.surveyed_jump_points.begin(), f.surveyed_jump_points.end(), jid) !=
        f.surveyed_jump_points.end()) {
      // If we already know this jump, discard any stale partial progress.
      f.jump_survey_progress.erase(jid);
      return false;
    }
    f.surveyed_jump_points.push_back(jid);
    // Survey completed: clear any partial progress entry.
    f.jump_survey_progress.erase(jid);
    return true;
  };

  if (!add_survey(*fac, jump_point_id)) return;

  invalidate_jump_route_cache();

  const std::string jp_name = !jp->name.empty() ? jp->name : std::string("Jump Point");
  std::string dest_name = "(unknown)";
  if (jp->linked_jump_id != kInvalidId) {
    if (const auto* lnk = find_ptr(state_.jump_points, jp->linked_jump_id)) {
      if (const auto* dst_sys = find_ptr(state_.systems, lnk->system_id)) {
        if (!dst_sys->name.empty()) dest_name = dst_sys->name;
      }
    }
  }

  EventContext ctx;
  ctx.faction_id = faction_id;
  ctx.system_id = jp->system_id;

  const std::string msg = fac->name + " surveyed jump point " + jp_name + " -> " + dest_name;
  push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);

  // Journal entry.
  {
    JournalEntry je;
    je.category = EventCategory::Exploration;
    je.system_id = jp->system_id;
    je.title = "Jump Surveyed: " + jp_name + " -> " + dest_name;
    je.text = "Route confirmed for navigation planners.";
    push_journal_entry(faction_id, std::move(je));
  }

  // Share the survey with mutual-friendly factions.
  const auto faction_ids = sorted_keys(state_.factions);
  for (Id other_id : faction_ids) {
    if (other_id == faction_id) continue;
    if (!are_factions_mutual_friendly(faction_id, other_id)) continue;
    auto* other = find_ptr(state_.factions, other_id);
    if (!other) continue;

    if (!add_survey(*other, jump_point_id)) continue;

    EventContext ctx2;
    ctx2.faction_id = other_id;
    ctx2.faction_id2 = faction_id;
    ctx2.system_id = jp->system_id;

    const std::string msg2 = "Intel: " + fac->name + " shared jump survey of " + jp_name + " -> " + dest_name;
    push_event(EventLevel::Info, EventCategory::Intel, msg2, ctx2);

    {
      JournalEntry je;
      je.category = EventCategory::Intel;
      je.system_id = jp->system_id;
      je.title = "Intel: Jump Survey Shared";
      je.text = fac->name + " shared survey: " + jp_name + " -> " + dest_name;
      push_journal_entry(other_id, std::move(je));
    }
  }
}

void Simulation::new_game() {
  state_ = make_sol_scenario();
  ++state_generation_;
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);

    // heat_state is not serialized (it is a runtime bucket used to throttle
    // repeated warnings). Initialize it from the current heat fraction so
    // loading a save doesn't re-emit heat threshold events.
    if (!std::isfinite(ship.heat) || ship.heat < 0.0) ship.heat = 0.0;

    if (!cfg_.enable_ship_heat) {
      ship.heat_state = 0;
    } else {
      const ShipDesign* d = find_design(ship.design_id);
      if (!d) {
        ship.heat_state = 0;
      } else {
        const double cap =
            std::max(0.0, cfg_.ship_heat_base_capacity_per_mass_ton) * std::max(0.0, d->mass_tons) +
            std::max(0.0, d->heat_capacity_bonus);
        if (cap <= 1e-9) {
          ship.heat_state = 0;
          ship.heat = 0.0;
        } else {
          const double frac = std::clamp(ship.heat / cap, 0.0, 10.0);
          std::uint8_t st = 0;
          if (frac >= cfg_.ship_heat_damage_threshold_fraction) st = 3;
          else if (frac >= cfg_.ship_heat_penalty_full_fraction) st = 2;
          else if (frac >= cfg_.ship_heat_penalty_start_fraction) st = 1;
          ship.heat_state = st;
        }
      }
    }
  }
  for (auto& [_, f] : state_.factions) initialize_unlocks_for_faction(f);
  recompute_body_positions();
  tick_contacts(0.0, false);
  invalidate_jump_route_cache();

  // Seed initial procedural contract offers so the mission board is not empty
  // on a fresh start.
  tick_contracts();
  tick_score_history(/*force=*/true);
}

void Simulation::new_game_random(std::uint32_t seed, int num_systems) {
  state_ = make_random_scenario(seed, num_systems);
  ++state_generation_;
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);

    // heat_state is not serialized (it is a runtime bucket used to throttle
    // repeated warnings). Initialize it from the current heat fraction so
    // loading a save doesn't re-emit heat threshold events.
    if (!std::isfinite(ship.heat) || ship.heat < 0.0) ship.heat = 0.0;

    if (!cfg_.enable_ship_heat) {
      ship.heat_state = 0;
    } else {
      const ShipDesign* d = find_design(ship.design_id);
      if (!d) {
        ship.heat_state = 0;
      } else {
        const double cap =
            std::max(0.0, cfg_.ship_heat_base_capacity_per_mass_ton) * std::max(0.0, d->mass_tons) +
            std::max(0.0, d->heat_capacity_bonus);
        if (cap <= 1e-9) {
          ship.heat_state = 0;
          ship.heat = 0.0;
        } else {
          const double frac = std::clamp(ship.heat / cap, 0.0, 10.0);
          std::uint8_t st = 0;
          if (frac >= cfg_.ship_heat_damage_threshold_fraction) st = 3;
          else if (frac >= cfg_.ship_heat_penalty_full_fraction) st = 2;
          else if (frac >= cfg_.ship_heat_penalty_start_fraction) st = 1;
          ship.heat_state = st;
        }
      }
    }
  }
  for (auto& [_, f] : state_.factions) initialize_unlocks_for_faction(f);
  recompute_body_positions();
  tick_contacts(0.0, false);
  invalidate_jump_route_cache();

  // Seed initial procedural contract offers so the mission board is not empty
  // on a fresh start.
  tick_contracts();
  tick_score_history(/*force=*/true);
}

void Simulation::load_game(GameState loaded) {
  state_ = std::move(loaded);
  ++state_generation_;

  // Ensure id allocation cannot collide with ids already present in loaded state.
  // This protects manually-constructed/legacy saves that forgot to bump next_id.
  {
    Id max_id = 0;
    auto bump = [&](Id id) {
      if (id != kInvalidId && id > max_id) max_id = id;
    };
    auto bump_keys = [&](const auto& m) {
      for (const auto& [id, _] : m) bump(id);
    };

    bump_keys(state_.systems);
    bump_keys(state_.regions);
    bump_keys(state_.bodies);
    bump_keys(state_.jump_points);
    bump_keys(state_.ships);
    bump_keys(state_.wrecks);
    bump_keys(state_.anomalies);
    bump_keys(state_.contracts);
    bump_keys(state_.missile_salvos);
    bump_keys(state_.colonies);
    bump_keys(state_.factions);
    bump_keys(state_.treaties);
    bump_keys(state_.diplomatic_offers);
    bump_keys(state_.fleets);

    if (state_.next_id == kInvalidId || state_.next_id <= max_id) {
      state_.next_id = max_id + 1;
      if (state_.next_id == kInvalidId) ++state_.next_id;
    }
  }

  {
    std::uint64_t max_seq = 0;
    for (const auto& ev : state_.events) max_seq = std::max(max_seq, ev.seq);
    if (state_.next_event_seq == 0) state_.next_event_seq = 1;
    if (state_.next_event_seq <= max_seq) state_.next_event_seq = max_seq + 1;
  }

  {
    std::uint64_t max_seq = 0;
    for (const auto& [_, fac] : state_.factions) {
      for (const auto& je : fac.journal) max_seq = std::max(max_seq, je.seq);
    }
    if (state_.next_journal_seq == 0) state_.next_journal_seq = 1;
    if (state_.next_journal_seq <= max_seq) state_.next_journal_seq = max_seq + 1;
  }

  if (!state_.custom_designs.empty()) {
    std::vector<ShipDesign> designs;
    designs.reserve(state_.custom_designs.size());
    for (const auto& [_, d] : state_.custom_designs) designs.push_back(d);
    state_.custom_designs.clear();
    for (auto& d : designs) {
      std::string err;
      if (!upsert_custom_design(d, &err)) {
        nebula4x::log::warn(std::string("Custom design '") + d.id + "' could not be re-derived: " + err);
        state_.custom_designs[d.id] = d; 
      }
    }
  }

  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);

    // heat_state is not serialized (it is a runtime bucket used to throttle
    // repeated warnings). Initialize it from the current heat fraction so
    // loading a save doesn't re-emit heat threshold events.
    if (!std::isfinite(ship.heat) || ship.heat < 0.0) ship.heat = 0.0;

    if (!cfg_.enable_ship_heat) {
      ship.heat_state = 0;
    } else {
      const ShipDesign* d = find_design(ship.design_id);
      if (!d) {
        ship.heat_state = 0;
      } else {
        const double cap =
            std::max(0.0, cfg_.ship_heat_base_capacity_per_mass_ton) * std::max(0.0, d->mass_tons) +
            std::max(0.0, d->heat_capacity_bonus);
        if (cap <= 1e-9) {
          ship.heat_state = 0;
          ship.heat = 0.0;
        } else {
          const double frac = std::clamp(ship.heat / cap, 0.0, 10.0);
          std::uint8_t st = 0;
          if (frac >= cfg_.ship_heat_damage_threshold_fraction) st = 3;
          else if (frac >= cfg_.ship_heat_penalty_full_fraction) st = 2;
          else if (frac >= cfg_.ship_heat_penalty_start_fraction) st = 1;
          ship.heat_state = st;
        }
      }
    }
  }

  for (auto& [_, f] : state_.factions) {
    initialize_unlocks_for_faction(f);
  }

  // Older saves (or hand-edited JSON) may contain stale fleet references.
  // Clean them up on load.
  prune_fleets();

  recompute_body_positions();
  tick_contacts(0.0, false);
  invalidate_jump_route_cache();

  // If the user enabled score history on a legacy save, seed one snapshot so
  // the Victory window can immediately render a trend line.
  if (state_.victory_rules.score_history_enabled && state_.score_history.empty()) {
    tick_score_history(/*force=*/true);
  }
}


ReloadContentResult Simulation::reload_content_db(ContentDB new_content, bool validate_state) {
  ReloadContentResult result;

  // Preserve source paths if the caller didn't set them.
  if (new_content.content_source_paths.empty()) new_content.content_source_paths = content_.content_source_paths;
  if (new_content.tech_source_paths.empty()) new_content.tech_source_paths = content_.tech_source_paths;

  const auto errors = nebula4x::validate_content_db(new_content);
  if (!errors.empty()) {
    result.ok = false;
    result.errors = errors;

    nebula4x::log::error("Content hot reload failed: content validation errors (" + std::to_string(errors.size()) + ")");
    for (const auto& e : errors) nebula4x::log::error("  - " + e);

    push_event(EventLevel::Error, EventCategory::General,
               "Hot Reload: content validation failed (" + std::to_string(errors.size()) + " errors)");
    return result;
  }

  // Apply new content.
  content_ = std::move(new_content);
  ++content_generation_;

  // Re-derive custom designs against the updated component database.
  if (!state_.custom_designs.empty()) {
    std::vector<ShipDesign> designs;
    designs.reserve(state_.custom_designs.size());
    for (const auto& [_, d] : state_.custom_designs) designs.push_back(d);

    state_.custom_designs.clear();

    for (auto& d : designs) {
      std::string err;
      if (!upsert_custom_design(d, &err)) {
        // Preserve the (possibly stale) derived stats embedded in the save.
        // This mirrors load_game() behaviour and avoids deleting user designs.
        result.custom_designs_failed += 1;
        const std::string msg = std::string("Custom design '") + d.id + "' could not be re-derived: " + err;
        result.warnings.push_back(msg);
        nebula4x::log::warn(msg);
        state_.custom_designs[d.id] = d;
      } else {
        result.custom_designs_updated += 1;
      }
    }
  }

  // Refresh cached ship stats (speed, etc.).
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
    result.ships_updated += 1;
  }

  // Rebuild faction unlock lists (prune stale/unknown ids).
  for (auto& [_, fac] : state_.factions) {
    fac.unlocked_components.clear();
    fac.unlocked_installations.clear();
    initialize_unlocks_for_faction(fac);
    result.factions_rebuilt += 1;
  }

  // Sensors / contacts depend on design sensor ranges and installation defs.
  tick_contacts(0.0, false);

  if (validate_state) {
    const auto s_errors = nebula4x::validate_game_state(state_, &content_);
    if (!s_errors.empty()) {
      // Don't fail the reload; surface as warnings so modders can iterate.
      constexpr std::size_t kCap = 25;
      for (std::size_t i = 0; i < s_errors.size() && i < kCap; ++i) {
        result.warnings.push_back(std::string("State validation: ") + s_errors[i]);
      }
      if (s_errors.size() > kCap) {
        result.warnings.push_back("State validation: ... (" + std::to_string(s_errors.size() - kCap) + " more)");
      }

      nebula4x::log::warn("Content hot reload applied, but game state validation reported " +
                        std::to_string(s_errors.size()) + " issue(s)");
    }
  }

  result.ok = true;

  std::string cd_part = std::to_string(result.custom_designs_updated) + " ok";
  if (result.custom_designs_failed) {
    cd_part += ", " + std::to_string(result.custom_designs_failed) + " failed";
  }

  const std::string summary =
      "Hot Reload: applied content bundle (ships=" + std::to_string(result.ships_updated) +
      ", factions=" + std::to_string(result.factions_rebuilt) +
      ", custom designs=" + cd_part +
      ", warnings=" + std::to_string(result.warnings.size()) + ")";

  push_event(result.warnings.empty() ? EventLevel::Info : EventLevel::Warn, EventCategory::General, summary);

  return result;
}

void Simulation::advance_days(int days) {
  if (days <= 0) return;
  advance_hours(days * 24);
}

void Simulation::advance_hours(int hours) {
  if (hours <= 0) return;

  int remaining = hours;
  while (remaining > 0) {
    const int hod = std::clamp(state_.hour_of_day, 0, 23);
    const int until_midnight = 24 - hod;
    const int step = std::clamp(std::min(remaining, until_midnight), 1, 24);
    tick_one_tick_hours(step);
    remaining -= step;
  }
}

namespace {

bool event_matches_stop(const SimEvent& ev, const EventStopCondition& stop) {
  const bool level_ok = (ev.level == EventLevel::Info && stop.stop_on_info) ||
                        (ev.level == EventLevel::Warn && stop.stop_on_warn) ||
                        (ev.level == EventLevel::Error && stop.stop_on_error);
  if (!level_ok) return false;

  if (stop.filter_category && ev.category != stop.category) return false;

  if (stop.faction_id != kInvalidId) {
    if (ev.faction_id != stop.faction_id && ev.faction_id2 != stop.faction_id) return false;
  }

  if (stop.system_id != kInvalidId) {
    if (ev.system_id != stop.system_id) return false;
  }

  if (stop.ship_id != kInvalidId) {
    if (ev.ship_id != stop.ship_id) return false;
  }

  if (stop.colony_id != kInvalidId) {
    if (ev.colony_id != stop.colony_id) return false;
  }

  if (!stop.message_contains.empty()) {
    const auto it = std::search(
        ev.message.begin(), ev.message.end(),
        stop.message_contains.begin(), stop.message_contains.end(),
        [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    if (it == ev.message.end()) return false;
  }

  return true;
}

} // namespace

AdvanceUntilEventResult Simulation::advance_until_event(int max_days, const EventStopCondition& stop) {
  // Preserve the existing day-oriented API, but implement it on top of the
  // hour-stepped variant.
  const int max_hours = (max_days <= 0) ? 0 : max_days * 24;
  return advance_until_event_hours(max_hours, stop, /*step_hours=*/24);
}

AdvanceUntilEventResult Simulation::advance_until_event_hours(int max_hours, const EventStopCondition& stop, int step_hours) {
  AdvanceUntilEventResult out;
  if (max_hours <= 0) return out;

  step_hours = std::clamp(step_hours, 1, 24);

  std::uint64_t last_seq = 0;
  if (state_.next_event_seq > 0) last_seq = state_.next_event_seq - 1;

  int remaining = max_hours;
  while (remaining > 0) {
    // Don't allow a single step to cross midnight so that we can stop on
    // events precisely at a boundary and keep hour-of-day stamps intuitive.
    const int hod = std::clamp(state_.hour_of_day, 0, 23);
    const int until_midnight = 24 - hod;
    const int step = std::min({remaining, step_hours, until_midnight});

    const std::int64_t day_before = state_.date.days_since_epoch();
    tick_one_tick_hours(step);
    const std::int64_t day_after = state_.date.days_since_epoch();

    out.hours_advanced += step;
    if (day_after != day_before) {
      // In practice, this should be at most 1 due to the no-midnight-crossing
      // constraint above, but keep it robust.
      out.days_advanced += static_cast<int>(day_after - day_before);
    }

    const std::uint64_t newest_seq = (state_.next_event_seq > 0) ? (state_.next_event_seq - 1) : 0;
    if (newest_seq > last_seq) {
      for (int j = static_cast<int>(state_.events.size()) - 1; j >= 0; --j) {
        const auto& ev = state_.events[static_cast<std::size_t>(j)];
        if (ev.seq <= last_seq) break;
        if (!event_matches_stop(ev, stop)) continue;
        out.hit = true;
        out.event = ev; // copy
        return out;
      }

      last_seq = newest_seq;
    }

    remaining -= step;
  }

  return out;
}


} // namespace nebula4x
