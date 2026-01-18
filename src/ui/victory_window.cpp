#include "ui/victory_window.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
    case FactionControl::AI_Explorer:
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

  r.score_history_interval_days = std::clamp(r.score_history_interval_days, 1, 3650);
  r.score_history_max_samples = std::clamp(r.score_history_max_samples, 0, 20000);
}

std::string faction_name_or_id(const GameState& s, Id fid) {
  auto it = s.factions.find(fid);
  if (it != s.factions.end()) return it->second.name;
  return std::to_string(static_cast<unsigned long long>(fid));
}


double score_in_sample(const ScoreHistorySample& sh, Id fid) {
  const auto& v = sh.scores;
  auto it = std::lower_bound(v.begin(), v.end(), fid,
                             [](const ScoreHistoryEntry& e, Id id) { return e.faction_id < id; });
  if (it != v.end() && it->faction_id == fid) return it->total;
  return 0.0;
}

struct TrendUiState {
  int show_last_samples{120};
  int plot_top_n{5};
  bool show_etas{true};
  bool show_grid{true};
};

TrendUiState& trend_state() {
  static TrendUiState st;
  return st;
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

    ImGui::Separator();
    ImGui::Text("Score history (analytics)");
    ImGui::Checkbox("Track score history", &rules.score_history_enabled);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Records periodic score snapshots into the save (trend graph / ETA estimates).\nSnapshots are recorded at day boundaries.");
    }

    if (rules.score_history_enabled) {
      ImGui::Indent();
      ImGui::InputInt("Snapshot interval (days)", &rules.score_history_interval_days);
      ImGui::InputInt("Max stored snapshots", &rules.score_history_max_samples);
      if (ImGui::Button("Clear score history")) {
        s.score_history.clear();
      }
      ImGui::Unindent();
    }

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

  ImGui::Separator();
  if (ImGui::CollapsingHeader("Score Trend & Projection", ImGuiTreeNodeFlags_DefaultOpen)) {
    auto& t = trend_state();

    if (!rules.score_history_enabled) {
      ImGui::TextDisabled("Score history tracking is disabled. Enable it above to start recording snapshots.");
    } else if (s.score_history.size() < 2) {
      ImGui::TextDisabled("Not enough score history yet. Snapshots are recorded at day boundaries.");
      ImGui::TextDisabled("Advance time to midnight to record the first snapshots.");
    } else {
      const auto& hist = s.score_history;
      const int available = static_cast<int>(hist.size());

      t.show_last_samples = std::clamp(t.show_last_samples, 8, available);
      t.plot_top_n = std::clamp(t.plot_top_n, 1, std::max(1, static_cast<int>(entries.size())));

      ImGui::SliderInt("Show last samples", &t.show_last_samples, 8, available);
      ImGui::SliderInt("Plot top N factions", &t.plot_top_n, 1, std::min(8, std::max(1, static_cast<int>(entries.size()))));
      ImGui::Checkbox("Show ETA estimates", &t.show_etas);
      ImGui::SameLine();
      ImGui::Checkbox("Grid", &t.show_grid);

      const int n = std::min(t.show_last_samples, available);
      const int start_i = std::max(0, available - n);
      const auto& first = hist[static_cast<std::size_t>(start_i)];
      const auto& last = hist.back();

      std::vector<Id> fids;
      fids.reserve(static_cast<std::size_t>(t.plot_top_n));
      for (int i = 0; i < t.plot_top_n && i < static_cast<int>(entries.size()); ++i) {
        if (entries[static_cast<std::size_t>(i)].faction_id != kInvalidId) {
          fids.push_back(entries[static_cast<std::size_t>(i)].faction_id);
        }
      }

      double ymin = std::numeric_limits<double>::infinity();
      double ymax = -std::numeric_limits<double>::infinity();
      for (Id fid : fids) {
        for (int i = start_i; i < available; ++i) {
          const double y = score_in_sample(hist[static_cast<std::size_t>(i)], fid);
          ymin = std::min(ymin, y);
          ymax = std::max(ymax, y);
        }
      }
      if (!std::isfinite(ymin) || !std::isfinite(ymax)) {
        ymin = 0.0;
        ymax = 1.0;
      }
      if (std::fabs(ymax - ymin) < 1e-9) ymax = ymin + 1.0;

      if (rules.score_threshold > 0.0) {
        ymin = std::min(ymin, rules.score_threshold);
        ymax = std::max(ymax, rules.score_threshold);
      }

      const double pad = 0.06 * (ymax - ymin);
      ymin = std::max(0.0, ymin - pad);
      ymax = ymax + pad;

      const ImVec2 plot_size(ImGui::GetContentRegionAvail().x, 220.0f);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const ImVec2 p1(p0.x + plot_size.x, p0.y + plot_size.y);
      ImGui::InvisibleButton("##score_trend_plot", plot_size);

      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
      const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
      const ImU32 grid = ImGui::GetColorU32(ImGuiCol_BorderShadow);
      dl->AddRectFilled(p0, p1, bg, 3.0f);
      dl->AddRect(p0, p1, border, 3.0f);

      if (t.show_grid) {
        const int grid_lines = 4;
        for (int gi = 1; gi < grid_lines; ++gi) {
          const float frac = (float)gi / (float)grid_lines;
          const float y = p0.y + frac * plot_size.y;
          dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), grid, 1.0f);
        }
      }

      if (rules.score_threshold > 0.0) {
        const double ty = (rules.score_threshold - ymin) / (ymax - ymin);
        const float y = (float)(p1.y - std::clamp(ty, 0.0, 1.0) * plot_size.y);
        const ImU32 thr = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), thr, 2.0f);
      }

      const ImU32 palette[] = {
          ImGui::GetColorU32(ImGuiCol_PlotLines),
          ImGui::GetColorU32(ImGuiCol_PlotHistogram),
          ImGui::GetColorU32(ImGuiCol_ButtonHovered),
          ImGui::GetColorU32(ImGuiCol_HeaderHovered),
          ImGui::GetColorU32(ImGuiCol_TextSelectedBg),
          ImGui::GetColorU32(ImGuiCol_ButtonActive),
      };
      const int palette_n = (int)(sizeof(palette) / sizeof(palette[0]));

      auto to_screen = [&](int idx, double y) -> ImVec2 {
        const float x = p0.x + (n <= 1 ? 0.0f : (float)idx / (float)(n - 1) * plot_size.x);
        const double ty = (y - ymin) / (ymax - ymin);
        const float sy = (float)(p1.y - std::clamp(ty, 0.0, 1.0) * plot_size.y);
        return ImVec2(x, sy);
      };

      for (int si = 0; si < static_cast<int>(fids.size()); ++si) {
        const Id fid = fids[static_cast<std::size_t>(si)];
        std::vector<ImVec2> pts;
        pts.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
          const int hidx = start_i + i;
          const double y = score_in_sample(hist[static_cast<std::size_t>(hidx)], fid);
          pts.push_back(to_screen(i, y));
        }
        const ImU32 col = palette[si % palette_n];
        if (pts.size() >= 2) dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 2.0f);
      }

      ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.0f, p0.y + 6.0f));
      ImGui::Text("%.0f", ymax);
      ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.0f, p1.y - 20.0f));
      ImGui::Text("%.0f", ymin);

      ImGui::Spacing();
      ImGui::TextDisabled("%d samples (every %d days).", n, std::max(1, rules.score_history_interval_days));

      const double t0 = (static_cast<double>(first.day) + (double)first.hour / 24.0);
      const double t1 = (static_cast<double>(last.day) + (double)last.hour / 24.0);
      const double dt_days = t1 - t0;

      for (int si = 0; si < static_cast<int>(fids.size()); ++si) {
        const Id fid = fids[static_cast<std::size_t>(si)];
        const ImU32 col = palette[si % palette_n];

        const double y0 = score_in_sample(first, fid);
        const double y1 = score_in_sample(last, fid);
        const double dy = y1 - y0;
        const double slope = (dt_days > 1e-9) ? (dy / dt_days) : 0.0;

        const std::string name = faction_name_or_id(s, fid);

        ImGui::ColorButton(("##score_series_" + std::to_string(static_cast<unsigned long long>(fid))).c_str(),
                           ImGui::ColorConvertU32ToFloat4(col),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(12.0f, 12.0f));
        ImGui::SameLine();
        ImGui::Text("%s  %.1f  (%+.1f)", name.c_str(), y1, dy);

        if (t.show_etas && rules.score_threshold > 0.0) {
          ImGui::SameLine();
          if (slope > 1e-6 && y1 < rules.score_threshold) {
            const double eta_days = (rules.score_threshold - y1) / slope;
            if (std::isfinite(eta_days) && eta_days >= 0.0 && eta_days < 1e7) {
              ImGui::TextDisabled("ETA ~%.0f days", eta_days);
            } else {
              ImGui::TextDisabled("ETA n/a");
            }
          } else if (y1 >= rules.score_threshold) {
            ImGui::TextDisabled("(at/above threshold)");
          } else {
            ImGui::TextDisabled("ETA n/a");
          }
        }
      }
    }
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
