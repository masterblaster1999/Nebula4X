#include "nebula4x/core/research_planner.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/tech_tree.h"

namespace nebula4x {
namespace {

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

} // namespace

ResearchPlanResult compute_research_plan(const ContentDB& content, const Faction& f,
                                        const std::string& target_tech_id) {
  ResearchPlanResult out;
  out.plan.total_cost = 0.0;
  out.plan.tech_ids.clear();

  const auto& techs = content.techs;

  if (target_tech_id.empty()) {
    out.errors.push_back("Target tech id is empty");
    return out;
  }

  if (techs.find(target_tech_id) == techs.end()) {
    out.errors.push_back("Unknown tech id: '" + target_tech_id + "'");
    return out;
  }

  // Visiting state: 0 = unvisited, 1 = visiting, 2 = done.
  std::unordered_map<std::string, int> visit;
  visit.reserve(techs.size() * 2);

  // Keep a deterministic-ish stack to render a helpful cycle trace.
  std::vector<std::string> stack;
  stack.reserve(32);
  std::unordered_map<std::string, std::size_t> stack_pos;
  stack_pos.reserve(techs.size() * 2);

  auto emit_cycle = [&](const std::string& id) {
    auto it = stack_pos.find(id);
    if (it == stack_pos.end()) {
      out.errors.push_back("Prerequisite cycle detected involving '" + id + "'");
      return;
    }
    std::string msg = "Prerequisite cycle: ";
    const std::size_t start = it->second;
    for (std::size_t i = start; i < stack.size(); ++i) {
      msg += stack[i];
      msg += " -> ";
    }
    msg += id;
    out.errors.push_back(std::move(msg));
  };

  std::function<void(const std::string&)> dfs = [&](const std::string& id) {
    if (out.errors.size() > 64) return;  // avoid runaway error spam

    if (id.empty()) {
      out.errors.push_back("Empty prerequisite id encountered");
      return;
    }

    // Known techs are satisfied; omit them from the plan and don't traverse.
    if (vec_contains(f.known_techs, id)) return;

    auto it_state = visit.find(id);
    const int st = (it_state == visit.end()) ? 0 : it_state->second;
    if (st == 2) return;
    if (st == 1) {
      emit_cycle(id);
      return;
    }

    const auto it = techs.find(id);
    if (it == techs.end()) {
      out.errors.push_back("Missing tech definition for prerequisite '" + id + "'");
      return;
    }

    visit[id] = 1;
    stack_pos[id] = stack.size();
    stack.push_back(id);

    const TechDef& t = it->second;
    for (const auto& pre : t.prereqs) {
      if (pre == id) {
        out.errors.push_back("Tech '" + id + "' lists itself as a prerequisite");
        continue;
      }
      if (vec_contains(f.known_techs, pre)) continue;
      if (techs.find(pre) == techs.end()) {
        out.errors.push_back("Tech '" + id + "' has unknown prerequisite '" + pre + "'");
        continue;
      }
      dfs(pre);
    }

    stack.pop_back();
    stack_pos.erase(id);
    visit[id] = 2;

    // Append this tech after its prereqs.
    out.plan.tech_ids.push_back(id);
  };

  dfs(target_tech_id);

  // If we found errors, do not attempt to compute cost; still return whatever
  // partial plan we built so callers can inspect/debug.
  if (!out.errors.empty()) {
    std::sort(out.errors.begin(), out.errors.end());
    out.errors.erase(std::unique(out.errors.begin(), out.errors.end()), out.errors.end());
    return out;
  }

  // Deduplicate while preserving order (can happen if multiple techs share prereqs).
  std::vector<std::string> unique;
  unique.reserve(out.plan.tech_ids.size());
  std::unordered_set<std::string> seen;
  seen.reserve(out.plan.tech_ids.size() * 2);
  for (const auto& id : out.plan.tech_ids) {
    if (seen.insert(id).second) unique.push_back(id);
  }
  out.plan.tech_ids = std::move(unique);

  // Total cost.
  double total = 0.0;
  for (const auto& id : out.plan.tech_ids) {
    const auto it = techs.find(id);
    if (it == techs.end()) continue;
    total += std::max(0.0, it->second.cost);
  }
  out.plan.total_cost = total;

  return out;
}

} // namespace nebula4x
