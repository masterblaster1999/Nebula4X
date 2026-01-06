#include "nebula4x/core/advisor.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace nebula4x {

namespace {

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> out;
  out.reserve(m.size());
  for (const auto& [k, _] : m) out.push_back(k);
  std::sort(out.begin(), out.end());
  return out;
}

int level_rank(EventLevel l) {
  // Higher = more important.
  switch (l) {
    case EventLevel::Error: return 3;
    case EventLevel::Warn: return 2;
    case EventLevel::Info: return 1;
  }
  return 0;
}

std::string fmt_double(double v, int precision) {
  std::ostringstream ss;
  ss.setf(std::ios::fixed);
  ss << std::setprecision(std::max(0, precision)) << v;
  return ss.str();
}

}  // namespace

const char* advisor_issue_kind_label(AdvisorIssueKind k) {
  switch (k) {
    case AdvisorIssueKind::LogisticsNeed: return "Logistics";
    case AdvisorIssueKind::ShipLowFuel: return "Low Fuel";
    case AdvisorIssueKind::ShipDamaged: return "Damaged";
    case AdvisorIssueKind::ColonyHabitationShortfall: return "Habitation";
    case AdvisorIssueKind::ColonyGarrisonProblem: return "Garrison";
  }
  return "Unknown";
}

const char* logistics_need_kind_label(LogisticsNeedKind k) {
  switch (k) {
    case LogisticsNeedKind::Shipyard: return "Shipyard";
    case LogisticsNeedKind::Construction: return "Construction";
    case LogisticsNeedKind::TroopTraining: return "Troop Training";
    case LogisticsNeedKind::IndustryInput: return "Industry Input";
    case LogisticsNeedKind::StockpileTarget: return "Stockpile Target";
    case LogisticsNeedKind::Fuel: return "Fuel";
  }
  return "Unknown";
}

std::vector<AdvisorIssue> advisor_issues_for_faction(const Simulation& sim, Id faction_id,
                                                    const AdvisorIssueOptions& opt) {
  std::vector<AdvisorIssue> out;
  if (faction_id == kInvalidId) return out;

  const auto& s = sim.state();

  auto push_issue = [&](AdvisorIssue issue) {
    if ((int)out.size() >= std::max(0, opt.max_total_issues)) return;
    out.push_back(std::move(issue));
  };

  // --- Logistics needs ---
  if (opt.include_logistics) {
    std::vector<LogisticsNeed> needs = sim.logistics_needs_for_faction(faction_id);
    needs.erase(std::remove_if(needs.begin(), needs.end(), [](const LogisticsNeed& n) {
                  return n.missing_tons <= 1e-9;
                }),
                needs.end());

    std::sort(needs.begin(), needs.end(), [](const LogisticsNeed& a, const LogisticsNeed& b) {
      if (std::fabs(a.missing_tons - b.missing_tons) > 1e-9) return a.missing_tons > b.missing_tons;
      if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
      if (a.colony_id != b.colony_id) return a.colony_id < b.colony_id;
      if (a.mineral != b.mineral) return a.mineral < b.mineral;
      return a.context_id < b.context_id;
    });

    const int cap = std::max(0, opt.max_logistics_issues);
    if ((int)needs.size() > cap) needs.resize((std::size_t)cap);

    for (const auto& n : needs) {
      AdvisorIssue is;
      is.kind = AdvisorIssueKind::LogisticsNeed;
      // Logistics needs are usually actionable but not "fatal", so warn when missing is meaningful.
      is.level = (n.missing_tons > 1e-3) ? EventLevel::Warn : EventLevel::Info;
      is.severity = std::max(0.0, n.missing_tons);
      is.faction_id = faction_id;
      is.colony_id = n.colony_id;
      is.logistics_kind = n.kind;
      is.resource = n.mineral;
      is.context_id = n.context_id;
      is.desired = n.desired_tons;
      is.have = n.have_tons;
      is.missing = n.missing_tons;

      std::ostringstream ss;
      ss << logistics_need_kind_label(n.kind) << ": missing " << fmt_double(n.missing_tons, 2) << "t " << n.mineral;
      if (!n.context_id.empty()) ss << " (" << n.context_id << ")";
      is.summary = ss.str();

      push_issue(std::move(is));
    }
  }

  // --- Ship readiness issues ---
  if (opt.include_ships) {
    const double fuel_thresh = std::clamp(opt.low_fuel_fraction, 0.0, 1.0);
    const double hp_thresh = std::clamp(opt.low_hp_fraction, 0.0, 1.0);

    const auto ship_ids = sorted_keys(s.ships);
    int ship_issue_count = 0;
    for (Id shid : ship_ids) {
      if (ship_issue_count >= std::max(0, opt.max_ship_issues)) break;
      const Ship* sh = find_ptr(s.ships, shid);
      if (!sh) continue;
      if (sh->faction_id != faction_id) continue;

      const ShipDesign* d = sim.find_design(sh->design_id);
      if (!d) continue;

      // Low fuel.
      if (d->fuel_capacity_tons > 1e-9 && fuel_thresh > 1e-9) {
        const double cap = std::max(0.0, d->fuel_capacity_tons);
        const double have = (sh->fuel_tons < 0.0) ? cap : std::max(0.0, sh->fuel_tons);
        const double frac = (cap > 1e-9) ? std::clamp(have / cap, 0.0, 1.0) : 1.0;
        if (frac + 1e-9 < fuel_thresh) {
          AdvisorIssue is;
          is.kind = AdvisorIssueKind::ShipLowFuel;
          is.level = (frac <= 0.05 || have <= 1e-6) ? EventLevel::Warn : EventLevel::Info;
          is.severity = std::max(0.0, fuel_thresh * cap - have);
          is.faction_id = faction_id;
          is.system_id = sh->system_id;
          is.ship_id = shid;
          is.resource = "Fuel";
          is.desired = cap;
          is.have = have;
          is.missing = std::max(0.0, fuel_thresh * cap - have);

          std::ostringstream ss;
          ss << "Fuel " << fmt_double(have, 1) << "/" << fmt_double(cap, 1) << "t (" << fmt_double(frac * 100.0, 0)
             << "%)";
          is.summary = ss.str();

          push_issue(std::move(is));
          ++ship_issue_count;
          if (ship_issue_count >= std::max(0, opt.max_ship_issues)) break;
        }
      }

      // Damage / low HP.
      if (d->max_hp > 1e-9 && hp_thresh > 1e-9) {
        const double cap = std::max(0.0, d->max_hp);
        const double have = std::max(0.0, sh->hp);
        const double frac = (cap > 1e-9) ? std::clamp(have / cap, 0.0, 1.0) : 1.0;
        if (frac + 1e-9 < hp_thresh) {
          AdvisorIssue is;
          is.kind = AdvisorIssueKind::ShipDamaged;
          is.level = (frac <= 0.25) ? EventLevel::Warn : EventLevel::Info;
          is.severity = std::max(0.0, hp_thresh * cap - have);
          is.faction_id = faction_id;
          is.system_id = sh->system_id;
          is.ship_id = shid;
          is.resource = "HP";
          is.desired = cap;
          is.have = have;
          is.missing = std::max(0.0, hp_thresh * cap - have);

          std::ostringstream ss;
          ss << "HP " << fmt_double(have, 1) << "/" << fmt_double(cap, 1) << " (" << fmt_double(frac * 100.0, 0)
             << "%)";
          is.summary = ss.str();

          push_issue(std::move(is));
          ++ship_issue_count;
          if (ship_issue_count >= std::max(0, opt.max_ship_issues)) break;
        }
      }
    }
  }

  // --- Colony health issues ---
  if (opt.include_colonies) {
    const auto colony_ids = sorted_keys(s.colonies);
    int col_issue_count = 0;

    for (Id cid : colony_ids) {
      if (col_issue_count >= std::max(0, opt.max_colony_issues)) break;
      const Colony* c = find_ptr(s.colonies, cid);
      if (!c) continue;
      if (c->faction_id != faction_id) continue;

      // Habitability / habitation shortfall.
      if (opt.include_habitability) {
        const double req = sim.required_habitation_capacity_millions(*c);
        const double have = sim.habitation_capacity_millions(*c);
        if (req > 1e-6 && have + 1e-6 < req) {
          AdvisorIssue is;
          is.kind = AdvisorIssueKind::ColonyHabitationShortfall;
          is.level = EventLevel::Warn;
          is.severity = std::max(0.0, req - have);
          is.faction_id = faction_id;
          is.colony_id = cid;
          is.resource = "Habitation";
          is.desired = req;
          is.have = have;
          is.missing = std::max(0.0, req - have);

          const double hab = sim.body_habitability(c->body_id);
          std::ostringstream ss;
          ss << "Need " << fmt_double(req, 1) << "M; have " << fmt_double(have, 1) << "M (hab "
             << fmt_double(hab * 100.0, 0) << "%)";
          is.summary = ss.str();
          push_issue(std::move(is));
          ++col_issue_count;
          if (col_issue_count >= std::max(0, opt.max_colony_issues)) break;
        }
      }

      // Garrison target problems (stalled training or no capacity).
      if (opt.include_garrison) {
        const double target = std::max(0.0, c->garrison_target_strength);
        const double defenders = std::max(0.0, c->ground_forces);

        // Only flag when the target is meaningfully above current forces.
        if (target > defenders + 1e-6) {
          const double points = std::max(0.0, sim.troop_training_points_per_day(*c));
          const double strength_per_point = std::max(0.0, sim.cfg().troop_strength_per_training_point);
          const double strength_per_day = points * strength_per_point;

          bool problem = false;
          std::string why;

          if (strength_per_day <= 1e-9) {
            problem = true;
            why = "No troop training capacity";
          } else {
            // Mineral affordability check (if training consumes minerals).
            double afford = 1.0;
            const double one_day_strength = std::min(std::max(0.0, c->troop_training_queue), strength_per_day);
            if (one_day_strength > 1e-9) {
              if (sim.cfg().troop_training_duranium_per_strength > 1e-9) {
                const double need = one_day_strength * sim.cfg().troop_training_duranium_per_strength;
                const double have_m = c->minerals.contains("Duranium") ? c->minerals.at("Duranium") : 0.0;
                if (need > 1e-9) afford = std::min(afford, have_m / need);
              }
              if (sim.cfg().troop_training_neutronium_per_strength > 1e-9) {
                const double need = one_day_strength * sim.cfg().troop_training_neutronium_per_strength;
                const double have_m = c->minerals.contains("Neutronium") ? c->minerals.at("Neutronium") : 0.0;
                if (need > 1e-9) afford = std::min(afford, have_m / need);
              }
              afford = std::clamp(afford, 0.0, 1.0);
              if (afford <= 1e-6) {
                problem = true;
                why = "Troop training stalled (missing minerals)";
              }
            }
          }

          if (problem) {
            AdvisorIssue is;
            is.kind = AdvisorIssueKind::ColonyGarrisonProblem;
            is.level = EventLevel::Warn;
            is.severity = std::max(0.0, target - defenders);
            is.faction_id = faction_id;
            is.colony_id = cid;
            is.resource = "Garrison";
            is.desired = target;
            is.have = defenders;
            is.missing = std::max(0.0, target - defenders);

            std::ostringstream ss;
            ss << why << ": " << fmt_double(defenders, 1) << "/" << fmt_double(target, 1);
            is.summary = ss.str();
            push_issue(std::move(is));
            ++col_issue_count;
            if (col_issue_count >= std::max(0, opt.max_colony_issues)) break;
          }
        }
      }
    }
  }

  // Deterministic sort across all issue kinds.
  std::sort(out.begin(), out.end(), [](const AdvisorIssue& a, const AdvisorIssue& b) {
    const int ar = level_rank(a.level);
    const int br = level_rank(b.level);
    if (ar != br) return ar > br;
    if (std::fabs(a.severity - b.severity) > 1e-9) return a.severity > b.severity;
    if (a.kind != b.kind) return (int)a.kind < (int)b.kind;
    if (a.colony_id != b.colony_id) return a.colony_id < b.colony_id;
    if (a.ship_id != b.ship_id) return a.ship_id < b.ship_id;
    if (a.system_id != b.system_id) return a.system_id < b.system_id;
    if (a.resource != b.resource) return a.resource < b.resource;
    if (a.context_id != b.context_id) return a.context_id < b.context_id;
    return a.summary < b.summary;
  });

  // Apply max_total_issues cap in case a caller set it smaller than per-kind caps.
  const int cap = std::max(0, opt.max_total_issues);
  if ((int)out.size() > cap) out.resize((std::size_t)cap);
  return out;
}

}  // namespace nebula4x
