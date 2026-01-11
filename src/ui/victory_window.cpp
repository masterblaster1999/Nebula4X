#include "ui/victory_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nebula4x::ui {
namespace {

const char* victory_reason_label(VictoryReason r) {
  switch (r) {
    case VictoryReason::None:
      return "None";
    case VictoryReason::ScoreThreshold:
      return "Score Threshold";
    case VictoryReason::LastFactionStanding:
      return "Elimination";
  }
  return "?";
}

const char* faction_control_label(FactionControl c) {
  switch (c) {
    case FactionControl::Player:
      return "Player";
    case FactionControl::AI_Empire:
      return "AI";
    case FactionControl::AI_Pirate:
      return "Pirate";
    case FactionControl::AI_Passive:
      return "Passive";
  }
  return "?";
}

double sane_nonneg(double v, double fallback) {
  if (!std::isfinite(v)) return fallback;
  if (v < 0.0) return 0.0;
  return v;
}

void sanitize_rules(VictoryRules& r) {
  r.score_threshold = sane_nonneg(r.score_threshold, 0.0);
  r.score_lead_margin = sane_nonneg(r.score_lead_margin, 0.0);

  r.score_colony_points = sane_nonneg(r.score_colony_points, 100.0);
  r.score_population_per_million = sane_nonneg(r.score_population_per_million, 1.0);
  r.score_installation_cost_mult = sane_nonneg(r.score_installation_cost_mult, 0.1);
  r.score_ship_mass_ton_mult = sane_nonneg(r.score_ship_mass_ton_mult, 0.05);
  r.score_known_tech_points = sane_nonneg(r.score_known_tech_points, 5.0);
  r.score_discovered_system_points = sane_nonneg(r.score_discovered_system_points, 10.0);
  r.score_discovered_anomaly_points = sane_nonneg(r.score_discovered_anomaly_points, 5.0);
}

std::string faction_name_or_id(const GameState& s, Id fid) {
  auto it = s.factions.find(fid);
  if (it != s.factions.end()) return it->second.name;
  return std::to_string(static_cast<unsigned long long>(fid));
}

}  // namespace

void draw_victory_window(Simulation& sim, UIState& ui) {
  if (!ui.show_victory_window) return;

  auto& s = sim.state();
  auto& rules = s.victory_rules;
  auto& vstate = s.victory_state;

  ImGui::SetNextWindowSize(ImVec2(860, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Victory & Score", &ui.show_victory_window)) {
    ImGui::End();
    return;
  }

  // --- Game over banner ---
  if (vstate.game_over) {
    const std::string winner = faction_name_or_id(s, vstate.winner_faction_id);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "GAME OVER");
    ImGui::Text("Winner: %s", winner.c_str());
    ImGui::Text("Reason: %s", victory_reason_label(vstate.reason));
    ImGui::Text("Victory day: %lld", static_cast<long long>(vstate.victory_day));
    if (vstate.reason == VictoryReason::ScoreThreshold) {
      ImGui::Text("Winner score: %.1f", vstate.winner_score);
    }

    if (ImGui::Button("Clear Game Over (Continue simulation)")) {
      vstate = VictoryState{};
    }
    ImGui::Separator();
  }

  // --- Rules editor ---
  if (ImGui::CollapsingHeader("Victory Rules", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Reset rules to defaults")) {
      rules = VictoryRules{};
    }

    ImGui::SameLine();
    ImGui::Checkbox("Enabled", &rules.enabled);

    ImGui::Checkbox("Exclude Pirates from victory checks", &rules.exclude_pirates);
    ImGui::Checkbox("Elimination victory", &rules.elimination_enabled);
    ImGui::SameLine();
    ImGui::Checkbox("Alive requires colony", &rules.elimination_requires_colony);

    ImGui::InputDouble("Score threshold (0 disables)", &rules.score_threshold, 100.0, 1000.0, "%.1f");
    ImGui::InputDouble("Lead margin (0 allows ties)", &rules.score_lead_margin, 10.0, 100.0, "%.1f");

    ImGui::Separator();
    ImGui::Text("Score weights (points)");
    ImGui::InputDouble("Per colony", &rules.score_colony_points, 10.0, 100.0, "%.1f");
    ImGui::InputDouble("Per million population", &rules.score_population_per_million, 0.1, 1.0, "%.3f");
    ImGui::InputDouble("Per installation construction cost", &rules.score_installation_cost_mult, 0.01, 0.1, "%.3f");
    ImGui::InputDouble("Per ship mass ton", &rules.score_ship_mass_ton_mult, 0.01, 0.1, "%.3f");
    ImGui::InputDouble("Per known tech", &rules.score_known_tech_points, 1.0, 5.0, "%.1f");
    ImGui::InputDouble("Per discovered system", &rules.score_discovered_system_points, 1.0, 10.0, "%.1f");
    ImGui::InputDouble("Per discovered anomaly", &rules.score_discovered_anomaly_points, 1.0, 10.0, "%.1f");

    sanitize_rules(rules);
  }

  ImGui::Separator();

  // --- Scoreboard ---
  ImGui::Text("Scoreboard");

  static bool show_breakdown = false;
  ImGui::Checkbox("Show breakdown columns", &show_breakdown);

  const auto entries = sim.compute_scoreboard(rules);

  if (rules.score_threshold > 0.0) {
    ImGui::Text("Score victory threshold: %.1f (lead margin %.1f)", rules.score_threshold, rules.score_lead_margin);
  }

  const int cols = show_breakdown ? 12 : 6;
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

  if (ImGui::BeginTable("##scoreboard_table", cols, flags, ImVec2(0, 0))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Faction", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Eligible", ImGuiTableColumnFlags_WidthFixed, 55.0f);
    ImGui::TableSetupColumn("Alive", ImGuiTableColumnFlags_WidthFixed, 45.0f);
    ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 70.0f);

    if (show_breakdown) {
      ImGui::TableSetupColumn("Colonies", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Pop", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Ships", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Inst", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Tech", ImGuiTableColumnFlags_WidthFixed, 70.0f);
      ImGui::TableSetupColumn("Explore", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    }

    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
      const auto& e = entries[static_cast<std::size_t>(i)];
      const double total = e.score.total_points();

      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%d", i + 1);

      ImGui::TableSetColumnIndex(1);
      std::string name = e.faction_name;
      if (vstate.game_over && e.faction_id == vstate.winner_faction_id) {
        name += "  (Winner)";
      }
      ImGui::TextUnformatted(name.c_str());

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(faction_control_label(e.control));

      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(e.eligible_for_victory ? "Yes" : "No");

      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(e.alive ? "Yes" : "No");

      ImGui::TableSetColumnIndex(5);
      if (rules.score_threshold > 0.0 && total >= rules.score_threshold) {
        ImGui::TextColored(ImVec4(0.25f, 0.9f, 0.25f, 1.0f), "%.1f", total);
      } else {
        ImGui::Text("%.1f", total);
      }

      if (show_breakdown) {
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%.1f", e.score.colonies_points);

        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%.1f", e.score.population_points);

        ImGui::TableSetColumnIndex(8);
        ImGui::Text("%.1f", e.score.ships_points);

        ImGui::TableSetColumnIndex(9);
        ImGui::Text("%.1f", e.score.installations_points);

        ImGui::TableSetColumnIndex(10);
        ImGui::Text("%.1f", e.score.tech_points);

        ImGui::TableSetColumnIndex(11);
        ImGui::Text("%.1f", e.score.exploration_points);
      }
    }

    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
