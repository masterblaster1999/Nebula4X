#include "nebula4x/core/research_schedule.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_set>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/simulation.h"

// Internal helper for economy multipliers.
#include "simulation_internal.h"

namespace nebula4x {
namespace {

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool prereqs_met(const std::unordered_set<std::string>& known, const TechDef& t) {
  for (const auto& p : t.prereqs) {
    if (p.empty()) return false;
    if (known.find(p) == known.end()) return false;
  }
  return true;
}

std::string describe_missing_prereqs(const ContentDB& content, const std::unordered_set<std::string>& known,
                                     const std::string& tech_id) {
  const auto it = content.techs.find(tech_id);
  if (it == content.techs.end()) return "unknown tech";

  const TechDef& t = it->second;
  std::vector<std::string> missing;
  missing.reserve(t.prereqs.size());
  for (const auto& p : t.prereqs) {
    if (p.empty()) continue;
    if (known.find(p) == known.end()) missing.push_back(p);
  }
  if (missing.empty()) return "(none)";

  std::ostringstream ss;
  ss << "missing prereqs: ";
  for (std::size_t i = 0; i < missing.size(); ++i) {
    if (i) ss << ", ";
    const auto itp = content.techs.find(missing[i]);
    if (itp != content.techs.end()) {
      ss << itp->second.name;
    } else {
      ss << missing[i];
    }
  }
  return ss.str();
}

} // namespace

ResearchSchedule estimate_research_schedule(const Simulation& sim, Id faction_id, const ResearchScheduleOptions& opt) {
  ResearchSchedule out;
  out.ok = false;
  out.items.clear();

  const auto* fac0 = find_ptr(sim.state().factions, faction_id);
  if (!fac0) {
    out.stalled = true;
    out.stall_reason = "unknown faction";
    return out;
  }

  // Working copy so we can evolve known techs during the forecast.
  Faction fac = *fac0;

  std::unordered_set<std::string> known;
  known.reserve(fac.known_techs.size() * 2 + 32);
  for (const auto& id : fac.known_techs) {
    if (!id.empty()) known.insert(id);
  }

  // Base RP/day from colony installations (ignoring tech multipliers).
  double base_rp = 0.0;
  for (const auto& [cid, col] : sim.state().colonies) {
    (void)cid;
    if (col.faction_id != faction_id) continue;
    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      const auto it = sim.content().installations.find(inst_id);
      if (it == sim.content().installations.end()) continue;
      const double rp = std::max(0.0, it->second.research_points_per_day);
      if (rp <= 0.0) continue;
      base_rp += rp * static_cast<double>(count);
    }
  }

  out.rp_bank_start = std::max(0.0, fac.research_points);
  out.base_rp_per_day = base_rp;

  // Compute current multiplier snapshot (for UI display).
  {
    const auto mult = sim_internal::compute_faction_economy_multipliers(sim.content(), fac);
    out.research_multiplier = std::max(0.0, mult.research);
    out.effective_rp_per_day = base_rp * out.research_multiplier;
  }

  // Working copies of the active project and queue.
  std::string active_id = fac.active_research_id;
  double active_progress = std::max(0.0, fac.active_research_progress);
  std::vector<std::string> queue = fac.research_queue;

  // Track when the current active project became active (relative to now).
  int active_start_day = 0;
  bool active_was_active_at_start = !active_id.empty();

  auto enqueue_unique = [&](const std::string& tech_id) {
    if (tech_id.empty()) return;
    if (known.find(tech_id) != known.end()) return;
    if (std::find(queue.begin(), queue.end(), tech_id) != queue.end()) return;
    queue.push_back(tech_id);
  };

  auto clean_queue = [&]() {
    auto keep = [&](const std::string& id) {
      if (id.empty()) return false;
      if (known.find(id) != known.end()) return false;
      return (sim.content().techs.find(id) != sim.content().techs.end());
    };
    queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const std::string& id) { return !keep(id); }), queue.end());
  };

  auto select_next_available = [&](int today) {
    clean_queue();
    active_id.clear();
    active_progress = 0.0;
    active_start_day = today;
    active_was_active_at_start = false;

    for (std::size_t i = 0; i < queue.size(); ++i) {
      const std::string& id = queue[i];
      const auto it = sim.content().techs.find(id);
      if (it == sim.content().techs.end()) continue;
      if (!prereqs_met(known, it->second)) continue;
      active_id = id;
      active_progress = 0.0;
      queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  };

  auto complete_tech = [&](const TechDef& tech) {
    if (known.insert(tech.id).second) {
      fac.known_techs.push_back(tech.id);
    }

    // Mirror the sim's unlock bookkeeping so economy multiplier forecasts can
    // depend on tech effects if future systems query these.
    for (const auto& eff : tech.effects) {
      if (eff.type == "unlock_component") {
        if (!vec_contains(fac.unlocked_components, eff.value)) fac.unlocked_components.push_back(eff.value);
      } else if (eff.type == "unlock_installation") {
        if (!vec_contains(fac.unlocked_installations, eff.value)) fac.unlocked_installations.push_back(eff.value);
      }
    }
  };

  // If we have an active project, sanity-check it against current prereqs/defs
  // like tick_research does (but do not select a replacement until day 1 tick).
  if (!active_id.empty()) {
    if (known.find(active_id) != known.end()) {
      active_id.clear();
      active_progress = 0.0;
      active_was_active_at_start = false;
    } else {
      const auto it = sim.content().techs.find(active_id);
      if (it == sim.content().techs.end()) {
        active_id.clear();
        active_progress = 0.0;
        active_was_active_at_start = false;
      } else if (!prereqs_met(known, it->second)) {
        enqueue_unique(active_id);
        active_id.clear();
        active_progress = 0.0;
        active_was_active_at_start = false;
      }
    }
  }

  // If there's nothing to do, return ok with empty items.
  clean_queue();
  if (active_id.empty() && queue.empty()) {
    out.ok = true;
    return out;
  }

  // Working RP bank (the simulation stores faction research_points).
  double bank = out.rp_bank_start;

  const int max_days = std::max(0, opt.max_days);
  const int max_items = std::max(0, opt.max_items);

  // Day loop: day 1..max_days inclusive.
  for (int day = 1; day <= max_days; ++day) {
    // RP generation uses economy multipliers based on techs known at *start of day*.
    const auto mult = sim_internal::compute_faction_economy_multipliers(sim.content(), fac);
    const double rp_gain = base_rp * std::max(0.0, mult.research);
    if (rp_gain > 0.0) bank += rp_gain;

    // Ensure we have an active project.
    if (active_id.empty()) {
      select_next_available(day);
    }

    // If nothing is active but the queue is non-empty, we're blocked.
    if (active_id.empty()) {
      clean_queue();
      if (!queue.empty()) {
        // Provide a concise, deterministic reason.
        std::string reason = "queue blocked";
        const std::string first = queue.front();
        if (!first.empty()) {
          reason += ": ";
          reason += describe_missing_prereqs(sim.content(), known, first);
        }
        out.ok = true;
        out.stalled = true;
        out.stall_reason = std::move(reason);
        return out;
      }
      // queue empty too.
      out.ok = true;
      return out;
    }

    // Spend RP on active project(s), potentially completing multiple techs this day.
    for (;;) {
      if (active_id.empty()) break;

      // Validate tech.
      const auto it = sim.content().techs.find(active_id);
      if (it == sim.content().techs.end()) {
        active_id.clear();
        active_progress = 0.0;
        select_next_available(day);
        continue;
      }
      const TechDef& tech = it->second;

      if (known.find(tech.id) != known.end()) {
        active_id.clear();
        active_progress = 0.0;
        select_next_available(day);
        continue;
      }

      if (!prereqs_met(known, tech)) {
        enqueue_unique(tech.id);
        active_id.clear();
        active_progress = 0.0;
        select_next_available(day);
        continue;
      }

      const double cost = std::max(0.0, tech.cost);
      const double remaining = std::max(0.0, cost - active_progress);
      if (remaining <= 1e-9) {
        // Completion without spending (e.g. already fully progressed).
        ResearchScheduleItem item;
        item.tech_id = tech.id;
        item.start_day = active_start_day;
        item.end_day = day;
        item.cost = cost;
        item.progress_at_start = active_was_active_at_start ? std::min(cost, fac0->active_research_progress) : 0.0;
        item.was_active_at_start = active_was_active_at_start;
        out.items.push_back(std::move(item));
        complete_tech(tech);
        active_id.clear();
        active_progress = 0.0;
        select_next_available(day);

        if (max_items > 0 && static_cast<int>(out.items.size()) >= max_items) {
          out.ok = true;
          out.truncated = true;
          out.truncated_reason = "max_items reached";
          return out;
        }
        continue;
      }

      if (bank <= 0.0) {
        // Can't make progress this day. If there is no RP income, we're stalled.
        if (rp_gain <= 0.0) {
          out.ok = true;
          out.stalled = true;
          out.stall_reason = "no research output (RP/day is 0)";
          return out;
        }
        break;
      }

      // Spend what we can today.
      const double spend = std::min(bank, remaining);
      bank -= spend;
      active_progress += spend;

      if (active_progress + 1e-9 >= cost) {
        // Completed this day.
        ResearchScheduleItem item;
        item.tech_id = tech.id;
        item.start_day = active_start_day;
        item.end_day = day;
        item.cost = cost;
        item.progress_at_start = active_was_active_at_start ? std::min(cost, fac0->active_research_progress) : 0.0;
        item.was_active_at_start = active_was_active_at_start;
        out.items.push_back(std::move(item));

        complete_tech(tech);
        active_id.clear();
        active_progress = 0.0;
        select_next_available(day);

        if (max_items > 0 && static_cast<int>(out.items.size()) >= max_items) {
          out.ok = true;
          out.truncated = true;
          out.truncated_reason = "max_items reached";
          return out;
        }
        // Continue the loop: we may be able to complete more techs today.
        continue;
      }

      // Still in progress; stop spending for today.
      break;
    }

    // Finished everything.
    clean_queue();
    if (active_id.empty() && queue.empty()) {
      out.ok = true;
      return out;
    }
  }

  // Ran out of horizon days.
  out.ok = true;
  out.truncated = true;
  out.truncated_reason = "max_days reached";
  return out;
}

} // namespace nebula4x
