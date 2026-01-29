#include "ui/research_roadmap_window.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/research_planner.h"
#include "nebula4x/core/research_schedule.h"
#include "nebula4x/core/tech_tree.h"
#include "nebula4x/util/time.h"

namespace nebula4x::ui {
namespace {

bool icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
  return it != haystack.end();
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

std::string faction_label(const Faction& f) {
  if (!f.name.empty()) return f.name;
  return "Faction " + std::to_string(static_cast<std::uint64_t>(f.id));
}

struct ResearchRoadmapWindowState {
  Id faction_id{kInvalidId};

  // UI state.
  std::string filter;
  std::vector<std::string> targets;

  // Apply knobs.
  int apply_mode{0};  // 0=Append, 1=Prepend, 2=Replace
  bool set_active{true};
  bool override_active{false};

  // Cached tech ordering for the picker list.
  std::vector<std::string> tech_ids_sorted;
  std::size_t tech_count_cached{0};

  // Derived.
  ResearchPlanResult plan;
  ResearchSchedule schedule_preview;
  std::string apply_error;
};

void ensure_default_faction(ResearchRoadmapWindowState& st, const Simulation& sim, Id selected_ship,
                            Id selected_colony) {
  const auto& factions = sim.state().factions;
  if (factions.empty()) {
    st.faction_id = kInvalidId;
    return;
  }

  // If current selection is valid, keep it.
  if (st.faction_id != kInvalidId && factions.find(st.faction_id) != factions.end()) return;

  // Prefer selection context.
  if (selected_colony != kInvalidId) {
    const auto itc = sim.state().colonies.find(selected_colony);
    if (itc != sim.state().colonies.end()) {
      const Id fid = itc->second.faction_id;
      if (fid != kInvalidId && factions.find(fid) != factions.end()) {
        st.faction_id = fid;
        return;
      }
    }
  }
  if (selected_ship != kInvalidId) {
    const auto its = sim.state().ships.find(selected_ship);
    if (its != sim.state().ships.end()) {
      const Id fid = its->second.faction_id;
      if (fid != kInvalidId && factions.find(fid) != factions.end()) {
        st.faction_id = fid;
        return;
      }
    }
  }

  // Fall back to first faction id (stable-ish).
  st.faction_id = factions.begin()->first;
}

std::unordered_set<std::string> build_known_set(const Faction& f) {
  std::unordered_set<std::string> known;
  known.reserve(f.known_techs.size() * 2 + 32);
  for (const auto& id : f.known_techs) {
    if (!id.empty()) known.insert(id);
  }
  return known;
}

bool prereqs_met(const std::unordered_set<std::string>& known, const TechDef& t) {
  for (const auto& p : t.prereqs) {
    if (p.empty()) return false;
    if (known.find(p) == known.end()) return false;
  }
  return true;
}

}  // namespace

void draw_research_roadmap_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                                 Id& selected_body) {
  (void)selected_body;

  static ResearchRoadmapWindowState st;

  ensure_default_faction(st, sim, selected_ship, selected_colony);

  if (!ImGui::Begin("Research Roadmap", &ui.show_research_roadmap_window)) {
    ImGui::End();
    return;
  }

  const auto& content = sim.content();

  // --- Faction picker ---
  {
    const auto& factions = sim.state().factions;
    const Faction* fac = nullptr;
    auto it = factions.find(st.faction_id);
    if (it != factions.end()) fac = &it->second;

    const std::string current = fac ? faction_label(*fac) : std::string{"<none>"};
    if (ImGui::BeginCombo("Faction", current.c_str())) {
      for (const auto& [fid, f] : factions) {
        const bool selected = (fid == st.faction_id);
        const std::string label = faction_label(f);
        if (ImGui::Selectable(label.c_str(), selected)) {
          st.faction_id = fid;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  // --- Filter ---
  ImGui::InputTextWithHint("Filter", "tech id or name (substring)", &st.filter);

  const auto itf = sim.state().factions.find(st.faction_id);
  if (itf == sim.state().factions.end()) {
    ImGui::TextUnformatted("No faction selected.");
    ImGui::End();
    return;
  }

  Faction& fac_live = sim.state().factions[st.faction_id];
  const auto known = build_known_set(fac_live);

  // Cache a deterministic picker ordering.
  if (st.tech_count_cached != content.techs.size() || st.tech_ids_sorted.empty()) {
    st.tech_ids_sorted.clear();
    st.tech_ids_sorted.reserve(content.techs.size());
    for (const auto& [id, tech] : content.techs) {
      (void)tech;
      st.tech_ids_sorted.push_back(id);
    }
    std::sort(st.tech_ids_sorted.begin(), st.tech_ids_sorted.end(), [&](const std::string& a, const std::string& b) {
      const auto ita = content.techs.find(a);
      const auto itb = content.techs.find(b);
      const std::string an = (ita != content.techs.end()) ? ita->second.name : a;
      const std::string bn = (itb != content.techs.end()) ? itb->second.name : b;
      if (an != bn) return an < bn;
      return a < b;
    });
    st.tech_count_cached = content.techs.size();
  }

  const float avail_w = ImGui::GetContentRegionAvail().x;
  const float avail_h = ImGui::GetContentRegionAvail().y;
  const float left_w = std::max(320.0f, avail_w * 0.55f);

  // Left: tech picker
  ImGui::BeginChild("##tech_picker", ImVec2(left_w, avail_h), true);
  ImGui::TextUnformatted("Tech Picker");
  ImGui::Separator();

  ImGui::Text("Known: %d   Queue: %d   Active: %s", (int)fac_live.known_techs.size(),
              (int)fac_live.research_queue.size(), fac_live.active_research_id.empty() ? "<none>" : fac_live.active_research_id.c_str());
  ImGui::Spacing();

  if (ImGui::BeginTable("##tech_table", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("Add", ImGuiTableColumnFlags_WidthFixed, 42.0f);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Id");
    ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableHeadersRow();

    for (const auto& id : st.tech_ids_sorted) {
      const auto it = content.techs.find(id);
      if (it == content.techs.end()) continue;
      const TechDef& tech = it->second;

      // Filter by id or name.
      if (!icontains(id, st.filter) && !icontains(tech.name, st.filter)) continue;

      const bool is_known = (known.find(id) != known.end());
      const bool is_active = (!fac_live.active_research_id.empty() && fac_live.active_research_id == id);
      const bool is_queued = vec_contains(fac_live.research_queue, id);
      const bool can_start = prereqs_met(known, tech);

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      const bool already_target = vec_contains(st.targets, id);
      if (is_known) {
        ImGui::TextUnformatted("-");
      } else {
        ImGui::BeginDisabled(already_target);
        if (ImGui::SmallButton(("+##add_" + id).c_str())) {
          st.targets.push_back(id);
        }
        ImGui::EndDisabled();
      }

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(tech.name.empty() ? id.c_str() : tech.name.c_str());

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(id.c_str());

      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%.0f", std::max(0.0, tech.cost));

      ImGui::TableSetColumnIndex(4);
      if (is_known) {
        ImGui::TextUnformatted("Known");
      } else if (is_active) {
        ImGui::TextUnformatted("Active");
      } else if (is_queued) {
        ImGui::TextUnformatted("Queued");
      } else if (!can_start) {
        ImGui::TextUnformatted("Blocked");
      } else {
        ImGui::TextUnformatted("Ready");
      }

      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", tech.name.empty() ? id.c_str() : tech.name.c_str());
        ImGui::Separator();
        ImGui::Text("Id: %s", id.c_str());
        ImGui::Text("Cost: %.0f", std::max(0.0, tech.cost));
        ImGui::Text("Prereqs: %d", (int)tech.prereqs.size());
        for (const auto& p : tech.prereqs) {
          if (p.empty()) continue;
          const bool ok = (known.find(p) != known.end());
          ImGui::Text("%s %s", ok ? "[OK]" : "[ ]", p.c_str());
        }
        ImGui::EndTooltip();
      }
    }

    ImGui::EndTable();
  }

  ImGui::EndChild();

  ImGui::SameLine();

  // Right: roadmap + preview + apply
  ImGui::BeginChild("##roadmap", ImVec2(0, avail_h), true);
  ImGui::TextUnformatted("Roadmap");
  ImGui::Separator();

  // Targets list
  ImGui::TextUnformatted("Targets");
  if (st.targets.empty()) {
    ImGui::TextUnformatted("  (Add techs from the left list)");
  } else {
    for (std::size_t i = 0; i < st.targets.size(); ++i) {
      const std::string& tid = st.targets[i];
      const auto it = content.techs.find(tid);
      const std::string name = (it != content.techs.end() && !it->second.name.empty()) ? it->second.name : tid;

      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("X")) {
        st.targets.erase(st.targets.begin() + static_cast<std::ptrdiff_t>(i));
        ImGui::PopID();
        --i;
        continue;
      }
      ImGui::SameLine();
      ImGui::Text("%s (%s)", name.c_str(), tid.c_str());
      ImGui::PopID();
    }
  }

  ImGui::Spacing();
  if (ImGui::SmallButton("Clear Targets")) {
    st.targets.clear();
  }

  ImGui::Separator();

  // Planning and apply options.
  const char* modes[] = {"Append to queue", "Prepend to queue", "Replace queue"};
  ImGui::Combo("Apply Mode", &st.apply_mode, modes, IM_ARRAYSIZE(modes));
  ImGui::Checkbox("Set active to first planned tech", &st.set_active);
  if (st.set_active) {
    ImGui::Checkbox("Override existing active project", &st.override_active);
  }

  // Compute plan.
  st.plan = ResearchPlanResult{};
  st.schedule_preview = ResearchSchedule{};
  st.apply_error.clear();

  if (!st.targets.empty()) {
    st.plan = compute_research_plan(content, fac_live, st.targets);
  }

  if (!st.plan.ok()) {
    ImGui::TextColored(ImVec4(1, 0.25f, 0.25f, 1), "Plan errors:");
    for (const auto& e : st.plan.errors) {
      ImGui::BulletText("%s", e.c_str());
    }
  }

  if (st.targets.empty()) {
    ImGui::EndChild();
    ImGui::End();
    return;
  }

  if (st.plan.ok() && st.plan.plan.tech_ids.empty()) {
    ImGui::TextUnformatted("All targets are already known (or no missing prerequisites).");
  }

  // Preview schedule using a cloned faction with the plan applied.
  if (st.plan.ok() && !st.plan.plan.tech_ids.empty()) {
    Faction fac_preview = fac_live;

    ResearchQueueApplyOptions opt;
    opt.mode = (st.apply_mode == 2) ? ResearchQueueApplyMode::Replace
             : (st.apply_mode == 1) ? ResearchQueueApplyMode::Prepend
                                    : ResearchQueueApplyMode::Append;
    opt.set_active = st.set_active;
    opt.override_active = st.override_active;

    std::string err;
    if (!apply_research_plan(fac_preview, st.plan.plan, opt, &err)) {
      st.apply_error = err;
    } else {
      st.schedule_preview = estimate_research_schedule_for_faction(sim, fac_preview);
    }
  }

  if (!st.apply_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.25f, 0.25f, 1), "Apply preview error: %s", st.apply_error.c_str());
  }

  ImGui::Separator();

  // Roadmap table
  if (st.plan.ok() && !st.plan.plan.tech_ids.empty()) {
    ImGui::Text("Planned techs: %d   Total cost: %.0f", (int)st.plan.plan.tech_ids.size(), st.plan.plan.total_cost);

    // Build completion lookup from schedule preview.
    std::unordered_map<std::string, int> completion_day;
    completion_day.reserve(st.schedule_preview.items.size() * 2 + 8);
    for (const auto& item : st.schedule_preview.items) {
      completion_day[item.tech_id] = item.end_day;
    }

    if (ImGui::BeginTable("##roadmap_table", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 220))) {
      ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 26.0f);
      ImGui::TableSetupColumn("Tech");
      ImGui::TableSetupColumn("Id");
      ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableHeadersRow();

      for (std::size_t i = 0; i < st.plan.plan.tech_ids.size(); ++i) {
        const std::string& tid = st.plan.plan.tech_ids[i];
        const auto it = content.techs.find(tid);
        const TechDef* tech = (it != content.techs.end()) ? &it->second : nullptr;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%d", (int)(i + 1));

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted((tech && !tech->name.empty()) ? tech->name.c_str() : tid.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(tid.c_str());

        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.0f", tech ? std::max(0.0, tech->cost) : 0.0);

        ImGui::TableSetColumnIndex(4);
        auto itd = completion_day.find(tid);
        if (itd != completion_day.end()) {
          const Date eta = sim.state().date.add_days(itd->second);
          ImGui::Text("%s", eta.to_string().c_str());
        } else {
          ImGui::TextUnformatted("-");
        }
      }

      ImGui::EndTable();
    }

    // Schedule summary
    if (st.schedule_preview.ok) {
      ImGui::Text("RP/day: base %.1f  multiplier %.2f  effective %.1f",
                  st.schedule_preview.base_rp_per_day, st.schedule_preview.research_multiplier,
                  st.schedule_preview.effective_rp_per_day);
      if (st.schedule_preview.stalled) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Forecast stalled: %s", st.schedule_preview.stall_reason.c_str());
      }
      if (st.schedule_preview.truncated) {
        ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Forecast truncated: %s", st.schedule_preview.truncated_reason.c_str());
      }
    } else {
      ImGui::TextColored(ImVec4(1, 0.25f, 0.25f, 1), "Forecast failed.");
      if (!st.schedule_preview.stall_reason.empty()) {
        ImGui::TextUnformatted(st.schedule_preview.stall_reason.c_str());
      }
    }

    ImGui::Separator();

    // Apply to live faction.
    if (ImGui::Button("Apply Plan to Faction")) {
      ResearchQueueApplyOptions opt;
      opt.mode = (st.apply_mode == 2) ? ResearchQueueApplyMode::Replace
               : (st.apply_mode == 1) ? ResearchQueueApplyMode::Prepend
                                      : ResearchQueueApplyMode::Append;
      opt.set_active = st.set_active;
      opt.override_active = st.override_active;

      std::string err;
      if (!apply_research_plan(fac_live, st.plan.plan, opt, &err)) {
        st.apply_error = err;
      } else {
        st.apply_error.clear();
      }
    }
    if (!st.apply_error.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1, 0.25f, 0.25f, 1), "%s", st.apply_error.c_str());
    }
  }

  ImGui::EndChild();
  ImGui::End();
}

}  // namespace nebula4x::ui
