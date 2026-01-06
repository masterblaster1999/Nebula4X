#include "ui/advisor_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <imgui.h>

#include "nebula4x/core/advisor.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {

namespace {

struct AdvisorWindowState {
  Id faction_id{kInvalidId};

  // UI options (persisted in-memory only; low risk to store in ui_prefs later).
  bool auto_refresh{true};
  AdvisorIssueOptions opt{};

  // Filter
  char filter[128]{0};
  bool filter_case_sensitive{false};

  // Cache
  std::uint64_t last_state_gen{0};
  std::int64_t last_day{0};
  int last_hour{0};
  bool dirty{true};

  std::vector<AdvisorIssue> cached;
};

AdvisorWindowState& advisor_state() {
  static AdvisorWindowState s;
  return s;
}

bool str_contains_case_insensitive(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  const std::string h = nebula4x::to_lower(haystack);
  const std::string n = nebula4x::to_lower(needle);
  return h.find(n) != std::string::npos;
}

bool issue_passes_filter(const AdvisorIssue& is, const char* filter, bool case_sensitive) {
  if (!filter || !filter[0]) return true;
  const std::string f(filter);
  if (case_sensitive) {
    // Search in summary + resource + context_id.
    if (is.summary.find(f) != std::string::npos) return true;
    if (is.resource.find(f) != std::string::npos) return true;
    if (is.context_id.find(f) != std::string::npos) return true;
    return false;
  }
  if (str_contains_case_insensitive(is.summary, f)) return true;
  if (str_contains_case_insensitive(is.resource, f)) return true;
  if (str_contains_case_insensitive(is.context_id, f)) return true;
  return false;
}

ImVec4 level_color(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return ImVec4(0.75f, 0.80f, 0.85f, 1.0f);
    case EventLevel::Warn: return ImVec4(1.0f, 0.75f, 0.25f, 1.0f);
    case EventLevel::Error: return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
  }
  return ImVec4(0.75f, 0.80f, 0.85f, 1.0f);
}

const char* level_short(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "INFO";
    case EventLevel::Warn: return "WARN";
    case EventLevel::Error: return "ERR";
  }
  return "INFO";
}

void select_ship(Simulation& sim, UIState& ui, Id ship_id, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  selected_ship = ship_id;
  selected_colony = kInvalidId;
  selected_body = kInvalidId;
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Ship;

  // Also focus the system map when possible.
  if (const auto* sh = find_ptr(sim.state().ships, ship_id)) {
    if (sh->system_id != kInvalidId) {
      sim.state().selected_system = sh->system_id;
      ui.show_map_window = true;
      ui.request_map_tab = MapTab::System;
    }
  }
}

void select_colony(Simulation& sim, UIState& ui, Id colony_id, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  selected_ship = kInvalidId;
  selected_colony = colony_id;
  selected_body = kInvalidId;
  ui.show_details_window = true;
  ui.request_details_tab = DetailsTab::Colony;

  if (const auto* c = find_ptr(sim.state().colonies, colony_id)) {
    if (c->body_id != kInvalidId) {
      selected_body = c->body_id;
      if (const auto* b = find_ptr(sim.state().bodies, c->body_id)) {
        if (b->system_id != kInvalidId) {
          sim.state().selected_system = b->system_id;
          ui.show_map_window = true;
          ui.request_map_tab = MapTab::System;
        }
      }
    }
  }
}

}  // namespace

void draw_advisor_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_advisor_window) return;

  AdvisorWindowState& st = advisor_state();
  auto& s = sim.state();

  // Seed faction selection.
  if (st.faction_id == kInvalidId) {
    st.faction_id = ui.viewer_faction_id;
    if (st.faction_id == kInvalidId && !s.factions.empty()) {
      st.faction_id = s.factions.begin()->first;
    }
  }

  bool open = ui.show_advisor_window;
  if (!ImGui::Begin("Advisor##advisor", &open)) {
    ImGui::End();
    ui.show_advisor_window = open;
    return;
  }
  ui.show_advisor_window = open;

  // --- Controls ---
  ImGui::TextDisabled("Faction:");
  ImGui::SameLine();
  if (ImGui::BeginCombo("##advisor_faction", [&]() {
        const auto* f = find_ptr(s.factions, st.faction_id);
        return f ? f->name.c_str() : "(none)";
      }())) {
    for (const auto& [fid, fac] : s.factions) {
      const bool sel = (fid == st.faction_id);
      if (ImGui::Selectable(fac.name.c_str(), sel)) {
        st.faction_id = fid;
        st.dirty = true;
      }
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine();
  if (ImGui::Checkbox("Auto-refresh", &st.auto_refresh)) {
    // no-op
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Refresh")) {
    st.dirty = true;
  }

  // Filters / options.
  ImGui::Separator();

  if (ImGui::Checkbox("Logistics", &st.opt.include_logistics)) st.dirty = true;
  ImGui::SameLine();
  if (ImGui::Checkbox("Ships", &st.opt.include_ships)) st.dirty = true;
  ImGui::SameLine();
  if (ImGui::Checkbox("Colonies", &st.opt.include_colonies)) st.dirty = true;

  if (st.opt.include_ships) {
    if (ImGui::SliderFloat("Low fuel threshold", &st.opt.low_fuel_fraction, 0.05f, 0.95f, "%.0f%%",
                           ImGuiSliderFlags_AlwaysClamp)) {
      st.dirty = true;
    }
    if (ImGui::SliderFloat("Low HP threshold", &st.opt.low_hp_fraction, 0.10f, 0.99f, "%.0f%%",
                           ImGuiSliderFlags_AlwaysClamp)) {
      st.dirty = true;
    }
  }

  ImGui::InputTextWithHint("Filter", "(substring)", st.filter, sizeof(st.filter));
  ImGui::SameLine();
  ImGui::Checkbox("Aa", &st.filter_case_sensitive);

  // Refresh cache.
  const std::uint64_t gen = sim.state_generation();
  const std::int64_t day = s.date.days_since_epoch();
  const int hour = s.hour_of_day;
  if (st.auto_refresh) {
    if (gen != st.last_state_gen || day != st.last_day || hour != st.last_hour) {
      st.dirty = true;
    }
  }

  if (st.dirty) {
    st.cached = advisor_issues_for_faction(sim, st.faction_id, st.opt);
    st.last_state_gen = gen;
    st.last_day = day;
    st.last_hour = hour;
    st.dirty = false;
  }

  // Summary counts.
  int cnt_log = 0;
  int cnt_ship = 0;
  int cnt_col = 0;
  for (const auto& is : st.cached) {
    if (is.kind == AdvisorIssueKind::LogisticsNeed) ++cnt_log;
    else if (is.kind == AdvisorIssueKind::ShipLowFuel || is.kind == AdvisorIssueKind::ShipDamaged) ++cnt_ship;
    else ++cnt_col;
  }

  ImGui::Separator();
  ImGui::Text("Issues: %d  |  Logistics: %d  Ships: %d  Colonies: %d", (int)st.cached.size(), cnt_log, cnt_ship, cnt_col);
  ImGui::Spacing();

  // --- Issue table ---
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY;

  const float table_h = ImGui::GetContentRegionAvail().y;
  if (ImGui::BeginTable("##advisor_table", 6, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 95.0f);
    ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 180.0f);
    ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableHeadersRow();

    for (std::size_t i = 0; i < st.cached.size(); ++i) {
      const AdvisorIssue& is = st.cached[i];
      if (!issue_passes_filter(is, st.filter, st.filter_case_sensitive)) continue;

      ImGui::TableNextRow();

      // Level
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(level_color(is.level), "%s", level_short(is.level));

      // Kind
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(advisor_issue_kind_label(is.kind));

      // Target
      ImGui::TableSetColumnIndex(2);
      std::string target;
      if (is.ship_id != kInvalidId) {
        if (const auto* sh = find_ptr(s.ships, is.ship_id)) target = sh->name;
        else target = "Ship #" + std::to_string((unsigned long long)is.ship_id);
      } else if (is.colony_id != kInvalidId) {
        if (const auto* c = find_ptr(s.colonies, is.colony_id)) target = c->name;
        else target = "Colony #" + std::to_string((unsigned long long)is.colony_id);
      } else {
        target = "";
      }
      ImGui::TextUnformatted(target.c_str());

      // Resource
      ImGui::TableSetColumnIndex(3);
      if (is.kind == AdvisorIssueKind::LogisticsNeed) {
        ImGui::Text("%s (%s)", is.resource.c_str(), logistics_need_kind_label(is.logistics_kind));
      } else {
        ImGui::TextUnformatted(is.resource.c_str());
      }

      // Summary
      ImGui::TableSetColumnIndex(4);
      ImGui::TextWrapped("%s", is.summary.c_str());

      // Actions
      ImGui::TableSetColumnIndex(5);

      // Navigation.
      if (is.ship_id != kInvalidId) {
        if (ImGui::SmallButton(("Select##ship_" + std::to_string((unsigned long long)is.ship_id)).c_str())) {
          select_ship(sim, ui, is.ship_id, selected_ship, selected_colony, selected_body);
        }
        ImGui::SameLine();
      } else if (is.colony_id != kInvalidId) {
        if (ImGui::SmallButton(("Select##col_" + std::to_string((unsigned long long)is.colony_id)).c_str())) {
          select_colony(sim, ui, is.colony_id, selected_ship, selected_colony, selected_body);
        }
        ImGui::SameLine();
      }

      // Quick fixes for ship issues.
      bool did_fix = false;
      if (is.ship_id != kInvalidId) {
        if (auto* sh = find_ptr(sim.state().ships, is.ship_id)) {
          if (is.kind == AdvisorIssueKind::ShipLowFuel && !sh->auto_refuel) {
            if (ImGui::SmallButton(("Enable auto-refuel##" + std::to_string((unsigned long long)is.ship_id)).c_str())) {
              sh->auto_refuel = true;
              did_fix = true;
            }
          } else if (is.kind == AdvisorIssueKind::ShipDamaged && !sh->auto_repair) {
            if (ImGui::SmallButton(("Enable auto-repair##" + std::to_string((unsigned long long)is.ship_id)).c_str())) {
              sh->auto_repair = true;
              did_fix = true;
            }
          }
        }
      }

      // Planner shortcuts.
      if (is.kind == AdvisorIssueKind::LogisticsNeed) {
        if (ImGui::SmallButton(("Freight##need_" + std::to_string((unsigned long long)i)).c_str())) {
          ui.show_freight_window = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(("Logistics tab##need_" + std::to_string((unsigned long long)i)).c_str())) {
          ui.show_details_window = true;
          ui.request_details_tab = DetailsTab::Logistics;
          if (is.colony_id != kInvalidId) {
            selected_colony = is.colony_id;
          }
        }
      }

      if (did_fix) {
        // A quick fix mutated state; recompute next frame.
        st.dirty = true;
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
