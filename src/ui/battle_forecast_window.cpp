#include "ui/battle_forecast_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "nebula4x/core/fleet_battle_forecast.h"
#include "nebula4x/util/sorted_keys.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {

namespace {

using nebula4x::find_ptr;
using nebula4x::Id;
using nebula4x::kInvalidId;
using nebula4x::util::sorted_keys;

const char* winner_label(nebula4x::FleetBattleWinner w) {
  switch (w) {
    case nebula4x::FleetBattleWinner::Attacker:
      return "Attacker";
    case nebula4x::FleetBattleWinner::Defender:
      return "Defender";
    case nebula4x::FleetBattleWinner::Draw:
    default:
      return "Draw";
  }
}

const char* dmg_model_label(nebula4x::FleetBattleDamageModel m) {
  switch (m) {
    case nebula4x::FleetBattleDamageModel::EvenSpread:
      return "Even spread";
    case nebula4x::FleetBattleDamageModel::FocusFire:
    default:
      return "Focus fire";
  }
}

const char* range_model_label(nebula4x::FleetBattleRangeModel m) {
  switch (m) {
    case nebula4x::FleetBattleRangeModel::RangeAdvantage:
      return "Range advantage";
    case nebula4x::FleetBattleRangeModel::Instant:
    default:
      return "Instant";
  }
}

std::vector<Id> fleet_ids_for_faction(const Simulation& sim, Id faction_id) {
  std::vector<std::pair<std::string, Id>> tmp;
  tmp.reserve(sim.state().fleets.size());
  for (const auto& [fid, f] : sim.state().fleets) {
    if (faction_id != kInvalidId && f.faction_id != faction_id) continue;
    tmp.emplace_back(f.name, fid);
  }
  std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  });
  std::vector<Id> out;
  out.reserve(tmp.size());
  for (const auto& [name, id] : tmp) out.push_back(id);
  return out;
}

bool combo_faction(const Simulation& sim, const char* label, Id* faction_id) {
  bool changed = false;

  const auto faction_ids = sorted_keys(sim.state().factions);
  const Faction* cur = find_ptr(sim.state().factions, *faction_id);

  std::string preview = cur ? cur->name : "(select faction)";
  if (ImGui::BeginCombo(label, preview.c_str())) {
    for (Id fid : faction_ids) {
      const Faction* f = find_ptr(sim.state().factions, fid);
      if (!f) continue;
      const bool selected = (fid == *faction_id);
      if (ImGui::Selectable(f->name.c_str(), selected)) {
        *faction_id = fid;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

bool combo_fleet(const Simulation& sim, const char* label, Id faction_id, Id* fleet_id) {
  bool changed = false;

  const auto fleet_ids = fleet_ids_for_faction(sim, faction_id);
  const Fleet* cur = find_ptr(sim.state().fleets, *fleet_id);
  std::string preview = cur ? cur->name : "(select fleet)";

  if (ImGui::BeginCombo(label, preview.c_str())) {
    for (Id fid : fleet_ids) {
      const Fleet* f = find_ptr(sim.state().fleets, fid);
      if (!f) continue;
      const bool selected = (fid == *fleet_id);
      if (ImGui::Selectable(f->name.c_str(), selected)) {
        *fleet_id = fid;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void side_summary_table_row(const char* label, double a, double d, const char* fmt = "%.2f") {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text(fmt, a);
  ImGui::TableSetColumnIndex(2);
  ImGui::Text(fmt, d);
}

void side_summary_table_row_int(const char* label, int a, int d) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("%d", a);
  ImGui::TableSetColumnIndex(2);
  ImGui::Text("%d", d);
}

void plot_series(const char* label, const std::vector<double>& data) {
  if (data.empty()) return;
  static std::vector<float> tmp;
  tmp.clear();
  tmp.reserve(data.size());
  for (double v : data) tmp.push_back(static_cast<float>(v));
  ImGui::PlotLines(label, tmp.data(), static_cast<int>(tmp.size()), 0, nullptr, 0.0f,
                   *std::max_element(tmp.begin(), tmp.end()), ImVec2(0, 80));
}

}  // namespace

void draw_battle_forecast_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  (void)selected_colony;
  (void)selected_body;

  if (!ui.show_battle_forecast_window) return;

  ImGui::SetNextWindowSize(ImVec2(840, 620), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Battle Forecast", &ui.show_battle_forecast_window)) {
    ImGui::End();
    return;
  }

  // Persistent UI selections.
  static Id attacker_faction = kInvalidId;
  static Id defender_faction = kInvalidId;
  static Id attacker_fleet = kInvalidId;
  static Id defender_fleet = kInvalidId;

  static nebula4x::FleetBattleForecastOptions opt;

  static bool initialized = false;
  if (!initialized) {
    initialized = true;
    attacker_faction = ui.viewer_faction_id;
    if (attacker_faction == kInvalidId) {
      // Fallback to first faction.
      const auto fac_ids = sorted_keys(sim.state().factions);
      if (!fac_ids.empty()) attacker_faction = fac_ids.front();
    }

    // Default defender faction: first non-attacker faction (if any).
    const auto fac_ids = sorted_keys(sim.state().factions);
    for (Id fid : fac_ids) {
      if (fid != attacker_faction) {
        defender_faction = fid;
        break;
      }
    }
    if (defender_faction == kInvalidId) defender_faction = attacker_faction;

    // Default fleets: first fleet per faction.
    const auto a_fleets = fleet_ids_for_faction(sim, attacker_faction);
    if (!a_fleets.empty()) attacker_fleet = a_fleets.front();
    const auto d_fleets = fleet_ids_for_faction(sim, defender_faction);
    if (!d_fleets.empty()) defender_fleet = d_fleets.front();
  }

  bool dirty = false;

  if (ImGui::BeginTable("bf_sel", 4, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    dirty |= combo_faction(sim, "Attacker Faction", &attacker_faction);
    ImGui::TableSetColumnIndex(1);
    dirty |= combo_fleet(sim, "Attacker Fleet", attacker_faction, &attacker_fleet);

    ImGui::TableSetColumnIndex(2);
    dirty |= combo_faction(sim, "Defender Faction", &defender_faction);
    ImGui::TableSetColumnIndex(3);
    dirty |= combo_fleet(sim, "Defender Fleet", defender_faction, &defender_fleet);

    ImGui::EndTable();
  }

  // Quick tools
  if (ImGui::Button("Swap")) {
    std::swap(attacker_faction, defender_faction);
    std::swap(attacker_fleet, defender_fleet);
    dirty = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Use selected ship as Attacker")) {
    const Id fid = sim.fleet_for_ship(selected_ship);
    if (fid != kInvalidId) {
      attacker_fleet = fid;
      if (const Fleet* f = find_ptr(sim.state().fleets, fid)) attacker_faction = f->faction_id;
      dirty = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Use selected ship as Defender")) {
    const Id fid = sim.fleet_for_ship(selected_ship);
    if (fid != kInvalidId) {
      defender_fleet = fid;
      if (const Fleet* f = find_ptr(sim.state().fleets, fid)) defender_faction = f->faction_id;
      dirty = true;
    }
  }

  ImGui::Separator();

  // Options.
  ImGui::TextUnformatted("Model options");
  ImGui::Indent();

  dirty |= ImGui::SliderInt("Max days", &opt.max_days, 1, 365);

  // dt combo
  {
    const double dt_choices[] = {0.10, 0.25, 0.50, 1.0};
    const char* dt_labels[] = {"0.10", "0.25", "0.50", "1.0"};
    int cur = 1;  // default 0.25
    for (int i = 0; i < 4; ++i) {
      if (std::fabs(opt.dt_days - dt_choices[i]) < 1e-6) cur = i;
    }
    if (ImGui::BeginCombo("dt (days)", dt_labels[cur])) {
      for (int i = 0; i < 4; ++i) {
        const bool sel = (i == cur);
        if (ImGui::Selectable(dt_labels[i], sel)) {
          opt.dt_days = dt_choices[i];
          dirty = true;
        }
      }
      ImGui::EndCombo();
    }
  }

  // Damage model combo
  {
    const char* preview = dmg_model_label(opt.damage_model);
    if (ImGui::BeginCombo("Damage distribution", preview)) {
      for (nebula4x::FleetBattleDamageModel m : {nebula4x::FleetBattleDamageModel::FocusFire,
                                                nebula4x::FleetBattleDamageModel::EvenSpread}) {
        const bool sel = (m == opt.damage_model);
        if (ImGui::Selectable(dmg_model_label(m), sel)) {
          opt.damage_model = m;
          dirty = true;
        }
      }
      ImGui::EndCombo();
    }
  }

  // Range model combo
  {
    const char* preview = range_model_label(opt.range_model);
    if (ImGui::BeginCombo("Engagement model", preview)) {
      for (nebula4x::FleetBattleRangeModel m : {nebula4x::FleetBattleRangeModel::Instant,
                                               nebula4x::FleetBattleRangeModel::RangeAdvantage}) {
        const bool sel = (m == opt.range_model);
        if (ImGui::Selectable(range_model_label(m), sel)) {
          opt.range_model = m;
          dirty = true;
        }
      }
      ImGui::EndCombo();
    }
  }

  dirty |= ImGui::Checkbox("Include beams", &opt.include_beams);
  dirty |= ImGui::Checkbox("Include missiles", &opt.include_missiles);
  dirty |= ImGui::Checkbox("Include point defense", &opt.include_point_defense);
  dirty |= ImGui::Checkbox("Include shields", &opt.include_shields);
  dirty |= ImGui::Checkbox("Include shield regen", &opt.include_shield_regen);
  dirty |= ImGui::Checkbox("Record timeline", &opt.record_timeline);

  ImGui::Unindent();

  // Forecast computation (cheap enough to run each frame, but only do so when required).
  static nebula4x::FleetBattleForecast cached;
  static Id cached_a = kInvalidId;
  static Id cached_d = kInvalidId;
  static std::uint64_t cached_hash = 0;

  auto hash_opts = [&]() -> std::uint64_t {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t x) {
      h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    mix(static_cast<std::uint64_t>(opt.max_days));
    mix(static_cast<std::uint64_t>(opt.dt_days * 1000.0));
    mix(static_cast<std::uint64_t>(opt.damage_model));
    mix(static_cast<std::uint64_t>(opt.range_model));
    mix(static_cast<std::uint64_t>(opt.include_beams));
    mix(static_cast<std::uint64_t>(opt.include_missiles));
    mix(static_cast<std::uint64_t>(opt.include_point_defense));
    mix(static_cast<std::uint64_t>(opt.include_shields));
    mix(static_cast<std::uint64_t>(opt.include_shield_regen));
    mix(static_cast<std::uint64_t>(opt.record_timeline));
    return h;
  };

  const std::uint64_t cur_hash = hash_opts();
  if (dirty || cached_a != attacker_fleet || cached_d != defender_fleet || cached_hash != cur_hash) {
    cached_a = attacker_fleet;
    cached_d = defender_fleet;
    cached_hash = cur_hash;

    if (attacker_fleet != kInvalidId && defender_fleet != kInvalidId && attacker_fleet != defender_fleet) {
      cached = nebula4x::forecast_fleet_battle_fleets(sim, attacker_fleet, defender_fleet, opt);
    } else {
      cached = {};
      cached.ok = false;
      cached.message = "Select two different fleets.";
    }
  }

  ImGui::Separator();

  // Results.
  if (!cached.ok) {
    ImGui::Text("Forecast: %s", cached.message.c_str());
    ImGui::End();
    return;
  }

  if (cached.truncated) {
    ImGui::Text("Forecast: %s (winner by remaining HP heuristic)", winner_label(cached.winner));
    ImGui::Text("Simulated %.1f / %d days", cached.days_simulated, opt.max_days);
  } else {
    ImGui::Text("Forecast winner: %s", winner_label(cached.winner));
    ImGui::Text("Time to resolution: %.1f days", cached.days_simulated);
  }
  if (opt.range_model == nebula4x::FleetBattleRangeModel::RangeAdvantage) {
    ImGui::Text("Final separation: %.2f mkm", cached.final_separation_mkm);
  }

  if (ImGui::BeginTable("bf_summary", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("Metric");
    ImGui::TableSetupColumn("Attacker");
    ImGui::TableSetupColumn("Defender");
    ImGui::TableHeadersRow();

    side_summary_table_row_int("Ships (start)", cached.attacker.start_ships, cached.defender.start_ships);
    side_summary_table_row_int("Ships (lost)", cached.attacker.ships_lost, cached.defender.ships_lost);
    side_summary_table_row_int("Ships (end)", cached.attacker.end_ships, cached.defender.end_ships);

    side_summary_table_row("HP (start)", cached.attacker.start_hp, cached.defender.start_hp);
    side_summary_table_row("Shields (start)", cached.attacker.start_shields, cached.defender.start_shields);
    side_summary_table_row("HP (end)", cached.attacker.end_hp, cached.defender.end_hp);
    side_summary_table_row("Shields (end)", cached.attacker.end_shields, cached.defender.end_shields);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Offense (per day)");
    ImGui::TableSetColumnIndex(1);
    ImGui::TableSetColumnIndex(2);

    side_summary_table_row("Beam dmg/day", cached.attacker.beam_damage_per_day, cached.defender.beam_damage_per_day);
    side_summary_table_row("PD dmg/day", cached.attacker.point_defense_damage_per_day,
                           cached.defender.point_defense_damage_per_day);
    side_summary_table_row("Shield regen/day", cached.attacker.shield_regen_per_day, cached.defender.shield_regen_per_day);

    side_summary_table_row("Max beam range (mkm)", cached.attacker.max_beam_range_mkm, cached.defender.max_beam_range_mkm);
    side_summary_table_row("Max missile range (mkm)", cached.attacker.max_missile_range_mkm,
                           cached.defender.max_missile_range_mkm);
    side_summary_table_row("Avg speed (km/s)", cached.attacker.avg_speed_km_s, cached.defender.avg_speed_km_s);

    ImGui::EndTable();
  }

  if (opt.record_timeline) {
    if (ImGui::CollapsingHeader("Timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextUnformatted("Effective HP over time");
      plot_series("Attacker eff HP", cached.attacker_effective_hp);
      plot_series("Defender eff HP", cached.defender_effective_hp);

      ImGui::TextUnformatted("Ship count over time");
      // Convert ints to doubles for plotting helper.
      std::vector<double> a_cnt;
      std::vector<double> d_cnt;
      a_cnt.reserve(cached.attacker_ships.size());
      d_cnt.reserve(cached.defender_ships.size());
      for (int v : cached.attacker_ships) a_cnt.push_back(static_cast<double>(v));
      for (int v : cached.defender_ships) d_cnt.push_back(static_cast<double>(v));
      plot_series("Attacker ships", a_cnt);
      plot_series("Defender ships", d_cnt);

      if (opt.range_model == nebula4x::FleetBattleRangeModel::RangeAdvantage) {
        ImGui::TextUnformatted("Separation (mkm) over time");
        plot_series("Separation", cached.separation_mkm);
      }
    }
  }

  ImGui::Separator();
  ImGui::TextWrapped(
      "Notes: This forecast uses a simplified deterministic model. It ignores many tactical details (targeting doctrine, "
      "sensor quality, ECM/ECCM edge cases, terrain, reinforcements). Use it as a planning tool, not a promise.");

  ImGui::End();
}

}  // namespace nebula4x::ui
