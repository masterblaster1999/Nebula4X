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

// Compute a prerequisite-ordered plan to reach multiple target tech ids.
//
// The returned plan is a union of all prerequisites and targets, ordered so that
// prerequisites always appear before dependents. Techs already known by the
// faction are omitted.
//
// Notes:
// - target_tech_ids may contain duplicates; they are ignored.
// - Errors are accumulated across all targets (unknown tech ids, missing prereqs, cycles).
ResearchPlanResult compute_research_plan(const ContentDB& content, const Faction& f,
                                        const std::vector<std::string>& target_tech_ids);

// Helper options for applying a computed plan to a faction's research state.
enum class ResearchQueueApplyMode : int {
  Append = 0,   // Keep existing queue, append any missing plan items at the end.
  Prepend = 1,  // Insert missing plan items at the front (ahead of existing queue).
  Replace = 2,  // Replace the entire queue with the plan items.
};

struct ResearchQueueApplyOptions {
  ResearchQueueApplyMode mode{ResearchQueueApplyMode::Append};

  // When true, set the faction's active project to the first tech in the plan.
  // This resets active_research_progress to 0.0.
  //
  // If override_active=false (default), the active project is only changed when
  // the faction currently has no active project.
  bool set_active{false};
  bool override_active{false};
};

// Apply a research plan to a faction by mutating its active project / queue.
//
// Returns false only for obvious misuse (e.g. empty plan + set_active request).
bool apply_research_plan(Faction& f, const ResearchPlan& plan,
                         const ResearchQueueApplyOptions& opt = {},
                         std::string* error = nullptr);

} // namespace nebula4x
