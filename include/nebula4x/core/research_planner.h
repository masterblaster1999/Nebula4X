#pragma once

#include <string>
#include <vector>

namespace nebula4x {

struct ContentDB;
struct Faction;

// A computed research plan (prereqs first) to reach a target tech.
//
// This is a UI/CLI convenience helper: the simulation can already queue techs
// in any order and will only start projects whose prereqs are met. The planner
// simply answers: "Which tech ids do I still need to research, and in what order
// should I queue them so prerequisites come first?".
struct ResearchPlan {
  // Ordered list of tech ids to research, prerequisites first.
  std::vector<std::string> tech_ids;

  // Sum of full tech costs for all tech_ids in the plan.
  // (Does not account for in-progress progress on the active project.)
  double total_cost{0.0};
};

// Result wrapper so callers can surface actionable diagnostics.
struct ResearchPlanResult {
  ResearchPlan plan;
  std::vector<std::string> errors;

  bool ok() const { return errors.empty(); }
};

// Compute a prerequisite-ordered plan to reach target_tech_id for faction f.
//
// Rules:
// - Techs already known by the faction are omitted from the plan.
// - If target_tech_id is unknown, errors will be populated.
// - If a prerequisite is missing from content, errors will be populated.
// - If a prerequisite cycle is detected within the relevant subgraph, errors
//   will be populated.
ResearchPlanResult compute_research_plan(const ContentDB& content, const Faction& f,
                                        const std::string& target_tech_id);

} // namespace nebula4x
