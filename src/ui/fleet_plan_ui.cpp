#include "ui/fleet_plan_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/order_planner.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/json.h"

#include "ui/order_plan_ui.h"

namespace nebula4x::ui {
namespace {

struct FleetPlanRow {
  Id ship_id{kInvalidId};

  std::string ship_name;
  std::string design_name;
  std::string start_system;

  double speed_km_s{0.0};

  // Orders.
  int base_queue_orders{0};
  int compiled_added_orders{0};
  int final_queue_orders{0};

  bool compile_ok{false};
  std::string compile_error;

  // Planner.
  bool plan_ok{false};
  bool truncated{false};
  std::string truncated_reason;
  bool all_steps_feasible{false};

  double eta_days{0.0};

  double fuel_cap_tons{0.0};
  double fuel_start_tons{0.0};
  double fuel_end_tons{0.0};
  double fuel_min_tons{0.0};

  bool reserve_warning{false};

  // For detail view.
  std::vector<Order> final_queue;
  OrderPlan plan;
};

struct FleetPlanState {
  bool auto_refresh{true};
  bool have_plan{false};
  int last_day{-1};
  int last_hour{-1};

  std::uint64_t signature{0};

  bool ok{false};
  bool truncated{false};
  std::string message;

  std::vector<FleetPlanRow> rows;

  Id selected_ship_for_details{kInvalidId};

  // Detail rendering toggles.
  bool detail_show_system{true};
  bool detail_show_position{false};
  bool detail_show_notes{true};
  bool detail_collapse_jumps{true};
  int detail_max_rows{256};
};

FleetPlanState& fleet_plan_state(const char* id_suffix) {
  static std::unordered_map<std::string, FleetPlanState> states;
  const std::string key = id_suffix ? std::string(id_suffix) : std::string("##fleet_plan_default");
  return states[key];
}

std::string fmt_days(double days) {
  if (!std::isfinite(days)) return "∞";
  if (days < 0.0) days = 0.0;
  char buf[64];
  if (days < 10.0) {
    std::snprintf(buf, sizeof(buf), "%.2f", days);
  } else if (days < 100.0) {
    std::snprintf(buf, sizeof(buf), "%.1f", days);
  } else {
    std::snprintf(buf, sizeof(buf), "%.0f", days);
  }
  return std::string(buf);
}

std::string fmt_tons(double tons) {
  if (!std::isfinite(tons)) return "?";
  if (std::abs(tons - std::round(tons)) < 1e-6) {
    const long long v = static_cast<long long>(std::llround(tons));
    return std::to_string(v);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.1f", tons);
  return std::string(buf);
}

std::string fmt_pct(double frac01) {
  if (!std::isfinite(frac01)) return "?";
  frac01 = std::clamp(frac01, 0.0, 1.0);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f%%", frac01 * 100.0);
  return std::string(buf);
}

std::string ship_label_fow(const Simulation& sim, Id ship_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& st = sim.state();
  const auto* sh = find_ptr(st.ships, ship_id);
  if (!sh) return "<ship " + std::to_string(ship_id) + ">";

  if (fog_of_war && viewer_faction_id != kInvalidId) {
    if (!sim.is_ship_detected_by_faction(viewer_faction_id, ship_id)) {
      return "Ship #" + std::to_string(ship_id);
    }
  }

  if (!sh->name.empty()) return sh->name;
  return "Ship #" + std::to_string(ship_id);
}

std::string system_label_fow(const Simulation& sim, Id system_id, Id viewer_faction_id, bool fog_of_war) {
  const auto& st = sim.state();
  const auto* sys = find_ptr(st.systems, system_id);
  if (!sys) return "<system " + std::to_string(system_id) + ">";

  if (fog_of_war && viewer_faction_id != kInvalidId) {
    if (!sim.is_system_discovered_by_faction(viewer_faction_id, system_id)) {
      return "System #" + std::to_string(system_id);
    }
  }

  return sys->name;
}

std::uint64_t fnv1a64_init() { return 1469598103934665603ull; }

void fnv1a64_add(std::uint64_t& h, const void* data, std::size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < len; ++i) {
    h ^= static_cast<std::uint64_t>(p[i]);
    h *= 1099511628211ull;
  }
}

void fnv1a64_add_u64(std::uint64_t& h, std::uint64_t v) { fnv1a64_add(h, &v, sizeof(v)); }

void fnv1a64_add_i64(std::uint64_t& h, std::int64_t v) { fnv1a64_add(h, &v, sizeof(v)); }

void fnv1a64_add_str(std::uint64_t& h, const std::string& s) {
  fnv1a64_add(h, s.data(), s.size());
  const unsigned char z = 0;
  fnv1a64_add(h, &z, 1);
}

std::uint64_t compute_signature(const Simulation& sim, Id fleet_id, const std::vector<Order>& orders,
                                const FleetPlanPreviewOptions& opts) {
  std::uint64_t h = fnv1a64_init();

  fnv1a64_add_u64(h, static_cast<std::uint64_t>(fleet_id));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.viewer_faction_id));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.fog_of_war));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.smart_apply));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.append_when_applying));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.restrict_to_discovered));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.predict_orbits));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.simulate_refuel));
  fnv1a64_add_i64(h, static_cast<std::int64_t>(opts.max_orders));
  fnv1a64_add_i64(h, static_cast<std::int64_t>(opts.max_ships));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.highlight_reserve));
  fnv1a64_add_u64(h, static_cast<std::uint64_t>(opts.collapse_jump_chains));

  // Reserve fraction: quantize for stable hashing.
  const std::int64_t reserve_bp = static_cast<std::int64_t>(std::llround(std::clamp(opts.reserve_fraction, 0.0, 1.0) * 10000.0));
  fnv1a64_add_i64(h, reserve_bp);

  // Time affects planning (ship positions/fuel can change each tick).
  const auto& st = sim.state();
  fnv1a64_add_i64(h, static_cast<std::int64_t>(st.date.days_since_epoch()));
  fnv1a64_add_i64(h, static_cast<std::int64_t>(st.hour_of_day));

  // Template orders.
  fnv1a64_add_i64(h, static_cast<std::int64_t>(orders.size()));
  for (const auto& o : orders) {
    fnv1a64_add_str(h, order_to_string(o));
  }

  return h;
}

void compute_plan(FleetPlanState& st, const Simulation& sim, Id fleet_id, const std::vector<Order>& orders_to_apply,
                  const FleetPlanPreviewOptions& opts) {
  st.rows.clear();
  st.ok = false;
  st.truncated = false;
  st.message.clear();

  const auto& gs = sim.state();
  const auto* fl = find_ptr(gs.fleets, fleet_id);
  if (!fl) {
    st.message = "Fleet not found.";
    return;
  }

  std::vector<Id> ship_ids = fl->ship_ids;
  std::sort(ship_ids.begin(), ship_ids.end());

  const int total = static_cast<int>(ship_ids.size());
  const int max_ships = std::clamp(opts.max_ships, 1, 4096);
  const int n = std::min(total, max_ships);
  if (n < total) {
    st.truncated = true;
  }

  st.rows.reserve(static_cast<std::size_t>(n));

  // Planner options.
  OrderPlannerOptions po;
  po.max_orders = std::clamp(opts.max_orders, 1, 4096);
  po.predict_orbits = opts.predict_orbits;
  po.simulate_refuel = opts.simulate_refuel;
  po.viewer_faction_id = (opts.fog_of_war ? opts.viewer_faction_id : kInvalidId);

  const double reserve_frac = std::clamp(opts.reserve_fraction, 0.0, 1.0);

  for (int i = 0; i < n; ++i) {
    const Id sid = ship_ids[static_cast<std::size_t>(i)];

    FleetPlanRow row;
    row.ship_id = sid;
    row.ship_name = ship_label_fow(sim, sid, opts.viewer_faction_id, opts.fog_of_war);

    const auto* sh = find_ptr(gs.ships, sid);
    if (!sh) {
      row.compile_ok = false;
      row.compile_error = "Ship not found";
      st.rows.push_back(std::move(row));
      continue;
    }

    row.speed_km_s = sh->speed_km_s;
    row.start_system = system_label_fow(sim, sh->system_id, opts.viewer_faction_id, opts.fog_of_war);

    const ShipDesign* d = sim.find_design(sh->design_id);
    row.design_name = d ? d->name : (std::string("Design #") + sh->design_id);
    row.fuel_cap_tons = d ? std::max(0.0, d->fuel_capacity_tons) : 0.0;

    // Base queue (if appending).
    if (opts.append_when_applying) {
      if (const auto* so = find_ptr(gs.ship_orders, sid)) {
        row.final_queue = so->queue;
      }
    }
    row.base_queue_orders = static_cast<int>(row.final_queue.size());

    // Orders to add.
    std::vector<Order> compiled;
    std::string err;
    if (opts.smart_apply) {
      const bool ok = sim.compile_orders_smart(sid, orders_to_apply, opts.append_when_applying, opts.restrict_to_discovered,
                                               &compiled, &err);
      row.compile_ok = ok;
      row.compile_error = err;
      if (!ok) {
        st.rows.push_back(std::move(row));
        continue;
      }
    } else {
      row.compile_ok = true;
      compiled = orders_to_apply;
    }

    row.compiled_added_orders = static_cast<int>(compiled.size());
    row.final_queue.insert(row.final_queue.end(), compiled.begin(), compiled.end());
    row.final_queue_orders = static_cast<int>(row.final_queue.size());

    row.plan = compute_order_plan_for_queue(sim, sid, row.final_queue, po);
    row.plan_ok = row.plan.ok;
    row.truncated = row.plan.truncated;
    row.truncated_reason = row.plan.truncated_reason;
    row.eta_days = row.plan.total_eta_days;
    row.fuel_start_tons = row.plan.start_fuel_tons;
    row.fuel_end_tons = row.plan.end_fuel_tons;

    bool all_ok = true;
    double min_fuel = row.plan.start_fuel_tons;
    for (const auto& step : row.plan.steps) {
      if (!step.feasible) all_ok = false;
      min_fuel = std::min(min_fuel, step.fuel_before_tons);
      min_fuel = std::min(min_fuel, step.fuel_after_tons);
    }
    row.all_steps_feasible = all_ok;
    row.fuel_min_tons = min_fuel;

    if (opts.highlight_reserve && row.fuel_cap_tons > 1e-9) {
      row.reserve_warning = (row.fuel_min_tons < row.fuel_cap_tons * reserve_frac);
    }

    st.rows.push_back(std::move(row));
  }

  st.ok = true;

  // Preserve the selected ship if it's still present; otherwise fall back.
  if (st.selected_ship_for_details != kInvalidId) {
    const bool still = std::any_of(st.rows.begin(), st.rows.end(), [&](const FleetPlanRow& r) { return r.ship_id == st.selected_ship_for_details; });
    if (!still) st.selected_ship_for_details = kInvalidId;
  }
  if (st.selected_ship_for_details == kInvalidId && !st.rows.empty()) {
    st.selected_ship_for_details = st.rows.front().ship_id;
  }

  // Compose summary message.
  st.message = fl->name;
  st.message += " (" + std::to_string(total) + " ship" + (total == 1 ? "" : "s") + ")";
}

std::string fleet_summary_to_csv(const FleetPlanState& st) {
  std::ostringstream out;
  out << "ship_id,ship,design,start_system,speed_km_s,base_orders,added_orders,final_orders,eta_days,"
         "fuel_start_tons,fuel_end_tons,fuel_min_tons,fuel_cap_tons,feasible,truncated,truncated_reason,reserve_warning,compile_ok,compile_error\n";

  for (const auto& r : st.rows) {
    const auto q = [](const std::string& s) {
      std::string t;
      t.reserve(s.size() + 2);
      t.push_back('"');
      for (char c : s) {
        if (c == '"') t.push_back('"');
        t.push_back(c);
      }
      t.push_back('"');
      return t;
    };

    out << r.ship_id << ','
        << q(r.ship_name) << ','
        << q(r.design_name) << ','
        << q(r.start_system) << ','
        << r.speed_km_s << ','
        << r.base_queue_orders << ','
        << r.compiled_added_orders << ','
        << r.final_queue_orders << ','
        << r.eta_days << ','
        << r.fuel_start_tons << ','
        << r.fuel_end_tons << ','
        << r.fuel_min_tons << ','
        << r.fuel_cap_tons << ','
        << (r.all_steps_feasible ? 1 : 0) << ','
        << (r.truncated ? 1 : 0) << ','
        << q(r.truncated_reason) << ','
        << (r.reserve_warning ? 1 : 0) << ','
        << (r.compile_ok ? 1 : 0) << ','
        << q(r.compile_error) << '\n';
  }

  return out.str();
}

std::string fleet_summary_to_json(const Simulation& sim, Id fleet_id, const FleetPlanState& st,
                                 const FleetPlanPreviewOptions& opts, int indent = 2) {
  using nebula4x::json::Array;
  using nebula4x::json::Object;

  Object root;
  root["ok"] = st.ok;
  root["fleet_id"] = static_cast<double>(static_cast<unsigned long long>(fleet_id));
  {
    const auto* fl = find_ptr(sim.state().fleets, fleet_id);
    root["fleet_name"] = fl ? fl->name : std::string();
  }
  root["truncated"] = st.truncated;
  root["message"] = st.message;

  {
    Object o;
    o["viewer_faction_id"] = static_cast<double>(static_cast<unsigned long long>(opts.viewer_faction_id));
    o["fog_of_war"] = opts.fog_of_war;
    o["smart_apply"] = opts.smart_apply;
    o["append_when_applying"] = opts.append_when_applying;
    o["restrict_to_discovered"] = opts.restrict_to_discovered;
    o["predict_orbits"] = opts.predict_orbits;
    o["simulate_refuel"] = opts.simulate_refuel;
    o["max_orders"] = static_cast<double>(opts.max_orders);
    o["max_ships"] = static_cast<double>(opts.max_ships);
    o["reserve_fraction"] = opts.reserve_fraction;
    o["highlight_reserve"] = opts.highlight_reserve;
    o["collapse_jump_chains"] = opts.collapse_jump_chains;
    root["options"] = std::move(o);
  }

  Array rows;
  rows.reserve(st.rows.size());
  for (const auto& r : st.rows) {
    Object o;
    o["ship_id"] = static_cast<double>(static_cast<unsigned long long>(r.ship_id));
    o["ship"] = r.ship_name;
    o["design"] = r.design_name;
    o["start_system"] = r.start_system;
    o["speed_km_s"] = r.speed_km_s;
    o["base_orders"] = static_cast<double>(r.base_queue_orders);
    o["added_orders"] = static_cast<double>(r.compiled_added_orders);
    o["final_orders"] = static_cast<double>(r.final_queue_orders);
    o["plan_ok"] = r.plan_ok;
    o["eta_days"] = r.eta_days;
    o["fuel_start_tons"] = r.fuel_start_tons;
    o["fuel_end_tons"] = r.fuel_end_tons;
    o["fuel_min_tons"] = r.fuel_min_tons;
    o["fuel_cap_tons"] = r.fuel_cap_tons;
    o["feasible"] = r.all_steps_feasible;
    o["truncated"] = r.truncated;
    o["truncated_reason"] = r.truncated_reason;
    o["reserve_warning"] = r.reserve_warning;
    o["compile_ok"] = r.compile_ok;
    o["compile_error"] = r.compile_error;
    rows.push_back(std::move(o));
  }
  root["ships"] = std::move(rows);

  return nebula4x::json::stringify(root, indent);
}

}  // namespace

void draw_fleet_plan_preview(const Simulation& sim, Id fleet_id, const std::vector<Order>& orders_to_apply,
                            const FleetPlanPreviewOptions& opts, const char* id_suffix) {
  FleetPlanState& st = fleet_plan_state(id_suffix);

  // Detect input changes and invalidate cache.
  const std::uint64_t sig = compute_signature(sim, fleet_id, orders_to_apply, opts);
  if (sig != st.signature) {
    st.signature = sig;
    st.have_plan = false;
  }

  // Controls.
  ImGui::PushID(id_suffix ? id_suffix : "##fleet_plan");

  if (ImGui::Checkbox("Auto-refresh##fleet_plan_auto_refresh", &st.auto_refresh)) {
    // no-op
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh##fleet_plan_refresh")) {
    st.have_plan = false;
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Copy summary CSV##fleet_plan_csv")) {
    const std::string csv = fleet_summary_to_csv(st);
    ImGui::SetClipboardText(csv.c_str());
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Copy summary JSON##fleet_plan_json")) {
    const std::string js = fleet_summary_to_json(sim, fleet_id, st, opts, 2);
    ImGui::SetClipboardText(js.c_str());
  }

  const auto& gs = sim.state();
  const int day = static_cast<int>(gs.date.days_since_epoch());
  const int hour = gs.hour_of_day;
  const bool time_changed = (day != st.last_day || hour != st.last_hour);

  if (!st.have_plan || (st.auto_refresh && time_changed)) {
    compute_plan(st, sim, fleet_id, orders_to_apply, opts);
    st.have_plan = true;
    st.last_day = day;
    st.last_hour = hour;
  }

  // Summary.
  if (!st.ok) {
    ImGui::Text("Fleet preview: %s", st.message.c_str());
    ImGui::PopID();
    return;
  }

  ImGui::Text("Fleet: %s", st.message.c_str());
  if (st.truncated) {
    ImGui::SameLine();
    ImGui::TextDisabled("(showing first %d ships)", std::clamp(opts.max_ships, 1, 4096));
  }

  // Aggregate quick stats.
  int feasible_count = 0;
  int warn_count = 0;
  int truncated_count = 0;
  int compile_fail = 0;
  for (const auto& r : st.rows) {
    if (!r.compile_ok) {
      compile_fail++;
      continue;
    }
    if (r.all_steps_feasible) feasible_count++;
    if (r.reserve_warning) warn_count++;
    if (r.truncated) truncated_count++;
  }
  ImGui::TextDisabled("Ships: %d | Feasible: %d | Reserve warnings: %d | Truncated: %d | Compile failed: %d",
                      static_cast<int>(st.rows.size()), feasible_count, warn_count, truncated_count, compile_fail);

  ImGui::Spacing();

  const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

  const float table_h = std::min(260.0f, ImGui::GetContentRegionAvail().y * 0.45f);
  if (ImGui::BeginTable("##fleet_plan_table", 10, flags, ImVec2(0.0f, table_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Ship", ImGuiTableColumnFlags_WidthStretch, 220.0f);
    ImGui::TableSetupColumn("Design", ImGuiTableColumnFlags_WidthStretch, 180.0f);
    ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthStretch, 120.0f);
    ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Orders", ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableSetupColumn("ETA (d)", ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("End fuel", ImGuiTableColumnFlags_WidthFixed, 92.0f);
    ImGui::TableSetupColumn("Min fuel", ImGuiTableColumnFlags_WidthFixed, 92.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 200.0f);
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableHeadersRow();

    for (const auto& r : st.rows) {
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      const bool selected = (r.ship_id == st.selected_ship_for_details);
      if (ImGui::Selectable(r.ship_name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
        st.selected_ship_for_details = r.ship_id;
      }

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(r.design_name.c_str());

      ImGui::TableSetColumnIndex(2);
      ImGui::TextUnformatted(r.start_system.c_str());

      ImGui::TableSetColumnIndex(3);
      if (r.speed_km_s > 0.0) {
        ImGui::Text("%.0f", r.speed_km_s);
      } else {
        ImGui::TextDisabled("--");
      }

      ImGui::TableSetColumnIndex(4);
      if (!r.compile_ok) {
        ImGui::TextDisabled("--");
      } else {
        ImGui::Text("%d", r.final_queue_orders);
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::Text("Base: %d", r.base_queue_orders);
          ImGui::Text("Added: %d", r.compiled_added_orders);
          ImGui::EndTooltip();
        }
      }

      ImGui::TableSetColumnIndex(5);
      if (!r.compile_ok || !r.plan_ok) {
        ImGui::TextDisabled("--");
      } else {
        ImGui::TextUnformatted(fmt_days(r.eta_days).c_str());
      }

      ImGui::TableSetColumnIndex(6);
      if (!r.compile_ok || !r.plan_ok) {
        ImGui::TextDisabled("--");
      } else {
        if (r.fuel_cap_tons > 1e-9) {
          const std::string s = fmt_tons(r.fuel_end_tons) + "/" + fmt_tons(r.fuel_cap_tons);
          ImGui::TextUnformatted(s.c_str());
        } else {
          ImGui::TextUnformatted(fmt_tons(r.fuel_end_tons).c_str());
        }
      }

      ImGui::TableSetColumnIndex(7);
      if (!r.compile_ok || !r.plan_ok) {
        ImGui::TextDisabled("--");
      } else {
        if (r.fuel_cap_tons > 1e-9) {
          const std::string s = fmt_tons(r.fuel_min_tons) + "/" + fmt_tons(r.fuel_cap_tons);
          ImGui::TextUnformatted(s.c_str());
          if (r.reserve_warning && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Reserve warning: min %.0f%%", std::clamp(opts.reserve_fraction, 0.0, 1.0) * 100.0);
            ImGui::EndTooltip();
          }
        } else {
          ImGui::TextUnformatted(fmt_tons(r.fuel_min_tons).c_str());
        }
      }

      ImGui::TableSetColumnIndex(8);
      if (!r.compile_ok) {
        ImGui::TextDisabled("Compile failed");
        if (!r.compile_error.empty() && ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(r.compile_error.c_str());
          ImGui::EndTooltip();
        }
      } else if (!r.plan_ok) {
        ImGui::TextDisabled("No plan");
      } else {
        if (!r.all_steps_feasible) {
          ImGui::TextDisabled("Infeasible");
        } else if (r.truncated) {
          ImGui::TextDisabled("Truncated");
        } else {
          ImGui::TextUnformatted("OK");
        }
        if (r.truncated && !r.truncated_reason.empty() && ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::Text("Truncated: %s", r.truncated_reason.c_str());
          ImGui::EndTooltip();
        }
      }

      ImGui::TableSetColumnIndex(9);
      if (ImGui::SmallButton((std::string("Show##fleet_plan_show_") + std::to_string(r.ship_id)).c_str())) {
        st.selected_ship_for_details = r.ship_id;
      }
    }

    ImGui::EndTable();
  }

  // Detailed plan for selected ship.
  auto it = std::find_if(st.rows.begin(), st.rows.end(), [&](const FleetPlanRow& r) {
    return r.ship_id == st.selected_ship_for_details;
  });

  if (it != st.rows.end() && it->compile_ok && it->plan_ok) {
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::Text("Details: %s", it->ship_name.c_str());

    ImGui::Checkbox("Show system##fleet_plan_detail_system", &st.detail_show_system);
    ImGui::SameLine();
    ImGui::Checkbox("Show position##fleet_plan_detail_pos", &st.detail_show_position);
    ImGui::SameLine();
    ImGui::Checkbox("Show notes##fleet_plan_detail_notes", &st.detail_show_notes);

    ImGui::Checkbox("Collapse jump chains##fleet_plan_detail_collapse", &st.detail_collapse_jumps);
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    ImGui::InputInt("Max rows##fleet_plan_detail_maxrows", &st.detail_max_rows);
    ImGui::PopItemWidth();

    OrderPlanRenderOptions ro;
    ro.viewer_faction_id = opts.viewer_faction_id;
    ro.fog_of_war = opts.fog_of_war;
    ro.max_rows = std::clamp(st.detail_max_rows, 1, 4096);
    ro.show_system = st.detail_show_system;
    ro.show_position = st.detail_show_position;
    ro.show_note = st.detail_show_notes;
    ro.collapse_jump_chains = st.detail_collapse_jumps;

    draw_order_plan_table(sim, it->final_queue, it->plan, it->fuel_cap_tons, ro, "##fleet_plan_detail_table");
  } else if (it != st.rows.end() && !it->compile_ok) {
    ImGui::Spacing();
    ImGui::TextDisabled("Details unavailable: compile failed (%s)", it->compile_error.c_str());
  } else if (it != st.rows.end() && !it->plan_ok) {
    ImGui::Spacing();
    ImGui::TextDisabled("Details unavailable: no plan");
  }

  ImGui::PopID();
}

}  // namespace nebula4x::ui
