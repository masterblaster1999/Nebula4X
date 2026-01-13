#include "ui/ship_profiles_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/ship_profiles.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

std::vector<std::string> sorted_profile_names(const std::unordered_map<std::string, ShipAutomationProfile>& m) {
  std::vector<std::string> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::string unique_profile_name(const std::unordered_map<std::string, ShipAutomationProfile>& profiles,
                                const std::string& base) {
  if (base.empty()) return {};
  if (profiles.find(base) == profiles.end()) return base;

  for (int n = 2; n < 10000; ++n) {
    const std::string cand = base + " (" + std::to_string(n) + ")";
    if (profiles.find(cand) == profiles.end()) return cand;
  }
  return base + " (copy)";
}

std::vector<Id> faction_colonies_sorted_by_name(const GameState& s, Id faction_id) {
  struct Row {
    std::string name;
    Id id{kInvalidId};
  };
  std::vector<Row> rows;
  for (const auto& [cid, c] : s.colonies) {
    if (c.faction_id != faction_id) continue;
    rows.push_back(Row{c.name, cid});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });
  std::vector<Id> ids;
  ids.reserve(rows.size());
  for (const auto& r : rows) ids.push_back(r.id);
  return ids;
}


int apply_profile_to_fleet(GameState& s, const Simulation& sim, Id fleet_id, Id faction_id,
                          const ShipAutomationProfile& p, const ShipProfileApplyOptions& opt) {
  if (fleet_id == kInvalidId) return 0;
  auto* fl = find_ptr(s.fleets, fleet_id);
  if (!fl) return 0;
  int applied = 0;
  for (Id sid : fl->ship_ids) {
    auto* sh = find_ptr(s.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;
    // Fleet missions currently own movement, but profiles are intended for
    // automation/policy; allow applying regardless.
    apply_ship_profile(*sh, p, opt);
    applied++;
  }
  (void)sim;
  return applied;
}

int apply_profile_to_all_ships(GameState& s, Id faction_id, const ShipAutomationProfile& p,
                              const ShipProfileApplyOptions& opt) {
  int applied = 0;
  for (auto& [sid, sh] : s.ships) {
    if (sh.faction_id != faction_id) continue;
    apply_ship_profile(sh, p, opt);
    applied++;
  }
  return applied;
}

}  // namespace

void draw_ship_profiles_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& /*selected_colony*/,
                               Id& /*selected_body*/) {
  if (!ui.show_ship_profiles_window) return;

  auto& s = sim.state();

  // Choose an active faction context.
  static Id active_faction_id = kInvalidId;
  if (active_faction_id == kInvalidId || s.factions.find(active_faction_id) == s.factions.end()) {
    if (selected_ship != kInvalidId) {
      if (const auto* sh = find_ptr(s.ships, selected_ship)) active_faction_id = sh->faction_id;
    }
    if (active_faction_id == kInvalidId && ui.viewer_faction_id != kInvalidId) active_faction_id = ui.viewer_faction_id;
    if (active_faction_id == kInvalidId && !s.factions.empty()) active_faction_id = s.factions.begin()->first;
  }

  if (!ImGui::Begin("Ship Profiles", &ui.show_ship_profiles_window)) {
    ImGui::End();
    return;
  }

  Faction* fac = (active_faction_id != kInvalidId) ? find_ptr(s.factions, active_faction_id) : nullptr;
  if (!fac) {
    ImGui::TextDisabled("No faction selected.");
    ImGui::End();
    return;
  }

  // Faction picker.
  if (ImGui::BeginCombo("Faction", fac->name.c_str())) {
    for (const auto& [fid, f] : s.factions) {
      const bool sel = (fid == active_faction_id);
      if (ImGui::Selectable(f.name.c_str(), sel)) {
        active_faction_id = fid;
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  static std::string selected_profile_name;
  static char profile_filter[64] = "";
  static ShipProfileApplyOptions apply_opt;

  // Ensure selection is valid.
  if (!selected_profile_name.empty() && fac->ship_profiles.find(selected_profile_name) == fac->ship_profiles.end()) {
    selected_profile_name.clear();
  }
  if (selected_profile_name.empty() && !fac->ship_profiles.empty()) {
    selected_profile_name = sorted_profile_names(fac->ship_profiles).front();
  }

  const ImGuiTableFlags layout_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
  if (ImGui::BeginTable("ship_profiles_layout", 2, layout_flags)) {
    ImGui::TableSetupColumn("Profiles", ImGuiTableColumnFlags_WidthFixed, 240.0f);
    ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    // --- Left: profile list ---
    ImGui::TableSetColumnIndex(0);
    {
      ImGui::InputTextWithHint("##ship_profile_filter", "Filter...", profile_filter, IM_ARRAYSIZE(profile_filter));

      if (ImGui::BeginChild("ship_profiles_list", ImVec2(0, 0), true)) {
        const std::string ftxt = nebula4x::to_lower(std::string(profile_filter));
        const auto names = sorted_profile_names(fac->ship_profiles);
        for (const auto& nm : names) {
          if (!ftxt.empty()) {
            const std::string nml = nebula4x::to_lower(nm);
            if (nml.find(ftxt) == std::string::npos) continue;
          }
          const bool sel = (nm == selected_profile_name);
          if (ImGui::Selectable(nm.c_str(), sel)) {
            selected_profile_name = nm;
          }
        }
      }
      ImGui::EndChild();

      ImGui::Separator();

      if (ImGui::Button("New##ship_profile_new")) ImGui::OpenPopup("New ship profile");
      ImGui::SameLine();

      const bool has_sel = !selected_profile_name.empty() &&
                           fac->ship_profiles.find(selected_profile_name) != fac->ship_profiles.end();
      if (!has_sel) ImGui::BeginDisabled();
      if (ImGui::Button("Rename##ship_profile_rename")) ImGui::OpenPopup("Rename ship profile");
      ImGui::SameLine();
      if (ImGui::Button("Delete##ship_profile_delete")) ImGui::OpenPopup("Delete ship profile?");
      if (!has_sel) ImGui::EndDisabled();

      // New profile popup.
      if (ImGui::BeginPopupModal("New ship profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name_buf[64] = "";
        static bool init_from_ship = true;

        ImGui::InputTextWithHint("Name", "e.g. Surveyors", name_buf, IM_ARRAYSIZE(name_buf));
        ImGui::Checkbox("Initialize from selected ship (if valid)", &init_from_ship);

        const bool ok = std::string(name_buf).size() > 0;
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button("Create")) {
          const std::string base(name_buf);
          const std::string nm = unique_profile_name(fac->ship_profiles, base);

          ShipAutomationProfile p;
          if (init_from_ship && selected_ship != kInvalidId) {
            if (const auto* sh = find_ptr(s.ships, selected_ship)) {
              if (sh->faction_id == fac->id) p = make_ship_profile_from_ship(*sh);
            }
          }

          fac->ship_profiles[nm] = std::move(p);
          selected_profile_name = nm;

          name_buf[0] = 0;
          ImGui::CloseCurrentPopup();
        }
        if (!ok) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
          name_buf[0] = 0;
          ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
      }

      // Rename popup.
      if (ImGui::BeginPopupModal("Rename ship profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name_buf[64] = "";
        if (name_buf[0] == 0 && has_sel) {
          std::snprintf(name_buf, sizeof(name_buf), "%s", selected_profile_name.c_str());
        }

        ImGui::InputTextWithHint("New name", "e.g. Tankers", name_buf, IM_ARRAYSIZE(name_buf));

        const bool ok = has_sel && std::string(name_buf).size() > 0;
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button("Apply")) {
          const std::string base(name_buf);
          const std::string nm = unique_profile_name(fac->ship_profiles, base);

          if (nm != selected_profile_name) {
            auto it = fac->ship_profiles.find(selected_profile_name);
            if (it != fac->ship_profiles.end()) {
              ShipAutomationProfile tmp = std::move(it->second);
              fac->ship_profiles.erase(it);
              fac->ship_profiles[nm] = std::move(tmp);
              selected_profile_name = nm;
            }
          }

          name_buf[0] = 0;
          ImGui::CloseCurrentPopup();
        }
        if (!ok) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel##rename_cancel")) {
          name_buf[0] = 0;
          ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
      }

      // Delete popup.
      if (ImGui::BeginPopupModal("Delete ship profile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete profile '%s'?", selected_profile_name.c_str());
        if (ImGui::Button("Delete")) {
          fac->ship_profiles.erase(selected_profile_name);
          selected_profile_name.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##delete_cancel")) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
    }

    // --- Right: profile editor ---
    ImGui::TableSetColumnIndex(1);
    {
      auto itp = fac->ship_profiles.find(selected_profile_name);
      if (itp == fac->ship_profiles.end()) {
        ImGui::TextDisabled("No profile selected.");
      } else {
        ShipAutomationProfile& p = itp->second;

        // Selected ship / fleet context.
        const Ship* sel_ship = (selected_ship != kInvalidId) ? find_ptr(s.ships, selected_ship) : nullptr;
        const bool sel_ship_ok = sel_ship && sel_ship->faction_id == fac->id;

        const Fleet* sel_fleet = (ui.selected_fleet_id != kInvalidId) ? find_ptr(s.fleets, ui.selected_fleet_id) : nullptr;
        const bool sel_fleet_ok = [&]() {
          if (!sel_fleet) return false;
          // Consider the fleet "owned" by this faction if its leader matches.
          if (sel_fleet->leader_ship_id != kInvalidId) {
            if (const auto* lead = find_ptr(s.ships, sel_fleet->leader_ship_id)) return lead->faction_id == fac->id;
          }
          // Otherwise, if any ship matches.
          for (Id sid : sel_fleet->ship_ids) {
            if (const auto* sh = find_ptr(s.ships, sid)) {
              if (sh->faction_id == fac->id) return true;
            }
          }
          return false;
        }();

        ImGui::Text("Profile: %s", selected_profile_name.c_str());
        if (sel_ship_ok) {
          ImGui::TextDisabled("Selected ship: %s", sel_ship->name.c_str());
        } else {
          ImGui::TextDisabled("Selected ship: (none / other faction)");
        }
        if (sel_fleet && sel_fleet_ok) {
          ImGui::TextDisabled("Selected fleet: %s (%zu ships)", sel_fleet->name.c_str(), sel_fleet->ship_ids.size());
        } else {
          ImGui::TextDisabled("Selected fleet: (none / other faction)");
        }

        ImGui::Separator();

        // Apply options.
        ImGui::TextDisabled("Apply options");
        ImGui::Checkbox("Automation", &apply_opt.apply_automation);
        ImGui::SameLine();
        ImGui::Checkbox("Repair priority", &apply_opt.apply_repair_priority);
        ImGui::SameLine();
        ImGui::Checkbox("Power policy", &apply_opt.apply_power_policy);
        ImGui::SameLine();
        ImGui::Checkbox("Sensor mode", &apply_opt.apply_sensor_mode);
        ImGui::SameLine();
        ImGui::Checkbox("Combat doctrine", &apply_opt.apply_combat_doctrine);

        // Capture/apply actions.
        static char action_status[128] = "";

        if (!sel_ship_ok) ImGui::BeginDisabled();
        if (ImGui::Button("Capture from selected ship")) {
          p = make_ship_profile_from_ship(*sel_ship);
          std::snprintf(action_status, sizeof(action_status), "Captured from %s", sel_ship->name.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply to selected ship")) {
          auto* sh = find_ptr(s.ships, sel_ship->id);
          if (sh) {
            apply_ship_profile(*sh, p, apply_opt);
            std::snprintf(action_status, sizeof(action_status), "Applied to %s", sh->name.c_str());
          }
        }
        if (!sel_ship_ok) ImGui::EndDisabled();

        if (!sel_fleet_ok) ImGui::BeginDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Apply to selected fleet")) {
          const int n = apply_profile_to_fleet(s, sim, ui.selected_fleet_id, fac->id, p, apply_opt);
          std::snprintf(action_status, sizeof(action_status), "Applied to %d ships in fleet", n);
        }
        if (!sel_fleet_ok) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Apply to all faction ships")) {
          const int n = apply_profile_to_all_ships(s, fac->id, p, apply_opt);
          std::snprintf(action_status, sizeof(action_status), "Applied to %d ships", n);
        }

        if (action_status[0] != 0) {
          ImGui::TextDisabled("%s", action_status);
        }

        ImGui::Separator();

        // --- Profile editor ---
        ImGui::TextDisabled("Mission automation");
        ImGui::Checkbox("Auto explore", &p.auto_explore);
        ImGui::SameLine();
        ImGui::Checkbox("Auto freight", &p.auto_freight);
        ImGui::SameLine();
        ImGui::Checkbox("Auto troop transport", &p.auto_troop_transport);
        ImGui::SameLine();
        ImGui::Checkbox("Auto salvage", &p.auto_salvage);

        ImGui::Checkbox("Auto mine", &p.auto_mine);
        if (p.auto_mine) {
          ImGui::Indent();
          // Home colony picker.
          {
            std::string cur = "None";
            if (p.auto_mine_home_colony_id != kInvalidId) {
              if (const auto* c = find_ptr(s.colonies, p.auto_mine_home_colony_id)) {
                cur = c->name;
              } else {
                cur = "#" + std::to_string(static_cast<unsigned long long>(p.auto_mine_home_colony_id));
              }
            }
            if (ImGui::BeginCombo("Home colony##mine_home", cur.c_str())) {
              const bool none_sel = (p.auto_mine_home_colony_id == kInvalidId);
              if (ImGui::Selectable("None", none_sel)) p.auto_mine_home_colony_id = kInvalidId;
              if (none_sel) ImGui::SetItemDefaultFocus();

              const auto cols = faction_colonies_sorted_by_name(s, fac->id);
              for (Id cid : cols) {
                const auto* c = find_ptr(s.colonies, cid);
                if (!c) continue;
                const bool sel = (p.auto_mine_home_colony_id == cid);
                if (ImGui::Selectable(c->name.c_str(), sel)) p.auto_mine_home_colony_id = cid;
                if (sel) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
          }

          // Mineral filter (string).
          {
            static std::string last_profile;
            static char mineral_buf[64] = "";
            if (last_profile != selected_profile_name) {
              std::snprintf(mineral_buf, sizeof(mineral_buf), "%s", p.auto_mine_mineral.c_str());
              last_profile = selected_profile_name;
            }

            if (ImGui::InputTextWithHint("Mineral filter##mine_mineral", "(empty = any)", mineral_buf,
                                         IM_ARRAYSIZE(mineral_buf))) {
              p.auto_mine_mineral = std::string(mineral_buf);
            }
          }
          ImGui::Unindent();
        }

        ImGui::Checkbox("Auto colonize", &p.auto_colonize);

        ImGui::Separator();
        ImGui::TextDisabled("Sustainment automation");

        ImGui::Checkbox("Auto refuel", &p.auto_refuel);
        ImGui::SameLine();
        {
          float thr = static_cast<float>(p.auto_refuel_threshold_fraction);
          if (ImGui::SliderFloat("Refuel threshold##refuel_thr", &thr, 0.0f, 1.0f, "%.2f")) {
            p.auto_refuel_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }

        ImGui::Checkbox("Auto tanker", &p.auto_tanker);
        ImGui::SameLine();
        {
          float thr = static_cast<float>(p.auto_tanker_reserve_fraction);
          if (ImGui::SliderFloat("Tanker reserve##tanker_res", &thr, 0.0f, 1.0f, "%.2f")) {
            p.auto_tanker_reserve_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }

        ImGui::Checkbox("Auto repair", &p.auto_repair);
        ImGui::SameLine();
        {
          float thr = static_cast<float>(p.auto_repair_threshold_fraction);
          if (ImGui::SliderFloat("Repair threshold##repair_thr", &thr, 0.0f, 1.0f, "%.2f")) {
            p.auto_repair_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }

        ImGui::Checkbox("Auto rearm", &p.auto_rearm);
        ImGui::SameLine();
        {
          float thr = static_cast<float>(p.auto_rearm_threshold_fraction);
          if (ImGui::SliderFloat("Rearm threshold##rearm_thr", &thr, 0.0f, 1.0f, "%.2f")) {
            p.auto_rearm_threshold_fraction = std::clamp(static_cast<double>(thr), 0.0, 1.0);
          }
        }

        // Repair priority.
        {
          int pr = static_cast<int>(p.repair_priority);
          const char* labels[] = {"Low", "Normal", "High"};
          pr = std::clamp(pr, 0, 2);
          if (ImGui::Combo("Repair priority##ship_prof_repair_prio", &pr, labels, IM_ARRAYSIZE(labels))) {
            pr = std::clamp(pr, 0, 2);
            p.repair_priority = static_cast<RepairPriority>(pr);
          }
        }

        ImGui::Separator();

        // Sensor mode.
        {
          ImGui::TextDisabled("Sensor mode (EMCON)");
          int mode_i = static_cast<int>(p.sensor_mode);
          const char* modes[] = {"Passive", "Normal", "Active"};
          mode_i = std::clamp(mode_i, 0, 2);
          if (ImGui::Combo("Sensor mode##ship_prof_sensor", &mode_i, modes, IM_ARRAYSIZE(modes))) {
            mode_i = std::clamp(mode_i, 0, 2);
            p.sensor_mode = static_cast<SensorMode>(mode_i);
          }
        }

        // Power policy.
        {
          ImGui::Separator();
          ImGui::TextDisabled("Power policy");

          sanitize_power_policy(p.power_policy);

          ImGui::PushID("ship_prof_power_policy");
          bool changed = false;
          changed |= ImGui::Checkbox("Engines", &p.power_policy.engines_enabled);
          ImGui::SameLine();
          changed |= ImGui::Checkbox("Shields", &p.power_policy.shields_enabled);
          ImGui::SameLine();
          changed |= ImGui::Checkbox("Weapons", &p.power_policy.weapons_enabled);
          ImGui::SameLine();
          changed |= ImGui::Checkbox("Sensors", &p.power_policy.sensors_enabled);

          ImGui::TextDisabled("Priority (top = keep online). Drag to reorder:");
          for (int i = 0; i < 4; ++i) {
            const PowerSubsystem subsys = p.power_policy.priority[(std::size_t)i];
            std::string label = std::string(power_subsystem_label(subsys)) + "##prio" + std::to_string(i);
            ImGui::Selectable(label.c_str(), false);

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
              ImGui::SetDragDropPayload("PWR_PRIO_SHIP_PROFILE", &i, sizeof(int));
              ImGui::Text("%s", power_subsystem_label(subsys));
              ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
              if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PWR_PRIO_SHIP_PROFILE")) {
                if (payload->DataSize == sizeof(int)) {
                  const int src = *static_cast<const int*>(payload->Data);
                  if (src >= 0 && src < 4 && src != i) {
                    std::swap(p.power_policy.priority[(std::size_t)src], p.power_policy.priority[(std::size_t)i]);
                    changed = true;
                  }
                }
              }
              ImGui::EndDragDropTarget();
            }
          }

          if (ImGui::SmallButton("Default")) {
            p.power_policy.priority = {PowerSubsystem::Engines, PowerSubsystem::Shields, PowerSubsystem::Weapons,
                                       PowerSubsystem::Sensors};
            changed = true;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Recon")) {
            p.power_policy.priority = {PowerSubsystem::Sensors, PowerSubsystem::Engines, PowerSubsystem::Shields,
                                       PowerSubsystem::Weapons};
            changed = true;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Offense")) {
            p.power_policy.priority = {PowerSubsystem::Weapons, PowerSubsystem::Engines, PowerSubsystem::Shields,
                                       PowerSubsystem::Sensors};
            changed = true;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Defense")) {
            p.power_policy.priority = {PowerSubsystem::Shields, PowerSubsystem::Engines, PowerSubsystem::Weapons,
                                       PowerSubsystem::Sensors};
            changed = true;
          }

          if (changed) sanitize_power_policy(p.power_policy);
          ImGui::PopID();
        }

        // Combat doctrine.
        {
          ImGui::Separator();
          ImGui::TextDisabled("Combat doctrine");

          int mode_i = static_cast<int>(p.combat_doctrine.range_mode);
          const char* mode_labels[] = {"Auto", "Beam", "Missile", "Max", "Min", "Custom"};
          if (ImGui::Combo("Range mode##ship_prof_eng_range_mode", &mode_i, mode_labels,
                           IM_ARRAYSIZE(mode_labels))) {
            mode_i = std::clamp(mode_i, 0, 5);
            p.combat_doctrine.range_mode = static_cast<EngagementRangeMode>(mode_i);
          }

          if (p.combat_doctrine.range_mode == EngagementRangeMode::Custom) {
            double cr = p.combat_doctrine.custom_range_mkm;
            if (ImGui::InputDouble("Custom range (mkm)##ship_prof_eng_custom", &cr, 1.0, 10.0, "%.1f")) {
              p.combat_doctrine.custom_range_mkm = std::max(0.0, cr);
            }
          }

          float frac = static_cast<float>(p.combat_doctrine.range_fraction);
          if (ImGui::SliderFloat("Range fraction##ship_prof_eng_frac", &frac, 0.05f, 1.0f, "%.2f")) {
            p.combat_doctrine.range_fraction = std::clamp(static_cast<double>(frac), 0.0, 1.0);
          }

          double min_r = p.combat_doctrine.min_range_mkm;
          if (ImGui::InputDouble("Min range (mkm)##ship_prof_eng_min", &min_r, 0.05, 0.5, "%.2f")) {
            p.combat_doctrine.min_range_mkm = std::max(0.0, min_r);
          }

          ImGui::Checkbox("Kite if too close##ship_prof_eng_kite", &p.combat_doctrine.kite_if_too_close);
          if (p.combat_doctrine.kite_if_too_close) {
            float db = static_cast<float>(p.combat_doctrine.kite_deadband_fraction);
            if (ImGui::SliderFloat("Kite deadband##ship_prof_eng_db", &db, 0.0f, 0.50f, "%.2f")) {
              p.combat_doctrine.kite_deadband_fraction = std::clamp(static_cast<double>(db), 0.0, 0.90);
            }
          }

          // Reset.
          if (ImGui::SmallButton("Reset##ship_prof_eng_reset")) {
            p.combat_doctrine = ShipCombatDoctrine{};
          }

          // Optional preview based on selected ship.
          if (sel_ship_ok) {
            const ShipDesign* d = sim.find_design(sel_ship->design_id);
            if (d) {
              const double beam_range = std::max(0.0, d->weapon_range_mkm);
              const double missile_range = std::max(0.0, d->missile_range_mkm);

              auto select_range = [&](EngagementRangeMode mode) -> double {
                switch (mode) {
                  case EngagementRangeMode::Beam: return beam_range;
                  case EngagementRangeMode::Missile: return missile_range;
                  case EngagementRangeMode::Max: return std::max(beam_range, missile_range);
                  case EngagementRangeMode::Min: {
                    double r = 0.0;
                    if (beam_range > 1e-9) r = beam_range;
                    if (missile_range > 1e-9) r = (r > 1e-9) ? std::min(r, missile_range) : missile_range;
                    return r;
                  }
                  case EngagementRangeMode::Custom: return std::max(0.0, p.combat_doctrine.custom_range_mkm);
                  case EngagementRangeMode::Auto:
                  default:
                    return (beam_range > 1e-9) ? beam_range : ((missile_range > 1e-9) ? missile_range : 0.0);
                }
              };

              const double base = select_range(p.combat_doctrine.range_mode);
              const double frac_cl = std::clamp(p.combat_doctrine.range_fraction, 0.0, 1.0);
              const double min_cl = std::max(0.0, p.combat_doctrine.min_range_mkm);
              double desired = base * frac_cl;
              if (base <= 1e-9) desired = min_cl;
              desired = std::max(desired, min_cl);
              if (!std::isfinite(desired)) desired = min_cl;

              ImGui::TextDisabled("Preview (selected ship design)");
              ImGui::Text("Beam range: %.1f mkm | Missile range: %.1f mkm", beam_range, missile_range);
              ImGui::Text("Desired standoff: %.1f mkm", desired);
            }
          }
        }
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
