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

ResearchPlanResult compute_plan_impl(const ContentDB& content, const Faction& f,
                                    const std::vector<std::string>& target_tech_ids) {
  ResearchPlanResult out;
  out.plan.total_cost = 0.0;
  out.plan.tech_ids.clear();

  const auto& techs = content.techs;

  if (target_tech_ids.empty()) {
    out.errors.push_back("No target tech ids provided");
    return out;
  }

  // Validate targets and preserve caller order (deduped).
  std::vector<std::string> targets;
  targets.reserve(target_tech_ids.size());
  std::unordered_set<std::string> seen_targets;
  seen_targets.reserve(target_tech_ids.size() * 2 + 8);
  for (const auto& tid : target_tech_ids) {
    if (tid.empty()) {
      out.errors.push_back("Target tech id is empty");
      continue;
    }
    if (techs.find(tid) == techs.end()) {
      out.errors.push_back("Unknown tech id: '" + tid + "'");
      continue;
    }
    if (seen_targets.insert(tid).second) targets.push_back(tid);
  }

  if (targets.empty()) {
    std::sort(out.errors.begin(), out.errors.end());
    out.errors.erase(std::unique(out.errors.begin(), out.errors.end()), out.errors.end());
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
    // Already researched.
    if (vec_contains(f.known_techs, id)) {
      visit[id] = 2;
      return;
    }

    const auto it = techs.find(id);
    if (it == techs.end()) {
      out.errors.push_back("Unknown tech id: '" + id + "'");
      return;
    }

    const int st = visit[id];
    if (st == 2) return;
    if (st == 1) {
      emit_cycle(id);
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
      if (pre.empty()) {
        out.errors.push_back("Tech '" + id + "' has an empty prerequisite id");
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

  for (const auto& tid : targets) dfs(tid);

  // If we found errors, do not attempt to compute cost; still return whatever
  // partial plan we built so callers can inspect/debug.
  if (!out.errors.empty()) {
    std::sort(out.errors.begin(), out.errors.end());
    out.errors.erase(std::unique(out.errors.begin(), out.errors.end()), out.errors.end());
    return out;
  }

  // Deduplicate while preserving order (can happen if multiple targets share prereqs).
  std::vector<std::string> unique;
  unique.reserve(out.plan.tech_ids.size());
  std::unordered_set<std::string> seen;
  seen.reserve(out.plan.tech_ids.size() * 2 + 8);
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

} // namespace

ResearchPlanResult compute_research_plan(const ContentDB& content, const Faction& f,
                                        const std::string& target_tech_id) {
  return compute_plan_impl(content, f, std::vector<std::string>{target_tech_id});
}

ResearchPlanResult compute_research_plan(const ContentDB& content, const Faction& f,
                                        const std::vector<std::string>& target_tech_ids) {
  return compute_plan_impl(content, f, target_tech_ids);
}

bool apply_research_plan(Faction& f, const ResearchPlan& plan, const ResearchQueueApplyOptions& opt,
                         std::string* error) {
  if (plan.tech_ids.empty()) {
    // Empty plans are a valid outcome (target already known).
    if (opt.set_active && opt.override_active) {
      if (error) *error = "Cannot set active research: plan is empty";
      return false;
    }
    return true;
  }

  // Build a fast known set.
  std::unordered_set<std::string> known;
  known.reserve(f.known_techs.size() * 2 + 8);
  for (const auto& id : f.known_techs) {
    if (!id.empty()) known.insert(id);
  }

  // Filter plan -> missing (excluding known).
  std::vector<std::string> missing;
  missing.reserve(plan.tech_ids.size());
  for (const auto& id : plan.tech_ids) {
    if (id.empty()) continue;
    if (known.find(id) != known.end()) continue;
    missing.push_back(id);
  }

  if (missing.empty()) return true;

  auto push_unique = [](std::vector<std::string>& v, const std::string& id) {
    if (id.empty()) return;
    if (std::find(v.begin(), v.end(), id) != v.end()) return;
    v.push_back(id);
  };

  std::vector<std::string> new_q;
  new_q.reserve(f.research_queue.size() + missing.size());

  if (opt.mode == ResearchQueueApplyMode::Replace) {
    for (const auto& id : missing) push_unique(new_q, id);
  } else if (opt.mode == ResearchQueueApplyMode::Prepend) {
    for (const auto& id : missing) push_unique(new_q, id);
    for (const auto& id : f.research_queue) {
      if (known.find(id) != known.end()) continue;
      push_unique(new_q, id);
    }
  } else { // Append
    for (const auto& id : f.research_queue) {
      if (known.find(id) != known.end()) continue;
      push_unique(new_q, id);
    }
    for (const auto& id : missing) push_unique(new_q, id);
  }

  f.research_queue = std::move(new_q);

  if (opt.set_active) {
    const bool has_active = !f.active_research_id.empty();
    if (!has_active || opt.override_active) {
      // Prefer the first item from the plan (prereqs first).
      const std::string next = missing.front();
      if (!next.empty()) {
        f.active_research_id = next;
        f.active_research_progress = 0.0;
        f.research_queue.erase(std::remove(f.research_queue.begin(), f.research_queue.end(), next),
                               f.research_queue.end());
      }
    }
  }

  return true;
}

}  // namespace nebula4x
