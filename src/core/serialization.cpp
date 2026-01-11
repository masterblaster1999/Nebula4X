#include "nebula4x/core/serialization.h"

#include "nebula4x/core/enum_strings.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"

namespace nebula4x {
namespace {

using json::Array;
using json::Object;
using json::Value;

constexpr int kCurrentSaveVersion = 50;

Object ship_power_policy_to_json(const ShipPowerPolicy& p) {
  Object o;
  o["engines_enabled"] = p.engines_enabled;
  o["shields_enabled"] = p.shields_enabled;
  o["weapons_enabled"] = p.weapons_enabled;
  o["sensors_enabled"] = p.sensors_enabled;
  Array pr;
  pr.reserve(p.priority.size());
  for (PowerSubsystem s : p.priority) {
    pr.push_back(power_subsystem_to_string(s));
  }
  o["priority"] = pr;
  return o;
}

ShipPowerPolicy ship_power_policy_from_json(const Value& v) {
  ShipPowerPolicy p;
  if (!v.is_object()) return p;
  const Object& o = v.object();
  if (auto it = o.find("engines_enabled"); it != o.end()) p.engines_enabled = it->second.bool_value(true);
  if (auto it = o.find("shields_enabled"); it != o.end()) p.shields_enabled = it->second.bool_value(true);
  if (auto it = o.find("weapons_enabled"); it != o.end()) p.weapons_enabled = it->second.bool_value(true);
  if (auto it = o.find("sensors_enabled"); it != o.end()) p.sensors_enabled = it->second.bool_value(true);

  if (auto it = o.find("priority"); it != o.end()) {
    std::array<PowerSubsystem, 4> prio = p.priority;
    std::size_t n = 0;
    for (const auto& pv : it->second.array()) {
      if (n >= prio.size()) break;
      prio[n++] = power_subsystem_from_string(pv.string_value());
    }
    p.priority = prio;
  }

  sanitize_power_policy(p);
  return p;
}

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

template <typename T>
std::vector<T> sorted_unique_copy(const std::vector<T>& v) {
  std::vector<T> out = v;
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Value vec2_to_json(const Vec2& v) {
  Object o;
  o["x"] = v.x;
  o["y"] = v.y;
  return o;
}

Vec2 vec2_from_json(const Value& v) {
  const auto& o = v.object();
  return Vec2{o.at("x").number_value(), o.at("y").number_value()};
}



std::string ship_role_to_string(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "freighter";
    case ShipRole::Surveyor: return "surveyor";
    case ShipRole::Combatant: return "combatant";
    default: return "unknown";
  }
}

ShipRole ship_role_from_string(const std::string& s) {
  if (s == "freighter") return ShipRole::Freighter;
  if (s == "surveyor") return ShipRole::Surveyor;
  if (s == "combatant") return ShipRole::Combatant;
  return ShipRole::Unknown;
}


std::string faction_control_to_string(FactionControl c) {
  switch (c) {
    case FactionControl::Player: return "player";
    case FactionControl::AI_Passive: return "ai_passive";
    case FactionControl::AI_Explorer: return "ai_explorer";
    case FactionControl::AI_Pirate: return "ai_pirate";
  }
  return "player";
}

FactionControl faction_control_from_string(const std::string& s) {
  if (s == "ai_passive" || s == "passive") return FactionControl::AI_Passive;
  if (s == "ai_explorer" || s == "explorer") return FactionControl::AI_Explorer;
  if (s == "ai_pirate" || s == "pirate") return FactionControl::AI_Pirate;
  return FactionControl::Player;
}

std::string diplomacy_status_to_string(DiplomacyStatus s) {
  switch (s) {
    case DiplomacyStatus::Friendly: return "friendly";
    case DiplomacyStatus::Neutral: return "neutral";
    case DiplomacyStatus::Hostile: return "hostile";
  }
  return "hostile";
}

DiplomacyStatus diplomacy_status_from_string(const std::string& s) {
  if (s == "friendly" || s == "friend" || s == "ally" || s == "allied") return DiplomacyStatus::Friendly;
  if (s == "neutral" || s == "non_hostile") return DiplomacyStatus::Neutral;
  if (s == "hostile" || s == "enemy") return DiplomacyStatus::Hostile;
  // Backward compat / safe default: unknown strings are treated as hostile.
  return DiplomacyStatus::Hostile;
}



std::string treaty_type_to_string(TreatyType t) {
  switch (t) {
    case TreatyType::Ceasefire: return "ceasefire";
    case TreatyType::NonAggressionPact: return "non_aggression";
    case TreatyType::Alliance: return "alliance";
    case TreatyType::TradeAgreement: return "trade";
  }
  return "ceasefire";
}

TreatyType treaty_type_from_string(const std::string& s) {
  if (s == "ceasefire" || s == "cease" || s == "truce") return TreatyType::Ceasefire;
  if (s == "non_aggression" || s == "nonaggression" || s == "nap" || s == "pact") return TreatyType::NonAggressionPact;
  if (s == "alliance" || s == "ally" || s == "allied") return TreatyType::Alliance;
  if (s == "trade" || s == "trade_agreement" || s == "commerce") return TreatyType::TradeAgreement;
  return TreatyType::Ceasefire;
}

const char* victory_reason_to_string(VictoryReason r) {
  switch (r) {
    case VictoryReason::None:
      return "none";
    case VictoryReason::ScoreThreshold:
      return "score";
    case VictoryReason::LastFactionStanding:
      return "elimination";
  }
  return "none";
}

VictoryReason victory_reason_from_string(const std::string& s) {
  if (s == "score" || s == "score_threshold" || s == "points") return VictoryReason::ScoreThreshold;
  if (s == "elimination" || s == "last_faction" || s == "last_faction_standing" || s == "last" || s == "conquest") {
    return VictoryReason::LastFactionStanding;
  }
  return VictoryReason::None;
}

double sane_nonneg(double v, double fallback = 0.0) {
  if (!std::isfinite(v)) return fallback;
  return v < 0.0 ? 0.0 : v;
}

Object victory_rules_to_json(const VictoryRules& r) {
  Object o;
  o["enabled"] = r.enabled;
  o["exclude_pirates"] = r.exclude_pirates;
  o["elimination_enabled"] = r.elimination_enabled;
  o["elimination_requires_colony"] = r.elimination_requires_colony;
  o["score_threshold"] = r.score_threshold;
  o["score_lead_margin"] = r.score_lead_margin;

  Object w;
  w["colony_points"] = r.score_colony_points;
  w["population_per_million"] = r.score_population_per_million;
  w["installation_cost_mult"] = r.score_installation_cost_mult;
  w["ship_mass_ton_mult"] = r.score_ship_mass_ton_mult;
  w["known_tech_points"] = r.score_known_tech_points;
  w["discovered_system_points"] = r.score_discovered_system_points;
  w["discovered_anomaly_points"] = r.score_discovered_anomaly_points;
  o["weights"] = w;

  return o;
}

VictoryRules victory_rules_from_json(const Value& v) {
  VictoryRules r;
  if (!v.is_object()) return r;
  const Object& o = v.object();
  if (auto it = o.find("enabled"); it != o.end()) r.enabled = it->second.bool_value(r.enabled);
  if (auto it = o.find("exclude_pirates"); it != o.end()) r.exclude_pirates = it->second.bool_value(r.exclude_pirates);
  if (auto it = o.find("elimination_enabled"); it != o.end()) r.elimination_enabled = it->second.bool_value(r.elimination_enabled);
  if (auto it = o.find("elimination_requires_colony"); it != o.end()) {
    r.elimination_requires_colony = it->second.bool_value(r.elimination_requires_colony);
  }
  if (auto it = o.find("score_threshold"); it != o.end()) r.score_threshold = sane_nonneg(it->second.number_value(r.score_threshold), r.score_threshold);
  if (auto it = o.find("score_lead_margin"); it != o.end()) r.score_lead_margin = sane_nonneg(it->second.number_value(r.score_lead_margin), r.score_lead_margin);

  if (auto itw = o.find("weights"); itw != o.end() && itw->second.is_object()) {
    const Object& w = itw->second.object();
    if (auto it = w.find("colony_points"); it != w.end()) r.score_colony_points = sane_nonneg(it->second.number_value(r.score_colony_points), r.score_colony_points);
    if (auto it = w.find("population_per_million"); it != w.end()) r.score_population_per_million = sane_nonneg(it->second.number_value(r.score_population_per_million), r.score_population_per_million);
    if (auto it = w.find("installation_cost_mult"); it != w.end()) r.score_installation_cost_mult = sane_nonneg(it->second.number_value(r.score_installation_cost_mult), r.score_installation_cost_mult);
    if (auto it = w.find("ship_mass_ton_mult"); it != w.end()) r.score_ship_mass_ton_mult = sane_nonneg(it->second.number_value(r.score_ship_mass_ton_mult), r.score_ship_mass_ton_mult);
    if (auto it = w.find("known_tech_points"); it != w.end()) r.score_known_tech_points = sane_nonneg(it->second.number_value(r.score_known_tech_points), r.score_known_tech_points);
    if (auto it = w.find("discovered_system_points"); it != w.end()) r.score_discovered_system_points = sane_nonneg(it->second.number_value(r.score_discovered_system_points), r.score_discovered_system_points);
    if (auto it = w.find("discovered_anomaly_points"); it != w.end()) {
      r.score_discovered_anomaly_points = sane_nonneg(it->second.number_value(r.score_discovered_anomaly_points), r.score_discovered_anomaly_points);
    }
  }

  return r;
}

Object victory_state_to_json(const VictoryState& vs) {
  Object o;
  o["game_over"] = vs.game_over;
  o["winner_faction_id"] = static_cast<double>(vs.winner_faction_id);
  o["reason"] = victory_reason_to_string(vs.reason);
  o["victory_day"] = static_cast<double>(vs.victory_day);
  o["winner_score"] = vs.winner_score;
  return o;
}

VictoryState victory_state_from_json(const Value& v) {
  VictoryState vs;
  if (!v.is_object()) return vs;
  const Object& o = v.object();
  if (auto it = o.find("game_over"); it != o.end()) vs.game_over = it->second.bool_value(vs.game_over);
  if (auto it = o.find("winner_faction_id"); it != o.end()) vs.winner_faction_id = static_cast<Id>(it->second.int_value(vs.winner_faction_id));
  if (auto it = o.find("reason"); it != o.end()) vs.reason = victory_reason_from_string(it->second.string_value());
  if (auto it = o.find("victory_day"); it != o.end()) vs.victory_day = it->second.int_value(vs.victory_day);
  if (auto it = o.find("winner_score"); it != o.end()) vs.winner_score = sane_nonneg(it->second.number_value(vs.winner_score), vs.winner_score);
  return vs;
}


const char* sensor_mode_to_string(SensorMode m) {
  switch (m) {
    case SensorMode::Passive: return "passive";
    case SensorMode::Normal: return "normal";
    case SensorMode::Active: return "active";
  }
  return "normal";
}

SensorMode sensor_mode_from_string(const std::string& s) {
  if (s == "passive" || s == "emcon" || s == "silent") return SensorMode::Passive;
  if (s == "active" || s == "ping") return SensorMode::Active;
  return SensorMode::Normal;
}

const char* repair_priority_to_string(RepairPriority p) {
  switch (p) {
    case RepairPriority::Low: return "low";
    case RepairPriority::Normal: return "normal";
    case RepairPriority::High: return "high";
  }
  return "normal";
}

RepairPriority repair_priority_from_string(const std::string& s) {
  if (s == "low" || s == "l") return RepairPriority::Low;
  if (s == "high" || s == "h" || s == "urgent") return RepairPriority::High;
  return RepairPriority::Normal;
}

const char* engagement_range_mode_to_string(EngagementRangeMode m) {
  switch (m) {
    case EngagementRangeMode::Auto: return "auto";
    case EngagementRangeMode::Beam: return "beam";
    case EngagementRangeMode::Missile: return "missile";
    case EngagementRangeMode::Max: return "max";
    case EngagementRangeMode::Min: return "min";
    case EngagementRangeMode::Custom: return "custom";
  }
  return "auto";
}

EngagementRangeMode engagement_range_mode_from_string(const std::string& s) {
  if (s == "beam" || s == "laser" || s == "gun") return EngagementRangeMode::Beam;
  if (s == "missile" || s == "torpedo") return EngagementRangeMode::Missile;
  if (s == "max" || s == "maximum") return EngagementRangeMode::Max;
  if (s == "min" || s == "minimum") return EngagementRangeMode::Min;
  if (s == "custom" || s == "fixed") return EngagementRangeMode::Custom;
  return EngagementRangeMode::Auto;
}

const char* fleet_formation_to_string(FleetFormation f) {
  switch (f) {
    case FleetFormation::None: return "none";
    case FleetFormation::LineAbreast: return "line";
    case FleetFormation::Column: return "column";
    case FleetFormation::Wedge: return "wedge";
    case FleetFormation::Ring: return "ring";
  }
  return "none";
}

FleetFormation fleet_formation_from_string(const std::string& s) {
  if (s == "line") return FleetFormation::LineAbreast;
  if (s == "column") return FleetFormation::Column;
  if (s == "wedge") return FleetFormation::Wedge;
  if (s == "ring") return FleetFormation::Ring;
  return FleetFormation::None;
}

const char* fleet_mission_type_to_string(FleetMissionType t) {
  switch (t) {
    case FleetMissionType::None: return "none";
    case FleetMissionType::DefendColony: return "defend_colony";
    case FleetMissionType::PatrolSystem: return "patrol_system";
    case FleetMissionType::HuntHostiles: return "hunt_hostiles";
    case FleetMissionType::EscortFreighters: return "escort_freighters";
    case FleetMissionType::Explore: return "explore";
    case FleetMissionType::PatrolRegion: return "patrol_region";
    case FleetMissionType::AssaultColony: return "assault_colony";
  }
  return "none";
}

FleetMissionType fleet_mission_type_from_string(const std::string& s) {
  if (s == "defend_colony" || s == "defend") return FleetMissionType::DefendColony;
  if (s == "patrol_system" || s == "patrol") return FleetMissionType::PatrolSystem;
  if (s == "hunt_hostiles" || s == "hunt") return FleetMissionType::HuntHostiles;
  if (s == "escort_freighters" || s == "escort_freighter" || s == "escort") return FleetMissionType::EscortFreighters;
  if (s == "explore" || s == "explore_systems" || s == "exploration") return FleetMissionType::Explore;
  if (s == "patrol_region" || s == "patrolreg" || s == "region_patrol") return FleetMissionType::PatrolRegion;
  if (s == "assault_colony" || s == "assault" || s == "invade" || s == "conquer") return FleetMissionType::AssaultColony;
  return FleetMissionType::None;
}

const char* fleet_sustainment_mode_to_string(FleetSustainmentMode m) {
  switch (m) {
    case FleetSustainmentMode::None: return "none";
    case FleetSustainmentMode::Refuel: return "refuel";
    case FleetSustainmentMode::Repair: return "repair";
    case FleetSustainmentMode::Rearm: return "rearm";
    case FleetSustainmentMode::Maintenance: return "maintenance";
  }
  return "none";
}

FleetSustainmentMode fleet_sustainment_mode_from_string(const std::string& s) {
  if (s == "refuel" || s == "fuel") return FleetSustainmentMode::Refuel;
  if (s == "repair" || s == "rep") return FleetSustainmentMode::Repair;
  if (s == "rearm" || s == "ammo" || s == "munitions") return FleetSustainmentMode::Rearm;
  if (s == "maintenance" || s == "maint" || s == "resupply") return FleetSustainmentMode::Maintenance;
  return FleetSustainmentMode::None;
}


const char* event_level_to_string(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "info";
    case EventLevel::Warn: return "warn";
    case EventLevel::Error: return "error";
  }
  return "info";
}

const char* event_category_to_string(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "general";
    case EventCategory::Research: return "research";
    case EventCategory::Shipyard: return "shipyard";
    case EventCategory::Construction: return "construction";
    case EventCategory::Movement: return "movement";
    case EventCategory::Combat: return "combat";
    case EventCategory::Intel: return "intel";
    case EventCategory::Exploration: return "exploration";
    case EventCategory::Diplomacy: return "diplomacy";
  }
  return "general";
}

EventCategory event_category_from_string(const std::string& s) {
  if (s == "research") return EventCategory::Research;
  if (s == "shipyard") return EventCategory::Shipyard;
  if (s == "construction") return EventCategory::Construction;
  if (s == "movement") return EventCategory::Movement;
  if (s == "combat") return EventCategory::Combat;
  if (s == "intel") return EventCategory::Intel;
  if (s == "exploration") return EventCategory::Exploration;
  if (s == "diplomacy") return EventCategory::Diplomacy;
  return EventCategory::General;
}

EventLevel event_level_from_string(const std::string& s) {
  if (s == "warn") return EventLevel::Warn;
  if (s == "error") return EventLevel::Error;
  return EventLevel::Info;
}

Value order_to_json(const Order& order) {
  return std::visit(
      [](const auto& o) -> Value {
        using T = std::decay_t<decltype(o)>;
        Object obj;
        if constexpr (std::is_same_v<T, MoveToPoint>) {
          obj["type"] = std::string("move_to_point");
          obj["target"] = vec2_to_json(o.target_mkm);
        } else if constexpr (std::is_same_v<T, MoveToBody>) {
          obj["type"] = std::string("move_to_body");
          obj["body_id"] = static_cast<double>(o.body_id);
        } else if constexpr (std::is_same_v<T, ColonizeBody>) {
          obj["type"] = std::string("colonize_body");
          obj["body_id"] = static_cast<double>(o.body_id);
          if (!o.colony_name.empty()) obj["colony_name"] = o.colony_name;
        } else if constexpr (std::is_same_v<T, OrbitBody>) {
          obj["type"] = std::string("orbit_body");
          obj["body_id"] = static_cast<double>(o.body_id);
          obj["duration_days"] = static_cast<double>(o.duration_days);
          if (o.progress_days > 1e-12) obj["progress_days"] = o.progress_days;
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          obj["type"] = std::string("travel_via_jump");
          obj["jump_point_id"] = static_cast<double>(o.jump_point_id);
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          obj["type"] = std::string("attack_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          if (o.has_last_known) {
            obj["has_last_known"] = true;
            obj["last_known_position_mkm"] = vec2_to_json(o.last_known_position_mkm);
          }
        } else if constexpr (std::is_same_v<T, EscortShip>) {
          obj["type"] = std::string("escort_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          obj["follow_distance_mkm"] = o.follow_distance_mkm;
          if (o.restrict_to_discovered) obj["restrict_to_discovered"] = true;
        } else if constexpr (std::is_same_v<T, WaitDays>) {
          obj["type"] = std::string("wait_days");
          obj["days_remaining"] = static_cast<double>(o.days_remaining);
          if (o.progress_days > 1e-12) obj["progress_days"] = o.progress_days;
        } else if constexpr (std::is_same_v<T, LoadMineral>) {
          obj["type"] = std::string("load_mineral");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, UnloadMineral>) {
          obj["type"] = std::string("unload_mineral");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
                } else if constexpr (std::is_same_v<T, MineBody>) {
          obj["type"] = std::string("mine_body");
          obj["body_id"] = static_cast<double>(o.body_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          if (!o.stop_when_cargo_full) obj["stop_when_cargo_full"] = false;
} else if constexpr (std::is_same_v<T, LoadTroops>) {
          obj["type"] = std::string("load_troops");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          obj["strength"] = o.strength;
        } else if constexpr (std::is_same_v<T, UnloadTroops>) {
          obj["type"] = std::string("unload_troops");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          obj["strength"] = o.strength;
        } else if constexpr (std::is_same_v<T, LoadColonists>) {
          obj["type"] = std::string("load_colonists");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          obj["millions"] = o.millions;
        } else if constexpr (std::is_same_v<T, UnloadColonists>) {
          obj["type"] = std::string("unload_colonists");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          obj["millions"] = o.millions;
        } else if constexpr (std::is_same_v<T, InvadeColony>) {
          obj["type"] = std::string("invade_colony");
          obj["colony_id"] = static_cast<double>(o.colony_id);
        } else if constexpr (std::is_same_v<T, BombardColony>) {
          obj["type"] = std::string("bombard_colony");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          obj["duration_days"] = static_cast<double>(o.duration_days);
          if (o.progress_days > 1e-12) obj["progress_days"] = o.progress_days;
        } else if constexpr (std::is_same_v<T, SalvageWreck>) {
          obj["type"] = std::string("salvage_wreck");
          obj["wreck_id"] = static_cast<double>(o.wreck_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, InvestigateAnomaly>) {
          obj["type"] = std::string("investigate_anomaly");
          obj["anomaly_id"] = static_cast<double>(o.anomaly_id);
          obj["duration_days"] = static_cast<double>(o.duration_days);
          if (o.progress_days > 1e-12) obj["progress_days"] = o.progress_days;
        } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
          obj["type"] = std::string("transfer_cargo_to_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, TransferFuelToShip>) {
          obj["type"] = std::string("transfer_fuel_to_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, TransferTroopsToShip>) {
          obj["type"] = std::string("transfer_troops_to_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          obj["strength"] = o.strength;
        } else if constexpr (std::is_same_v<T, ScrapShip>) {
          obj["type"] = std::string("scrap_ship");
          obj["colony_id"] = static_cast<double>(o.colony_id);
        }
        return obj;
      },
      order);
}

Order order_from_json(const Value& v) {
  const auto& o = v.object();
  const std::string type = o.at("type").string_value();
  if (type == "move_to_point") {
    MoveToPoint m;
    m.target_mkm = vec2_from_json(o.at("target"));
    return m;
  }
  if (type == "move_to_body") {
    MoveToBody m;
    m.body_id = static_cast<Id>(o.at("body_id").int_value());
    return m;
  }
  if (type == "colonize_body") {
    ColonizeBody c;
    c.body_id = static_cast<Id>(o.at("body_id").int_value());
    if (auto itn = o.find("colony_name"); itn != o.end()) c.colony_name = itn->second.string_value();
    return c;
  }
  if (type == "orbit_body") {
    OrbitBody m;
    m.body_id = static_cast<Id>(o.at("body_id").int_value(kInvalidId));
    // Optional: older/forward-compatible saves may omit these.
    if (auto it = o.find("duration_days"); it != o.end()) {
      m.duration_days = static_cast<int>(it->second.int_value(-1));
    }
    if (auto it = o.find("progress_days"); it != o.end()) {
      m.progress_days = it->second.number_value(0.0);
    }
    return m;
  }
  if (type == "travel_via_jump") {
    TravelViaJump t;
    t.jump_point_id = static_cast<Id>(o.at("jump_point_id").int_value());
    return t;
  }
  if (type == "attack_ship") {
    AttackShip a;
    a.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value());

    if (auto lk_it = o.find("last_known_position_mkm"); lk_it != o.end()) {
      a.last_known_position_mkm = vec2_from_json(lk_it->second);
      a.has_last_known = true;
    }
    if (auto hk_it = o.find("has_last_known"); hk_it != o.end()) {
      a.has_last_known = hk_it->second.bool_value(a.has_last_known);
    }
    return a;
  }
  if (type == "escort_ship") {
    EscortShip e;
    e.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value());
    if (auto it = o.find("follow_distance_mkm"); it != o.end()) {
      e.follow_distance_mkm = it->second.number_value(1.0);
    } else if (auto it2 = o.find("follow_mkm"); it2 != o.end()) {
      // Backward compatible alias.
      e.follow_distance_mkm = it2->second.number_value(1.0);
    }
    if (!std::isfinite(e.follow_distance_mkm) || e.follow_distance_mkm < 0.0) e.follow_distance_mkm = 1.0;
    if (auto it3 = o.find("restrict_to_discovered"); it3 != o.end()) {
      e.restrict_to_discovered = it3->second.bool_value(false);
    }
    return e;
  }
  if (type == "wait_days") {
    WaitDays w;
    if (auto dr_it = o.find("days_remaining"); dr_it != o.end()) {
      w.days_remaining = static_cast<int>(dr_it->second.int_value(0));
    } else if (auto d_it = o.find("days"); d_it != o.end()) {
      w.days_remaining = static_cast<int>(d_it->second.int_value(0));
    }
    if (auto it = o.find("progress_days"); it != o.end()) {
      w.progress_days = it->second.number_value(0.0);
    }
    return w;
  }
  if (type == "load_mineral") {
    LoadMineral l;
    l.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) l.mineral = m_it->second.string_value();
    if (auto t_it = o.find("tons"); t_it != o.end()) l.tons = t_it->second.number_value(0.0);
    return l;
  }
  if (type == "unload_mineral") {
    UnloadMineral u;
    u.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) u.mineral = m_it->second.string_value();
    if (auto t_it = o.find("tons"); t_it != o.end()) u.tons = t_it->second.number_value(0.0);
    return u;
  }
  if (type == "mine_body") {
    MineBody mb;
    mb.body_id = static_cast<Id>(o.at("body_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) mb.mineral = m_it->second.string_value();
    if (auto it = o.find("stop_when_cargo_full"); it != o.end()) mb.stop_when_cargo_full = it->second.bool_value(true);
    return mb;
  }
  if (type == "load_troops") {
    LoadTroops l;
    l.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto it = o.find("strength"); it != o.end()) l.strength = it->second.number_value(0.0);
    return l;
  }
  if (type == "unload_troops") {
    UnloadTroops u;
    u.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto it = o.find("strength"); it != o.end()) u.strength = it->second.number_value(0.0);
    return u;
  }
  if (type == "load_colonists") {
    LoadColonists l;
    l.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto it = o.find("millions"); it != o.end()) l.millions = it->second.number_value(0.0);
    return l;
  }
  if (type == "unload_colonists") {
    UnloadColonists u;
    u.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto it = o.find("millions"); it != o.end()) u.millions = it->second.number_value(0.0);
    return u;
  }
  if (type == "invade_colony") {
    InvadeColony i;
    i.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    return i;
  }
  if (type == "bombard_colony") {
    BombardColony b;
    b.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    // Optional: older/forward-compatible saves may omit this.
    if (auto it = o.find("duration_days"); it != o.end()) {
      b.duration_days = static_cast<int>(it->second.int_value(-1));
    }
    if (auto it = o.find("progress_days"); it != o.end()) {
      b.progress_days = it->second.number_value(0.0);
    }
    return b;
  }
  if (type == "salvage_wreck") {
    SalvageWreck s;
    s.wreck_id = static_cast<Id>(o.at("wreck_id").int_value(kInvalidId));
    if (auto it = o.find("mineral"); it != o.end()) s.mineral = it->second.string_value();
    if (auto it = o.find("tons"); it != o.end()) s.tons = it->second.number_value(0.0);
    return s;
  }
  if (type == "investigate_anomaly") {
    InvestigateAnomaly i;
    i.anomaly_id = static_cast<Id>(o.at("anomaly_id").int_value(kInvalidId));
    if (auto it = o.find("duration_days"); it != o.end()) {
      i.duration_days = static_cast<int>(it->second.int_value(0));
    }
    if (auto it = o.find("progress_days"); it != o.end()) {
      i.progress_days = it->second.number_value(0.0);
    }
    return i;
  }
  if (type == "transfer_cargo_to_ship") {
    TransferCargoToShip t;
    t.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) t.mineral = m_it->second.string_value();
    if (auto t_it = o.find("tons"); t_it != o.end()) t.tons = t_it->second.number_value(0.0);
    return t;
  }
  if (type == "transfer_fuel_to_ship") {
    TransferFuelToShip t;
    t.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value(kInvalidId));
    if (auto t_it = o.find("tons"); t_it != o.end()) t.tons = t_it->second.number_value(0.0);
    return t;
  }
  if (type == "transfer_troops_to_ship") {
    TransferTroopsToShip t;
    t.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value(kInvalidId));
    if (auto s_it = o.find("strength"); s_it != o.end()) t.strength = s_it->second.number_value(0.0);
    return t;
  }
  if (type == "scrap_ship") {
    ScrapShip s;
    s.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    return s;
  }
  throw std::runtime_error("Unknown order type: " + type);
}

Value map_string_double_to_json(const std::unordered_map<std::string, double>& m) {
  Object o;
  for (const auto& [k, v] : m) o[k] = v;
  return o;
}

std::unordered_map<std::string, double> map_string_double_from_json(const Value& v) {
  std::unordered_map<std::string, double> m;
  for (const auto& [k, val] : v.object()) m[k] = val.number_value();
  return m;
}

Value map_string_int_to_json(const std::unordered_map<std::string, int>& m) {
  Object o;
  for (const auto& [k, v] : m) o[k] = static_cast<double>(v);
  return o;
}

std::unordered_map<std::string, int> map_string_int_from_json(const Value& v) {
  std::unordered_map<std::string, int> m;
  for (const auto& [k, val] : v.object()) m[k] = static_cast<int>(val.int_value());
  return m;
}

Array string_vector_to_json(const std::vector<std::string>& v) {
  Array a;
  a.reserve(v.size());
  for (const auto& s : v) a.push_back(s);
  return a;
}

std::vector<std::string> string_vector_from_json(const Value& v) {
  std::vector<std::string> out;
  for (const auto& x : v.array()) out.push_back(x.string_value());
  return out;
}

Object colony_automation_profile_to_json(const ColonyAutomationProfile& p) {
  Object o;
  if (p.garrison_target_strength > 0.0) o["garrison_target_strength"] = p.garrison_target_strength;
  if (!p.mineral_reserves.empty()) o["mineral_reserves"] = map_string_double_to_json(p.mineral_reserves);
  if (!p.mineral_targets.empty()) o["mineral_targets"] = map_string_double_to_json(p.mineral_targets);
  if (!p.installation_targets.empty()) o["installation_targets"] = map_string_int_to_json(p.installation_targets);
  return o;
}

ColonyAutomationProfile colony_automation_profile_from_json_value(const Value& v) {
  ColonyAutomationProfile p;
  if (!v.is_object()) return p;
  const auto& po = v.object();

  if (auto itg = po.find("garrison_target_strength"); itg != po.end()) {
    p.garrison_target_strength = itg->second.number_value(0.0);
  }
  if (auto it2 = po.find("mineral_reserves"); it2 != po.end() && it2->second.is_object()) {
    p.mineral_reserves = map_string_double_from_json(it2->second);
  }
  if (auto it2 = po.find("mineral_targets"); it2 != po.end() && it2->second.is_object()) {
    p.mineral_targets = map_string_double_from_json(it2->second);
  }
  if (auto it2 = po.find("installation_targets"); it2 != po.end() && it2->second.is_object()) {
    p.installation_targets = map_string_int_from_json(it2->second);
  }

  // Sanitize for legacy/modded saves.
  if (!std::isfinite(p.garrison_target_strength) || p.garrison_target_strength < 0.0) {
    p.garrison_target_strength = 0.0;
  }
  for (auto it2 = p.installation_targets.begin(); it2 != p.installation_targets.end();) {
    if (it2->first.empty() || it2->second <= 0) {
      it2 = p.installation_targets.erase(it2);
    } else {
      ++it2;
    }
  }
  for (auto it2 = p.mineral_reserves.begin(); it2 != p.mineral_reserves.end();) {
    if (it2->first.empty() || !std::isfinite(it2->second) || it2->second <= 1e-9) {
      it2 = p.mineral_reserves.erase(it2);
    } else {
      ++it2;
    }
  }
  for (auto it2 = p.mineral_targets.begin(); it2 != p.mineral_targets.end();) {
    if (it2->first.empty() || !std::isfinite(it2->second) || it2->second <= 1e-9) {
      it2 = p.mineral_targets.erase(it2);
    } else {
      ++it2;
    }
  }

  return p;
}


} // namespace

std::string serialize_game_to_json(const GameState& s) {
  Object root;
  root["save_version"] = static_cast<double>(s.save_version);
  root["date"] = s.date.to_string();
  root["hour_of_day"] = static_cast<double>(s.hour_of_day);
  root["next_id"] = static_cast<double>(s.next_id);
  root["next_event_seq"] = static_cast<double>(s.next_event_seq);
  root["selected_system"] = static_cast<double>(s.selected_system);

  root["victory_rules"] = victory_rules_to_json(s.victory_rules);
  root["victory_state"] = victory_state_to_json(s.victory_state);


  // Systems
  Array systems;
  systems.reserve(s.systems.size());
  for (Id id : sorted_keys(s.systems)) {
    const auto& sys = s.systems.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = sys.name;
    o["galaxy_pos"] = vec2_to_json(sys.galaxy_pos);
    if (sys.region_id != kInvalidId) o["region_id"] = static_cast<double>(sys.region_id);
    if (sys.nebula_density != 0.0) o["nebula_density"] = sys.nebula_density;
    if (sys.storm_peak_intensity != 0.0) o["storm_peak_intensity"] = sys.storm_peak_intensity;
    if (sys.storm_start_day != 0) o["storm_start_day"] = static_cast<double>(sys.storm_start_day);
    if (sys.storm_end_day != 0) o["storm_end_day"] = static_cast<double>(sys.storm_end_day);

    Array bodies;
    {
      auto ids = sys.bodies;
      std::sort(ids.begin(), ids.end());
      for (Id bid : ids) bodies.push_back(static_cast<double>(bid));
    }
    o["bodies"] = bodies;

    Array ships;
    {
      auto ids = sys.ships;
      std::sort(ids.begin(), ids.end());
      for (Id sid : ids) ships.push_back(static_cast<double>(sid));
    }
    o["ships"] = ships;

    Array jps;
    {
      auto ids = sys.jump_points;
      std::sort(ids.begin(), ids.end());
      for (Id jid : ids) jps.push_back(static_cast<double>(jid));
    }
    o["jump_points"] = jps;

    systems.push_back(o);
  }
  root["systems"] = systems;


  // Regions (optional)
  if (!s.regions.empty()) {
    Array regions;
    regions.reserve(s.regions.size());
    for (Id id : sorted_keys(s.regions)) {
      const auto& r = s.regions.at(id);
      Object o;
      o["id"] = static_cast<double>(id);
      o["name"] = r.name;
      o["center"] = vec2_to_json(r.center);
      if (!r.theme.empty()) o["theme"] = r.theme;
      if (r.mineral_richness_mult != 1.0) o["mineral_richness_mult"] = r.mineral_richness_mult;
      if (r.volatile_richness_mult != 1.0) o["volatile_richness_mult"] = r.volatile_richness_mult;
      if (r.salvage_richness_mult != 1.0) o["salvage_richness_mult"] = r.salvage_richness_mult;
      if (r.nebula_bias != 0.0) o["nebula_bias"] = r.nebula_bias;
      if (r.pirate_risk != 0.0) o["pirate_risk"] = r.pirate_risk;
      if (r.pirate_suppression != 0.0) o["pirate_suppression"] = r.pirate_suppression;
      if (r.ruins_density != 0.0) o["ruins_density"] = r.ruins_density;
      regions.push_back(o);
    }
    root["regions"] = regions;
  }

  // Bodies
  Array bodies;
  bodies.reserve(s.bodies.size());
  for (Id id : sorted_keys(s.bodies)) {
    const auto& b = s.bodies.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = b.name;
    o["type"] = body_type_to_string(b.type);
    o["system_id"] = static_cast<double>(b.system_id);
    if (b.parent_body_id != kInvalidId) o["parent_body_id"] = static_cast<double>(b.parent_body_id);
    if (b.mass_solar > 0.0) o["mass_solar"] = b.mass_solar;
    if (b.luminosity_solar > 0.0) o["luminosity_solar"] = b.luminosity_solar;
    if (b.mass_earths > 0.0) o["mass_earths"] = b.mass_earths;
    if (b.radius_km > 0.0) o["radius_km"] = b.radius_km;
    if (b.surface_temp_k > 0.0) o["surface_temp_k"] = b.surface_temp_k;
    if (b.atmosphere_atm > 0.0) o["atmosphere_atm"] = b.atmosphere_atm;
    if (b.terraforming_target_temp_k > 0.0) o["terraforming_target_temp_k"] = b.terraforming_target_temp_k;
    if (b.terraforming_target_atm > 0.0) o["terraforming_target_atm"] = b.terraforming_target_atm;
    if (b.terraforming_complete) o["terraforming_complete"] = true;
    o["orbit_radius_mkm"] = b.orbit_radius_mkm;
    o["orbit_period_days"] = b.orbit_period_days;
    o["orbit_phase_radians"] = b.orbit_phase_radians;
    o["orbit_eccentricity"] = b.orbit_eccentricity;
    o["orbit_arg_periapsis_radians"] = b.orbit_arg_periapsis_radians;
    if (!b.mineral_deposits.empty()) {
      o["mineral_deposits"] = map_string_double_to_json(b.mineral_deposits);
    }
    bodies.push_back(o);
  }
  root["bodies"] = bodies;

  // Jump points
  Array jump_points;
  jump_points.reserve(s.jump_points.size());
  for (Id id : sorted_keys(s.jump_points)) {
    const auto& jp = s.jump_points.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = jp.name;
    o["system_id"] = static_cast<double>(jp.system_id);
    o["position_mkm"] = vec2_to_json(jp.position_mkm);
    o["linked_jump_id"] = static_cast<double>(jp.linked_jump_id);
    jump_points.push_back(o);
  }
  root["jump_points"] = jump_points;

  // Ships
  Array ships;
  ships.reserve(s.ships.size());
  for (Id id : sorted_keys(s.ships)) {
    const auto& sh = s.ships.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = sh.name;
    o["faction_id"] = static_cast<double>(sh.faction_id);
    o["system_id"] = static_cast<double>(sh.system_id);
    o["position_mkm"] = vec2_to_json(sh.position_mkm);
    o["design_id"] = sh.design_id;
    o["auto_explore"] = sh.auto_explore;
    o["auto_freight"] = sh.auto_freight;
    o["auto_troop_transport"] = sh.auto_troop_transport;
    o["auto_salvage"] = sh.auto_salvage;
    o["auto_mine"] = sh.auto_mine;
    if (sh.auto_mine_home_colony_id != kInvalidId) {
      o["auto_mine_home_colony_id"] = static_cast<double>(sh.auto_mine_home_colony_id);
    }
    if (!sh.auto_mine_mineral.empty()) {
      o["auto_mine_mineral"] = sh.auto_mine_mineral;
    }
    o["auto_colonize"] = sh.auto_colonize;
    o["auto_refuel"] = sh.auto_refuel;
    o["auto_refuel_threshold_fraction"] = sh.auto_refuel_threshold_fraction;
    o["auto_tanker"] = sh.auto_tanker;
    if (sh.auto_tanker_reserve_fraction != 0.25) {
      o["auto_tanker_reserve_fraction"] = sh.auto_tanker_reserve_fraction;
    }
    o["auto_repair"] = sh.auto_repair;
    o["auto_repair_threshold_fraction"] = sh.auto_repair_threshold_fraction;
    o["auto_rearm"] = sh.auto_rearm;
    if (sh.auto_rearm_threshold_fraction != 0.25) {
      o["auto_rearm_threshold_fraction"] = sh.auto_rearm_threshold_fraction;
    }
    if (sh.repair_priority != RepairPriority::Normal) {
      o["repair_priority"] = repair_priority_to_string(sh.repair_priority);
    }
    o["power_policy"] = ship_power_policy_to_json(sh.power_policy);
    if (sh.sensor_mode != SensorMode::Normal) o["sensor_mode"] = sensor_mode_to_string(sh.sensor_mode);

    // Combat doctrine (optional)
    {
      const auto& doc = sh.combat_doctrine;
      Object dco;
      bool any = false;
      if (doc.range_mode != EngagementRangeMode::Auto) {
        dco["range_mode"] = engagement_range_mode_to_string(doc.range_mode);
        any = true;
      }
      if (doc.range_fraction != 0.9) {
        dco["range_fraction"] = doc.range_fraction;
        any = true;
      }
      if (doc.min_range_mkm != 0.1) {
        dco["min_range_mkm"] = doc.min_range_mkm;
        any = true;
      }
      if (doc.custom_range_mkm != 0.0) {
        dco["custom_range_mkm"] = doc.custom_range_mkm;
        any = true;
      }
      if (doc.kite_if_too_close) {
        dco["kite_if_too_close"] = true;
        any = true;
      }
      if (doc.kite_deadband_fraction != 0.10) {
        dco["kite_deadband_fraction"] = doc.kite_deadband_fraction;
        any = true;
      }
      if (any) o["combat_doctrine"] = dco;
    }
    o["speed_km_s"] = sh.speed_km_s;
    o["velocity_mkm_per_day"] = vec2_to_json(sh.velocity_mkm_per_day);
    o["hp"] = sh.hp;
    if (std::abs(sh.maintenance_condition - 1.0) > 1e-9) o["maintenance"] = sh.maintenance_condition;
    if (sh.crew_grade_points >= 0.0 && std::abs(sh.crew_grade_points - 100.0) > 1e-9) o["crew_grade_points"] = sh.crew_grade_points;
    o["missile_cooldown_days"] = sh.missile_cooldown_days;
    if (sh.missile_ammo >= 0) o["missile_ammo"] = static_cast<double>(sh.missile_ammo);
    if (sh.boarding_cooldown_days > 0.0) o["boarding_cooldown_days"] = sh.boarding_cooldown_days;
    o["fuel_tons"] = sh.fuel_tons;
    o["shields"] = sh.shields;
    o["cargo"] = map_string_double_to_json(sh.cargo);
    if (sh.troops > 0.0) o["troops"] = sh.troops;
    if (sh.colonists_millions > 0.0) o["colonists_millions"] = sh.colonists_millions;
    ships.push_back(o);
  }
  root["ships"] = ships;

  // Wrecks
  Array wrecks;
  wrecks.reserve(s.wrecks.size());
  for (Id id : sorted_keys(s.wrecks)) {
    const auto& w = s.wrecks.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = w.name;
    o["system_id"] = static_cast<double>(w.system_id);
    o["position_mkm"] = vec2_to_json(w.position_mkm);
    o["minerals"] = map_string_double_to_json(w.minerals);
    if (w.source_ship_id != kInvalidId) o["source_ship_id"] = static_cast<double>(w.source_ship_id);
    if (w.source_faction_id != kInvalidId) o["source_faction_id"] = static_cast<double>(w.source_faction_id);
    if (!w.source_design_id.empty()) o["source_design_id"] = w.source_design_id;
    if (w.created_day != 0) o["created_day"] = static_cast<double>(w.created_day);
    wrecks.push_back(o);
  }
  root["wrecks"] = wrecks;

  // Anomalies.
  Array anomalies;
  anomalies.reserve(s.anomalies.size());
  for (Id id : sorted_keys(s.anomalies)) {
    const auto& a = s.anomalies.at(id);
    Object o;
    o["id"] = static_cast<double>(a.id);
    if (!a.name.empty()) o["name"] = a.name;
    if (!a.kind.empty()) o["kind"] = a.kind;
    o["system_id"] = static_cast<double>(a.system_id);
    o["position_mkm"] = vec2_to_json(a.position_mkm);
    if (a.investigation_days != 1) o["investigation_days"] = static_cast<double>(a.investigation_days);
    if (std::abs(a.research_reward) > 1e-12) o["research_reward"] = a.research_reward;
    if (!a.unlock_component_id.empty()) o["unlock_component_id"] = a.unlock_component_id;
    if (!a.mineral_reward.empty()) o["mineral_reward"] = map_string_double_to_json(a.mineral_reward);
    if (a.hazard_chance > 1e-12) o["hazard_chance"] = a.hazard_chance;
    if (a.hazard_damage > 1e-12) o["hazard_damage"] = a.hazard_damage;
    if (a.resolved) o["resolved"] = true;
    if (a.resolved_by_faction_id != kInvalidId) {
      o["resolved_by_faction_id"] = static_cast<double>(a.resolved_by_faction_id);
    }
    if (a.resolved_day != 0) o["resolved_day"] = static_cast<double>(a.resolved_day);
    anomalies.push_back(o);
  }
  root["anomalies"] = anomalies;

  // Missile salvos.
  Array missile_salvos;
  missile_salvos.reserve(s.missile_salvos.size());
  for (Id id : sorted_keys(s.missile_salvos)) {
    const auto& ms = s.missile_salvos.at(id);
    Object o;
    o["id"] = static_cast<double>(ms.id);
    o["system_id"] = static_cast<double>(ms.system_id);
    o["attacker_ship_id"] = static_cast<double>(ms.attacker_ship_id);
    o["attacker_faction_id"] = static_cast<double>(ms.attacker_faction_id);
    o["target_ship_id"] = static_cast<double>(ms.target_ship_id);
    o["target_faction_id"] = static_cast<double>(ms.target_faction_id);
    o["damage"] = ms.damage;
    if (ms.damage_initial > 0.0) o["damage_initial"] = ms.damage_initial;

    // Flight model (optional / backward-compatible).
    if (ms.speed_mkm_per_day > 0.0) o["speed_mkm_per_day"] = ms.speed_mkm_per_day;
    if (ms.range_remaining_mkm > 0.0) o["range_remaining_mkm"] = ms.range_remaining_mkm;
    if (ms.pos_mkm.length() > 0.0) o["pos_mkm"] = vec2_to_json(ms.pos_mkm);
    if (ms.attacker_eccm_strength > 0.0) o["attacker_eccm_strength"] = ms.attacker_eccm_strength;
    if (ms.attacker_sensor_mkm_raw > 0.0) o["attacker_sensor_mkm_raw"] = ms.attacker_sensor_mkm_raw;

    if (ms.eta_days_total > 0.0) o["eta_days_total"] = static_cast<double>(ms.eta_days_total);
    o["eta_days_remaining"] = static_cast<double>(ms.eta_days_remaining);
    // Optional visualization metadata.
    if (ms.launch_pos_mkm.length() > 0.0) o["launch_pos_mkm"] = vec2_to_json(ms.launch_pos_mkm);
    if (ms.target_pos_mkm.length() > 0.0) o["target_pos_mkm"] = vec2_to_json(ms.target_pos_mkm);
    missile_salvos.push_back(o);
  }
  root["missile_salvos"] = missile_salvos;

  // Colonies
  Array colonies;
  colonies.reserve(s.colonies.size());
  for (Id id : sorted_keys(s.colonies)) {
    const auto& c = s.colonies.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = c.name;
    o["faction_id"] = static_cast<double>(c.faction_id);
    o["body_id"] = static_cast<double>(c.body_id);
    o["population_millions"] = c.population_millions;
    o["minerals"] = map_string_double_to_json(c.minerals);
    if (!c.mineral_reserves.empty()) {
      o["mineral_reserves"] = map_string_double_to_json(c.mineral_reserves);
    }
    if (!c.mineral_targets.empty()) {
      o["mineral_targets"] = map_string_double_to_json(c.mineral_targets);
    }
    if (!c.installation_targets.empty()) {
      o["installation_targets"] = map_string_int_to_json(c.installation_targets);
    }
    o["installations"] = map_string_int_to_json(c.installations);
    if (c.ground_forces > 0.0) o["ground_forces"] = c.ground_forces;
    if (c.garrison_target_strength > 0.0) o["garrison_target_strength"] = c.garrison_target_strength;
    if (c.troop_training_queue > 0.0) o["troop_training_queue"] = c.troop_training_queue;
    if (c.troop_training_auto_queued > 0.0) o["troop_training_auto_queued"] = c.troop_training_auto_queued;

    Array q;
    for (const auto& bo : c.shipyard_queue) {
      Object qo;
      qo["design_id"] = bo.design_id;
      qo["tons_remaining"] = bo.tons_remaining;
      if (bo.refit_ship_id != kInvalidId) {
        qo["refit_ship_id"] = static_cast<double>(bo.refit_ship_id);
      }
      if (bo.auto_queued) qo["auto_queued"] = true;
      q.push_back(qo);
    }
    o["shipyard_queue"] = q;

    Array cq;
    for (const auto& ord : c.construction_queue) {
      Object qo;
      qo["installation_id"] = ord.installation_id;
      qo["quantity_remaining"] = static_cast<double>(ord.quantity_remaining);
      qo["minerals_paid"] = ord.minerals_paid;
      qo["cp_remaining"] = ord.cp_remaining;
      qo["auto_queued"] = ord.auto_queued;
      cq.push_back(qo);
    }
    o["construction_queue"] = cq;

    colonies.push_back(o);
  }
  root["colonies"] = colonies;

  // Factions
  Array factions;
  factions.reserve(s.factions.size());
  for (Id id : sorted_keys(s.factions)) {
    const auto& f = s.factions.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = f.name;
    o["control"] = faction_control_to_string(f.control);
    o["research_points"] = f.research_points;
    o["active_research_id"] = f.active_research_id;
    o["active_research_progress"] = f.active_research_progress;
    o["research_queue"] = string_vector_to_json(f.research_queue);
    o["known_techs"] = string_vector_to_json(sorted_unique_copy(f.known_techs));
    o["unlocked_components"] = string_vector_to_json(sorted_unique_copy(f.unlocked_components));
    o["unlocked_installations"] = string_vector_to_json(sorted_unique_copy(f.unlocked_installations));

    // Reverse engineering progress (optional).
    if (!f.reverse_engineering_progress.empty()) {
      Object re;
      for (const auto& [cid, pts] : f.reverse_engineering_progress) {
        if (cid.empty()) continue;
        if (!std::isfinite(pts) || pts <= 0.0) continue;
        re[cid] = pts;
      }
      if (!re.empty()) o["reverse_engineering_progress"] = re;
    }
    if (!f.ship_design_targets.empty()) {
      o["ship_design_targets"] = map_string_int_to_json(f.ship_design_targets);
    }

    // Colony automation profiles (optional).
    if (!f.colony_profiles.empty()) {
      Object profiles;
      for (const auto& name : sorted_keys(f.colony_profiles)) {
        const auto itp = f.colony_profiles.find(name);
        if (itp == f.colony_profiles.end()) continue;
        profiles[name] = colony_automation_profile_to_json(itp->second);
      }
      o["colony_profiles"] = profiles;
    }

    // Colony founding defaults (optional).
    {
      const Object founding_po = colony_automation_profile_to_json(f.colony_founding_profile);
      if (f.auto_apply_colony_founding_profile || !founding_po.empty() || !f.colony_founding_profile_name.empty()) {
        if (f.auto_apply_colony_founding_profile) o["auto_apply_colony_founding_profile"] = true;
        if (!f.colony_founding_profile_name.empty()) o["colony_founding_profile_name"] = f.colony_founding_profile_name;
        if (!founding_po.empty()) o["colony_founding_profile"] = founding_po;
      }
    }

    // Diplomatic relations (optional; omitted when empty or default-hostile).
    if (!f.relations.empty()) {
      Object rel;
      for (const auto& [other_id, status] : f.relations) {
        if (other_id == kInvalidId || other_id == f.id) continue;
        // Hostile is the implicit default; don't write it out to keep saves tidy.
        if (status == DiplomacyStatus::Hostile) continue;
        rel[std::to_string(static_cast<unsigned long long>(other_id))] = diplomacy_status_to_string(status);
      }
      if (!rel.empty()) o["relations"] = rel;
    }


    Array discovered_systems;
    const auto disc = sorted_unique_copy(f.discovered_systems);
    discovered_systems.reserve(disc.size());
    for (Id sid : disc) discovered_systems.push_back(static_cast<double>(sid));
    o["discovered_systems"] = discovered_systems;

    Array discovered_anomalies;
    const auto da = sorted_unique_copy(f.discovered_anomalies);
    discovered_anomalies.reserve(da.size());
    for (Id aid : da) discovered_anomalies.push_back(static_cast<double>(aid));
    o["discovered_anomalies"] = discovered_anomalies;


    Array surveyed_jump_points;
    const auto sjp = sorted_unique_copy(f.surveyed_jump_points);
    surveyed_jump_points.reserve(sjp.size());
    for (Id jid : sjp) surveyed_jump_points.push_back(static_cast<double>(jid));
    o["surveyed_jump_points"] = surveyed_jump_points;

    // Incremental jump-point survey progress (optional).
    if (!f.jump_survey_progress.empty()) {
      Array prog;
      prog.reserve(f.jump_survey_progress.size());
      for (Id jid : sorted_keys(f.jump_survey_progress)) {
        const auto itp = f.jump_survey_progress.find(jid);
        const double p = (itp == f.jump_survey_progress.end()) ? 0.0 : itp->second;
        if (!std::isfinite(p) || p <= 1e-9) continue;
        Object po;
        po["jump_point_id"] = static_cast<double>(jid);
        po["progress"] = p;
        prog.push_back(po);
      }
      if (!prog.empty()) o["jump_survey_progress"] = prog;
    }

    Array contacts;
    contacts.reserve(f.ship_contacts.size());
    for (Id sid : sorted_keys(f.ship_contacts)) {
      const auto& c = f.ship_contacts.at(sid);
      Object co;
      co["ship_id"] = static_cast<double>(c.ship_id);
      co["system_id"] = static_cast<double>(c.system_id);
      co["last_seen_day"] = static_cast<double>(c.last_seen_day);
      co["last_seen_position_mkm"] = vec2_to_json(c.last_seen_position_mkm);
      if (c.prev_seen_day > 0) {
        co["prev_seen_day"] = static_cast<double>(c.prev_seen_day);
        co["prev_seen_position_mkm"] = vec2_to_json(c.prev_seen_position_mkm);
      }
      co["last_seen_name"] = c.last_seen_name;
      co["last_seen_design_id"] = c.last_seen_design_id;
      co["last_seen_faction_id"] = static_cast<double>(c.last_seen_faction_id);
      contacts.push_back(co);
    }
    // Pirate hideout rebuild cooldowns (optional).
    if (!f.pirate_hideout_cooldown_until_day.empty()) {
      Object cd;
      for (Id sid : sorted_keys(f.pirate_hideout_cooldown_until_day)) {
        if (sid == kInvalidId) continue;
        const auto itc = f.pirate_hideout_cooldown_until_day.find(sid);
        const int until = (itc == f.pirate_hideout_cooldown_until_day.end()) ? 0 : itc->second;
        if (until <= 0) continue;
        cd[std::to_string(static_cast<unsigned long long>(sid))] = static_cast<double>(until);
      }
      if (!cd.empty()) o["pirate_hideout_cooldowns"] = cd;
    }

    // Diplomatic offer cooldowns (optional).
    if (!f.diplomacy_offer_cooldown_until_day.empty()) {
      Object cd;
      for (Id sid : sorted_keys(f.diplomacy_offer_cooldown_until_day)) {
        if (sid == kInvalidId) continue;
        const auto itc = f.diplomacy_offer_cooldown_until_day.find(sid);
        const int until = (itc == f.diplomacy_offer_cooldown_until_day.end()) ? 0 : itc->second;
        if (until <= 0) continue;
        cd[std::to_string(static_cast<unsigned long long>(sid))] = static_cast<double>(until);
      }
      if (!cd.empty()) o["diplomacy_offer_cooldowns"] = cd;
    }

    o["ship_contacts"] = contacts;
    factions.push_back(o);
  }
  root["factions"] = factions;

  // Treaties (optional; empty means none).
  if (!s.treaties.empty()) {
    Array treaties;
    treaties.reserve(s.treaties.size());
    for (Id id : sorted_keys(s.treaties)) {
      const auto& t = s.treaties.at(id);
      Object o;
      o["id"] = static_cast<double>(id);
      o["faction_a"] = static_cast<double>(t.faction_a);
      o["faction_b"] = static_cast<double>(t.faction_b);
      o["type"] = treaty_type_to_string(t.type);
      o["start_day"] = static_cast<double>(t.start_day);
      o["duration_days"] = static_cast<double>(t.duration_days);
      treaties.push_back(o);
    }
    root["treaties"] = treaties;
  }

  // Diplomatic offers (optional; empty means none).
  if (!s.diplomatic_offers.empty()) {
    Array offers;
    offers.reserve(s.diplomatic_offers.size());
    for (Id id : sorted_keys(s.diplomatic_offers)) {
      const auto& o2 = s.diplomatic_offers.at(id);
      Object o;
      o["id"] = static_cast<double>(id);
      o["from_faction_id"] = static_cast<double>(o2.from_faction_id);
      o["to_faction_id"] = static_cast<double>(o2.to_faction_id);
      o["treaty_type"] = treaty_type_to_string(o2.treaty_type);
      o["treaty_duration_days"] = static_cast<double>(o2.treaty_duration_days);
      o["created_day"] = static_cast<double>(o2.created_day);
      o["expire_day"] = static_cast<double>(o2.expire_day);
      if (!o2.message.empty()) o["message"] = o2.message;
      offers.push_back(o);
    }
    root["diplomatic_offers"] = offers;
  }

  // Fleets
  Array fleets;
  fleets.reserve(s.fleets.size());
  for (Id id : sorted_keys(s.fleets)) {
    const auto& f = s.fleets.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = f.name;
    o["faction_id"] = static_cast<double>(f.faction_id);
    o["leader_ship_id"] = static_cast<double>(f.leader_ship_id);

    o["formation"] = std::string(fleet_formation_to_string(f.formation));
    o["formation_spacing_mkm"] = f.formation_spacing_mkm;


    // Fleet mission automation (optional).
    {
      Object m;
      m["type"] = std::string(fleet_mission_type_to_string(f.mission.type));
      m["defend_colony_id"] = static_cast<double>(f.mission.defend_colony_id);
      m["defend_radius_mkm"] = f.mission.defend_radius_mkm;

      m["patrol_system_id"] = static_cast<double>(f.mission.patrol_system_id);
      m["patrol_dwell_days"] = static_cast<double>(f.mission.patrol_dwell_days);
      m["patrol_leg_index"] = static_cast<double>(f.mission.patrol_leg_index);

      m["patrol_region_id"] = static_cast<double>(f.mission.patrol_region_id);
      m["patrol_region_dwell_days"] = static_cast<double>(f.mission.patrol_region_dwell_days);
      m["patrol_region_system_index"] = static_cast<double>(f.mission.patrol_region_system_index);
      m["patrol_region_waypoint_index"] = static_cast<double>(f.mission.patrol_region_waypoint_index);

      m["hunt_max_contact_age_days"] = static_cast<double>(f.mission.hunt_max_contact_age_days);

      m["escort_target_ship_id"] = static_cast<double>(f.mission.escort_target_ship_id);
      m["escort_active_ship_id"] = static_cast<double>(f.mission.escort_active_ship_id);
      m["escort_follow_distance_mkm"] = f.mission.escort_follow_distance_mkm;
      m["escort_defense_radius_mkm"] = f.mission.escort_defense_radius_mkm;
      m["escort_only_auto_freight"] = f.mission.escort_only_auto_freight;
      m["escort_retarget_interval_days"] = static_cast<double>(f.mission.escort_retarget_interval_days);
      m["escort_last_retarget_day"] = static_cast<double>(f.mission.escort_last_retarget_day);
      m["explore_survey_first"] = f.mission.explore_survey_first;
      m["explore_allow_transit"] = f.mission.explore_allow_transit;

      m["assault_colony_id"] = static_cast<double>(f.mission.assault_colony_id);
      m["assault_staging_colony_id"] = static_cast<double>(f.mission.assault_staging_colony_id);
      m["assault_auto_stage"] = f.mission.assault_auto_stage;
      m["assault_troop_margin_factor"] = f.mission.assault_troop_margin_factor;
      m["assault_use_bombardment"] = f.mission.assault_use_bombardment;
      m["assault_bombard_days"] = static_cast<double>(f.mission.assault_bombard_days);
      m["assault_bombard_executed"] = f.mission.assault_bombard_executed;

      m["auto_refuel"] = f.mission.auto_refuel;
      m["refuel_threshold_fraction"] = f.mission.refuel_threshold_fraction;
      m["refuel_resume_fraction"] = f.mission.refuel_resume_fraction;

      m["auto_repair"] = f.mission.auto_repair;
      m["repair_threshold_fraction"] = f.mission.repair_threshold_fraction;
      m["repair_resume_fraction"] = f.mission.repair_resume_fraction;

      m["auto_rearm"] = f.mission.auto_rearm;
      m["rearm_threshold_fraction"] = f.mission.rearm_threshold_fraction;
      m["rearm_resume_fraction"] = f.mission.rearm_resume_fraction;

      m["auto_maintenance"] = f.mission.auto_maintenance;
      m["maintenance_threshold_fraction"] = f.mission.maintenance_threshold_fraction;
      m["maintenance_resume_fraction"] = f.mission.maintenance_resume_fraction;

      m["sustainment_mode"] = std::string(fleet_sustainment_mode_to_string(f.mission.sustainment_mode));
      m["sustainment_colony_id"] = static_cast<double>(f.mission.sustainment_colony_id);

      m["last_target_ship_id"] = static_cast<double>(f.mission.last_target_ship_id);
      o["mission"] = m;
    }

    auto fleet_ship_ids = f.ship_ids;
    std::sort(fleet_ship_ids.begin(), fleet_ship_ids.end());
    fleet_ship_ids.erase(std::unique(fleet_ship_ids.begin(), fleet_ship_ids.end()), fleet_ship_ids.end());
    Array ship_ids;
    ship_ids.reserve(fleet_ship_ids.size());
    for (Id sid : fleet_ship_ids) ship_ids.push_back(static_cast<double>(sid));
    o["ship_ids"] = ship_ids;

    fleets.push_back(o);
  }
  root["fleets"] = fleets;

  // Custom designs
  Array designs;
  designs.reserve(s.custom_designs.size());
  for (const auto& id : sorted_keys(s.custom_designs)) {
    const auto& d = s.custom_designs.at(id);
    Object o;
    o["id"] = d.id;
    o["name"] = d.name;
    o["role"] = ship_role_to_string(d.role);
    o["components"] = string_vector_to_json(d.components);
    o["mass_tons"] = d.mass_tons;
    o["speed_km_s"] = d.speed_km_s;
    o["fuel_capacity_tons"] = d.fuel_capacity_tons;
    o["fuel_use_per_mkm"] = d.fuel_use_per_mkm;
    o["cargo_tons"] = d.cargo_tons;
    o["mining_tons_per_day"] = d.mining_tons_per_day;
    o["sensor_range_mkm"] = d.sensor_range_mkm;
    o["colony_capacity_millions"] = d.colony_capacity_millions;
    o["troop_capacity"] = d.troop_capacity;
    o["power_generation"] = d.power_generation;
    o["power_use_total"] = d.power_use_total;
    o["power_use_engines"] = d.power_use_engines;
    o["power_use_sensors"] = d.power_use_sensors;
    o["power_use_weapons"] = d.power_use_weapons;
    o["power_use_shields"] = d.power_use_shields;
    o["max_hp"] = d.max_hp;
    o["max_shields"] = d.max_shields;
    o["shield_regen_per_day"] = d.shield_regen_per_day;
    o["weapon_damage"] = d.weapon_damage;
    o["weapon_range_mkm"] = d.weapon_range_mkm;
    o["missile_damage"] = d.missile_damage;
    o["missile_range_mkm"] = d.missile_range_mkm;
    o["missile_speed_mkm_per_day"] = d.missile_speed_mkm_per_day;
    o["missile_reload_days"] = d.missile_reload_days;
    if (d.missile_launcher_count > 0) o["missile_launcher_count"] = static_cast<double>(d.missile_launcher_count);
    if (d.missile_ammo_capacity > 0) o["missile_ammo_capacity"] = static_cast<double>(d.missile_ammo_capacity);
    o["point_defense_damage"] = d.point_defense_damage;
    o["point_defense_range_mkm"] = d.point_defense_range_mkm;
    o["signature_multiplier"] = d.signature_multiplier;
    o["ecm_strength"] = d.ecm_strength;
    o["eccm_strength"] = d.eccm_strength;
    designs.push_back(o);
  }
  root["custom_designs"] = designs;

  // Player order templates
  Array templates;
  templates.reserve(s.order_templates.size());
  for (const auto& name : sorted_keys(s.order_templates)) {
    const auto& orders = s.order_templates.at(name);
    Object o;
    o["name"] = name;

    Array q;
    q.reserve(orders.size());
    for (const auto& ord : orders) q.push_back(order_to_json(ord));
    o["orders"] = q;

    templates.push_back(o);
  }
  root["order_templates"] = templates;

  // Orders
  Array ship_orders;
  ship_orders.reserve(s.ship_orders.size());
  for (Id ship_id : sorted_keys(s.ship_orders)) {
    const auto& orders = s.ship_orders.at(ship_id);
    Object o;
    o["ship_id"] = static_cast<double>(ship_id);
    Array q;
    for (const auto& ord : orders.queue) q.push_back(order_to_json(ord));
    o["queue"] = q;

    o["repeat"] = orders.repeat;
    o["repeat_count_remaining"] = static_cast<double>(orders.repeat_count_remaining);
    Array tmpl;
    tmpl.reserve(orders.repeat_template.size());
    for (const auto& ord : orders.repeat_template) tmpl.push_back(order_to_json(ord));
    o["repeat_template"] = tmpl;

    ship_orders.push_back(o);
  }
  root["ship_orders"] = ship_orders;

  // Persistent simulation event log.
  Array events;
  events.reserve(s.events.size());
  for (const auto& ev : s.events) {
    Object o;
    o["seq"] = static_cast<double>(ev.seq);
    o["day"] = static_cast<double>(ev.day);
    o["hour"] = static_cast<double>(ev.hour);
    o["level"] = std::string(event_level_to_string(ev.level));
    o["category"] = std::string(event_category_to_string(ev.category));
    o["faction_id"] = static_cast<double>(ev.faction_id);
    o["faction_id2"] = static_cast<double>(ev.faction_id2);
    o["system_id"] = static_cast<double>(ev.system_id);
    o["ship_id"] = static_cast<double>(ev.ship_id);
    o["colony_id"] = static_cast<double>(ev.colony_id);
    o["message"] = ev.message;
    events.push_back(o);
  }
  root["events"] = events;

  // Ground battles (optional)
  if (!s.ground_battles.empty()) {
    Array battles;
    battles.reserve(s.ground_battles.size());
    for (Id cid : sorted_keys(s.ground_battles)) {
      const auto& b = s.ground_battles.at(cid);
      Object o;
      o["colony_id"] = static_cast<double>(b.colony_id);
      o["system_id"] = static_cast<double>(b.system_id);
      o["attacker_faction_id"] = static_cast<double>(b.attacker_faction_id);
      o["defender_faction_id"] = static_cast<double>(b.defender_faction_id);
      o["attacker_strength"] = b.attacker_strength;
      o["defender_strength"] = b.defender_strength;
      o["fortification_damage_points"] = b.fortification_damage_points;
      o["days_fought"] = static_cast<double>(b.days_fought);
      battles.push_back(o);
    }
    root["ground_battles"] = battles;
  }

  return json::stringify(root, 2);
}

GameState deserialize_game_from_json(const std::string& json_text) {
  const auto root = json::parse(json_text).object();

  GameState s;
  int loaded_version = 1;
  if (auto itv = root.find("save_version"); itv != root.end()) {
    loaded_version = static_cast<int>(itv->second.int_value(1));
  }
  s.save_version = loaded_version < kCurrentSaveVersion ? kCurrentSaveVersion : loaded_version;

  s.date = Date::parse_iso_ymd(root.at("date").string_value());

  s.hour_of_day = 0;
  if (auto ith = root.find("hour_of_day"); ith != root.end()) {
    s.hour_of_day = static_cast<int>(ith->second.int_value(0));
  }
  s.hour_of_day = std::clamp(s.hour_of_day, 0, 23);

  s.next_id = 1;
  if (auto itnid = root.find("next_id"); itnid != root.end()) {
    s.next_id = static_cast<Id>(itnid->second.int_value(1));
  }

  s.next_event_seq = 1;
  if (auto itseq = root.find("next_event_seq"); itseq != root.end()) {
    s.next_event_seq = static_cast<std::uint64_t>(itseq->second.int_value(1));
  }
  if (s.next_event_seq == 0) s.next_event_seq = 1;

  s.selected_system = kInvalidId;
  if (auto itsel = root.find("selected_system"); itsel != root.end()) {
    s.selected_system = static_cast<Id>(itsel->second.int_value(kInvalidId));
  }

  // Victory rules/state (optional)
  if (auto it = root.find("victory_rules"); it != root.end()) {
    s.victory_rules = victory_rules_from_json(it->second);
  }
  if (auto it = root.find("victory_state"); it != root.end()) {
    s.victory_state = victory_state_from_json(it->second);
  }

  // Regions (optional)
  if (auto it = root.find("regions"); it != root.end()) {
    for (const auto& rv : it->second.array()) {
      const auto& o = rv.object();
      Region r;
      r.id = static_cast<Id>(o.at("id").int_value());
      r.name = o.at("name").string_value();
      if (auto ic = o.find("center"); ic != o.end()) r.center = vec2_from_json(ic->second);
      if (auto itheme = o.find("theme"); itheme != o.end()) r.theme = itheme->second.string_value();

      if (auto im = o.find("mineral_richness_mult"); im != o.end()) r.mineral_richness_mult = im->second.number_value(1.0);
      if (auto iv = o.find("volatile_richness_mult"); iv != o.end()) r.volatile_richness_mult = iv->second.number_value(1.0);
      if (auto isv = o.find("salvage_richness_mult"); isv != o.end()) r.salvage_richness_mult = isv->second.number_value(1.0);

      if (auto inb = o.find("nebula_bias"); inb != o.end()) r.nebula_bias = std::clamp(inb->second.number_value(0.0), -1.0, 1.0);
      if (auto ip = o.find("pirate_risk"); ip != o.end()) r.pirate_risk = std::clamp(ip->second.number_value(0.0), 0.0, 1.0);
      if (auto ips = o.find("pirate_suppression"); ips != o.end()) r.pirate_suppression = std::clamp(ips->second.number_value(0.0), 0.0, 1.0);
      if (auto ir = o.find("ruins_density"); ir != o.end()) r.ruins_density = std::clamp(ir->second.number_value(0.0), 0.0, 1.0);

      if (r.id != kInvalidId) s.regions[r.id] = r;
    }
  }

  // Systems
  for (const auto& sv : root.at("systems").array()) {
    const auto& o = sv.object();
    StarSystem sys;
    sys.id = static_cast<Id>(o.at("id").int_value());
    sys.name = o.at("name").string_value();
    sys.galaxy_pos = vec2_from_json(o.at("galaxy_pos"));
    if (auto ir = o.find("region_id"); ir != o.end()) sys.region_id = static_cast<Id>(ir->second.int_value(kInvalidId));
    if (auto it = o.find("nebula_density"); it != o.end()) {
      sys.nebula_density = std::clamp(it->second.number_value(0.0), 0.0, 1.0);
    }

    if (auto it = o.find("storm_peak_intensity"); it != o.end()) {
      sys.storm_peak_intensity = std::clamp(it->second.number_value(0.0), 0.0, 1.0);
    }
    if (auto it = o.find("storm_start_day"); it != o.end()) {
      sys.storm_start_day = std::max<std::int64_t>(0, static_cast<std::int64_t>(it->second.number_value(0.0)));
    }
    if (auto it = o.find("storm_end_day"); it != o.end()) {
      sys.storm_end_day = std::max<std::int64_t>(0, static_cast<std::int64_t>(it->second.number_value(0.0)));
    }
    // Sanity: if malformed, clear.
    if (!(sys.storm_peak_intensity > 0.0) || sys.storm_end_day <= sys.storm_start_day) {
      sys.storm_peak_intensity = 0.0;
      sys.storm_start_day = 0;
      sys.storm_end_day = 0;
    }

    for (const auto& bid : o.at("bodies").array()) sys.bodies.push_back(static_cast<Id>(bid.int_value()));
    for (const auto& sid : o.at("ships").array()) sys.ships.push_back(static_cast<Id>(sid.int_value()));

    if (auto it = o.find("jump_points"); it != o.end()) {
      for (const auto& jid : it->second.array()) sys.jump_points.push_back(static_cast<Id>(jid.int_value()));
    }

    s.systems[sys.id] = sys;
  }

  if (s.selected_system != kInvalidId && s.systems.find(s.selected_system) == s.systems.end()) {
    s.selected_system = kInvalidId;
  }

  // Validate region ids.
  if (!s.regions.empty()) {
    for (auto& [sid, sys] : s.systems) {
      if (sys.region_id != kInvalidId && s.regions.find(sys.region_id) == s.regions.end()) {
        sys.region_id = kInvalidId;
      }
    }
  }

  // Bodies
  for (const auto& bv : root.at("bodies").array()) {
    const auto& o = bv.object();
    Body b;
    b.id = static_cast<Id>(o.at("id").int_value());
    b.name = o.at("name").string_value();
    b.type = body_type_from_string(o.at("type").string_value());
    b.system_id = static_cast<Id>(o.at("system_id").int_value());
    if (auto it = o.find("parent_body_id"); it != o.end()) {
      b.parent_body_id = static_cast<Id>(it->second.int_value(kInvalidId));
    }
    if (auto it = o.find("mass_solar"); it != o.end()) b.mass_solar = it->second.number_value(0.0);
    if (auto it = o.find("luminosity_solar"); it != o.end()) b.luminosity_solar = it->second.number_value(0.0);
    if (auto it = o.find("mass_earths"); it != o.end()) b.mass_earths = it->second.number_value(0.0);
    if (auto it = o.find("radius_km"); it != o.end()) b.radius_km = it->second.number_value(0.0);
    if (auto it = o.find("surface_temp_k"); it != o.end()) b.surface_temp_k = it->second.number_value(0.0);
    if (auto it = o.find("atmosphere_atm"); it != o.end()) b.atmosphere_atm = it->second.number_value(0.0);
    if (auto it = o.find("terraforming_target_temp_k"); it != o.end()) {
      b.terraforming_target_temp_k = it->second.number_value(0.0);
    }
    if (auto it = o.find("terraforming_target_atm"); it != o.end()) {
      b.terraforming_target_atm = it->second.number_value(0.0);
    }
    if (auto it = o.find("terraforming_complete"); it != o.end()) {
      b.terraforming_complete = it->second.bool_value(false);
    }
    b.orbit_radius_mkm = o.at("orbit_radius_mkm").number_value();
    b.orbit_period_days = o.at("orbit_period_days").number_value();
    b.orbit_phase_radians = o.at("orbit_phase_radians").number_value();

    b.orbit_eccentricity = 0.0;
    if (auto ite = o.find("orbit_eccentricity"); ite != o.end()) {
      b.orbit_eccentricity = ite->second.number_value();
    }

    b.orbit_arg_periapsis_radians = 0.0;
    if (auto itw = o.find("orbit_arg_periapsis_radians"); itw != o.end()) {
      b.orbit_arg_periapsis_radians = itw->second.number_value();
    }
    if (auto it = o.find("mineral_deposits"); it != o.end()) {
      b.mineral_deposits = map_string_double_from_json(it->second);
    }
    s.bodies[b.id] = b;
  }

  // Jump points
  if (auto it = root.find("jump_points"); it != root.end()) {
    for (const auto& jv : it->second.array()) {
      const auto& o = jv.object();
      JumpPoint jp;
      jp.id = static_cast<Id>(o.at("id").int_value());
      jp.name = o.at("name").string_value();
      jp.system_id = static_cast<Id>(o.at("system_id").int_value());
      jp.position_mkm = vec2_from_json(o.at("position_mkm"));
      jp.linked_jump_id = static_cast<Id>(o.at("linked_jump_id").int_value(kInvalidId));
      s.jump_points[jp.id] = jp;
    }
  }

  // Ships
  for (const auto& shv : root.at("ships").array()) {
    const auto& o = shv.object();
    Ship sh;
    sh.id = static_cast<Id>(o.at("id").int_value());
    sh.name = o.at("name").string_value();
    sh.faction_id = static_cast<Id>(o.at("faction_id").int_value());
    sh.system_id = static_cast<Id>(o.at("system_id").int_value());
    sh.position_mkm = vec2_from_json(o.at("position_mkm"));
    sh.design_id = o.at("design_id").string_value();
    if (auto it = o.find("auto_explore"); it != o.end()) sh.auto_explore = it->second.bool_value(false);
    if (auto it = o.find("auto_freight"); it != o.end()) sh.auto_freight = it->second.bool_value(false);
    if (auto it = o.find("auto_troop_transport"); it != o.end()) sh.auto_troop_transport = it->second.bool_value(false);
    if (auto it = o.find("auto_salvage"); it != o.end()) sh.auto_salvage = it->second.bool_value(false);
    if (auto it = o.find("auto_mine"); it != o.end()) sh.auto_mine = it->second.bool_value(false);
    if (auto it = o.find("auto_mine_home_colony_id"); it != o.end()) {
      sh.auto_mine_home_colony_id = static_cast<Id>(it->second.int_value(kInvalidId));
    }
    if (auto it = o.find("auto_mine_mineral"); it != o.end()) sh.auto_mine_mineral = it->second.string_value();
    if (auto it = o.find("auto_colonize"); it != o.end()) sh.auto_colonize = it->second.bool_value(false);
    if (auto it = o.find("auto_refuel"); it != o.end()) sh.auto_refuel = it->second.bool_value(false);
    if (auto it = o.find("auto_refuel_threshold_fraction"); it != o.end()) {
      sh.auto_refuel_threshold_fraction = it->second.number_value(sh.auto_refuel_threshold_fraction);
    }
    if (auto it = o.find("auto_tanker"); it != o.end()) sh.auto_tanker = it->second.bool_value(false);
    if (auto it = o.find("auto_tanker_reserve_fraction"); it != o.end()) {
      sh.auto_tanker_reserve_fraction = it->second.number_value(sh.auto_tanker_reserve_fraction);
    }
    if (auto it = o.find("auto_repair"); it != o.end()) sh.auto_repair = it->second.bool_value(false);
    if (auto it = o.find("auto_repair_threshold_fraction"); it != o.end()) {
      sh.auto_repair_threshold_fraction = it->second.number_value(sh.auto_repair_threshold_fraction);
    }
    if (auto it = o.find("auto_rearm"); it != o.end()) sh.auto_rearm = it->second.bool_value(false);
    if (auto it = o.find("auto_rearm_threshold_fraction"); it != o.end()) {
      sh.auto_rearm_threshold_fraction = it->second.number_value(sh.auto_rearm_threshold_fraction);
    }
    if (auto it = o.find("repair_priority"); it != o.end()) {
      sh.repair_priority = repair_priority_from_string(it->second.string_value("normal"));
    }
    if (auto it = o.find("power_policy"); it != o.end()) sh.power_policy = ship_power_policy_from_json(it->second);
    if (auto it = o.find("sensor_mode"); it != o.end()) {
      sh.sensor_mode = sensor_mode_from_string(it->second.string_value("normal"));
    }

    // Combat doctrine (optional)
    if (auto it = o.find("combat_doctrine"); it != o.end() && it->second.is_object()) {
      const auto& dco = it->second.object();
      if (auto ir = dco.find("range_mode"); ir != dco.end()) {
        sh.combat_doctrine.range_mode = engagement_range_mode_from_string(ir->second.string_value("auto"));
      }
      if (auto irf = dco.find("range_fraction"); irf != dco.end()) {
        sh.combat_doctrine.range_fraction = std::clamp(irf->second.number_value(sh.combat_doctrine.range_fraction), 0.0, 1.0);
      }
      if (auto im = dco.find("min_range_mkm"); im != dco.end()) {
        sh.combat_doctrine.min_range_mkm = std::max(0.0, im->second.number_value(sh.combat_doctrine.min_range_mkm));
      }
      if (auto ic = dco.find("custom_range_mkm"); ic != dco.end()) {
        sh.combat_doctrine.custom_range_mkm = std::max(0.0, ic->second.number_value(sh.combat_doctrine.custom_range_mkm));
      }
      if (auto ik = dco.find("kite_if_too_close"); ik != dco.end()) {
        sh.combat_doctrine.kite_if_too_close = ik->second.bool_value(sh.combat_doctrine.kite_if_too_close);
      }
      if (auto idb = dco.find("kite_deadband_fraction"); idb != dco.end()) {
        sh.combat_doctrine.kite_deadband_fraction = std::clamp(idb->second.number_value(sh.combat_doctrine.kite_deadband_fraction), 0.0, 0.90);
      }
    }
    sh.speed_km_s = o.at("speed_km_s").number_value(0.0);
    if (auto it = o.find("velocity_mkm_per_day"); it != o.end()) {
      sh.velocity_mkm_per_day = vec2_from_json(it->second);
    }
    sh.hp = o.at("hp").number_value(0.0);
    if (auto it = o.find("maintenance"); it != o.end()) {
      sh.maintenance_condition = it->second.number_value(sh.maintenance_condition);
    }
    if (auto it = o.find("crew_grade_points"); it != o.end()) {
      sh.crew_grade_points = it->second.number_value(sh.crew_grade_points);
    }
    if (auto it = o.find("missile_cooldown_days"); it != o.end()) {
      sh.missile_cooldown_days = it->second.number_value(0.0);
    }
    if (auto it = o.find("missile_ammo"); it != o.end()) {
      sh.missile_ammo = static_cast<int>(it->second.int_value(-1));
    }
    if (auto it = o.find("boarding_cooldown_days"); it != o.end()) {
      sh.boarding_cooldown_days = it->second.number_value(0.0);
    }
    if (auto it = o.find("fuel_tons"); it != o.end()) sh.fuel_tons = it->second.number_value(-1.0);
    if (auto it = o.find("shields"); it != o.end()) sh.shields = it->second.number_value(-1.0);
    if (auto it = o.find("cargo"); it != o.end()) sh.cargo = map_string_double_from_json(it->second);
    if (auto it = o.find("troops"); it != o.end()) sh.troops = it->second.number_value(0.0);
    if (auto it = o.find("colonists_millions"); it != o.end()) sh.colonists_millions = it->second.number_value(0.0);
    s.ships[sh.id] = sh;
  }

  // Wrecks (optional in older saves)
  if (auto it = root.find("wrecks"); it != root.end()) {
    for (const auto& wv : it->second.array()) {
      const auto& o = wv.object();
      Wreck w;
      w.id = static_cast<Id>(o.at("id").int_value());
      w.name = o.at("name").string_value();
      w.system_id = static_cast<Id>(o.at("system_id").int_value(kInvalidId));
      w.position_mkm = vec2_from_json(o.at("position_mkm"));
      if (auto im = o.find("minerals"); im != o.end()) w.minerals = map_string_double_from_json(im->second);
      if (auto is = o.find("source_ship_id"); is != o.end()) {
        w.source_ship_id = static_cast<Id>(is->second.int_value(kInvalidId));
      }
      if (auto ifa = o.find("source_faction_id"); ifa != o.end()) {
        w.source_faction_id = static_cast<Id>(ifa->second.int_value(kInvalidId));
      }
      if (auto id = o.find("source_design_id"); id != o.end()) w.source_design_id = id->second.string_value();
      if (auto ic = o.find("created_day"); ic != o.end()) w.created_day = ic->second.int_value(0);
      s.wrecks[w.id] = w;
    }
  }

  // Anomalies (optional in older saves).
  if (auto it = root.find("anomalies"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'anomalies' is not an array; ignoring");
    } else {
      for (const auto& av : it->second.array()) {
        if (!av.is_object()) continue;
        const auto& o = av.object();
        Anomaly a;
        a.id = static_cast<Id>(o.at("id").int_value(kInvalidId));
        if (a.id == kInvalidId) continue;
        if (auto itn = o.find("name"); itn != o.end()) a.name = itn->second.string_value();
        if (auto itk = o.find("kind"); itk != o.end()) a.kind = itk->second.string_value();
        if (auto its = o.find("system_id"); its != o.end()) a.system_id = static_cast<Id>(its->second.int_value(kInvalidId));
        if (auto itp = o.find("position_mkm"); itp != o.end()) a.position_mkm = vec2_from_json(itp->second);
        if (auto itd = o.find("investigation_days"); itd != o.end()) a.investigation_days = static_cast<int>(itd->second.int_value(1));
        if (auto itr = o.find("research_reward"); itr != o.end()) a.research_reward = itr->second.number_value(0.0);
        if (auto itu = o.find("unlock_component_id"); itu != o.end()) a.unlock_component_id = itu->second.string_value();
        if (auto itm = o.find("mineral_reward"); itm != o.end()) a.mineral_reward = map_string_double_from_json(itm->second);
        if (auto ithc = o.find("hazard_chance"); ithc != o.end()) a.hazard_chance = std::clamp(ithc->second.number_value(0.0), 0.0, 1.0);
        if (auto ithd = o.find("hazard_damage"); ithd != o.end()) a.hazard_damage = std::max(0.0, ithd->second.number_value(0.0));
        // Prune invalid mineral entries.
        for (auto itmr = a.mineral_reward.begin(); itmr != a.mineral_reward.end();) {
          const double v = itmr->second;
          if (!(v > 1e-9) || std::isnan(v) || std::isinf(v)) itmr = a.mineral_reward.erase(itmr);
          else ++itmr;
        }
        if (auto itres = o.find("resolved"); itres != o.end()) a.resolved = itres->second.bool_value(false);
        if (auto itrf = o.find("resolved_by_faction_id"); itrf != o.end()) {
          a.resolved_by_faction_id = static_cast<Id>(itrf->second.int_value(kInvalidId));
        }
        if (auto itrd = o.find("resolved_day"); itrd != o.end()) a.resolved_day = static_cast<std::int64_t>(itrd->second.int_value(0));
        if (a.investigation_days < 0) a.investigation_days = 0;
        s.anomalies[a.id] = std::move(a);
      }
    }
  }


  // Missile salvos (optional).
  if (auto it = root.find("missile_salvos"); it != root.end()) {
    for (const auto& sv : it->second.array()) {
      const auto& o = sv.object();
      MissileSalvo ms;
      ms.id = static_cast<Id>(o.at("id").int_value(kInvalidId));
      if (auto is = o.find("system_id"); is != o.end()) ms.system_id = static_cast<Id>(is->second.int_value(kInvalidId));
      if (auto ia = o.find("attacker_ship_id"); ia != o.end()) {
        ms.attacker_ship_id = static_cast<Id>(ia->second.int_value(kInvalidId));
      }
      if (auto ifa = o.find("attacker_faction_id"); ifa != o.end()) {
        ms.attacker_faction_id = static_cast<Id>(ifa->second.int_value(kInvalidId));
      }
      if (auto itar = o.find("target_ship_id"); itar != o.end()) {
        ms.target_ship_id = static_cast<Id>(itar->second.int_value(kInvalidId));
      }
      if (auto itf = o.find("target_faction_id"); itf != o.end()) {
        ms.target_faction_id = static_cast<Id>(itf->second.int_value(kInvalidId));
      }
      if (auto idmg = o.find("damage"); idmg != o.end()) ms.damage = idmg->second.number_value(0.0);
      if (auto idmgi = o.find("damage_initial"); idmgi != o.end()) ms.damage_initial = idmgi->second.number_value(0.0);

      // Flight model (optional / backward-compatible).
      if (auto is = o.find("speed_mkm_per_day"); is != o.end()) ms.speed_mkm_per_day = is->second.number_value(0.0);
      if (auto is = o.find("range_remaining_mkm"); is != o.end()) ms.range_remaining_mkm = is->second.number_value(0.0);
      if (auto ip = o.find("pos_mkm"); ip != o.end()) ms.pos_mkm = vec2_from_json(ip->second);
      if (auto ie = o.find("attacker_eccm_strength"); ie != o.end()) ms.attacker_eccm_strength = ie->second.number_value(0.0);
      if (auto ie = o.find("attacker_sensor_mkm_raw"); ie != o.end()) ms.attacker_sensor_mkm_raw = ie->second.number_value(0.0);

      if (auto ieta = o.find("eta_days_total"); ieta != o.end()) ms.eta_days_total = ieta->second.number_value(0.0);
      if (auto ieta = o.find("eta_days_remaining"); ieta != o.end()) {
        ms.eta_days_remaining = ieta->second.number_value(0.0);
      }
      if (auto ip = o.find("launch_pos_mkm"); ip != o.end()) ms.launch_pos_mkm = vec2_from_json(ip->second);
      if (auto ip = o.find("target_pos_mkm"); ip != o.end()) ms.target_pos_mkm = vec2_from_json(ip->second);

      // Backfill for legacy saves (pre-save_version 41).
      if (ms.damage_initial <= 1e-12) ms.damage_initial = ms.damage;
      if (ms.eta_days_total <= 1e-12) ms.eta_days_total = ms.eta_days_remaining;

      // Best-effort: if positions weren't stored, infer them from the current
      // attacker/target ship positions at load time.
      if (ms.launch_pos_mkm.length() <= 1e-12) {
        if (const auto* sh = find_ptr(s.ships, ms.attacker_ship_id)) ms.launch_pos_mkm = sh->position_mkm;
      }
      if (ms.target_pos_mkm.length() <= 1e-12) {
        if (const auto* sh = find_ptr(s.ships, ms.target_ship_id)) ms.target_pos_mkm = sh->position_mkm;
      }

      // Backfill flight model for legacy saves.
      if (ms.speed_mkm_per_day <= 1e-12) {
        const double dist = (ms.target_pos_mkm - ms.launch_pos_mkm).length();
        const double total = std::max(1e-6, ms.eta_days_total);
        if (dist > 1e-9 && total > 1e-9) ms.speed_mkm_per_day = dist / total;
      }
      if (ms.pos_mkm.length() <= 1e-12) {
        const double total = std::max(1e-6, ms.eta_days_total);
        const double rem = std::clamp(ms.eta_days_remaining, 0.0, total);
        const double frac = std::clamp(1.0 - rem / total, 0.0, 1.0);
        ms.pos_mkm = ms.launch_pos_mkm + (ms.target_pos_mkm - ms.launch_pos_mkm) * frac;
      }
      if (ms.range_remaining_mkm <= 1e-12 && ms.speed_mkm_per_day > 1e-12) {
        ms.range_remaining_mkm = std::max(0.0, ms.speed_mkm_per_day * std::max(0.0, ms.eta_days_remaining));
      }

      s.missile_salvos[ms.id] = ms;
    }
  }

  // Colonies
  for (const auto& cv : root.at("colonies").array()) {
    const auto& o = cv.object();
    Colony c;
    c.id = static_cast<Id>(o.at("id").int_value());
    c.name = o.at("name").string_value();
    c.faction_id = static_cast<Id>(o.at("faction_id").int_value());
    c.body_id = static_cast<Id>(o.at("body_id").int_value());
    c.population_millions = o.at("population_millions").number_value();
    c.minerals = map_string_double_from_json(o.at("minerals"));
    if (auto it = o.find("mineral_reserves"); it != o.end()) {
      c.mineral_reserves = map_string_double_from_json(it->second);
    }
    if (auto it = o.find("mineral_targets"); it != o.end()) {
      c.mineral_targets = map_string_double_from_json(it->second);
    }
    if (auto it = o.find("installation_targets"); it != o.end()) {
      c.installation_targets = map_string_int_from_json(it->second);
    }
    c.installations = map_string_int_from_json(o.at("installations"));

    if (auto it = o.find("ground_forces"); it != o.end()) c.ground_forces = it->second.number_value(0.0);
    if (auto it = o.find("garrison_target_strength"); it != o.end()) {
      c.garrison_target_strength = it->second.number_value(0.0);
    }
    if (auto it = o.find("troop_training_queue"); it != o.end()) {
      c.troop_training_queue = it->second.number_value(0.0);
    }
    if (auto it = o.find("troop_training_auto_queued"); it != o.end()) {
      c.troop_training_auto_queued = it->second.number_value(0.0);
    }

    if (auto itq = o.find("shipyard_queue"); itq != o.end()) {
      for (const auto& qv : itq->second.array()) {
        const auto& qo = qv.object();
        BuildOrder bo;
        bo.design_id = qo.at("design_id").string_value();
        bo.tons_remaining = qo.at("tons_remaining").number_value();
        if (auto it = qo.find("refit_ship_id"); it != qo.end()) {
          bo.refit_ship_id = static_cast<Id>(it->second.int_value(kInvalidId));
        }
        if (auto it = qo.find("auto_queued"); it != qo.end()) bo.auto_queued = it->second.bool_value(false);
        c.shipyard_queue.push_back(bo);
      }
    }

    if (auto itq = o.find("construction_queue"); itq != o.end()) {
      for (const auto& qv : itq->second.array()) {
        const auto& qo = qv.object();
        InstallationBuildOrder ord;
        ord.installation_id = qo.at("installation_id").string_value();
        ord.quantity_remaining = static_cast<int>(qo.at("quantity_remaining").int_value(0));
        if (auto it = qo.find("minerals_paid"); it != qo.end()) ord.minerals_paid = it->second.bool_value(false);
        if (auto it = qo.find("cp_remaining"); it != qo.end()) ord.cp_remaining = it->second.number_value(0.0);
        if (auto it = qo.find("auto_queued"); it != qo.end()) ord.auto_queued = it->second.bool_value(false);
        c.construction_queue.push_back(ord);
      }
    }

    // Sanitize new gameplay automation fields for legacy/modded saves.
    if (!std::isfinite(c.garrison_target_strength) || c.garrison_target_strength < 0.0) {
      c.garrison_target_strength = 0.0;
    }
    if (!std::isfinite(c.troop_training_auto_queued) || c.troop_training_auto_queued < 0.0) {
      c.troop_training_auto_queued = 0.0;
    }
    if (!std::isfinite(c.troop_training_queue) || c.troop_training_queue < 0.0) {
      c.troop_training_queue = 0.0;
    }
    c.troop_training_auto_queued = std::clamp(c.troop_training_auto_queued, 0.0, c.troop_training_queue);

    s.colonies[c.id] = c;
  }

  // Factions
  for (const auto& fv : root.at("factions").array()) {
    const auto& o = fv.object();
    Faction f;
    f.id = static_cast<Id>(o.at("id").int_value());
    f.name = o.at("name").string_value();
    if (auto it = o.find("control"); it != o.end()) {
      f.control = faction_control_from_string(it->second.string_value("player"));
    }
    f.research_points = o.at("research_points").number_value(0.0);

    if (auto it = o.find("active_research_id"); it != o.end()) f.active_research_id = it->second.string_value();
    if (auto it = o.find("active_research_progress"); it != o.end()) f.active_research_progress = it->second.number_value(0.0);

    if (auto it = o.find("research_queue"); it != o.end()) f.research_queue = string_vector_from_json(it->second);
    if (auto it = o.find("known_techs"); it != o.end()) f.known_techs = string_vector_from_json(it->second);

    if (auto it = o.find("unlocked_components"); it != o.end()) f.unlocked_components = string_vector_from_json(it->second);
    if (auto it = o.find("unlocked_installations"); it != o.end()) f.unlocked_installations = string_vector_from_json(it->second);

    // Reverse engineering progress (optional).
    if (auto it = o.find("reverse_engineering_progress"); it != o.end()) {
      if (it->second.is_object()) {
        for (const auto& [cid, pv] : it->second.object()) {
          if (cid.empty()) continue;
          const double pts = pv.number_value(0.0);
          if (!std::isfinite(pts) || pts <= 0.0) continue;
          f.reverse_engineering_progress[cid] = pts;
        }
      }
    }
    if (auto it = o.find("ship_design_targets"); it != o.end()) {
      f.ship_design_targets = map_string_int_from_json(it->second);
    }

    // Colony automation profiles (optional).
    if (auto it = o.find("colony_profiles"); it != o.end()) {
      if (it->second.is_object()) {
        for (const auto& [name, pv] : it->second.object()) {
          if (name.empty()) continue;
          f.colony_profiles[name] = colony_automation_profile_from_json_value(pv);
        }
      } else if (it->second.is_array()) {
        // Also accept an array-of-objects format:
        //   [{"name":"Core Worlds", "garrison_target_strength": 500, ...}, ...]
        for (const auto& pv : it->second.array()) {
          if (!pv.is_object()) continue;
          const auto& po = pv.object();
          const std::string name = (po.find("name") != po.end()) ? po.at("name").string_value() : std::string();
          if (name.empty()) continue;
          f.colony_profiles[name] = colony_automation_profile_from_json_value(pv);
        }
      }
    }

    // Colony founding defaults (optional).
    if (auto it = o.find("auto_apply_colony_founding_profile"); it != o.end()) {
      f.auto_apply_colony_founding_profile = it->second.bool_value(false);
    }
    if (auto it = o.find("colony_founding_profile_name"); it != o.end()) {
      f.colony_founding_profile_name = it->second.string_value();
    }
    if (auto it = o.find("colony_founding_profile"); it != o.end()) {
      f.colony_founding_profile = colony_automation_profile_from_json_value(it->second);
    }

    // Diplomatic relations (optional in saves; if missing, default stance is Hostile).
    if (auto it = o.find("relations"); it != o.end()) {
      if (it->second.is_object()) {
        for (const auto& [k, v] : it->second.object()) {
          Id other = kInvalidId;
          try {
            other = static_cast<Id>(std::stoull(k));
          } catch (...) {
            continue;
          }
          if (other == kInvalidId || other == f.id) continue;
          const DiplomacyStatus ds = diplomacy_status_from_string(v.string_value("hostile"));
          if (ds == DiplomacyStatus::Hostile) continue;
          f.relations[other] = ds;
        }
      } else if (it->second.is_array()) {
        // Also accept an array-of-objects format:
        //   [{"faction_id": 2, "status": "neutral"}, ...]
        for (const auto& rv : it->second.array()) {
          if (!rv.is_object()) continue;
          const auto& ro = rv.object();
          auto itid = ro.find("faction_id");
          if (itid == ro.end()) continue;
          const Id other = static_cast<Id>(itid->second.int_value(kInvalidId));
          if (other == kInvalidId || other == f.id) continue;
          const std::string st = (ro.find("status") != ro.end()) ? ro.at("status").string_value("hostile") : std::string("hostile");
          const DiplomacyStatus ds = diplomacy_status_from_string(st);
          if (ds == DiplomacyStatus::Hostile) continue;
          f.relations[other] = ds;
        }
      }
    }


    if (auto it = o.find("discovered_systems"); it != o.end()) {
      for (const auto& sv : it->second.array()) {
        f.discovered_systems.push_back(static_cast<Id>(sv.int_value(kInvalidId)));
      }
    }

    if (auto it = o.find("discovered_anomalies"); it != o.end()) {
      for (const auto& av : it->second.array()) {
        f.discovered_anomalies.push_back(static_cast<Id>(av.int_value(kInvalidId)));
      }
    }


    if (auto it = o.find("surveyed_jump_points"); it != o.end()) {
      for (const auto& jv : it->second.array()) {
        f.surveyed_jump_points.push_back(static_cast<Id>(jv.int_value(kInvalidId)));
      }
    }

    // Incremental jump-point survey progress (optional).
    // Stored as either an array-of-objects:
    //   [{"jump_point_id": 123, "progress": 0.5}, ...]
    // or an object-map:
    //   {"123": 0.5, ...}
    if (auto it = o.find("jump_survey_progress"); it != o.end()) {
      auto is_surveyed = [&](Id jid) -> bool {
        return std::find(f.surveyed_jump_points.begin(), f.surveyed_jump_points.end(), jid) !=
               f.surveyed_jump_points.end();
      };

      if (it->second.is_array()) {
        for (const auto& pv : it->second.array()) {
          if (!pv.is_object()) continue;
          const auto& po = pv.object();
          const Id jid = static_cast<Id>(po.at("jump_point_id").int_value(kInvalidId));
          if (jid == kInvalidId) continue;
          if (is_surveyed(jid)) continue;
          const double prog = (po.find("progress") != po.end()) ? po.at("progress").number_value(0.0) : 0.0;
          if (!std::isfinite(prog) || prog <= 1e-9) continue;
          f.jump_survey_progress[jid] = prog;
        }
      } else if (it->second.is_object()) {
        for (const auto& [k, v] : it->second.object()) {
          Id jid = kInvalidId;
          try {
            jid = static_cast<Id>(std::stoull(k));
          } catch (...) {
            continue;
          }
          if (jid == kInvalidId) continue;
          if (is_surveyed(jid)) continue;
          const double prog = v.number_value(0.0);
          if (!std::isfinite(prog) || prog <= 1e-9) continue;
          f.jump_survey_progress[jid] = prog;
        }
      }
    }

    // Pirate hideout rebuild cooldowns (optional).
    if (auto it = o.find("pirate_hideout_cooldowns"); it != o.end()) {
      if (it->second.is_object()) {
        for (const auto& [k, v] : it->second.object()) {
          Id sid = kInvalidId;
          try {
            sid = static_cast<Id>(std::stoull(k));
          } catch (...) {
            continue;
          }
          if (sid == kInvalidId) continue;
          const int until = static_cast<int>(v.int_value(0));
          if (until <= 0) continue;
          f.pirate_hideout_cooldown_until_day[sid] = until;
        }
      } else if (it->second.is_array()) {
        // Also accept an array-of-objects format:
        //   [{"system_id": 123, "until_day": 456}, ...]
        for (const auto& pv : it->second.array()) {
          if (!pv.is_object()) continue;
          const auto& po = pv.object();
          const Id sid = static_cast<Id>((po.find("system_id") != po.end()) ? po.at("system_id").int_value(kInvalidId)
                                                                           : kInvalidId);
          if (sid == kInvalidId) continue;
          const int until = static_cast<int>((po.find("until_day") != po.end()) ? po.at("until_day").int_value(0) : 0);
          if (until <= 0) continue;
          f.pirate_hideout_cooldown_until_day[sid] = until;
        }
      }
    }

    // Diplomatic offer cooldowns (optional).
  if (auto itcd = o.find("diplomacy_offer_cooldowns"); itcd != o.end()) {
    if (itcd->second.is_object()) {
      for (const auto& [k, v] : itcd->second.object()) {
        Id other_id = kInvalidId;
        try {
          other_id = static_cast<Id>(std::stoull(std::string(k)));
        } catch (...) {
          continue;
        }
        if (other_id == kInvalidId || other_id == f.id) continue;
        f.diplomacy_offer_cooldown_until_day[other_id] = static_cast<int>(v.int_value(0));
      }
    } else if (itcd->second.is_array()) {
      // Backward/forward-compat: allow array-of-objects encoding.
      for (const auto& cv : itcd->second.array()) {
        if (!cv.is_object()) continue;
        const auto& c = cv.object();
        const Id other_id = static_cast<Id>(c.at("other_faction_id").int_value(kInvalidId));
        if (other_id == kInvalidId || other_id == f.id) continue;
        f.diplomacy_offer_cooldown_until_day[other_id] = static_cast<int>(c.at("until_day").int_value(0));
      }
    }
  }

  if (auto it = o.find("ship_contacts"); it != o.end()) {
      for (const auto& cv : it->second.array()) {
        const auto& co = cv.object();
        Contact c;
        c.ship_id = static_cast<Id>(co.at("ship_id").int_value());
        c.system_id = static_cast<Id>(co.at("system_id").int_value(kInvalidId));
        c.last_seen_day = static_cast<int>(co.at("last_seen_day").int_value(0));
        if (auto itp = co.find("last_seen_position_mkm"); itp != co.end()) c.last_seen_position_mkm = vec2_from_json(itp->second);
        if (auto itp = co.find("prev_seen_day"); itp != co.end()) c.prev_seen_day = static_cast<int>(itp->second.int_value(0));
        if (auto itp = co.find("prev_seen_position_mkm"); itp != co.end()) c.prev_seen_position_mkm = vec2_from_json(itp->second);
        if (auto itn = co.find("last_seen_name"); itn != co.end()) c.last_seen_name = itn->second.string_value();
        if (auto itd = co.find("last_seen_design_id"); itd != co.end()) c.last_seen_design_id = itd->second.string_value();
        if (auto itf = co.find("last_seen_faction_id"); itf != co.end()) c.last_seen_faction_id = static_cast<Id>(itf->second.int_value(kInvalidId));
        if (c.ship_id != kInvalidId) f.ship_contacts[c.ship_id] = std::move(c);
      }
    }

    s.factions[f.id] = f;
  }

  // Backfill anomaly discovery for legacy saves (pre-save_version 48).
  //
  // Prior to v48, anomalies were effectively "global" within a discovered system.
  // To preserve that behavior when loading older saves, seed each faction's
  // discovered_anomalies list with anomalies that exist in systems the faction
  // has already discovered.
  if (loaded_version < 48) {
    for (auto& [fid, fac] : s.factions) {
      for (const auto& [aid, a] : s.anomalies) {
        if (aid == kInvalidId) continue;
        if (a.system_id == kInvalidId) continue;
        if (std::find(fac.discovered_systems.begin(), fac.discovered_systems.end(), a.system_id) ==
            fac.discovered_systems.end()) {
          continue;
        }
        if (std::find(fac.discovered_anomalies.begin(), fac.discovered_anomalies.end(), aid) !=
            fac.discovered_anomalies.end()) {
          continue;
        }
        fac.discovered_anomalies.push_back(aid);
      }
    }
  }

  // Validate victory winner id (in case a save was edited/modded).
  if (s.victory_state.game_over) {
    if (s.victory_state.winner_faction_id == kInvalidId ||
        s.factions.find(s.victory_state.winner_faction_id) == s.factions.end()) {
      s.victory_state = VictoryState{};
    }
  }

  // Treaties (optional).
  if (auto it = root.find("treaties"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'treaties' is not an array; ignoring");
    } else {
      for (const auto& tv : it->second.array()) {
        if (!tv.is_object()) continue;
        const auto& o = tv.object();

        Treaty t;
        t.id = static_cast<Id>(o.at("id").int_value(kInvalidId));
        if (t.id == kInvalidId) continue;

        t.faction_a = static_cast<Id>(o.at("faction_a").int_value(kInvalidId));
        t.faction_b = static_cast<Id>(o.at("faction_b").int_value(kInvalidId));
        if (t.faction_a == kInvalidId || t.faction_b == kInvalidId || t.faction_a == t.faction_b) continue;

        // Normalize pair order to keep treaties symmetric.
        if (t.faction_b < t.faction_a) {
          std::swap(t.faction_a, t.faction_b);
        }

        if (auto itty = o.find("type"); itty != o.end()) {
          t.type = treaty_type_from_string(itty->second.string_value("ceasefire"));
        }
        if (auto itsd = o.find("start_day"); itsd != o.end()) {
          t.start_day = static_cast<std::int64_t>(itsd->second.int_value(0));
        }
        if (auto itd = o.find("duration_days"); itd != o.end()) {
          t.duration_days = static_cast<int>(itd->second.int_value(-1));
        }

        // Validate referenced factions exist.
        if (s.factions.find(t.faction_a) == s.factions.end() || s.factions.find(t.faction_b) == s.factions.end()) continue;

        s.treaties[t.id] = t;
      }
    }
  }

  // Diplomatic offers (optional).
  if (auto it = root.find("diplomatic_offers"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'diplomatic_offers' is not an array; ignoring");
    } else {
      for (const auto& ov : it->second.array()) {
        if (!ov.is_object()) continue;
        const auto& o = ov.object();

        DiplomaticOffer offer;
        offer.id = static_cast<Id>(o.at("id").int_value(kInvalidId));
        if (offer.id == kInvalidId) continue;

        offer.from_faction_id = static_cast<Id>(o.at("from_faction_id").int_value(kInvalidId));
        offer.to_faction_id = static_cast<Id>(o.at("to_faction_id").int_value(kInvalidId));
        if (offer.from_faction_id == kInvalidId || offer.to_faction_id == kInvalidId ||
            offer.from_faction_id == offer.to_faction_id) {
          continue;
        }

        // Validate referenced factions exist.
        if (s.factions.find(offer.from_faction_id) == s.factions.end() ||
            s.factions.find(offer.to_faction_id) == s.factions.end()) {
          continue;
        }

        if (auto itty = o.find("treaty_type"); itty != o.end()) {
          offer.treaty_type = treaty_type_from_string(itty->second.string_value("ceasefire"));
        }
        if (auto itd = o.find("treaty_duration_days"); itd != o.end()) {
          offer.treaty_duration_days = static_cast<int>(itd->second.int_value(-1));
        }
        if (auto itcd = o.find("created_day"); itcd != o.end()) {
          offer.created_day = static_cast<int>(itcd->second.int_value(0));
        }
        if (auto ited = o.find("expire_day"); ited != o.end()) {
          offer.expire_day = static_cast<int>(ited->second.int_value(-1));
        }
        if (auto itm = o.find("message"); itm != o.end()) {
          offer.message = itm->second.string_value("");
        }

        s.diplomatic_offers[offer.id] = offer;
      }
    }
  }

  // Fleets (optional field; older saves may not include it).
  if (auto it = root.find("fleets"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'fleets' is not an array; ignoring");
    } else {
      for (const auto& fv : it->second.array()) {
        if (!fv.is_object()) continue;
        const auto& o = fv.object();

        Fleet fl;
        fl.id = static_cast<Id>(o.at("id").int_value(kInvalidId));
        if (fl.id == kInvalidId) continue;
        fl.name = o.at("name").string_value();
        fl.faction_id = static_cast<Id>(o.at("faction_id").int_value(kInvalidId));

        if (auto itl = o.find("leader_ship_id"); itl != o.end()) {
          fl.leader_ship_id = static_cast<Id>(itl->second.int_value(kInvalidId));
        }

        if (auto its = o.find("ship_ids"); its != o.end() && its->second.is_array()) {
          for (const auto& sv : its->second.array()) {
            fl.ship_ids.push_back(static_cast<Id>(sv.int_value(kInvalidId)));
          }
        }

        if (auto itf = o.find("formation"); itf != o.end()) {
          fl.formation = fleet_formation_from_string(itf->second.string_value("none"));
        }
        if (auto its = o.find("formation_spacing_mkm"); its != o.end()) {
          fl.formation_spacing_mkm = std::max(0.0, its->second.number_value(1.0));
        }


        // Fleet mission automation (optional).
        if (auto itm = o.find("mission"); itm != o.end() && itm->second.is_object()) {
          const auto& m = itm->second.object();
          if (auto itt = m.find("type"); itt != m.end()) {
            fl.mission.type = fleet_mission_type_from_string(itt->second.string_value("none"));
          }
          if (auto itc = m.find("defend_colony_id"); itc != m.end()) {
            fl.mission.defend_colony_id = static_cast<Id>(itc->second.int_value(kInvalidId));
          }
          if (auto itr = m.find("defend_radius_mkm"); itr != m.end()) {
            fl.mission.defend_radius_mkm = std::max(0.0, itr->second.number_value(fl.mission.defend_radius_mkm));
          }
          if (auto itp = m.find("patrol_system_id"); itp != m.end()) {
            fl.mission.patrol_system_id = static_cast<Id>(itp->second.int_value(kInvalidId));
          }
          if (auto itd = m.find("patrol_dwell_days"); itd != m.end()) {
            fl.mission.patrol_dwell_days = std::max(0, static_cast<int>(itd->second.int_value(fl.mission.patrol_dwell_days)));
          }
          if (auto itl = m.find("patrol_leg_index"); itl != m.end()) {
            fl.mission.patrol_leg_index = std::max(0, static_cast<int>(itl->second.int_value(fl.mission.patrol_leg_index)));
          }
          if (auto itrgn = m.find("patrol_region_id"); itrgn != m.end()) {
            fl.mission.patrol_region_id = static_cast<Id>(itrgn->second.int_value(kInvalidId));
          }
          if (auto itrdw = m.find("patrol_region_dwell_days"); itrdw != m.end()) {
            fl.mission.patrol_region_dwell_days = std::max(1, static_cast<int>(itrdw->second.int_value(fl.mission.patrol_region_dwell_days)));
          }
          if (auto itrs = m.find("patrol_region_system_index"); itrs != m.end()) {
            fl.mission.patrol_region_system_index = std::max(0, static_cast<int>(itrs->second.int_value(fl.mission.patrol_region_system_index)));
          }
          if (auto itrw = m.find("patrol_region_waypoint_index"); itrw != m.end()) {
            fl.mission.patrol_region_waypoint_index = std::max(0, static_cast<int>(itrw->second.int_value(fl.mission.patrol_region_waypoint_index)));
          }
          if (auto ith = m.find("hunt_max_contact_age_days"); ith != m.end()) {
            fl.mission.hunt_max_contact_age_days = std::max(0, static_cast<int>(ith->second.int_value(fl.mission.hunt_max_contact_age_days)));
          }
          if (auto iet = m.find("escort_target_ship_id"); iet != m.end()) {
            fl.mission.escort_target_ship_id = static_cast<Id>(iet->second.int_value(kInvalidId));
          }
          if (auto iea = m.find("escort_active_ship_id"); iea != m.end()) {
            fl.mission.escort_active_ship_id = static_cast<Id>(iea->second.int_value(kInvalidId));
          }
          if (auto ief = m.find("escort_follow_distance_mkm"); ief != m.end()) {
            fl.mission.escort_follow_distance_mkm = std::max(0.0, ief->second.number_value(fl.mission.escort_follow_distance_mkm));
          }
          if (auto ied = m.find("escort_defense_radius_mkm"); ied != m.end()) {
            fl.mission.escort_defense_radius_mkm = std::max(0.0, ied->second.number_value(fl.mission.escort_defense_radius_mkm));
          }
          if (auto ieo = m.find("escort_only_auto_freight"); ieo != m.end()) {
            fl.mission.escort_only_auto_freight = ieo->second.bool_value(fl.mission.escort_only_auto_freight);
          }
          if (auto ier = m.find("escort_retarget_interval_days"); ier != m.end()) {
            fl.mission.escort_retarget_interval_days = std::max(0, static_cast<int>(ier->second.int_value(fl.mission.escort_retarget_interval_days)));
          }
          if (auto iel = m.find("escort_last_retarget_day"); iel != m.end()) {
            fl.mission.escort_last_retarget_day = static_cast<int>(iel->second.int_value(fl.mission.escort_last_retarget_day));
          }

          if (auto iesf = m.find("explore_survey_first"); iesf != m.end()) {
            fl.mission.explore_survey_first = iesf->second.bool_value(fl.mission.explore_survey_first);
          }
          if (auto ieat = m.find("explore_allow_transit"); ieat != m.end()) {
            fl.mission.explore_allow_transit = ieat->second.bool_value(fl.mission.explore_allow_transit);
          }

          if (auto iac = m.find("assault_colony_id"); iac != m.end()) {
            fl.mission.assault_colony_id = static_cast<Id>(iac->second.int_value(fl.mission.assault_colony_id));
          }
          if (auto ias = m.find("assault_staging_colony_id"); ias != m.end()) {
            fl.mission.assault_staging_colony_id = static_cast<Id>(ias->second.int_value(fl.mission.assault_staging_colony_id));
          }
          if (auto iaa = m.find("assault_auto_stage"); iaa != m.end()) {
            fl.mission.assault_auto_stage = iaa->second.bool_value(fl.mission.assault_auto_stage);
          }
          if (auto iam = m.find("assault_troop_margin_factor"); iam != m.end()) {
            fl.mission.assault_troop_margin_factor = std::clamp(iam->second.number_value(fl.mission.assault_troop_margin_factor), 1.0, 10.0);
          }
          if (auto iab = m.find("assault_use_bombardment"); iab != m.end()) {
            fl.mission.assault_use_bombardment = iab->second.bool_value(fl.mission.assault_use_bombardment);
          }
          if (auto iad = m.find("assault_bombard_days"); iad != m.end()) {
            fl.mission.assault_bombard_days = static_cast<int>(iad->second.int_value(fl.mission.assault_bombard_days));
          }
          if (auto iae = m.find("assault_bombard_executed"); iae != m.end()) {
            fl.mission.assault_bombard_executed = iae->second.bool_value(fl.mission.assault_bombard_executed);
          }

          if (auto itar = m.find("auto_refuel"); itar != m.end()) {
            fl.mission.auto_refuel = itar->second.bool_value(fl.mission.auto_refuel);
          }
          if (auto itrt = m.find("refuel_threshold_fraction"); itrt != m.end()) {
            fl.mission.refuel_threshold_fraction = std::clamp(itrt->second.number_value(fl.mission.refuel_threshold_fraction), 0.0, 1.0);
          }
          if (auto itrr = m.find("refuel_resume_fraction"); itrr != m.end()) {
            fl.mission.refuel_resume_fraction = std::clamp(itrr->second.number_value(fl.mission.refuel_resume_fraction), 0.0, 1.0);
          }
          if (auto itap = m.find("auto_repair"); itap != m.end()) {
            fl.mission.auto_repair = itap->second.bool_value(fl.mission.auto_repair);
          }
          if (auto itpt = m.find("repair_threshold_fraction"); itpt != m.end()) {
            fl.mission.repair_threshold_fraction = std::clamp(itpt->second.number_value(fl.mission.repair_threshold_fraction), 0.0, 1.0);
          }
          if (auto itpr = m.find("repair_resume_fraction"); itpr != m.end()) {
            fl.mission.repair_resume_fraction = std::clamp(itpr->second.number_value(fl.mission.repair_resume_fraction), 0.0, 1.0);
          }

          if (auto itar = m.find("auto_rearm"); itar != m.end()) {
            fl.mission.auto_rearm = itar->second.bool_value(fl.mission.auto_rearm);
          }
          if (auto itrt = m.find("rearm_threshold_fraction"); itrt != m.end()) {
            fl.mission.rearm_threshold_fraction = std::clamp(itrt->second.number_value(fl.mission.rearm_threshold_fraction), 0.0, 1.0);
          }
          if (auto itrr = m.find("rearm_resume_fraction"); itrr != m.end()) {
            fl.mission.rearm_resume_fraction = std::clamp(itrr->second.number_value(fl.mission.rearm_resume_fraction), 0.0, 1.0);
          }

          if (auto itam = m.find("auto_maintenance"); itam != m.end()) {
            fl.mission.auto_maintenance = itam->second.bool_value(fl.mission.auto_maintenance);
          }
          if (auto itmt = m.find("maintenance_threshold_fraction"); itmt != m.end()) {
            fl.mission.maintenance_threshold_fraction = std::clamp(itmt->second.number_value(fl.mission.maintenance_threshold_fraction), 0.0, 1.0);
          }
          if (auto itmr = m.find("maintenance_resume_fraction"); itmr != m.end()) {
            fl.mission.maintenance_resume_fraction = std::clamp(itmr->second.number_value(fl.mission.maintenance_resume_fraction), 0.0, 1.0);
          }

          if (auto itsm = m.find("sustainment_mode"); itsm != m.end()) {
            fl.mission.sustainment_mode = fleet_sustainment_mode_from_string(itsm->second.string_value("none"));
          }
          if (auto itsc = m.find("sustainment_colony_id"); itsc != m.end()) {
            fl.mission.sustainment_colony_id = static_cast<Id>(itsc->second.int_value(kInvalidId));
          }
          if (auto itlt = m.find("last_target_ship_id"); itlt != m.end()) {
            fl.mission.last_target_ship_id = static_cast<Id>(itlt->second.int_value(kInvalidId));
          }
        }

        s.fleets[fl.id] = std::move(fl);
      }
    }
  }

  // Custom designs
  if (auto it = root.find("custom_designs"); it != root.end()) {
    for (const auto& dv : it->second.array()) {
      const auto& o = dv.object();
      ShipDesign d;
      d.id = o.at("id").string_value();
      d.name = o.at("name").string_value();
      d.role = ship_role_from_string(o.at("role").string_value("unknown"));
      if (auto itc = o.find("components"); itc != o.end()) d.components = string_vector_from_json(itc->second);
      d.mass_tons = o.at("mass_tons").number_value(0.0);
      d.speed_km_s = o.at("speed_km_s").number_value(0.0);
      if (auto itfc = o.find("fuel_capacity_tons"); itfc != o.end()) d.fuel_capacity_tons = itfc->second.number_value(0.0);
      if (auto itfu = o.find("fuel_use_per_mkm"); itfu != o.end()) d.fuel_use_per_mkm = itfu->second.number_value(0.0);
      d.cargo_tons = o.at("cargo_tons").number_value(0.0);
      if (auto itm = o.find("mining_tons_per_day"); itm != o.end()) d.mining_tons_per_day = itm->second.number_value(0.0);
      d.sensor_range_mkm = o.at("sensor_range_mkm").number_value(0.0);
      if (auto itcc = o.find("colony_capacity_millions"); itcc != o.end()) {
        d.colony_capacity_millions = itcc->second.number_value(0.0);
      }
      if (auto ittc = o.find("troop_capacity"); ittc != o.end()) {
        d.troop_capacity = ittc->second.number_value(0.0);
      }
      if (auto itpg = o.find("power_generation"); itpg != o.end()) d.power_generation = itpg->second.number_value(0.0);
      if (auto itput = o.find("power_use_total"); itput != o.end()) d.power_use_total = itput->second.number_value(0.0);
      if (auto itpue = o.find("power_use_engines"); itpue != o.end()) d.power_use_engines = itpue->second.number_value(0.0);
      if (auto itpus = o.find("power_use_sensors"); itpus != o.end()) d.power_use_sensors = itpus->second.number_value(0.0);
      if (auto itpuw = o.find("power_use_weapons"); itpuw != o.end()) d.power_use_weapons = itpuw->second.number_value(0.0);
      if (auto itpush = o.find("power_use_shields"); itpush != o.end()) d.power_use_shields = itpush->second.number_value(0.0);
      d.max_hp = o.at("max_hp").number_value(0.0);
      if (auto itms = o.find("max_shields"); itms != o.end()) d.max_shields = itms->second.number_value(0.0);
      if (auto itsr = o.find("shield_regen_per_day"); itsr != o.end()) {
        d.shield_regen_per_day = itsr->second.number_value(0.0);
      }
      d.weapon_damage = o.at("weapon_damage").number_value(0.0);
      d.weapon_range_mkm = o.at("weapon_range_mkm").number_value(0.0);
      if (auto itmd = o.find("missile_damage"); itmd != o.end()) d.missile_damage = itmd->second.number_value(0.0);
      if (auto itmr = o.find("missile_range_mkm"); itmr != o.end()) d.missile_range_mkm = itmr->second.number_value(0.0);
      if (auto itms = o.find("missile_speed_mkm_per_day"); itms != o.end()) {
        d.missile_speed_mkm_per_day = itms->second.number_value(0.0);
      }
      if (auto itrl = o.find("missile_reload_days"); itrl != o.end()) d.missile_reload_days = itrl->second.number_value(0.0);
      if (auto itml = o.find("missile_launcher_count"); itml != o.end()) {
        d.missile_launcher_count = static_cast<int>(itml->second.int_value(0));
      }
      if (auto itma = o.find("missile_ammo_capacity"); itma != o.end()) {
        d.missile_ammo_capacity = static_cast<int>(itma->second.int_value(0));
      }
      if (auto itpd = o.find("point_defense_damage"); itpd != o.end()) {
        d.point_defense_damage = itpd->second.number_value(0.0);
      }
      if (auto itpr = o.find("point_defense_range_mkm"); itpr != o.end()) {
        d.point_defense_range_mkm = itpr->second.number_value(0.0);
      }
      if (auto itsig = o.find("signature_multiplier"); itsig != o.end()) {
        d.signature_multiplier = itsig->second.number_value(1.0);
      }
      if (auto itecm = o.find("ecm_strength"); itecm != o.end()) {
        d.ecm_strength = itecm->second.number_value(0.0);
      } else if (auto itecm2 = o.find("ecm"); itecm2 != o.end()) {
        d.ecm_strength = itecm2->second.number_value(0.0);
      }
      if (auto iteccm = o.find("eccm_strength"); iteccm != o.end()) {
        d.eccm_strength = iteccm->second.number_value(0.0);
      } else if (auto iteccm2 = o.find("eccm"); iteccm2 != o.end()) {
        d.eccm_strength = iteccm2->second.number_value(0.0);
      }
      s.custom_designs[d.id] = d;
    }
  }

  // Player order templates
  if (auto it = root.find("order_templates"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'order_templates' is not an array; ignoring");
    } else {
      std::size_t dropped = 0;
      std::size_t detail_logs = 0;
      constexpr std::size_t kMaxDetailLogs = 8;

      for (const auto& tv : it->second.array()) {
        if (!tv.is_object()) {
          dropped += 1;
          continue;
        }

        const auto& o = tv.object();
        const std::string name = (o.find("name") != o.end()) ? o.at("name").string_value() : std::string();
        if (name.empty()) {
          dropped += 1;
          continue;
        }

        std::vector<Order> orders;
        if (auto ito = o.find("orders"); ito != o.end()) {
          if (!ito->second.is_array()) {
            dropped += 1;
          } else {
            for (const auto& qv : ito->second.array()) {
              try {
                orders.push_back(order_from_json(qv));
              } catch (const std::exception& e) {
                dropped += 1;
                if (detail_logs < kMaxDetailLogs) {
                  detail_logs += 1;
                  log::warn(std::string("Save load: dropped invalid order in template '") + name + "': " + e.what());
                }
              }
            }
          }
        }

        if (!orders.empty()) {
          s.order_templates[name] = std::move(orders);
        }
      }

      if (dropped > 0) {
        log::warn("Save load: dropped " + std::to_string(static_cast<unsigned long long>(dropped)) +
                  " invalid order template entries");
      }
    }
  }

  // Orders
  if (auto it = root.find("ship_orders"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'ship_orders' is not an array; ignoring");
    } else {
      std::size_t dropped = 0;
      std::size_t detail_logs = 0;
      constexpr std::size_t kMaxDetailLogs = 8;

      for (const auto& ov : it->second.array()) {
        if (!ov.is_object()) {
          dropped += 1;
          continue;
        }

        const auto& o = ov.object();

        Id ship_id = kInvalidId;
        if (auto itid = o.find("ship_id"); itid != o.end()) {
          ship_id = static_cast<Id>(itid->second.int_value(kInvalidId));
        }
        if (ship_id == kInvalidId) {
          dropped += 1;
          continue;
        }

        ShipOrders so;

        auto parse_order_list = [&](const Value& list_v, std::vector<Order>& out, const char* label) {
          if (!list_v.is_array()) {
            dropped += 1;
            return;
          }
          for (const auto& qv : list_v.array()) {
            try {
              out.push_back(order_from_json(qv));
            } catch (const std::exception& e) {
              dropped += 1;
              if (detail_logs < kMaxDetailLogs) {
                detail_logs += 1;
                log::warn(std::string("Save load: dropped invalid order in '") + label +
                          "' for ship_id=" + std::to_string(static_cast<unsigned long long>(ship_id)) +
                          ": " + e.what());
              }
            }
          }
        };

        if (auto itq = o.find("queue"); itq != o.end()) {
          parse_order_list(itq->second, so.queue, "queue");
        }

        if (auto itrep = o.find("repeat"); itrep != o.end()) {
          so.repeat = itrep->second.bool_value(false);
        }
        bool has_repeat_count = false;
        if (auto itrc = o.find("repeat_count_remaining"); itrc != o.end()) {
          so.repeat_count_remaining = static_cast<int>(itrc->second.int_value(0));
          has_repeat_count = true;
        }
        if (auto ittmpl = o.find("repeat_template"); ittmpl != o.end()) {
          parse_order_list(ittmpl->second, so.repeat_template, "repeat_template");
        }

        // Backward compatibility: older saves didn't store repeat_count_remaining.
        if (!has_repeat_count) {
          if (so.repeat && !so.repeat_template.empty()) {
            so.repeat_count_remaining = -1;  // match historical infinite repeat behaviour
          } else {
            so.repeat_count_remaining = 0;
          }
        }
        if (so.repeat_count_remaining < -1) so.repeat_count_remaining = -1;
        if (!so.repeat) so.repeat_count_remaining = 0;

        s.ship_orders[ship_id] = std::move(so);
      }

      if (dropped > 0) {
        log::warn("Save load: dropped " + std::to_string(static_cast<unsigned long long>(dropped)) +
                  " invalid ship order entries");
      }
    }
  }

  // Persistent simulation event log.
  if (auto it = root.find("events"); it != root.end()) {
    std::uint64_t seq_cursor = 0;
    for (const auto& evv : it->second.array()) {
      const auto& o = evv.object();
      SimEvent ev;

      std::uint64_t wanted_seq = 0;
      if (auto its = o.find("seq"); its != o.end()) {
        wanted_seq = static_cast<std::uint64_t>(its->second.int_value(0));
      }
      if (wanted_seq == 0) wanted_seq = seq_cursor + 1;
      if (wanted_seq <= seq_cursor) wanted_seq = seq_cursor + 1;
      ev.seq = wanted_seq;
      seq_cursor = ev.seq;

      ev.day = static_cast<std::int64_t>(o.at("day").int_value(0));
      if (auto ith = o.find("hour"); ith != o.end()) ev.hour = static_cast<int>(ith->second.int_value(0));
      ev.hour = std::clamp(ev.hour, 0, 23);
      if (auto itl = o.find("level"); itl != o.end()) ev.level = event_level_from_string(itl->second.string_value("info"));
      if (auto itc = o.find("category"); itc != o.end()) {
        ev.category = event_category_from_string(itc->second.string_value("general"));
      }

      if (auto itf = o.find("faction_id"); itf != o.end()) ev.faction_id = static_cast<Id>(itf->second.int_value(kInvalidId));
      if (auto itf2 = o.find("faction_id2"); itf2 != o.end()) ev.faction_id2 = static_cast<Id>(itf2->second.int_value(kInvalidId));
      if (auto its = o.find("system_id"); its != o.end()) ev.system_id = static_cast<Id>(its->second.int_value(kInvalidId));
      if (auto itsh = o.find("ship_id"); itsh != o.end()) ev.ship_id = static_cast<Id>(itsh->second.int_value(kInvalidId));
      if (auto itcol = o.find("colony_id"); itcol != o.end()) ev.colony_id = static_cast<Id>(itcol->second.int_value(kInvalidId));
      if (auto itm = o.find("message"); itm != o.end()) ev.message = itm->second.string_value();
      s.events.push_back(std::move(ev));
    }

    if (s.next_event_seq <= seq_cursor) s.next_event_seq = seq_cursor + 1;
  }

  // Ground battles (optional)
  if (auto it = root.find("ground_battles"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'ground_battles' is not an array; ignoring");
    } else {
      for (const auto& bv : it->second.array()) {
        if (!bv.is_object()) continue;
        const auto& o = bv.object();
        GroundBattle b;
        if (auto itc = o.find("colony_id"); itc != o.end()) b.colony_id = static_cast<Id>(itc->second.int_value(kInvalidId));
        if (b.colony_id == kInvalidId) continue;
        if (auto its = o.find("system_id"); its != o.end()) b.system_id = static_cast<Id>(its->second.int_value(kInvalidId));
        if (auto ita = o.find("attacker_faction_id"); ita != o.end()) b.attacker_faction_id = static_cast<Id>(ita->second.int_value(kInvalidId));
        if (auto itd = o.find("defender_faction_id"); itd != o.end()) b.defender_faction_id = static_cast<Id>(itd->second.int_value(kInvalidId));
        if (auto itas = o.find("attacker_strength"); itas != o.end()) b.attacker_strength = itas->second.number_value(0.0);
        if (auto itds = o.find("defender_strength"); itds != o.end()) b.defender_strength = itds->second.number_value(0.0);
        if (auto itfd = o.find("fortification_damage_points"); itfd != o.end()) {
          b.fortification_damage_points = itfd->second.number_value(0.0);
        }
        if (auto itdf = o.find("days_fought"); itdf != o.end()) b.days_fought = static_cast<int>(itdf->second.int_value(0));
        s.ground_battles[b.colony_id] = b;
      }
    }
  }

  // Ensure next_id is sane.
  Id max_id = 0;
  auto bump = [&](Id id) { max_id = std::max(max_id, id); };
  for (auto& [id, _] : s.systems) bump(id);
  for (auto& [id, _] : s.bodies) bump(id);
  for (auto& [id, _] : s.jump_points) bump(id);
  for (auto& [id, _] : s.ships) bump(id);
  for (auto& [id, _] : s.colonies) bump(id);
  for (auto& [id, _] : s.factions) bump(id);
  for (auto& [id, _] : s.fleets) bump(id);
  for (auto& [id, _] : s.regions) bump(id);
  for (auto& [id, _] : s.wrecks) bump(id);
  for (auto& [id, _] : s.anomalies) bump(id);
  for (auto& [id, _] : s.missile_salvos) bump(id);
  for (auto& [id, _] : s.treaties) bump(id);
  if (s.next_id <= max_id) s.next_id = max_id + 1;

  return s;
}

} // namespace nebula4x