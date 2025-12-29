#include "ui/economy_window.h"

#include <imgui.h>

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/research_planner.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism in UI ordering.
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

// ImGui expects UTF-8 in `const char*`, but C++20 `u8"..."` literals are `const char8_t*`.
// This helper bridges the type gap without changing the underlying byte sequence.
inline const char* u8_cstr(const char8_t* s) {
  return reinterpret_cast<const char*>(s);
}


bool case_insensitive_contains(const std::string& haystack, const char* needle_cstr) {
  if (!needle_cstr) return true;
  if (needle_cstr[0] == '\0') return true;
  const std::string needle(needle_cstr);
  const auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      });
  return it != haystack.end();
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

void push_unique(std::vector<std::string>& v, const std::string& x) {
  if (!vec_contains(v, x)) v.push_back(x);
}

double colony_research_points_per_day(const Simulation& sim, const Colony& c) {
  double rp = 0.0;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    rp += std::max(0.0, it->second.research_points_per_day) * static_cast<double>(count);
  }
  return rp;
}

int colony_mining_units(const Simulation& sim, const Colony& c) {
  int mines = 0;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const InstallationDef& def = it->second;
    const bool mining = def.mining ||
                        (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
    if (!mining) continue;
    mines += count;
  }
  return mines;
}

std::unordered_map<std::string, double> colony_mining_request_per_day(const Simulation& sim, const Colony& c) {
  std::unordered_map<std::string, double> out;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const InstallationDef& def = it->second;
    if (def.produces_per_day.empty()) continue;
    const bool mining = def.mining ||
                        (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
    if (!mining) continue;
    for (const auto& [mineral, per_day] : def.produces_per_day) {
      out[mineral] += std::max(0.0, per_day) * static_cast<double>(count);
    }
  }
  return out;
}

std::unordered_map<std::string, double> colony_synthetic_production_per_day(const Simulation& sim, const Colony& c) {
  // "Synthetic" = non-mining produces_per_day (prototype behavior),
  // e.g. Fuel refineries.
  std::unordered_map<std::string, double> out;
  for (const auto& [inst_id, count_raw] : c.installations) {
    const int count = std::max(0, count_raw);
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const InstallationDef& def = it->second;
    if (def.produces_per_day.empty()) continue;

    const bool mining = def.mining ||
                        (!def.mining && nebula4x::to_lower(def.id).find("mine") != std::string::npos);
    if (mining) continue;

    for (const auto& [mineral, per_day] : def.produces_per_day) {
      out[mineral] += std::max(0.0, per_day) * static_cast<double>(count);
    }
  }
  return out;
}

double get_mineral_tons(const Colony& c, const std::string& mineral) {
  auto it = c.minerals.find(mineral);
  return (it == c.minerals.end()) ? 0.0 : it->second;
}

double get_mineral_reserve(const Colony& c, const std::string& mineral) {
  auto it = c.mineral_reserves.find(mineral);
  return (it == c.mineral_reserves.end()) ? 0.0 : it->second;
}

struct TechTierLayout {
  std::vector<std::vector<std::string>> tiers;
  std::unordered_map<std::string, int> tier_by_id;
};

// Compute a "tier" (distance from prerequisites) layout for techs.
TechTierLayout compute_tech_tiers(const ContentDB& content) {
  TechTierLayout out;

  std::unordered_map<std::string, int> memo;
  std::unordered_set<std::string> visiting;

  std::function<int(const std::string&)> dfs = [&](const std::string& id) -> int {
    auto it = memo.find(id);
    if (it != memo.end()) return it->second;
    if (visiting.count(id)) return 0;  // cycle guard; content validation should prevent.
    visiting.insert(id);

    int t = 0;
    auto it_def = content.techs.find(id);
    if (it_def != content.techs.end()) {
      for (const auto& pre : it_def->second.prereqs) {
        t = std::max(t, dfs(pre) + 1);
      }
    }

    visiting.erase(id);
    memo[id] = t;
    return t;
  };

  int max_tier = 0;
  for (const auto& [id, _] : content.techs) {
    max_tier = std::max(max_tier, dfs(id));
  }

  out.tiers.assign(static_cast<std::size_t>(max_tier + 1), {});
  for (const auto& id : sorted_keys(content.techs)) {
    const int t = dfs(id);
    out.tier_by_id[id] = t;
    out.tiers[static_cast<std::size_t>(t)].push_back(id);
  }

  // Within a tier, sort by tech name (then id) for readability.
  for (auto& tier : out.tiers) {
    std::sort(tier.begin(), tier.end(), [&](const std::string& a, const std::string& b) {
      const auto ita = content.techs.find(a);
      const auto itb = content.techs.find(b);
      const std::string na = (ita == content.techs.end()) ? a : ita->second.name;
      const std::string nb = (itb == content.techs.end()) ? b : itb->second.name;
      if (na != nb) return na < nb;
      return a < b;
    });
  }

  return out;
}

}  // namespace

void draw_economy_window(Simulation& sim, UIState& ui, Id& selected_colony, Id& selected_body) {
  if (!ui.show_economy_window) return;

  if (!ImGui::Begin("Economy", &ui.show_economy_window)) {
    ImGui::End();
    return;
  }

  GameState& s = sim.state();
  const Date::YMD ymd = s.date.to_ymd();
  ImGui::Text("Date: %04d-%02d-%02d", ymd.year, ymd.month, ymd.day);

  // --- Faction selector ---
  static Id view_faction_id = kInvalidId;

  auto faction_ids = sorted_keys(s.factions);

  auto ensure_valid_faction = [&]() {
    if (view_faction_id != kInvalidId && s.factions.find(view_faction_id) != s.factions.end()) return;

    // Prefer the current viewer faction, then the selected colony faction.
    if (ui.viewer_faction_id != kInvalidId && s.factions.find(ui.viewer_faction_id) != s.factions.end()) {
      view_faction_id = ui.viewer_faction_id;
      return;
    }
    if (selected_colony != kInvalidId) {
      if (const Colony* c = find_ptr(s.colonies, selected_colony)) {
        if (s.factions.find(c->faction_id) != s.factions.end()) {
          view_faction_id = c->faction_id;
          return;
        }
      }
    }
    view_faction_id = faction_ids.empty() ? kInvalidId : faction_ids.front();
  };
  ensure_valid_faction();

  if (view_faction_id == kInvalidId) {
    ImGui::TextDisabled("No factions in game state.");
    ImGui::End();
    return;
  }

  const Faction* view_faction = find_ptr(s.factions, view_faction_id);

  // Combo list.
  {
    std::string label = view_faction ? view_faction->name : std::string("(unknown)");
    if (ImGui::BeginCombo("Faction", label.c_str())) {
      for (Id fid : faction_ids) {
        const Faction* f = find_ptr(s.factions, fid);
        if (!f) continue;
        const bool sel = (fid == view_faction_id);
        if (ImGui::Selectable((f->name + "##econ_faction_" + std::to_string(static_cast<unsigned long long>(fid))).c_str(), sel)) {
          view_faction_id = fid;
        }
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
  }

  if (ImGui::BeginTabBar("economy_tabs")) {
    // --- Industry ---
    if (ImGui::BeginTabItem("Industry")) {
      std::vector<Id> colony_ids;
      colony_ids.reserve(s.colonies.size());
      for (Id cid : sorted_keys(s.colonies)) {
        const Colony* c = find_ptr(s.colonies, cid);
        if (!c || c->faction_id != view_faction_id) continue;
        colony_ids.push_back(cid);
      }

      double total_pop = 0.0;
      double total_cp = 0.0;
      double total_rp = 0.0;
      int total_mines = 0;
      int total_shipyards = 0;

      for (Id cid : colony_ids) {
        const Colony* c = find_ptr(s.colonies, cid);
        if (!c) continue;
        total_pop += std::max(0.0, c->population_millions);
        total_cp += std::max(0.0, sim.construction_points_per_day(*c));
        total_rp += std::max(0.0, colony_research_points_per_day(sim, *c));
        total_mines += colony_mining_units(sim, *c);
        total_shipyards += (c->installations.count("shipyard") ? c->installations.at("shipyard") : 0);
      }

      ImGui::Text("Colonies: %d", static_cast<int>(colony_ids.size()));
      ImGui::SameLine();
      ImGui::Text("Population: %.1f M", total_pop);
      ImGui::SameLine();
      ImGui::Text("CP/day: %.1f", total_cp);
      ImGui::SameLine();
      ImGui::Text("RP/day: %.1f", total_rp);

      const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX |
                                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
      const float table_h = std::max(200.0f, ImGui::GetContentRegionAvail().y * 0.70f);
      if (ImGui::BeginTable("economy_industry_table", 13, flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Colony", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pop (M)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("CP/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("RP/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Mines", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Dur/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Neu/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Fuel/d", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Yards", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("CQ", ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("SQ", ImGuiTableColumnFlags_WidthFixed, 35.0f);
        ImGui::TableSetupColumn("Fuel", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        for (Id cid : colony_ids) {
          const Colony* c = find_ptr(s.colonies, cid);
          if (!c) continue;
          const Body* b = find_ptr(s.bodies, c->body_id);
          const StarSystem* sys = b ? find_ptr(s.systems, b->system_id) : nullptr;

          const double pop = std::max(0.0, c->population_millions);
          const double cp = std::max(0.0, sim.construction_points_per_day(*c));
          const double rp = std::max(0.0, colony_research_points_per_day(sim, *c));

          const int mines = colony_mining_units(sim, *c);
          const auto mine_req = colony_mining_request_per_day(sim, *c);
          const double dur_d = mine_req.count("Duranium") ? mine_req.at("Duranium") : 0.0;
          const double neu_d = mine_req.count("Neutronium") ? mine_req.at("Neutronium") : 0.0;

          const auto synth = colony_synthetic_production_per_day(sim, *c);
          const double fuel_d = synth.count("Fuel") ? synth.at("Fuel") : 0.0;

          const int yards = c->installations.count("shipyard") ? c->installations.at("shipyard") : 0;
          const int cq = static_cast<int>(c->construction_queue.size());
          const int sq = static_cast<int>(c->shipyard_queue.size());

          const double fuel_stock = get_mineral_tons(*c, "Fuel");

          ImGui::TableNextRow();

          ImGui::TableSetColumnIndex(0);
          const bool is_sel = (selected_colony == cid);
          const std::string label = c->name + "##econ_col_" + std::to_string(static_cast<unsigned long long>(cid));
          if (ImGui::Selectable(label.c_str(), is_sel, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_colony = cid;
            selected_body = c->body_id;
          }

          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(sys ? sys->name.c_str() : "(unknown)");

          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.1f", pop);

          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%.1f", cp);

          ImGui::TableSetColumnIndex(4);
          ImGui::Text("%.1f", rp);

          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%d", mines);

          ImGui::TableSetColumnIndex(6);
          ImGui::Text("%.1f", dur_d);

          ImGui::TableSetColumnIndex(7);
          ImGui::Text("%.1f", neu_d);

          ImGui::TableSetColumnIndex(8);
          ImGui::Text("%.1f", fuel_d);

          ImGui::TableSetColumnIndex(9);
          ImGui::Text("%d", yards);

          ImGui::TableSetColumnIndex(10);
          ImGui::Text("%d", cq);

          ImGui::TableSetColumnIndex(11);
          ImGui::Text("%d", sq);

          ImGui::TableSetColumnIndex(12);
          ImGui::Text("%.1f", fuel_stock);
        }

        ImGui::EndTable();
      }

      ImGui::Separator();
      ImGui::TextDisabled("Tip: set per-colony mineral reserves in the Colony tab to keep local stockpiles safe from auto-freight.");

      ImGui::EndTabItem();
    }

    // --- Mining ---
    if (ImGui::BeginTabItem("Mining")) {
      static char body_filter[96] = "";
      static Id body_sel = kInvalidId;

      // Build list of bodies that have deposits OR have a colony with mines.
      struct BodyRow {
        Id body_id{kInvalidId};
        std::string label;
      };

      std::vector<BodyRow> body_rows;
      body_rows.reserve(s.bodies.size());

      // Precompute which bodies have any mines (any faction).
      std::unordered_set<Id> bodies_with_mines;
      for (Id cid : sorted_keys(s.colonies)) {
        const Colony* c = find_ptr(s.colonies, cid);
        if (!c) continue;
        const Body* b = find_ptr(s.bodies, c->body_id);
        if (!b) continue;
        if (colony_mining_units(sim, *c) > 0) bodies_with_mines.insert(b->id);
      }

      for (Id bid : sorted_keys(s.bodies)) {
        const Body* b = find_ptr(s.bodies, bid);
        if (!b) continue;
        const bool has_deposits = !b->mineral_deposits.empty();
        const bool has_mines = bodies_with_mines.count(bid) != 0;
        if (!has_deposits && !has_mines) continue;

        const StarSystem* sys = find_ptr(s.systems, b->system_id);
        const std::string label = (sys ? sys->name : std::string("(unknown)")) + " / " + b->name;
        body_rows.push_back(BodyRow{bid, label});
      }

      if (body_sel == kInvalidId) {
        // Prefer current selection, then first available.
        if (selected_body != kInvalidId) body_sel = selected_body;
        if (body_sel == kInvalidId && !body_rows.empty()) body_sel = body_rows.front().body_id;
      }

      // Left list / right details.
      const float left_w = 280.0f;
      ImGui::BeginChild("mining_left", ImVec2(left_w, 0), true);
      ImGui::Text("Bodies");
      ImGui::InputText("Filter##mining_body_filter", body_filter, IM_ARRAYSIZE(body_filter));
      ImGui::Separator();

      for (const auto& row : body_rows) {
        if (!case_insensitive_contains(row.label, body_filter)) continue;
        const bool sel = (row.body_id == body_sel);
        if (ImGui::Selectable((row.label + "##mine_body_" + std::to_string(static_cast<unsigned long long>(row.body_id))).c_str(), sel)) {
          body_sel = row.body_id;
          selected_body = row.body_id;
        }
      }
      ImGui::EndChild();

      ImGui::SameLine();

      ImGui::BeginChild("mining_right", ImVec2(0, 0), true);

      const Body* body = find_ptr(s.bodies, body_sel);
      if (!body) {
        ImGui::TextDisabled("Select a body.");
        ImGui::EndChild();
        ImGui::EndTabItem();
      } else {
        const StarSystem* sys = find_ptr(s.systems, body->system_id);
        ImGui::Text("%s", body->name.c_str());
        ImGui::TextDisabled("System: %s", sys ? sys->name.c_str() : "(unknown)");

        // Gather colonies on this body (all factions).
        struct ColMining {
          Id colony_id{kInvalidId};
          std::unordered_map<std::string, double> req;
        };

        std::vector<ColMining> cols;
        for (Id cid : sorted_keys(s.colonies)) {
          const Colony* c = find_ptr(s.colonies, cid);
          if (!c) continue;
          if (c->body_id != body->id) continue;
          cols.push_back(ColMining{cid, colony_mining_request_per_day(sim, *c)});
        }

        // Mineral -> list of (colony, req)
        struct ReqEntry { Id colony_id; double req; };
        std::unordered_map<std::string, std::vector<ReqEntry>> req_by_mineral;
        for (const auto& cm : cols) {
          for (const auto& [mineral, req] : cm.req) {
            if (req <= 1e-9) continue;
            req_by_mineral[mineral].push_back(ReqEntry{cm.colony_id, req});
          }
        }

        // Union minerals: deposits + req.
        std::vector<std::string> minerals;
        for (const auto& [m, _] : body->mineral_deposits) minerals.push_back(m);
        for (const auto& [m, _] : req_by_mineral) minerals.push_back(m);
        std::sort(minerals.begin(), minerals.end());
        minerals.erase(std::unique(minerals.begin(), minerals.end()), minerals.end());

        // Compute allocation based on current deposits.
        // mineral -> colony -> actual/day
        std::unordered_map<std::string, std::unordered_map<Id, double>> actual_by_mineral;
        std::unordered_map<std::string, double> total_req_by_mineral;

        for (const auto& mineral : minerals) {
          double total_req = 0.0;
          if (auto it = req_by_mineral.find(mineral); it != req_by_mineral.end()) {
            for (const auto& e : it->second) total_req += std::max(0.0, e.req);
          }
          total_req_by_mineral[mineral] = total_req;

          const double left = body->mineral_deposits.count(mineral) ? body->mineral_deposits.at(mineral)
                                                                    : std::numeric_limits<double>::infinity();
          const double actual_total = (std::isfinite(left) ? std::min(left, total_req) : total_req);
          const double ratio = (total_req > 1e-12) ? (actual_total / total_req) : 0.0;

          if (auto it = req_by_mineral.find(mineral); it != req_by_mineral.end()) {
            for (const auto& e : it->second) {
              actual_by_mineral[mineral][e.colony_id] += e.req * ratio;
            }
          }
        }

        ImGui::Separator();
        ImGui::Text("Deposits / depletion");
        if (minerals.empty()) {
          ImGui::TextDisabled("(no deposits / no mines)");
        } else {
          const ImGuiTableFlags dflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                         ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
          if (ImGui::BeginTable("mining_deposits_table", 6, dflags)) {
            ImGui::TableSetupColumn("Mineral", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Req/d", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Act/d", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("ETA (d)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("ETA (y)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (const auto& mineral : minerals) {
              const double left = body->mineral_deposits.count(mineral) ? body->mineral_deposits.at(mineral)
                                                                        : std::numeric_limits<double>::infinity();
              const double req = total_req_by_mineral.count(mineral) ? total_req_by_mineral.at(mineral) : 0.0;
              const double act = (std::isfinite(left) ? std::min(left, req) : req);

              ImGui::TableNextRow();

              ImGui::TableSetColumnIndex(0);
              ImGui::TextUnformatted(mineral.c_str());

              ImGui::TableSetColumnIndex(1);
              if (std::isfinite(left)) ImGui::Text("%.0f", left);
              else ImGui::TextDisabled("∞");

              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%.2f", req);

              ImGui::TableSetColumnIndex(3);
              ImGui::Text("%.2f", act);

              ImGui::TableSetColumnIndex(4);
              if (std::isfinite(left) && req > 1e-9) {
                const double eta_d = left / req;
                ImGui::Text("%.0f", eta_d);
              } else if (std::isfinite(left)) {
                ImGui::TextDisabled("-");
              } else {
                ImGui::TextDisabled("∞");
              }

              ImGui::TableSetColumnIndex(5);
              if (std::isfinite(left) && req > 1e-9) {
                const double eta_y = (left / req) / 365.25;
                ImGui::Text("%.1f", eta_y);
              } else if (std::isfinite(left)) {
                ImGui::TextDisabled("-");
              } else {
                ImGui::TextDisabled("∞");
              }
            }

            ImGui::EndTable();
          }
        }

        ImGui::Separator();
        ImGui::Text("Colony mining (predicted for today)");
        if (cols.empty()) {
          ImGui::TextDisabled("(no colonies on this body)");
        } else {
          for (const auto& cm : cols) {
            const Colony* c = find_ptr(s.colonies, cm.colony_id);
            if (!c) continue;
            const Faction* f = find_ptr(s.factions, c->faction_id);
            const std::string header = c->name + " (" + (f ? f->name : std::string("Unknown")) + ")";
            if (ImGui::TreeNode((header + "##mine_col_" + std::to_string(static_cast<unsigned long long>(c->id))).c_str())) {
              const int mine_units = colony_mining_units(sim, *c);
              ImGui::Text("Mines: %d", mine_units);

              // Show actual mining per mineral for this colony.
              std::vector<std::string> mlist;
              for (const auto& [m, _] : actual_by_mineral) mlist.push_back(m);
              std::sort(mlist.begin(), mlist.end());

              bool any = false;
              for (const auto& m : mlist) {
                const double act = actual_by_mineral[m].count(c->id) ? actual_by_mineral[m].at(c->id) : 0.0;
                if (act <= 1e-9) continue;
                any = true;
                ImGui::BulletText("%s: %.2f / day", m.c_str(), act);
              }
              if (!any) ImGui::TextDisabled("(no active mining)");

              ImGui::TreePop();
            }
          }
        }

        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    }

    // --- Tech Tree ---
    if (ImGui::BeginTabItem("Tech Tree")) {
      const auto itf = s.factions.find(view_faction_id);
      if (itf == s.factions.end()) {
        ImGui::TextDisabled("Faction not found.");
        ImGui::EndTabItem();
      } else {
        Faction& fac = s.factions.at(view_faction_id);

        static char tech_filter[128] = "";
        static std::string selected_tech;

        // Cache tiers (content is static).
        static int cached_tech_count = -1;
        static TechTierLayout cached_layout;

        if (cached_tech_count != static_cast<int>(sim.content().techs.size())) {
          cached_layout = compute_tech_tiers(sim.content());
          cached_tech_count = static_cast<int>(sim.content().techs.size());
        }

        ImGui::InputText("Filter##tech_tree_filter", tech_filter, IM_ARRAYSIZE(tech_filter));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##tech_tree_filter_clear")) tech_filter[0] = '\0';

        const float left_w = ImGui::GetContentRegionAvail().x * 0.62f;
        ImGui::BeginChild("tech_tree_left", ImVec2(left_w, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

        // Table layout: one column per tier.
        const int tiers = static_cast<int>(cached_layout.tiers.size());
        int max_rows = 0;
        for (const auto& t : cached_layout.tiers) max_rows = std::max(max_rows, static_cast<int>(t.size()));

        const ImGuiTableFlags tflags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                       ImGuiTableFlags_RowBg;
        if (ImGui::BeginTable("tech_tree_table", std::max(1, tiers), tflags)) {
          for (int i = 0; i < std::max(1, tiers); ++i) {
            const std::string col = "Tier " + std::to_string(i);
            ImGui::TableSetupColumn(col.c_str(), ImGuiTableColumnFlags_WidthFixed, 240.0f);
          }
          ImGui::TableHeadersRow();

          for (int r = 0; r < max_rows; ++r) {
            ImGui::TableNextRow();
            for (int t = 0; t < tiers; ++t) {
              ImGui::TableSetColumnIndex(t);
              if (r >= static_cast<int>(cached_layout.tiers[static_cast<std::size_t>(t)].size())) {
                ImGui::TextUnformatted("");
                continue;
              }
              const std::string& tid = cached_layout.tiers[static_cast<std::size_t>(t)][static_cast<std::size_t>(r)];
              const auto it = sim.content().techs.find(tid);
              if (it == sim.content().techs.end()) continue;

              const TechDef& def = it->second;

              // Filter by id or name.
              const std::string hay = def.name + " " + tid;
              if (!case_insensitive_contains(hay, tech_filter)) {
                ImGui::TextUnformatted("");
                continue;
              }

              const bool known = vec_contains(fac.known_techs, tid);
              const bool active = (!fac.active_research_id.empty() && fac.active_research_id == tid);
              const bool queued = vec_contains(fac.research_queue, tid);

              bool prereqs_met = true;
              for (const auto& pre : def.prereqs) {
                if (!vec_contains(fac.known_techs, pre)) {
                  prereqs_met = false;
                  break;
                }
              }

              std::string prefix;
              if (known) prefix = u8_cstr(u8"✓ ");
              else if (active) prefix = u8_cstr(u8"▶ ");
              else if (queued) prefix = u8_cstr(u8"⏳ ");
              else if (prereqs_met) prefix = u8_cstr(u8"• ");
              else prefix = "  ";

              const bool sel = (selected_tech == tid);

              if (known) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 255, 140, 255));
              else if (active) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 220, 120, 255));
              else if (queued) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 210, 255, 255));
              else if (prereqs_met) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
              else ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(170, 170, 170, 255));

              const std::string lbl = prefix + def.name + "##technode_" + tid;
              if (ImGui::Selectable(lbl.c_str(), sel)) {
                selected_tech = tid;
              }

              if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", def.name.c_str());
                ImGui::TextDisabled("%s", tid.c_str());
                ImGui::Text("Cost: %.0f", def.cost);
                if (!def.prereqs.empty()) {
                  ImGui::Separator();
                  ImGui::Text("Prereqs:");
                  for (const auto& pre : def.prereqs) {
                    ImGui::BulletText("%s", pre.c_str());
                  }
                }
                ImGui::EndTooltip();
              }

              ImGui::PopStyleColor();
            }
          }

          ImGui::EndTable();
        }

        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("tech_tree_right", ImVec2(0, 0), true);

        if (selected_tech.empty()) {
          ImGui::TextDisabled("Select a tech node to see details.");
        } else if (auto it = sim.content().techs.find(selected_tech); it == sim.content().techs.end()) {
          ImGui::TextDisabled("Tech not found.");
        } else {
          const TechDef& def = sim.content().techs.at(selected_tech);
          const bool known = vec_contains(fac.known_techs, def.id);
          const bool active = (!fac.active_research_id.empty() && fac.active_research_id == def.id);
          const bool queued = vec_contains(fac.research_queue, def.id);

          bool prereqs_met = true;
          for (const auto& pre : def.prereqs) {
            if (!vec_contains(fac.known_techs, pre)) {
              prereqs_met = false;
              break;
            }
          }

          ImGui::Text("%s", def.name.c_str());
          ImGui::TextDisabled("%s", def.id.c_str());
          ImGui::Separator();
          ImGui::Text("Cost: %.0f", def.cost);

          if (known) {
            ImGui::TextColored(ImVec4(0.47f, 1.0f, 0.55f, 1.0f), "Status: Known");
          } else if (active) {
            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.47f, 1.0f), "Status: Active (%.0f / %.0f)",
                               fac.active_research_progress, def.cost);
          } else if (queued) {
            ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.0f), "Status: Queued");
          } else if (prereqs_met) {
            ImGui::Text("Status: Available");
          } else {
            ImGui::TextDisabled("Status: Locked (missing prereqs)");
          }

          if (!def.prereqs.empty()) {
            ImGui::Separator();
            ImGui::Text("Prerequisites");
            for (const auto& pre : def.prereqs) {
              const bool have = vec_contains(fac.known_techs, pre);
              if (have) {
                ImGui::BulletText("%s  (known)", pre.c_str());
              } else {
                ImGui::BulletText("%s  (missing)", pre.c_str());
              }
            }
          }

          if (!def.effects.empty()) {
            ImGui::Separator();
            ImGui::Text("Effects");
            for (const auto& eff : def.effects) {
              ImGui::BulletText("%s: %s", eff.type.c_str(), eff.value.c_str());
            }
          }

          ImGui::Separator();
          ImGui::Text("Actions");

          if (!known) {
            if (ImGui::Button("Set Active")) {
              fac.active_research_id = def.id;
              fac.active_research_progress = 0.0;
              // Avoid duplicates: remove from queue if present.
              fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                       fac.research_queue.end());
            }
            ImGui::SameLine();
            if (!queued) {
              if (ImGui::Button("Queue")) {
                fac.research_queue.push_back(def.id);
              }
            } else {
              if (ImGui::Button("Unqueue")) {
                fac.research_queue.erase(std::remove(fac.research_queue.begin(), fac.research_queue.end(), def.id),
                                         fac.research_queue.end());
              }
            }

            if (ImGui::Button("Queue prereq plan")) {
              const auto plan_res = compute_research_plan(sim.content(), fac, def.id);
              if (plan_res.ok()) {
                for (const auto& tid : plan_res.plan.tech_ids) {
                  if (tid == fac.active_research_id) continue;
                  if (vec_contains(fac.known_techs, tid)) continue;
                  push_unique(fac.research_queue, tid);
                }
              }
            }

            // Plan preview.
            const auto plan_res = compute_research_plan(sim.content(), fac, def.id);
            if (plan_res.ok() && !plan_res.plan.tech_ids.empty()) {
              ImGui::Separator();
              ImGui::Text("Prereq plan (queue order)");
              ImGui::TextDisabled("Total cost (sum): %.0f", plan_res.plan.total_cost);
              for (const auto& tid : plan_res.plan.tech_ids) {
                const auto it2 = sim.content().techs.find(tid);
                const std::string nm = (it2 == sim.content().techs.end()) ? tid : it2->second.name;
                ImGui::BulletText("%s", nm.c_str());
              }
            } else if (!plan_res.ok()) {
              ImGui::Separator();
              ImGui::TextDisabled("Planner errors:");
              for (const auto& e : plan_res.errors) ImGui::BulletText("%s", e.c_str());
            }
          }

          ImGui::Separator();
          ImGui::Text("Research banked: %.0f RP", fac.research_points);
          if (ImGui::Button("Clear Research Queue")) {
            fac.research_queue.clear();
          }
        }

        ImGui::EndChild();

        ImGui::EndTabItem();
      }
    }

    ImGui::EndTabBar();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
