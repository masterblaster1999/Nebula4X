#include "ui/colony_profiles_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/colony_profiles.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

std::vector<std::string> sorted_profile_names(const std::unordered_map<std::string, ColonyAutomationProfile>& m) {
  std::vector<std::string> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<std::string> sorted_keys_double(const std::unordered_map<std::string, double>& m) {
  std::vector<std::string> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<std::string> sorted_keys_int(const std::unordered_map<std::string, int>& m) {
  std::vector<std::string> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::string unique_profile_name(const std::unordered_map<std::string, ColonyAutomationProfile>& profiles,
                                const std::string& base) {
  if (base.empty()) return {};
  if (profiles.find(base) == profiles.end()) return base;

  for (int n = 2; n < 10000; ++n) {
    const std::string cand = base + " (" + std::to_string(n) + ")";
    if (profiles.find(cand) == profiles.end()) return cand;
  }
  return base + " (copy)";
}

struct AddDoubleEntryState {
  char key[64]{0};
  double val{0.0};
};

struct AddIntEntryState {
  char key[64]{0};
  int val{0};
};

void draw_double_map_table(const char* id, std::unordered_map<std::string, double>& m, double step) {
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable(id, 3, flags)) return;

  ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 120.0f);
  ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 40.0f);
  ImGui::TableHeadersRow();

  const auto keys = sorted_keys_double(m);
  for (const auto& k : keys) {
    auto it = m.find(k);
    if (it == m.end()) continue;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(k.c_str());

    ImGui::TableSetColumnIndex(1);
    double v = it->second;
    ImGui::PushID((std::string(id) + "_v_" + k).c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputDouble("##v", &v, step, step * 5.0, "%.1f")) {
      v = std::max(0.0, v);
      if (v <= 1e-9) {
        m.erase(k);
      } else {
        m[k] = v;
      }
    }
    ImGui::PopID();

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID((std::string(id) + "_x_" + k).c_str());
    if (ImGui::SmallButton("X")) {
      m.erase(k);
    }
    ImGui::PopID();
  }

  ImGui::EndTable();
}

void draw_int_map_table(const char* id, std::unordered_map<std::string, int>& m, const ContentDB& content) {
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable(id, 4, flags)) return;

  ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 80.0f);
  ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 40.0f);
  ImGui::TableHeadersRow();

  const auto keys = sorted_keys_int(m);
  for (const auto& k : keys) {
    auto it = m.find(k);
    if (it == m.end()) continue;

    const auto itd = content.installations.find(k);
    const std::string nm = (itd == content.installations.end()) ? std::string() : itd->second.name;

    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(k.c_str());

    ImGui::TableSetColumnIndex(1);
    if (!nm.empty()) {
      ImGui::TextUnformatted(nm.c_str());
    } else {
      ImGui::TextDisabled("(unknown)");
    }

    ImGui::TableSetColumnIndex(2);
    int v = it->second;
    ImGui::PushID((std::string(id) + "_v_" + k).c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputInt("##v", &v)) {
      v = std::max(0, v);
      if (v <= 0) {
        m.erase(k);
      } else {
        m[k] = v;
      }
    }
    ImGui::PopID();

    ImGui::TableSetColumnIndex(3);
    ImGui::PushID((std::string(id) + "_x_" + k).c_str());
    if (ImGui::SmallButton("X")) {
      m.erase(k);
    }
    ImGui::PopID();
  }

  ImGui::EndTable();
}

void apply_profile_to_all_colonies(GameState& s, Id faction_id, const ColonyAutomationProfile& p,
                                   const ColonyProfileApplyOptions& opt) {
  for (auto& [cid, c] : s.colonies) {
    if (c.faction_id != faction_id) continue;
    apply_colony_profile(c, p, opt);
  }
}

}  // namespace

void draw_colony_profiles_window(Simulation& sim, UIState& ui, Id& /*selected_ship*/, Id& selected_colony,
                                 Id& /*selected_body*/) {
  if (!ui.show_colony_profiles_window) return;

  auto& s = sim.state();

  // Choose an active faction context.
  static Id active_faction_id = kInvalidId;
  if (active_faction_id == kInvalidId || s.factions.find(active_faction_id) == s.factions.end()) {
    if (selected_colony != kInvalidId) {
      if (const auto* c = find_ptr(s.colonies, selected_colony)) active_faction_id = c->faction_id;
    }
    if (active_faction_id == kInvalidId && ui.viewer_faction_id != kInvalidId) active_faction_id = ui.viewer_faction_id;
    if (active_faction_id == kInvalidId && !s.factions.empty()) active_faction_id = s.factions.begin()->first;
  }

  if (!ImGui::Begin("Colony Profiles", &ui.show_colony_profiles_window)) {
    ImGui::End();
    return;
  }

  // Faction picker.
  {
    const Faction* fac = find_ptr(s.factions, active_faction_id);
    const std::string cur_label =
        fac ? (fac->name + " (Id " + std::to_string(static_cast<unsigned long long>(fac->id)) + ")")
            : std::string("(none)");

    if (ImGui::BeginCombo("Faction", cur_label.c_str())) {
      // Sorted by name for UX.
      std::vector<Id> ids;
      ids.reserve(s.factions.size());
      for (const auto& [id, _] : s.factions) ids.push_back(id);
      std::sort(ids.begin(), ids.end(), [&](Id a, Id b) {
        const auto* fa = find_ptr(s.factions, a);
        const auto* fb = find_ptr(s.factions, b);
        const std::string na = fa ? fa->name : std::string();
        const std::string nb = fb ? fb->name : std::string();
        if (na != nb) return na < nb;
        return a < b;
      });

      for (Id id : ids) {
        const auto* f = find_ptr(s.factions, id);
        if (!f) continue;
        const bool sel = (id == active_faction_id);
        const std::string label = f->name + " (Id " + std::to_string(static_cast<unsigned long long>(id)) + ")";
        if (ImGui::Selectable(label.c_str(), sel)) {
          active_faction_id = id;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }

      ImGui::EndCombo();
    }
  }

  auto* fac = find_ptr(s.factions, active_faction_id);
  if (!fac) {
    ImGui::TextDisabled("No faction.");
    ImGui::End();
    return;
  }

  ImGui::Separator();
  ImGui::TextDisabled(
      "Profiles capture colony targets/reserves so you can re-apply the same automation settings across multiple colonies.");
  ImGui::TextDisabled("Tip: select a colony, then use 'Capture from selected colony'.");

  // Persistent UI state.
  static std::string selected_profile_name;
  static char profile_filter[64] = "";

  static AddDoubleEntryState add_reserves;
  static AddDoubleEntryState add_targets;
  static AddIntEntryState add_installations;

  static ColonyProfileApplyOptions apply_opt;

  // Founding defaults editor state.
  static AddDoubleEntryState add_founding_reserves;
  static AddDoubleEntryState add_founding_targets;
  static AddIntEntryState add_founding_installations;

  static ColonyProfileApplyOptions founding_apply_opt;

  static char founding_label_buf[64] = "";
  static Id founding_label_faction_id = kInvalidId;

  // Ensure selection is valid.
  if (!selected_profile_name.empty() && fac->colony_profiles.find(selected_profile_name) == fac->colony_profiles.end()) {
    selected_profile_name.clear();
  }
  if (selected_profile_name.empty() && !fac->colony_profiles.empty()) {
    selected_profile_name = sorted_profile_names(fac->colony_profiles).front();
  }

  const ImGuiTableFlags layout_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
  if (ImGui::BeginTable("colony_profiles_layout", 2, layout_flags)) {
    ImGui::TableSetupColumn("Profiles", ImGuiTableColumnFlags_WidthFixed, 240.0f);
    ImGui::TableSetupColumn("Editor", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();

    // --- Left: profile list ---
    ImGui::TableSetColumnIndex(0);
    {
      ImGui::InputTextWithHint("##profile_filter", "Filter...", profile_filter, IM_ARRAYSIZE(profile_filter));

      if (ImGui::BeginChild("profiles_list", ImVec2(0, 0), true)) {
        const std::string ftxt = nebula4x::to_lower(std::string(profile_filter));
        const auto names = sorted_profile_names(fac->colony_profiles);
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

      if (ImGui::Button("New##profile_new")) ImGui::OpenPopup("New profile");
      ImGui::SameLine();

      const bool has_sel = !selected_profile_name.empty() &&
                           fac->colony_profiles.find(selected_profile_name) != fac->colony_profiles.end();
      if (!has_sel) ImGui::BeginDisabled();
      if (ImGui::Button("Rename##profile_rename")) ImGui::OpenPopup("Rename profile");
      ImGui::SameLine();
      if (ImGui::Button("Delete##profile_delete")) ImGui::OpenPopup("Delete profile?");
      if (!has_sel) ImGui::EndDisabled();

      // New profile popup.
      if (ImGui::BeginPopupModal("New profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name_buf[64] = "";
        static bool init_from_colony = true;

        ImGui::InputTextWithHint("Name", "e.g. Core Worlds", name_buf, IM_ARRAYSIZE(name_buf));
        ImGui::Checkbox("Initialize from selected colony (if valid)", &init_from_colony);

        const bool ok = std::string(name_buf).size() > 0;
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button("Create")) {
          const std::string base(name_buf);
          const std::string nm = unique_profile_name(fac->colony_profiles, base);

          ColonyAutomationProfile p;
          if (init_from_colony && selected_colony != kInvalidId) {
            if (const auto* c = find_ptr(s.colonies, selected_colony)) {
              if (c->faction_id == fac->id) p = make_colony_profile_from_colony(*c);
            }
          }

          fac->colony_profiles[nm] = std::move(p);
          selected_profile_name = nm;

          // Reset editor add buffers.
          add_reserves.key[0] = 0;
          add_targets.key[0] = 0;
          add_installations.key[0] = 0;

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
      if (ImGui::BeginPopupModal("Rename profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char name_buf[64] = "";
        // Initialize buffer once.
        if (name_buf[0] == 0 && has_sel) {
          std::snprintf(name_buf, sizeof(name_buf), "%s", selected_profile_name.c_str());
        }

        ImGui::InputTextWithHint("New name", "e.g. Frontier Outposts", name_buf, IM_ARRAYSIZE(name_buf));

        const bool ok = has_sel && std::string(name_buf).size() > 0;
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button("Apply")) {
          const std::string base(name_buf);
          const std::string nm = unique_profile_name(fac->colony_profiles, base);

          if (nm != selected_profile_name) {
            auto it = fac->colony_profiles.find(selected_profile_name);
            if (it != fac->colony_profiles.end()) {
              ColonyAutomationProfile tmp = std::move(it->second);
              fac->colony_profiles.erase(it);
              fac->colony_profiles[nm] = std::move(tmp);
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
      if (ImGui::BeginPopupModal("Delete profile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete profile '%s'?", selected_profile_name.c_str());
        if (ImGui::Button("Delete")) {
          fac->colony_profiles.erase(selected_profile_name);
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

    // --- Right: profile editor / founding defaults ---
    ImGui::TableSetColumnIndex(1);
    {
      if (ImGui::BeginTabBar("colony_profiles_tabs")) {
        // === Tab: Profiles ===
        if (ImGui::BeginTabItem("Profiles")) {
          auto itp = fac->colony_profiles.find(selected_profile_name);
          if (itp == fac->colony_profiles.end()) {
            ImGui::TextDisabled("No profile selected.");
          } else {
            ColonyAutomationProfile& p = itp->second;

            ImGui::Text("Profile: %s", selected_profile_name.c_str());

            // Quick actions related to colony founding defaults.
            if (ImGui::Button("Copy to Founding Defaults##profile_copy_to_founding")) {
              fac->colony_founding_profile = p;
              fac->colony_founding_profile_name = selected_profile_name;
              fac->auto_apply_colony_founding_profile = true;

              founding_label_faction_id = fac->id;
              std::snprintf(founding_label_buf, sizeof(founding_label_buf), "%s", selected_profile_name.c_str());

              // Reset founding editor add buffers.
              add_founding_reserves.key[0] = 0;
              add_founding_targets.key[0] = 0;
              add_founding_installations.key[0] = 0;
            }
            ImGui::SameLine();
            {
              const char* lab = fac->colony_founding_profile_name.empty() ? "(unnamed)" : fac->colony_founding_profile_name.c_str();
              ImGui::TextDisabled("Founding defaults: %s%s", lab, fac->auto_apply_colony_founding_profile ? " (enabled)" : " (disabled)");
            }

            // Apply options.
            if (ImGui::CollapsingHeader("Apply options", ImGuiTreeNodeFlags_DefaultOpen)) {
              ImGui::Checkbox("Installation targets", &apply_opt.apply_installation_targets);
              ImGui::Checkbox("Mineral reserves", &apply_opt.apply_mineral_reserves);
              ImGui::Checkbox("Mineral targets", &apply_opt.apply_mineral_targets);
              ImGui::Checkbox("Garrison target", &apply_opt.apply_garrison_target);
              ImGui::Checkbox("Population target", &apply_opt.apply_population_target);
              ImGui::Checkbox("Population reserve", &apply_opt.apply_population_reserve);
            }

            // Action buttons.
            const bool colony_ok =
                (selected_colony != kInvalidId && find_ptr(s.colonies, selected_colony) &&
                 find_ptr(s.colonies, selected_colony)->faction_id == fac->id);

            if (!colony_ok) ImGui::BeginDisabled();
            if (ImGui::Button("Capture from selected colony##profile_capture")) {
              const Colony* c = find_ptr(s.colonies, selected_colony);
              if (c && c->faction_id == fac->id) {
                p = make_colony_profile_from_colony(*c);
              }
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply to selected colony##profile_apply")) {
              Colony* c = find_ptr(s.colonies, selected_colony);
              if (c && c->faction_id == fac->id) {
                apply_colony_profile(*c, p, apply_opt);
              }
            }
            if (!colony_ok) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Apply to ALL colonies (faction)##profile_apply_all")) {
              apply_profile_to_all_colonies(s, fac->id, p, apply_opt);
            }

            if (!colony_ok) {
              ImGui::TextDisabled("(Select a colony belonging to this faction to enable capture/apply-to-selected.)");
            }

            ImGui::Separator();

            // --- Editor: garrison target ---
            ImGui::TextUnformatted("Garrison target");
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputDouble("##garrison_target_profile", &p.garrison_target_strength, 50.0, 200.0, "%.1f")) {
              if (!std::isfinite(p.garrison_target_strength) || p.garrison_target_strength < 0.0) p.garrison_target_strength = 0.0;
            }

            ImGui::Separator();
            // --- Editor: population logistics ---
            ImGui::TextUnformatted("Population logistics");
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputDouble("Target (M)##pop_target_profile", &p.population_target_millions, 10.0, 100.0, "%.0f")) {
              if (!std::isfinite(p.population_target_millions) || p.population_target_millions < 0.0) p.population_target_millions = 0.0;
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::InputDouble("Reserve (M)##pop_reserve_profile", &p.population_reserve_millions, 10.0, 100.0, "%.0f")) {
              if (!std::isfinite(p.population_reserve_millions) || p.population_reserve_millions < 0.0) p.population_reserve_millions = 0.0;
            }
            ImGui::TextDisabled("Used by auto-colonist transports (ships with 'Auto-colonist transport when idle').");

            ImGui::Separator();


            // --- Editor: minerals ---
            if (ImGui::CollapsingHeader("Mineral reserves (export floor)", ImGuiTreeNodeFlags_DefaultOpen)) {
              draw_double_map_table("##profile_reserves", p.mineral_reserves, 100.0);

              ImGui::Separator();
              ImGui::TextUnformatted("Add / set");
              ImGui::InputTextWithHint("Key##reserve_add_key", "e.g. Duranium", add_reserves.key, IM_ARRAYSIZE(add_reserves.key));
              ImGui::InputDouble("Value##reserve_add_val", &add_reserves.val, 100.0, 500.0, "%.1f");
              add_reserves.val = std::max(0.0, add_reserves.val);
              if (ImGui::SmallButton("Set##reserve_set")) {
                const std::string k(add_reserves.key);
                if (!k.empty()) {
                  if (add_reserves.val <= 1e-9) p.mineral_reserves.erase(k);
                  else p.mineral_reserves[k] = add_reserves.val;
                }
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Clear all##reserve_clear")) p.mineral_reserves.clear();
            }

            if (ImGui::CollapsingHeader("Mineral targets (import goal)", ImGuiTreeNodeFlags_DefaultOpen)) {
              draw_double_map_table("##profile_targets", p.mineral_targets, 100.0);

              ImGui::Separator();
              ImGui::TextUnformatted("Add / set");
              ImGui::InputTextWithHint("Key##target_add_key", "e.g. Duranium", add_targets.key, IM_ARRAYSIZE(add_targets.key));
              ImGui::InputDouble("Value##target_add_val", &add_targets.val, 100.0, 500.0, "%.1f");
              add_targets.val = std::max(0.0, add_targets.val);
              if (ImGui::SmallButton("Set##target_set")) {
                const std::string k(add_targets.key);
                if (!k.empty()) {
                  if (add_targets.val <= 1e-9) p.mineral_targets.erase(k);
                  else p.mineral_targets[k] = add_targets.val;
                }
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Clear all##target_clear")) p.mineral_targets.clear();
            }

            ImGui::Separator();

            // --- Editor: installations ---
            if (ImGui::CollapsingHeader("Installation targets (auto-build)", ImGuiTreeNodeFlags_DefaultOpen)) {
              draw_int_map_table("##profile_installations", p.installation_targets, sim.content());

              ImGui::Separator();
              ImGui::TextUnformatted("Add / set");
              ImGui::InputTextWithHint("Id##inst_add_key", "e.g. mine, factory, shipyard", add_installations.key,
                                       IM_ARRAYSIZE(add_installations.key));
              ImGui::InputInt("Target##inst_add_val", &add_installations.val);
              add_installations.val = std::max(0, add_installations.val);
              if (ImGui::SmallButton("Set##inst_set")) {
                const std::string k(add_installations.key);
                if (!k.empty()) {
                  if (add_installations.val <= 0) p.installation_targets.erase(k);
                  else p.installation_targets[k] = add_installations.val;
                }
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Clear all##inst_clear")) p.installation_targets.clear();
            }
          }

          ImGui::EndTabItem();
        }

        // === Tab: Founding Defaults ===
        if (ImGui::BeginTabItem("Founding Defaults")) {
          ColonyAutomationProfile& fp = fac->colony_founding_profile;

          ImGui::TextUnformatted("Colony founding defaults");
          ImGui::TextDisabled(
              "These settings can be auto-applied to newly established colonies when a colonizer completes a Colonize order.");
          ImGui::Checkbox("Auto-apply on colonize##founding_enable", &fac->auto_apply_colony_founding_profile);

          // Keep the label buffer in sync per-faction.
          if (founding_label_faction_id != fac->id) {
            founding_label_faction_id = fac->id;
            std::snprintf(founding_label_buf, sizeof(founding_label_buf), "%s", fac->colony_founding_profile_name.c_str());
          }
          if (ImGui::InputTextWithHint("Label##founding_label", "(optional)", founding_label_buf,
                                       IM_ARRAYSIZE(founding_label_buf))) {
            fac->colony_founding_profile_name = std::string(founding_label_buf);
          }

          ImGui::Separator();

          const bool has_sel_profile = !selected_profile_name.empty() &&
                                       fac->colony_profiles.find(selected_profile_name) != fac->colony_profiles.end();
          const bool colony_ok =
              (selected_colony != kInvalidId && find_ptr(s.colonies, selected_colony) &&
               find_ptr(s.colonies, selected_colony)->faction_id == fac->id);

          if (!has_sel_profile) ImGui::BeginDisabled();
          if (ImGui::Button("Load from selected profile##founding_load_profile")) {
            auto it = fac->colony_profiles.find(selected_profile_name);
            if (it != fac->colony_profiles.end()) {
              fp = it->second;
              fac->colony_founding_profile_name = selected_profile_name;
              std::snprintf(founding_label_buf, sizeof(founding_label_buf), "%s", selected_profile_name.c_str());
              fac->auto_apply_colony_founding_profile = true;

              add_founding_reserves.key[0] = 0;
              add_founding_targets.key[0] = 0;
              add_founding_installations.key[0] = 0;
            }
          }
          if (!has_sel_profile) ImGui::EndDisabled();

          ImGui::SameLine();

          if (!colony_ok) ImGui::BeginDisabled();
          if (ImGui::Button("Capture from selected colony##founding_capture")) {
            const Colony* c = find_ptr(s.colonies, selected_colony);
            if (c && c->faction_id == fac->id) {
              fp = make_colony_profile_from_colony(*c);
              if (fac->colony_founding_profile_name.empty()) {
                fac->colony_founding_profile_name = "From " + c->name;
                std::snprintf(founding_label_buf, sizeof(founding_label_buf), "%s", fac->colony_founding_profile_name.c_str());
              }

              add_founding_reserves.key[0] = 0;
              add_founding_targets.key[0] = 0;
              add_founding_installations.key[0] = 0;
            }
          }
          if (!colony_ok) ImGui::EndDisabled();

          ImGui::SameLine();

          if (ImGui::Button("Save as profile##founding_save_as_profile")) {
            const std::string base = fac->colony_founding_profile_name.empty() ? std::string("Founding Defaults")
                                                                              : fac->colony_founding_profile_name;
            const std::string nm = unique_profile_name(fac->colony_profiles, base);
            fac->colony_profiles[nm] = fp;
            selected_profile_name = nm;
          }

          ImGui::Separator();

          // Optional: apply founding defaults to existing colonies (same UI as profiles).
          if (ImGui::CollapsingHeader("Apply to existing colonies", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Installation targets##founding_opt_installation", &founding_apply_opt.apply_installation_targets);
            ImGui::Checkbox("Mineral reserves##founding_opt_reserves", &founding_apply_opt.apply_mineral_reserves);
            ImGui::Checkbox("Mineral targets##founding_opt_targets", &founding_apply_opt.apply_mineral_targets);
            ImGui::Checkbox("Garrison target##founding_opt_garrison", &founding_apply_opt.apply_garrison_target);
            ImGui::Checkbox("Population target##founding_opt_pop_target", &founding_apply_opt.apply_population_target);
            ImGui::Checkbox("Population reserve##founding_opt_pop_reserve", &founding_apply_opt.apply_population_reserve);

            if (!colony_ok) ImGui::BeginDisabled();
            if (ImGui::Button("Apply to selected colony##founding_apply_selected")) {
              Colony* c = find_ptr(s.colonies, selected_colony);
              if (c && c->faction_id == fac->id) {
                apply_colony_profile(*c, fp, founding_apply_opt);
              }
            }
            if (!colony_ok) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Apply to ALL colonies (faction)##founding_apply_all")) {
              apply_profile_to_all_colonies(s, fac->id, fp, founding_apply_opt);
            }
          }

          ImGui::Separator();

          // --- Editor: garrison target ---
          ImGui::TextUnformatted("Garrison target");
          ImGui::SetNextItemWidth(200.0f);
          if (ImGui::InputDouble("##garrison_target_founding", &fp.garrison_target_strength, 50.0, 200.0, "%.1f")) {
            if (!std::isfinite(fp.garrison_target_strength) || fp.garrison_target_strength < 0.0) fp.garrison_target_strength = 0.0;
          }

          ImGui::Separator();
          // --- Editor: population logistics ---
          ImGui::TextUnformatted("Population logistics");
          ImGui::SetNextItemWidth(200.0f);
          if (ImGui::InputDouble("Target (M)##founding_pop_target", &fp.population_target_millions, 10.0, 100.0, "%.0f")) {
            if (!std::isfinite(fp.population_target_millions) || fp.population_target_millions < 0.0) fp.population_target_millions = 0.0;
          }
          ImGui::SetNextItemWidth(200.0f);
          if (ImGui::InputDouble("Reserve (M)##founding_pop_reserve", &fp.population_reserve_millions, 10.0, 100.0, "%.0f")) {
            if (!std::isfinite(fp.population_reserve_millions) || fp.population_reserve_millions < 0.0) fp.population_reserve_millions = 0.0;
          }
          ImGui::TextDisabled("Used by auto-colonist transports (ships with 'Auto-colonist transport when idle').");

          ImGui::Separator();


          // --- Editor: minerals ---
          if (ImGui::CollapsingHeader("Mineral reserves (export floor)##founding_reserves", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_double_map_table("##founding_reserves_table", fp.mineral_reserves, 100.0);

            ImGui::Separator();
            ImGui::TextUnformatted("Add / set");
            ImGui::InputTextWithHint("Key##founding_reserve_add_key", "e.g. Duranium", add_founding_reserves.key,
                                     IM_ARRAYSIZE(add_founding_reserves.key));
            ImGui::InputDouble("Value##founding_reserve_add_val", &add_founding_reserves.val, 100.0, 500.0, "%.1f");
            add_founding_reserves.val = std::max(0.0, add_founding_reserves.val);
            if (ImGui::SmallButton("Set##founding_reserve_set")) {
              const std::string k(add_founding_reserves.key);
              if (!k.empty()) {
                if (add_founding_reserves.val <= 1e-9) fp.mineral_reserves.erase(k);
                else fp.mineral_reserves[k] = add_founding_reserves.val;
              }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear all##founding_reserve_clear")) fp.mineral_reserves.clear();
          }

          if (ImGui::CollapsingHeader("Mineral targets (import goal)##founding_targets", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_double_map_table("##founding_targets_table", fp.mineral_targets, 100.0);

            ImGui::Separator();
            ImGui::TextUnformatted("Add / set");
            ImGui::InputTextWithHint("Key##founding_target_add_key", "e.g. Duranium", add_founding_targets.key,
                                     IM_ARRAYSIZE(add_founding_targets.key));
            ImGui::InputDouble("Value##founding_target_add_val", &add_founding_targets.val, 100.0, 500.0, "%.1f");
            add_founding_targets.val = std::max(0.0, add_founding_targets.val);
            if (ImGui::SmallButton("Set##founding_target_set")) {
              const std::string k(add_founding_targets.key);
              if (!k.empty()) {
                if (add_founding_targets.val <= 1e-9) fp.mineral_targets.erase(k);
                else fp.mineral_targets[k] = add_founding_targets.val;
              }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear all##founding_target_clear")) fp.mineral_targets.clear();
          }

          ImGui::Separator();

          // --- Editor: installations ---
          if (ImGui::CollapsingHeader("Installation targets (auto-build)##founding_installations", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_int_map_table("##founding_installations_table", fp.installation_targets, sim.content());

            ImGui::Separator();
            ImGui::TextUnformatted("Add / set");
            ImGui::InputTextWithHint("Id##founding_inst_add_key", "e.g. mine, factory, shipyard", add_founding_installations.key,
                                     IM_ARRAYSIZE(add_founding_installations.key));
            ImGui::InputInt("Target##founding_inst_add_val", &add_founding_installations.val);
            add_founding_installations.val = std::max(0, add_founding_installations.val);
            if (ImGui::SmallButton("Set##founding_inst_set")) {
              const std::string k(add_founding_installations.key);
              if (!k.empty()) {
                if (add_founding_installations.val <= 0) fp.installation_targets.erase(k);
                else fp.installation_targets[k] = add_founding_installations.val;
              }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear all##founding_inst_clear")) fp.installation_targets.clear();
          }

          ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
      }
    }
    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
