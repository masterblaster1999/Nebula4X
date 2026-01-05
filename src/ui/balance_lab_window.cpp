#include "ui/balance_lab_window.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nebula4x/util/duel_tournament.h"
#include "nebula4x/util/file_io.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

bool icontains(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return true;
  return nebula4x::to_lower(haystack).find(nebula4x::to_lower(needle)) != std::string::npos;
}

std::vector<std::string> sorted_all_design_ids(const Simulation& sim) {
  std::vector<std::string> ids;
  ids.reserve(sim.content().designs.size() + sim.state().custom_designs.size());

  for (const auto& [id, _] : sim.content().designs) ids.push_back(id);
  for (const auto& [id, _] : sim.state().custom_designs) ids.push_back(id);

  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  std::sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) {
    const auto* da = sim.find_design(a);
    const auto* db = sim.find_design(b);
    const std::string an = da ? da->name : a;
    const std::string bn = db ? db->name : b;
    if (an != bn) return an < bn;
    return a < b;
  });

  return ids;
}

const char* ship_role_label(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "Freighter";
    case ShipRole::Surveyor: return "Surveyor";
    case ShipRole::Combatant: return "Combatant";
    default: return "Unknown";
  }
}

// Builds/refreshes a sandbox Simulation suitable for duel tournaments.
//
// Key detail: duel simulations repeatedly load fresh GameState instances into the Simulation,
// wiping state.custom_designs. To support custom designs across many matchups, we merge the
// current save's custom designs into the sandbox's ContentDB (by id) so they remain discoverable
// via Simulation::find_design throughout the run.
std::unique_ptr<Simulation> make_duel_sandbox(const Simulation& sim) {
  auto sandbox = std::make_unique<Simulation>(sim.content(), sim.cfg());
  sandbox->new_game();

  // Overlay custom designs into the sandbox content so they survive load_game() resets.
  for (const auto& [id, d] : sim.state().custom_designs) {
    sandbox->content().designs[id] = d;
  }

  return sandbox;
}

}  // namespace

void draw_balance_lab_window(Simulation& sim, UIState& ui, Id&, Id&, Id&) {
  if (!ui.show_balance_lab_window) return;

  ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Balance Lab", &ui.show_balance_lab_window)) {
    ImGui::End();
    return;
  }

  ImGui::TextDisabled(
      "Round-robin duel tournaments for balancing ship designs.\n"
      "Tip: Use this with the Design Studio to iterate quickly on blueprint changes.");

  static char search_buf[96] = "";
  static std::vector<std::string> roster;
  static int selected_all_idx = -1;

  static int count_per_side = 1;
  static int runs_per_task = 10;
  static int max_days = 200;
  static double distance_mkm = -1.0;
  static double jitter_mkm = 0.0;
  static bool two_way = true;
  static bool attack_orders = true;
  static int seed = 1;

  static bool compute_elo = true;
  static double elo_initial = 1000.0;
  static double elo_k = 32.0;

  static int tasks_per_frame = 1;
  tasks_per_frame = std::clamp(tasks_per_frame, 1, 64);

  static char out_path[256] = "duel_round_robin.json";
  static std::string last_status;
  static std::string last_error;

  static std::unique_ptr<Simulation> sandbox;
  static std::unique_ptr<nebula4x::DuelRoundRobinRunner> runner;

  // Advance the running tournament a few tasks per frame to keep the UI responsive.
  if (runner && runner->ok() && !runner->done()) {
    runner->step(tasks_per_frame);
    if (!runner->ok()) {
      last_error = runner->error();
    }
  }

  const bool running = (runner && runner->ok() && !runner->done());
  const bool have_result = (runner && runner->ok() && runner->done());

  if (running) {
    const float p = static_cast<float>(runner->progress());
    ImGui::ProgressBar(p, ImVec2(-1, 0));
    ImGui::Text("%d / %d tasks", runner->completed_tasks(), runner->total_tasks());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", runner->current_task_label().c_str());
    if (ImGui::Button("Cancel")) {
      runner.reset();
      sandbox.reset();
      last_status.clear();
      last_error = "Cancelled.";
    }
    ImGui::Separator();
  }

  if (!last_error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Error: %s", last_error.c_str());
  } else if (!last_status.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1), "%s", last_status.c_str());
  }

  auto all_ids = sorted_all_design_ids(sim);
  const std::string search = std::string(search_buf);

  ImGui::BeginDisabled(running);
  if (ImGui::BeginTable("balance_lab_layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Designs", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    ImGui::TableSetupColumn("Roster", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableNextRow();

    // Column 0: All designs.
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("Available Designs");
    ImGui::InputTextWithHint("##balance_search", "Filter by name/id...", search_buf, sizeof(search_buf));

    ImGui::BeginChild("all_designs", ImVec2(0, 320), true);
    int visible_idx = 0;
    for (int i = 0; i < static_cast<int>(all_ids.size()); ++i) {
      const auto& id = all_ids[i];
      const auto* d = sim.find_design(id);
      const std::string label = d ? (d->name + "##" + id) : id;
      const std::string searchable = (d ? (d->name + " " + id) : id);
      if (!icontains(searchable, search)) continue;

      const bool selected = (selected_all_idx == i);
      if (ImGui::Selectable(label.c_str(), selected)) {
        selected_all_idx = i;
      }
      if (ImGui::IsItemHovered() && d) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", d->name.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("id: %s", d->id.c_str());
        ImGui::TextDisabled("role: %s", ship_role_label(d->role));
        ImGui::TextDisabled("hp: %.0f  speed: %.0f km/s", d->max_hp, d->speed_km_s);
        ImGui::TextDisabled("weapon: dmg=%.2f range=%.2f mkm", d->weapon_damage, d->weapon_range_mkm);
        ImGui::EndTooltip();
      }
      ++visible_idx;
    }
    if (visible_idx == 0) {
      ImGui::TextDisabled("(no matches)");
    }
    ImGui::EndChild();

    if (ImGui::Button("Add selected##balance_add") && selected_all_idx >= 0 && selected_all_idx < static_cast<int>(all_ids.size())) {
      const std::string id = all_ids[selected_all_idx];
      if (std::find(roster.begin(), roster.end(), id) == roster.end()) {
        roster.push_back(id);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add all visible##balance_add_all")) {
      for (const auto& id : all_ids) {
        const auto* d = sim.find_design(id);
        const std::string searchable = d ? (d->name + " " + id) : id;
        if (!icontains(searchable, search)) continue;
        if (std::find(roster.begin(), roster.end(), id) == roster.end()) roster.push_back(id);
      }
    }

    // Column 1: Roster.
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("Roster");
    ImGui::BeginChild("roster", ImVec2(0, 320), true);
    if (roster.empty()) {
      ImGui::TextDisabled("(empty) Add at least two designs.");
    } else {
      for (int i = 0; i < static_cast<int>(roster.size()); ++i) {
        ImGui::PushID(i);
        if (ImGui::SmallButton("X")) {
          roster.erase(roster.begin() + i);
          ImGui::PopID();
          break;
        }
        ImGui::SameLine();
        const auto* d = sim.find_design(roster[i]);
        const std::string name = d ? d->name : roster[i];
        ImGui::Text("%s", name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", roster[i].c_str());
        ImGui::PopID();
      }
    }
    ImGui::EndChild();
    if (ImGui::Button("Clear roster")) roster.clear();

    ImGui::EndTable();
  }
  ImGui::EndDisabled();

  ImGui::SeparatorText("Tournament Settings");
  ImGui::BeginDisabled(running);
  ImGui::InputInt("Ships per side", &count_per_side);
  ImGui::InputInt("Runs per matchup direction", &runs_per_task);
  ImGui::InputInt("Max days per run", &max_days);
  ImGui::InputDouble("Initial distance (mkm)", &distance_mkm, 0.1, 1.0, "%.3f");
  ImGui::InputDouble("Spawn jitter (mkm)", &jitter_mkm, 0.1, 1.0, "%.3f");
  ImGui::Checkbox("Two-way matchups (swap sides)", &two_way);
  ImGui::Checkbox("Issue Attack orders", &attack_orders);
  ImGui::InputInt("Seed", &seed);
  ImGui::InputInt("Tasks per frame", &tasks_per_frame);

  ImGui::SeparatorText("Elo");
  ImGui::Checkbox("Compute Elo", &compute_elo);
  ImGui::BeginDisabled(!compute_elo);
  ImGui::InputDouble("Initial Elo", &elo_initial, 1.0, 10.0, "%.1f");
  ImGui::InputDouble("K-factor", &elo_k, 1.0, 5.0, "%.1f");
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  // Start button.
  ImGui::BeginDisabled(running);
  const bool can_start = (roster.size() >= 2);
  ImGui::BeginDisabled(!can_start);
  if (ImGui::Button("Start Tournament")) {
    last_error.clear();
    last_status.clear();

    sandbox = make_duel_sandbox(sim);

    nebula4x::DuelRoundRobinOptions opt;
    opt.count_per_side = count_per_side;
    opt.two_way = two_way;
    opt.compute_elo = compute_elo;
    opt.elo_initial = elo_initial;
    opt.elo_k_factor = elo_k;

    opt.duel.max_days = max_days;
    opt.duel.initial_separation_mkm = distance_mkm;
    opt.duel.position_jitter_mkm = jitter_mkm;
    opt.duel.runs = std::max(1, runs_per_task);
    opt.duel.seed = static_cast<std::uint32_t>(seed);
    opt.duel.issue_attack_orders = attack_orders;
    opt.duel.include_final_state_digest = false;

    runner = std::make_unique<nebula4x::DuelRoundRobinRunner>(*sandbox, roster, opt);
    if (!runner->ok()) {
      last_error = runner->error();
      runner.reset();
      sandbox.reset();
    }
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();

  if (have_result) {
    const auto& res = runner->result();

    ImGui::SeparatorText("Results");

    // Leaderboard.
    std::vector<int> idx(res.design_ids.size());
    for (int i = 0; i < static_cast<int>(idx.size()); ++i) idx[i] = i;
    if (compute_elo) {
      std::sort(idx.begin(), idx.end(), [&](int a, int b) { return res.elo[a] > res.elo[b]; });
    } else {
      std::sort(idx.begin(), idx.end(), [&](int a, int b) { return res.total_wins[a] > res.total_wins[b]; });
    }

    if (ImGui::BeginTable("balance_leaderboard", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
      ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed, 50);
      ImGui::TableSetupColumn("Design", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Elo", ImGuiTableColumnFlags_WidthFixed, 80);
      ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthFixed, 90);
      ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, 80);
      ImGui::TableHeadersRow();

      for (int r = 0; r < static_cast<int>(idx.size()); ++r) {
        const int i = idx[r];
        const int w = res.total_wins[i];
        const int l = res.total_losses[i];
        const int d = res.total_draws[i];
        const int g = w + l + d;
        const double score = g > 0 ? (w + 0.5 * d) / static_cast<double>(g) : 0.0;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%d", r + 1);
        ImGui::TableSetColumnIndex(1);
        const auto* design = sim.find_design(res.design_ids[i]);
        const std::string name = design ? design->name : res.design_ids[i];
        if (ImGui::Selectable((name + "##leader_" + res.design_ids[i]).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
          ui.show_design_studio_window = true;
          ui.request_focus_design_studio_id = res.design_ids[i];
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", res.design_ids[i].c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.0f", res.elo[i]);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d-%d-%d", w, l, d);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.3f", score);
      }

      ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Matchup matrix", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextDisabled("Cell(i,j) = win rate of row i vs column j (wins / games).\n"
                          "Diagonal is blank.");
      const int n = static_cast<int>(res.design_ids.size());
      if (ImGui::BeginTable("balance_matrix", n + 1,
                             ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY,
                             ImVec2(0, 260))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("##row_header", ImGuiTableColumnFlags_WidthFixed, 160);
        for (int j = 0; j < n; ++j) {
          const auto* dj = sim.find_design(res.design_ids[j]);
          const std::string col = dj ? dj->name : res.design_ids[j];
          ImGui::TableSetupColumn(col.c_str(), ImGuiTableColumnFlags_WidthFixed, 85);
        }
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; ++i) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          const auto* di = sim.find_design(res.design_ids[i]);
          const std::string row = di ? di->name : res.design_ids[i];
          ImGui::Text("%s", row.c_str());

          for (int j = 0; j < n; ++j) {
            ImGui::TableSetColumnIndex(j + 1);
            if (i == j) {
              ImGui::TextDisabled("-");
              continue;
            }
            const int games = res.games[i][j];
            if (games <= 0) {
              ImGui::TextDisabled("-");
              continue;
            }
            const double wr = static_cast<double>(res.wins[i][j]) / static_cast<double>(games);
            ImGui::Text("%.2f", wr);
          }
        }

        ImGui::EndTable();
      }
    }

    ImGui::SeparatorText("Export");
    ImGui::InputText("Output path##balance_out", out_path, sizeof(out_path));
    if (ImGui::Button("Save JSON")) {
      try {
        const std::string json = nebula4x::duel_round_robin_to_json(res, 2);
        nebula4x::write_text_file(out_path, json);
        last_status = std::string("Wrote ") + out_path;
        last_error.clear();
      } catch (const std::exception& e) {
        last_error = e.what();
        last_status.clear();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy JSON to clipboard")) {
      const std::string json = nebula4x::duel_round_robin_to_json(res, 2);
      ImGui::SetClipboardText(json.c_str());
      last_status = "JSON copied to clipboard.";
      last_error.clear();
    }
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
