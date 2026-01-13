#include "ui/shipyard_targets_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nebula4x::ui {

namespace {

struct ShipyardTargetsWindowState {
  Id faction_id{kInvalidId};

  // Add/update control.
  int add_design_idx{0};
  int add_target_count{1};

  // Display options.
  bool show_unmet_only{false};
  bool show_unbuildable{true};

  // Target seeding convenience.
  bool seed_include_manual_pending{false};

  // Lightweight filter over design id/name.
  std::string filter;
  bool filter_case_sensitive{false};
};

ShipyardTargetsWindowState& window_state() {
  static ShipyardTargetsWindowState st;
  return st;
}

std::string to_lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool filter_match(const std::string& haystack, const std::string& filter, bool case_sensitive) {
  if (filter.empty()) return true;
  if (case_sensitive) return haystack.find(filter) != std::string::npos;
  return to_lower_ascii(haystack).find(to_lower_ascii(filter)) != std::string::npos;
}

std::vector<std::pair<Id, std::string>> sorted_factions(const GameState& s) {
  std::vector<std::pair<Id, std::string>> out;
  out.reserve(s.factions.size());
  for (const auto& [id, f] : s.factions) out.push_back({id, f.name});
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second < b.second;
    return a.first < b.first;
  });
  return out;
}

std::vector<std::string> sorted_all_design_ids(const Simulation& sim) {
  std::vector<std::string> ids;
  ids.reserve(sim.content().ship_designs.size() + sim.state().custom_designs.size());

  for (const auto& [id, d] : sim.content().ship_designs) ids.push_back(id);
  for (const auto& [id, d] : sim.state().custom_designs) ids.push_back(id);

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

std::vector<std::string> sorted_buildable_design_ids(const Simulation& sim, Id faction_id) {
  auto ids = sorted_all_design_ids(sim);
  ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) {
              return !sim.is_design_buildable_for_faction(faction_id, id);
            }),
            ids.end());
  return ids;
}

std::string design_label(const Simulation& sim, const std::string& design_id) {
  if (const ShipDesign* d = sim.find_design(design_id)) {
    if (!d->name.empty() && d->name != design_id) return d->name + " (" + design_id + ")";
  }
  return design_id;
}

void focus_colony(Id colony_id, Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  auto& st = sim.state();
  selected_ship = kInvalidId;
  selected_colony = colony_id;
  selected_body = kInvalidId;

  if (const auto* c = find_ptr(st.colonies, colony_id)) {
    selected_body = c->body_id;

    if (const auto* b = find_ptr(st.bodies, c->body_id)) {
      st.selected_system = b->system_id;
    }
    ui.show_map_window = true;
    ui.request_map_tab = MapTab::System;
    ui.request_focus_faction_id = c->faction_id;
  }
}

}  // namespace

void draw_shipyard_targets_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony, Id& selected_body) {
  if (!ui.show_shipyard_targets_window) return;

  auto& st = window_state();
  auto& gs = sim.state();

  const auto factions = sorted_factions(gs);
  if (st.faction_id == kInvalidId) {
    if (ui.viewer_faction_id != kInvalidId) st.faction_id = ui.viewer_faction_id;
    else if (!factions.empty()) st.faction_id = factions.front().first;
  }

  if (!ImGui::Begin("Shipyard Targets", &ui.show_shipyard_targets_window)) {
    ImGui::End();
    return;
  }

  // --- Faction picker ---
  if (factions.empty()) {
    ImGui::TextDisabled("No factions.");
    ImGui::End();
    return;
  }

  {
    const char* preview = "Select...";
    for (const auto& [fid, name] : factions) {
      if (fid == st.faction_id) {
        preview = name.c_str();
        break;
      }
    }

    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("Faction", preview)) {
      for (const auto& [fid, name] : factions) {
        const bool is_selected = (fid == st.faction_id);
        if (ImGui::Selectable(name.c_str(), is_selected)) st.faction_id = fid;
        if (is_selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  Faction* fac = find_ptr(gs.factions, st.faction_id);
  if (!fac) {
    ImGui::TextDisabled("Faction not found.");
    ImGui::End();
    return;
  }

  // --- Aggregate shipyard + fleet counts ---
  int shipyard_colonies = 0;
  int shipyard_colonies_enabled = 0;
  int shipyard_installations = 0;
  int total_shipyard_orders = 0;

  for (const auto& [cid, c] : gs.colonies) {
    if (c.faction_id != st.faction_id) continue;
    const auto it_yard = c.installations.find("shipyard");
    const int yards = (it_yard != c.installations.end()) ? std::max(0, it_yard->second) : 0;
    if (yards <= 0) continue;
    shipyard_colonies += 1;
    shipyard_installations += yards;
    if (c.shipyard_auto_build_enabled) shipyard_colonies_enabled += 1;
    total_shipyard_orders += static_cast<int>(c.shipyard_queue.size());
  }

  std::unordered_map<std::string, int> have_by_design;
  have_by_design.reserve(gs.ships.size());
  for (const auto& [sid, sh] : gs.ships) {
    if (sh.faction_id != st.faction_id) continue;
    if (sh.design_id.empty()) continue;
    have_by_design[sh.design_id] += 1;
  }

  std::unordered_map<std::string, int> pending_manual_by_design;
  std::unordered_map<std::string, int> pending_auto_by_design;
  for (const auto& [cid, c] : gs.colonies) {
    if (c.faction_id != st.faction_id) continue;
    const auto it_yard = c.installations.find("shipyard");
    const int yards = (it_yard != c.installations.end()) ? std::max(0, it_yard->second) : 0;
    if (yards <= 0) continue;

    for (const auto& bo : c.shipyard_queue) {
      if (bo.is_refit()) continue;
      if (bo.design_id.empty()) continue;
      if (bo.auto_queued) pending_auto_by_design[bo.design_id] += 1;
      else pending_manual_by_design[bo.design_id] += 1;
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled("Shipyards: %d colony(ies), %d installation(s) (%d enabled for auto-build), %d queued order(s).",
                      shipyard_colonies, shipyard_installations, shipyard_colonies_enabled, total_shipyard_orders);
  ImGui::TextDisabled(
      "Auto-build rule: targets count existing ships + manual new-build orders. The simulation auto-enqueues build orders (auto_queued=true) to cover the gap.");
  ImGui::Spacing();

  // --- Target convenience actions ---
  if (ImGui::Button("Clear targets")) {
    fac->ship_design_targets.clear();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Include manual pending when seeding", &st.seed_include_manual_pending);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("When seeding targets from the current fleet, optionally add manual shipyard new-build orders.");
  }

  ImGui::SameLine();
  if (ImGui::Button("Seed targets from current fleet")) {
    fac->ship_design_targets.clear();
    for (const auto& [did, have] : have_by_design) {
      int target = have;
      if (st.seed_include_manual_pending) {
        const int man = (pending_manual_by_design.find(did) != pending_manual_by_design.end()) ? pending_manual_by_design[did] : 0;
        target += man;
      }
      if (target > 0) fac->ship_design_targets[did] = target;
    }
  }

  ImGui::Spacing();

  // --- Add/update a target ---
  {
    const auto buildable = sorted_buildable_design_ids(sim, st.faction_id);

    if (buildable.empty()) {
      ImGui::TextDisabled("No buildable ship designs for this faction.");
    } else {
      if (st.add_design_idx < 0) st.add_design_idx = 0;
      if (st.add_design_idx >= static_cast<int>(buildable.size())) st.add_design_idx = static_cast<int>(buildable.size()) - 1;

      const std::string& did = buildable[static_cast<std::size_t>(st.add_design_idx)];
      const std::string label = design_label(sim, did);

      ImGui::SetNextItemWidth(320.0f);
      if (ImGui::BeginCombo("Design", label.c_str())) {
        for (int i = 0; i < static_cast<int>(buildable.size()); ++i) {
          const bool is_selected = (i == st.add_design_idx);
          const std::string l = design_label(sim, buildable[static_cast<std::size_t>(i)]);
          if (ImGui::Selectable(l.c_str(), is_selected)) st.add_design_idx = i;
          if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }

      ImGui::SameLine();
      ImGui::SetNextItemWidth(90.0f);
      ImGui::InputInt("Target##ship_targets_add", &st.add_target_count);
      if (st.add_target_count < 0) st.add_target_count = 0;

      ImGui::SameLine();
      if (ImGui::Button("Set##ship_targets_add")) {
        if (st.add_target_count <= 0) fac->ship_design_targets.erase(did);
        else fac->ship_design_targets[did] = st.add_target_count;
      }
    }
  }

  ImGui::Separator();

  // --- Target table ---
  ImGui::Checkbox("Show unmet only", &st.show_unmet_only);
  ImGui::SameLine();
  ImGui::Checkbox("Show unbuildable targets", &st.show_unbuildable);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(260.0f);
  ImGui::InputTextWithHint("Filter##ship_targets_filter", "design name/id filter", &st.filter);
  ImGui::SameLine();
  ImGui::Checkbox("Aa##ship_targets_case", &st.filter_case_sensitive);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Case-sensitive filter");
  }

  if (fac->ship_design_targets.empty()) {
    ImGui::TextDisabled("No ship design targets set.");
  } else {
    std::vector<std::string> ids;
    ids.reserve(fac->ship_design_targets.size());
    for (const auto& [did, t] : fac->ship_design_targets) {
      if (t > 0) ids.push_back(did);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("ship_targets_table", 7, flags)) {
      ImGui::TableSetupColumn("Design", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Buildable", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Have", ImGuiTableColumnFlags_WidthFixed, 44.0f);
      ImGui::TableSetupColumn("Manual", ImGuiTableColumnFlags_WidthFixed, 54.0f);
      ImGui::TableSetupColumn("Auto", ImGuiTableColumnFlags_WidthFixed, 44.0f);
      ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 64.0f);
      ImGui::TableSetupColumn("Gap", ImGuiTableColumnFlags_WidthFixed, 44.0f);
      ImGui::TableHeadersRow();

      for (const auto& did : ids) {
        const int target = std::max(0, fac->ship_design_targets[did]);
        if (target <= 0) continue;

        const bool buildable = sim.is_design_buildable_for_faction(st.faction_id, did);
        if (!st.show_unbuildable && !buildable) continue;

        const int have = (have_by_design.find(did) != have_by_design.end()) ? have_by_design[did] : 0;
        const int man = (pending_manual_by_design.find(did) != pending_manual_by_design.end()) ? pending_manual_by_design[did] : 0;
        const int aut = (pending_auto_by_design.find(did) != pending_auto_by_design.end()) ? pending_auto_by_design[did] : 0;
        const int gap = target - (have + man + aut);
        if (st.show_unmet_only && gap <= 0) continue;

        const std::string label = design_label(sim, did);
        if (!filter_match(label, st.filter, st.filter_case_sensitive) &&
            !filter_match(did, st.filter, st.filter_case_sensitive)) {
          continue;
        }

        ImGui::TableNextRow();
        ImGui::PushID(did.c_str());

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label.c_str());

        ImGui::TableNextColumn();
        if (buildable) {
          ImGui::TextUnformatted("Yes");
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "No");
        }

        ImGui::TableNextColumn();
        ImGui::Text("%d", have);

        ImGui::TableNextColumn();
        ImGui::Text("%d", man);

        ImGui::TableNextColumn();
        ImGui::Text("%d", aut);

        ImGui::TableNextColumn();
        int tedit = target;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputInt("##target", &tedit, 0, 0)) {
          if (tedit < 0) tedit = 0;
          if (tedit == 0) fac->ship_design_targets.erase(did);
          else fac->ship_design_targets[did] = tedit;
        }

        ImGui::TableNextColumn();
        if (gap > 0) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "%d", gap);
        else ImGui::Text("%d", gap);

        ImGui::PopID();
      }

      ImGui::EndTable();
    }
  }

  ImGui::Separator();
  ImGui::Text("Shipyard colonies");
  ImGui::TextDisabled("Toggling auto-build off cancels any *unstarted* auto-queued orders at that colony.");

  // --- Shipyard colony list ---
  {
    struct YardRow {
      Id colony_id{kInvalidId};
      std::string name;
      int yards{0};
      int queue_len{0};
      bool auto_enabled{true};
      std::string front_order;
    };

    std::vector<YardRow> rows;
    rows.reserve(gs.colonies.size());

    for (const auto& [cid, c] : gs.colonies) {
      if (c.faction_id != st.faction_id) continue;
      const auto it_yard = c.installations.find("shipyard");
      const int yards = (it_yard != c.installations.end()) ? std::max(0, it_yard->second) : 0;
      if (yards <= 0) continue;

      YardRow r;
      r.colony_id = cid;
      r.name = c.name;
      r.yards = yards;
      r.queue_len = static_cast<int>(c.shipyard_queue.size());
      r.auto_enabled = c.shipyard_auto_build_enabled;

      if (!c.shipyard_queue.empty()) {
        const auto& bo = c.shipyard_queue.front();
        if (bo.is_refit()) {
          r.front_order = "Refit " + std::to_string(static_cast<unsigned long long>(bo.refit_ship_id));
        } else if (!bo.design_id.empty()) {
          r.front_order = design_label(sim, bo.design_id);
          if (bo.auto_queued) r.front_order = "[AUTO] " + r.front_order;
        }
      }

      rows.push_back(r);
    }

    std::sort(rows.begin(), rows.end(), [](const YardRow& a, const YardRow& b) {
      if (a.name != b.name) return a.name < b.name;
      return a.colony_id < b.colony_id;
    });

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("shipyard_colonies_table", 5, flags)) {
      ImGui::TableSetupColumn("Colony", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Shipyards", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Auto", ImGuiTableColumnFlags_WidthFixed, 50.0f);
      ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 54.0f);
      ImGui::TableSetupColumn("Front order", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();

      for (const auto& r : rows) {
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(r.colony_id));

        ImGui::TableNextColumn();
        if (ImGui::Selectable(r.name.c_str(), r.colony_id == selected_colony)) {
          focus_colony(r.colony_id, sim, ui, selected_ship, selected_colony, selected_body);
        }

        ImGui::TableNextColumn();
        ImGui::Text("%d", r.yards);

        ImGui::TableNextColumn();
        bool enabled = r.auto_enabled;
        if (ImGui::Checkbox("##auto", &enabled)) {
          if (auto* c = find_ptr(gs.colonies, r.colony_id)) {
            c->shipyard_auto_build_enabled = enabled;
          }
        }

        ImGui::TableNextColumn();
        ImGui::Text("%d", r.queue_len);

        ImGui::TableNextColumn();
        if (r.front_order.empty()) ImGui::TextDisabled("-");
        else ImGui::TextUnformatted(r.front_order.c_str());

        ImGui::PopID();
      }

      ImGui::EndTable();
    }
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
