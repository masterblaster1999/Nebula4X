#include "ui/survey_network_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace nebula4x::ui {

namespace {

struct SurveyNetworkWindowState {
  Id faction_id{kInvalidId};

  bool show_surveyed{true};
  bool show_unsurveyed{true};
  bool show_in_progress_only{false};

  bool replace_queue{false};
  bool issue_to_fleet{false};

  std::string filter;
  bool filter_case_sensitive{false};
};

SurveyNetworkWindowState& window_state() {
  static SurveyNetworkWindowState st;
  return st;
}

std::string to_lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains_substr(const std::string& haystack, const std::string& needle, bool case_sensitive) {
  if (needle.empty()) return true;
  if (case_sensitive) return haystack.find(needle) != std::string::npos;
  return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

struct Row {
  Id system_id{kInvalidId};
  Id jump_id{kInvalidId};
  bool surveyed{false};
  double progress_points{0.0};
  double progress_frac{0.0};
  std::string system_name;
  std::string jump_name;
  std::string dest_label;
};

}  // namespace

void draw_survey_network_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& /*selected_colony*/, Id& /*selected_body*/) {
  if (!ui.show_survey_network_window) return;

  auto& st = window_state();
  const auto& s = sim.state();

  if (st.faction_id == kInvalidId && ui.viewer_faction_id != kInvalidId) {
    st.faction_id = ui.viewer_faction_id;
  }

  if (!ImGui::Begin("Survey Network (Jump Points)", &ui.show_survey_network_window)) {
    ImGui::End();
    return;
  }

  // --- Header / target selection ---
  const bool has_fleet = (ui.selected_fleet_id != kInvalidId) && (s.fleets.find(ui.selected_fleet_id) != s.fleets.end());
  const bool has_ship = (selected_ship != kInvalidId) && (s.ships.find(selected_ship) != s.ships.end());
  const bool can_issue = has_ship || has_fleet;

  if (has_ship && has_fleet) {
    ImGui::Text("Issue orders to:");
    ImGui::SameLine();
    bool ship_mode = !st.issue_to_fleet;
    if (ImGui::RadioButton("Ship", ship_mode)) st.issue_to_fleet = false;
    ImGui::SameLine();
    bool fleet_mode = st.issue_to_fleet;
    if (ImGui::RadioButton("Fleet", fleet_mode)) st.issue_to_fleet = true;
  } else if (has_fleet) {
    st.issue_to_fleet = true;
  } else if (has_ship) {
    st.issue_to_fleet = false;
  } else {
    st.issue_to_fleet = false;
  }

  if (st.issue_to_fleet && has_fleet) {
    const auto it = s.fleets.find(ui.selected_fleet_id);
    ImGui::TextDisabled("Target: Fleet '%s' (%d ships)", it->second.name.c_str(), (int)it->second.ship_ids.size());
  } else if (has_ship) {
    const auto it = s.ships.find(selected_ship);
    ImGui::TextDisabled("Target: Ship '%s'", it->second.name.c_str());
  } else {
    ImGui::TextDisabled("Target: (select a ship or fleet to issue orders)");
  }

  ImGui::Checkbox("Replace current queue when issuing", &st.replace_queue);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("When enabled, issuing a survey order will clear the current queue first.\n"
                      "Use Shift on the System Map to queue orders without clearing.");
  }

  ImGui::Separator();

  // --- Faction selector ---
  {
    ImGui::Text("Faction view");
    std::vector<Id> fac_ids;
    std::vector<const char*> fac_names;
    fac_ids.reserve(s.factions.size());
    fac_names.reserve(s.factions.size());

    for (const auto& [fid, f] : s.factions) {
      fac_ids.push_back(fid);
    }
    std::sort(fac_ids.begin(), fac_ids.end(), [&](Id a, Id b) {
      const auto ia = s.factions.find(a);
      const auto ib = s.factions.find(b);
      const std::string an = (ia != s.factions.end()) ? ia->second.name : std::string();
      const std::string bn = (ib != s.factions.end()) ? ib->second.name : std::string();
      return an < bn;
    });

    std::vector<std::string> fac_name_storage;
    fac_name_storage.reserve(fac_ids.size());
    int idx = 0;
    for (Id fid : fac_ids) {
      const auto it = s.factions.find(fid);
      fac_name_storage.push_back((it != s.factions.end()) ? it->second.name : std::string("(unknown)"));
      fac_names.push_back(fac_name_storage.back().c_str());
      if (fid == st.faction_id) idx = (int)fac_names.size() - 1;
    }

    if (!fac_ids.empty()) {
      if (ImGui::Combo("##survey_faction", &idx, fac_names.data(), (int)fac_names.size())) {
        st.faction_id = fac_ids[(size_t)idx];
      }
    } else {
      ImGui::TextDisabled("(no factions)");
    }
  }

  const auto* fac = find_ptr(s.factions, st.faction_id);

  ImGui::Separator();

  // --- Filters ---
  {
    ImGui::Text("Show");
    ImGui::Checkbox("Surveyed", &st.show_surveyed);
    ImGui::SameLine();
    ImGui::Checkbox("Unsurveyed", &st.show_unsurveyed);
    ImGui::SameLine();
    ImGui::Checkbox("In-progress only", &st.show_in_progress_only);

    ImGui::InputTextWithHint("Filter", "system / jump / destination...", &st.filter);
    ImGui::SameLine();
    ImGui::Checkbox("Aa", &st.filter_case_sensitive);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Case-sensitive filter");
  }

  ImGui::Separator();

  // --- Build rows ---
  std::vector<Row> rows;
  rows.reserve(256);

  const double required_points = sim.cfg().jump_survey_points_required;
  const bool timed_surveys = (ui.fog_of_war && required_points > 1e-9 && fac);

  auto add_rows_for_system = [&](Id sys_id) {
    const auto* sys = find_ptr(s.systems, sys_id);
    if (!sys) return;

    for (Id jid : sys->jump_points) {
      const auto* jp = find_ptr(s.jump_points, jid);
      if (!jp) continue;

      Row r;
      r.system_id = sys_id;
      r.jump_id = jid;
      r.system_name = sys->name;
      r.jump_name = jp->name;

      r.surveyed = (!ui.fog_of_war) || (!fac ? true : sim.is_jump_point_surveyed_by_faction(fac->id, jid));

      if (timed_surveys && !r.surveyed) {
        auto itp = fac->jump_survey_progress.find(jid);
        if (itp != fac->jump_survey_progress.end() && std::isfinite(itp->second) && itp->second > 0.0) {
          r.progress_points = std::max(0.0, itp->second);
          r.progress_frac = std::clamp(r.progress_points / required_points, 0.0, 1.0);
        }
      }

      // Destination label (respect fog-of-war).
      r.dest_label = "(unknown)";
      if (!ui.fog_of_war || r.surveyed) {
        if (jp->linked_jump_id != kInvalidId) {
          if (const auto* other = find_ptr(s.jump_points, jp->linked_jump_id)) {
            if (const auto* dest = find_ptr(s.systems, other->system_id)) {
              if (!ui.fog_of_war || !fac) {
                r.dest_label = dest->name;
              } else {
                const bool dest_known = sim.is_system_discovered_by_faction(fac->id, dest->id);
                r.dest_label = dest_known ? dest->name : std::string("(undiscovered system)");
              }
            }
          }
        }
      }

      // Status filters.
      if (r.surveyed && !st.show_surveyed) continue;
      if (!r.surveyed && !st.show_unsurveyed) continue;
      if (st.show_in_progress_only && (!r.surveyed) && !(r.progress_points > 1e-9)) continue;
      if (st.show_in_progress_only && (r.surveyed)) continue;

      // Text filter.
      if (!st.filter.empty()) {
        if (!contains_substr(r.system_name, st.filter, st.filter_case_sensitive) &&
            !contains_substr(r.jump_name, st.filter, st.filter_case_sensitive) &&
            !contains_substr(r.dest_label, st.filter, st.filter_case_sensitive)) {
          continue;
        }
      }

      rows.push_back(std::move(r));
    }
  };

  if (ui.fog_of_war && fac) {
    for (Id sid : fac->discovered_systems) add_rows_for_system(sid);
  } else {
    for (const auto& [sid, _] : s.systems) add_rows_for_system(sid);
  }

  std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
    if (a.system_name != b.system_name) return a.system_name < b.system_name;
    return a.jump_name < b.jump_name;
  });

  // --- Summary ---
  int unsurveyed = 0;
  int in_progress = 0;
  for (const auto& r : rows) {
    if (!r.surveyed) unsurveyed++;
    if (!r.surveyed && r.progress_points > 1e-9) in_progress++;
  }
  ImGui::TextDisabled("Jump points shown: %d   Unsurveyed: %d   In-progress: %d", (int)rows.size(), unsurveyed, in_progress);

  // --- Table ---
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("##survey_table", can_issue ? 6 : 5, tf, ImVec2(0, 0))) {
    ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableSetupColumn("Jump", ImGuiTableColumnFlags_WidthStretch, 0.20f);
    ImGui::TableSetupColumn("Surveyed", ImGuiTableColumnFlags_WidthFixed, 0.10f);
    ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch, 0.18f);
    ImGui::TableSetupColumn("Destination", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    if (can_issue) ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 0.16f);
    ImGui::TableHeadersRow();

    for (const auto& r : rows) {
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(r.system_name.c_str());

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(r.jump_name.c_str());

      ImGui::TableSetColumnIndex(2);
      ImGui::TextDisabled("%s", r.surveyed ? "Yes" : "No");

      ImGui::TableSetColumnIndex(3);
      if (r.surveyed) {
        ImGui::TextDisabled("-");
      } else if (timed_surveys && required_points > 1e-9) {
        const float frac = (float)std::clamp(r.progress_frac, 0.0, 1.0);
        const int pct = (int)std::round(frac * 100.0);
        std::string label = std::to_string(pct) + "%";
        ImGui::ProgressBar(frac, ImVec2(-1, 0), label.c_str());
      } else {
        ImGui::TextDisabled("(instant)");
      }

      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(r.dest_label.c_str());

      if (can_issue) {
        ImGui::TableSetColumnIndex(5);

        bool disable = r.surveyed || !fac;
        if (disable) ImGui::BeginDisabled();

        ImGui::PushID((int)r.jump_id);

        if (ImGui::SmallButton("Survey")) {
          if (st.replace_queue) {
            if (st.issue_to_fleet && has_fleet) sim.clear_fleet_orders(ui.selected_fleet_id);
            if (!st.issue_to_fleet && has_ship) sim.clear_orders(selected_ship);
          }
          if (st.issue_to_fleet && has_fleet) {
            sim.issue_fleet_survey_jump_point(ui.selected_fleet_id, r.jump_id, /*transit_when_done=*/false, ui.fog_of_war);
          } else if (has_ship) {
            sim.issue_survey_jump_point(selected_ship, r.jump_id, /*transit_when_done=*/false, ui.fog_of_war);
          }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Survey+Transit")) {
          if (st.replace_queue) {
            if (st.issue_to_fleet && has_fleet) sim.clear_fleet_orders(ui.selected_fleet_id);
            if (!st.issue_to_fleet && has_ship) sim.clear_orders(selected_ship);
          }
          if (st.issue_to_fleet && has_fleet) {
            sim.issue_fleet_survey_jump_point(ui.selected_fleet_id, r.jump_id, /*transit_when_done=*/true, ui.fog_of_war);
          } else if (has_ship) {
            sim.issue_survey_jump_point(selected_ship, r.jump_id, /*transit_when_done=*/true, ui.fog_of_war);
          }
        }

        ImGui::PopID();

        if (disable) ImGui::EndDisabled();
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
