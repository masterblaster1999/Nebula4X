#include "ui/terraforming_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/date.h"
#include "nebula4x/core/terraforming_schedule.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

struct TerraformingRow {
  Id body_id{kInvalidId};
  Id system_id{kInvalidId};
  Id nav_colony_id{kInvalidId};

  std::string body_name;
  std::string system_name;

  TerraformingSchedule sched;
};

struct TerraformingWindowState {
  Id faction_id{kInvalidId};

  bool auto_refresh{true};
  bool only_owned_or_controlled{true};
  bool restrict_to_discovered{true};
  bool show_completed{false};
  bool show_stalled{true};
  bool ignore_mineral_costs{false};

  int max_projects{512};
  int max_days{36500};

  char filter[128]{0};

  std::vector<TerraformingRow> rows;
  bool have_rows{false};
  std::int64_t last_day{-1};
  int last_hour{-1};
};

TerraformingWindowState& tws() {
  static TerraformingWindowState s;
  return s;
}

std::string body_label(const Simulation& sim, Id body_id) {
  const auto* b = find_ptr(sim.state().bodies, body_id);
  if (!b) return "(missing body)";
  if (!b->name.empty()) return b->name;
  return "Body #" + std::to_string(body_id);
}

std::string system_label(const Simulation& sim, Id system_id) {
  const auto* s = find_ptr(sim.state().systems, system_id);
  if (!s) return "(missing system)";
  if (!s->name.empty()) return s->name;
  return "System #" + std::to_string(system_id);
}

void focus_body(Simulation& sim, UIState& ui, Id body_id, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  selected_body = body_id;
  selected_colony = kInvalidId;
  selected_ship = kInvalidId;
  if (const auto* b = find_ptr(sim.state().bodies, body_id)) {
    sim.state().selected_system = b->system_id;
  }
  ui.show_map_window = true;
  ui.request_map_tab = MapTab::System;
}

void focus_colony(Simulation& sim, UIState& ui, Id colony_id, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  selected_colony = colony_id;
  selected_body = kInvalidId;
  selected_ship = kInvalidId;
  if (const auto* c = find_ptr(sim.state().colonies, colony_id)) {
    // Colonies live on a body; the system is derived from that body.
    if (const auto* b = find_ptr(sim.state().bodies, c->body_id)) {
      sim.state().selected_system = b->system_id;
    }
  }
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Colony;
}

void recompute_rows(Simulation& sim) {
  auto& s = tws();
  s.rows.clear();

  // Precompute per-body colony info for this faction (for performance on large saves).
  std::unordered_map<Id, Id> min_any_colony;
  std::unordered_map<Id, Id> min_own_colony;
  std::unordered_set<Id> owned_bodies;

  min_any_colony.reserve(sim.state().colonies.size());
  min_own_colony.reserve(sim.state().colonies.size());
  owned_bodies.reserve(sim.state().colonies.size());

  for (const auto& [cid, c] : sim.state().colonies) {
    if (c.body_id == kInvalidId) continue;

    // Deterministic "any colony" choice: smallest id.
    auto it_any = min_any_colony.find(c.body_id);
    if (it_any == min_any_colony.end() || cid < it_any->second) {
      min_any_colony[c.body_id] = cid;
    }

    if (c.faction_id == s.faction_id) {
      owned_bodies.insert(c.body_id);
      auto it_own = min_own_colony.find(c.body_id);
      if (it_own == min_own_colony.end() || cid < it_own->second) {
        min_own_colony[c.body_id] = cid;
      }
    }
  }

  TerraformingScheduleOptions opt;
  opt.max_days = std::max(0, s.max_days);
  opt.ignore_mineral_costs = s.ignore_mineral_costs;

  std::vector<Id> body_ids;
  body_ids.reserve(sim.state().bodies.size());

  // Bodies are the canonical source of terraform targets.
  for (const auto& [bid, b] : sim.state().bodies) {
    const bool has_target = (b.terraforming_target_temp_k > 0.0 || b.terraforming_target_atm > 0.0);
    if (!has_target) continue;

    if (s.only_owned_or_controlled) {
      if (owned_bodies.find(bid) == owned_bodies.end()) continue;
    }

    if (s.restrict_to_discovered) {
      if (!sim.is_system_discovered_by_faction(s.faction_id, b.system_id) &&
          owned_bodies.find(bid) == owned_bodies.end()) {
        continue;
      }
    }

    body_ids.push_back(bid);
  }

  std::sort(body_ids.begin(), body_ids.end());

  const std::string filter_lower = nebula4x::to_lower(std::string(s.filter));

  for (Id bid : body_ids) {
    const auto* b = find_ptr(sim.state().bodies, bid);
    if (!b) continue;

    TerraformingSchedule sched = estimate_terraforming_schedule(sim, bid, opt);
    if (!sched.ok || !sched.has_target) continue;

    if (!s.show_completed && sched.complete && sched.days_to_complete <= 0) continue;
    if (!s.show_stalled && sched.stalled) continue;

    TerraformingRow row;
    row.body_id = bid;
    row.system_id = b->system_id;
    row.body_name = body_label(sim, bid);
    row.system_name = system_label(sim, b->system_id);
    row.sched = std::move(sched);

    // Representative colony for navigation.
    row.nav_colony_id = kInvalidId;
    auto it_own = min_own_colony.find(bid);
    if (it_own != min_own_colony.end()) {
      row.nav_colony_id = it_own->second;
    } else {
      auto it_any = min_any_colony.find(bid);
      if (it_any != min_any_colony.end()) row.nav_colony_id = it_any->second;
    }

    if (!filter_lower.empty()) {
      const std::string hay = nebula4x::to_lower(row.body_name + " " + row.system_name);
      if (hay.find(filter_lower) == std::string::npos) continue;
    }

    s.rows.push_back(std::move(row));
    if ((int)s.rows.size() >= std::max(1, s.max_projects)) break;
  }

  // Sort by ETA (soonest first), then by name.
  std::sort(s.rows.begin(), s.rows.end(), [](const TerraformingRow& a, const TerraformingRow& b) {
    const auto key = [](const TerraformingSchedule& s) -> std::pair<int, int> {
      if (s.complete && s.days_to_complete <= 0) return {0, 0};
      if (s.stalled) return {std::numeric_limits<int>::max() / 2, 0};
      if (s.truncated) return {std::numeric_limits<int>::max() / 2, 1};
      const int eta = (s.days_to_complete > 0) ? s.days_to_complete : (std::numeric_limits<int>::max() / 2);
      return {eta, 2};
    };
    const auto ka = key(a.sched);
    const auto kb = key(b.sched);
    if (ka != kb) return ka < kb;
    return a.body_name < b.body_name;
  });

  s.have_rows = true;
  s.last_day = sim.state().date.days_since_epoch();
  s.last_hour = sim.state().hour_of_day;
}

void ensure_default_faction(Simulation& sim, UIState& ui) {
  auto& s = tws();
  if (s.faction_id != kInvalidId) return;

  s.faction_id = ui.viewer_faction_id;
  if (s.faction_id == kInvalidId && !sim.state().factions.empty()) {
    Id best = sim.state().factions.begin()->first;
    for (const auto& [fid, _] : sim.state().factions) best = std::min(best, fid);
    s.faction_id = best;
  }
}

} // namespace

void draw_terraforming_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_terraforming_window) return;

  ensure_default_faction(sim, ui);

  auto& s = tws();

  ImGui::SetNextWindowSize(ImVec2(1040, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Terraforming Planner", &ui.show_terraforming_window)) {
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted("Empire-wide terraforming overview (best-effort forecast).");
  ImGui::TextUnformatted("Forecast assumes current installations/stockpiles persist; mineral replenishment is ignored unless you tick 'Ignore mineral costs'.");

  // --- Faction selector ---
  {
    std::vector<Id> fids;
    fids.reserve(sim.state().factions.size());
    for (const auto& [fid, _] : sim.state().factions) fids.push_back(fid);
    std::sort(fids.begin(), fids.end());

    std::string cur_label = "(none)";
    if (const auto* fac = find_ptr(sim.state().factions, s.faction_id)) {
      cur_label = fac->name.empty() ? ("Faction " + std::to_string(s.faction_id)) : fac->name;
      cur_label += " (#" + std::to_string(s.faction_id) + ")";
    }

    if (ImGui::BeginCombo("Faction", cur_label.c_str())) {
      for (Id fid : fids) {
        const auto* fac = find_ptr(sim.state().factions, fid);
        std::string label = fac ? (fac->name.empty() ? ("Faction " + std::to_string(fid)) : fac->name) : std::to_string(fid);
        label += " (#" + std::to_string(fid) + ")";
        const bool sel = (fid == s.faction_id);
        if (ImGui::Selectable(label.c_str(), sel)) {
          s.faction_id = fid;
          s.have_rows = false;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  // --- Options ---
  ImGui::Separator();

  ImGui::Checkbox("Auto-refresh on time advance", &s.auto_refresh);
  ImGui::SameLine();
  if (ImGui::Button("Refresh now")) {
    recompute_rows(sim);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(Forecasts can be expensive on huge saves; use caps.)");

  ImGui::Checkbox("Only show owned targets", &s.only_owned_or_controlled);
  ImGui::SameLine();
  ImGui::Checkbox("Restrict to discovered systems", &s.restrict_to_discovered);

  ImGui::Checkbox("Show completed", &s.show_completed);
  ImGui::SameLine();
  ImGui::Checkbox("Show stalled", &s.show_stalled);
  ImGui::SameLine();
  ImGui::Checkbox("Ignore mineral costs", &s.ignore_mineral_costs);

  ImGui::PushItemWidth(140);
  ImGui::InputInt("Max projects", &s.max_projects);
  ImGui::SameLine();
  ImGui::InputInt("Max forecast days", &s.max_days);
  ImGui::PopItemWidth();

  ImGui::InputTextWithHint("Filter", "body or system name...", s.filter, sizeof(s.filter));

  // Auto refresh when time advances.
  if (s.auto_refresh) {
    const bool time_changed = (!s.have_rows || s.last_day != sim.state().date.days_since_epoch() || s.last_hour != sim.state().hour_of_day);
    if (time_changed) {
      recompute_rows(sim);
    }
  }

  if (!s.have_rows) {
    recompute_rows(sim);
  }

  ImGui::Separator();

  if (s.rows.empty()) {
    ImGui::TextDisabled("No terraforming targets found for this faction (or filtered out).");
    ImGui::End();
    return;
  }

  ImGui::TextDisabled("Projects: %d", (int)s.rows.size());

  if (ImGui::BeginTable("terraforming_table", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                        ImVec2(0, 0))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Body", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("Pts/day", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Temp K", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Target K", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Atm", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Target Atm", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableSetupColumn("Minerals", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableHeadersRow();

    for (std::size_t i = 0; i < s.rows.size(); ++i) {
      const auto& row = s.rows[i];
      const auto& sc = row.sched;

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(row.body_name.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(row.system_name.c_str());

      ImGui::TableSetColumnIndex(2);
      if (sc.complete && sc.days_to_complete <= 0) {
        ImGui::TextUnformatted("Complete");
      } else if (sc.stalled) {
        ImGui::TextUnformatted("Stalled");
        if (ImGui::IsItemHovered() && !sc.stall_reason.empty()) {
          ImGui::SetTooltip("%s", sc.stall_reason.c_str());
        }
      } else if (sc.truncated) {
        ImGui::TextUnformatted("Forecast");
        if (ImGui::IsItemHovered() && !sc.truncated_reason.empty()) {
          ImGui::SetTooltip("%s", sc.truncated_reason.c_str());
        }
      } else {
        ImGui::TextUnformatted("In progress");
      }

      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%.1f", sc.points_per_day);

      ImGui::TableSetColumnIndex(4);
      ImGui::Text("%.1f", sc.start_temp_k);

      ImGui::TableSetColumnIndex(5);
      ImGui::Text("%.1f", sc.target_temp_k);

      ImGui::TableSetColumnIndex(6);
      ImGui::Text("%.3f", sc.start_atm);

      ImGui::TableSetColumnIndex(7);
      ImGui::Text("%.3f", sc.target_atm);

      ImGui::TableSetColumnIndex(8);
      if (sc.complete && sc.days_to_complete <= 0) {
        ImGui::TextUnformatted("Done");
      } else if (sc.stalled) {
        ImGui::TextUnformatted("—");
      } else if (sc.days_to_complete > 0) {
        const Date eta_date = sim.state().date.add_days(sc.days_to_complete);
        ImGui::Text("%dd (%s)", sc.days_to_complete, eta_date.to_string().c_str());
      } else {
        ImGui::TextUnformatted("?");
      }

      ImGui::TableSetColumnIndex(9);
      if (s.ignore_mineral_costs || (sc.duranium_per_point <= 0.0 && sc.neutronium_per_point <= 0.0)) {
        ImGui::TextUnformatted("(n/a)");
      } else {
        // Show remaining/available after forecast horizon.
        const double d_rem = std::max(0.0, sc.duranium_available - sc.duranium_consumed);
        const double n_rem = std::max(0.0, sc.neutronium_available - sc.neutronium_consumed);
        ImGui::Text("D %.0f / N %.0f", d_rem, n_rem);
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Start: D %.0f, N %.0f\nConsumed: D %.0f, N %.0f\nCost/pt: D %.3f, N %.3f",
                            sc.duranium_available, sc.neutronium_available, sc.duranium_consumed, sc.neutronium_consumed,
                            sc.duranium_per_point, sc.neutronium_per_point);
        }
      }

      ImGui::TableSetColumnIndex(10);
      {
        const std::string id = std::to_string((std::uint64_t)row.body_id);
        if (ImGui::SmallButton(("Focus##tf_focus_" + id).c_str())) {
          focus_body(sim, ui, row.body_id, selected_ship, selected_colony, selected_body);
        }
        ImGui::SameLine();
        if (row.nav_colony_id != kInvalidId) {
          if (ImGui::SmallButton(("Colony##tf_col_" + id).c_str())) {
            focus_colony(sim, ui, row.nav_colony_id, selected_ship, selected_colony, selected_body);
          }
          ImGui::SameLine();
        }
        if (ImGui::SmallButton(("Clear##tf_clear_" + id).c_str())) {
          (void)sim.clear_terraforming_target(row.body_id);
          s.have_rows = false;
        }
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

} // namespace nebula4x::ui
