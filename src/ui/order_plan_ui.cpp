#include "ui/order_plan_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/json.h"

#include "ui/order_ui.h"

namespace nebula4x::ui {
namespace {

std::string id_fallback(const char* kind, Id id) {
  std::ostringstream ss;
  ss << kind << " #" << static_cast<unsigned long long>(id);
  return ss.str();
}

bool can_show_system_name(const Simulation& sim, Id viewer_faction_id, bool fog_of_war, Id system_id) {
  if (system_id == kInvalidId) return false;
  if (!fog_of_war) return true;
  if (viewer_faction_id == kInvalidId) return true;
  return sim.is_system_discovered_by_faction(viewer_faction_id, system_id);
}

std::string system_label_fow(const Simulation& sim, Id system_id, Id viewer_faction_id, bool fog_of_war) {
  const GameState& st = sim.state();
  const auto* sys = find_ptr(st.systems, system_id);
  if (!sys) return "(unknown system)";

  if (!can_show_system_name(sim, viewer_faction_id, fog_of_war, system_id)) {
    return id_fallback("System", system_id);
  }

  if (sys->name.empty()) return id_fallback("System", system_id);
  return sys->name;
}

std::string jump_point_label_fow(const Simulation& sim, Id jump_id, Id viewer_faction_id, bool fog_of_war) {
  const GameState& st = sim.state();
  const auto* jp = find_ptr(st.jump_points, jump_id);
  if (!jp) return id_fallback("Jump", jump_id);

  // Gate the name based on the origin system discovery.
  if (jp->system_id != kInvalidId && !can_show_system_name(sim, viewer_faction_id, fog_of_war, jp->system_id)) {
    return id_fallback("Jump", jump_id);
  }

  std::string nm = jp->name.empty() ? id_fallback("Jump", jump_id) : jp->name;

  // Append destination system if visible.
  Id dst_sys = kInvalidId;
  if (jp->linked_jump_id != kInvalidId) {
    const auto* other = find_ptr(st.jump_points, jp->linked_jump_id);
    if (other) dst_sys = other->system_id;
  }
  if (dst_sys != kInvalidId && can_show_system_name(sim, viewer_faction_id, fog_of_war, dst_sys)) {
    nm += " -> " + system_label_fow(sim, dst_sys, viewer_faction_id, fog_of_war);
  }

  return nm;
}

std::string pos_compact(Vec2 p) {
  std::ostringstream oss;
  oss.setf(std::ios::fixed);
  oss.precision(2);
  oss << p.x << "," << p.y;
  return oss.str();
}

std::string csv_escape(const std::string& s) {
  bool needs_quotes = false;
  for (char c : s) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) return s;

  std::string out;
  out.reserve(s.size() + 8);
  out.push_back('"');
  for (char c : s) {
    if (c == '"') out.push_back('"');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

struct RowSpan {
  std::size_t start{0};
  std::size_t end{0};  // exclusive
  bool is_jump_chain{false};
};

bool is_jump_travel(const Order& o) {
  return std::holds_alternative<TravelViaJump>(o);
}

std::vector<RowSpan> build_spans(const std::vector<Order>& queue, std::size_t show_n_orders, bool collapse_jump_chains) {
  std::vector<RowSpan> spans;
  spans.reserve(show_n_orders);

  std::size_t i = 0;
  while (i < show_n_orders) {
    if (collapse_jump_chains && is_jump_travel(queue[i])) {
      const std::size_t start = i;
      while (i < show_n_orders && is_jump_travel(queue[i])) {
        ++i;
      }
      spans.push_back(RowSpan{start, i, /*is_jump_chain=*/(i - start) > 1});
    } else {
      spans.push_back(RowSpan{i, i + 1, /*is_jump_chain=*/false});
      ++i;
    }
  }

  return spans;
}

std::string combine_notes(const std::vector<PlannedOrderStep>& steps, std::size_t start, std::size_t end) {
  std::string out;
  for (std::size_t i = start; i < end; ++i) {
    if (steps[i].note.empty()) continue;
    if (!out.empty()) out += "\n";
    out += steps[i].note;
  }
  return out;
}

PlannedOrderStep aggregate_steps(const std::vector<PlannedOrderStep>& steps, std::size_t start, std::size_t end) {
  PlannedOrderStep agg;
  if (start >= end) return agg;

  agg = steps[end - 1];
  agg.delta_days = 0.0;
  agg.fuel_before_tons = steps[start].fuel_before_tons;
  agg.feasible = true;
  for (std::size_t i = start; i < end; ++i) {
    agg.delta_days += steps[i].delta_days;
    if (!steps[i].feasible) agg.feasible = false;
  }
  agg.note = combine_notes(steps, start, end);
  return agg;
}

std::string note_first_line_compact(std::string note, std::size_t max_len = 120) {
  const auto nl = note.find('\n');
  if (nl != std::string::npos) note = note.substr(0, nl);
  if (note.size() > max_len) note = note.substr(0, max_len - 3) + "...";
  return note;
}

std::string jump_chain_summary_label(const Simulation& sim, const std::vector<Order>& queue,
                                    const std::vector<PlannedOrderStep>& steps, std::size_t start,
                                    std::size_t end, Id viewer_faction_id, bool fog_of_war) {
  const int count = static_cast<int>(end - start);

  // Attempt to infer the source system from the first jump point.
  Id src_sys = kInvalidId;
  if (const auto* tj = std::get_if<TravelViaJump>(&queue[start])) {
    const auto* jp = find_ptr(sim.state().jump_points, tj->jump_point_id);
    if (jp) src_sys = jp->system_id;
  }
  if (src_sys == kInvalidId && start > 0) {
    src_sys = steps[start - 1].system_id;
  }

  const Id dst_sys = steps[end - 1].system_id;

  std::string src = (src_sys == kInvalidId) ? "(unknown)" : system_label_fow(sim, src_sys, viewer_faction_id, fog_of_war);
  std::string dst = (dst_sys == kInvalidId) ? "(unknown)" : system_label_fow(sim, dst_sys, viewer_faction_id, fog_of_war);

  std::ostringstream ss;
  ss << "Jump chain (" << count << "): " << src << " -> " << dst;
  return ss.str();
}

void jump_chain_tooltip(const Simulation& sim, const std::vector<Order>& queue, std::size_t start, std::size_t end,
                        Id viewer_faction_id, bool fog_of_war) {
  ImGui::TextDisabled("Legs:");
  for (std::size_t i = start; i < end; ++i) {
    const auto* tj = std::get_if<TravelViaJump>(&queue[i]);
    if (!tj) continue;
    const std::string jp = jump_point_label_fow(sim, tj->jump_point_id, viewer_faction_id, fog_of_war);
    ImGui::BulletText("%s", jp.c_str());
  }
}

}  // namespace

void draw_order_plan_table(const Simulation& sim, const std::vector<Order>& queue, const OrderPlan& plan,
                           double fuel_capacity_tons, const OrderPlanRenderOptions& opts, const char* table_id) {
  if (!plan.ok) {
    ImGui::TextDisabled("Plan unavailable");
    return;
  }

  const int max_orders = std::clamp(opts.max_rows, 1, 16384);
  const std::size_t n_orders = std::min<std::size_t>(queue.size(), plan.steps.size());
  const std::size_t show_n_orders = std::min<std::size_t>(n_orders, static_cast<std::size_t>(max_orders));

  const auto spans = build_spans(queue, show_n_orders, opts.collapse_jump_chains);
  const int rows = static_cast<int>(spans.size());

  // Clipboard exports.
  if (ImGui::SmallButton("Copy plan CSV")) {
    const std::string csv = order_plan_to_csv(sim, queue, plan, opts);
    ImGui::SetClipboardText(csv.c_str());
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Copies a CSV table of the simulated mission plan.\n"
        "Tip: enable 'Collapse jump chains' to reduce multi-jump clutter.");
  }

  ImGui::SameLine();
  if (ImGui::SmallButton("Copy plan JSON")) {
    const std::string js = order_plan_to_json(sim, queue, plan, opts, 2);
    ImGui::SetClipboardText(js.c_str());
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Copies a JSON object containing plan metadata + per-row step details.");
  }

  ImGui::SameLine();
  if (opts.collapse_jump_chains) {
    ImGui::TextDisabled("Rows: %d (from %d orders)%s", rows, static_cast<int>(show_n_orders),
                        (plan.truncated ? " (truncated)" : ""));
  } else {
    ImGui::TextDisabled("Rows: %d%s", static_cast<int>(show_n_orders), (plan.truncated ? " (truncated)" : ""));
  }

  if (plan.truncated) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", plan.truncated_reason.c_str());
  }

  int cols = 7;  // idx, order, eta, delta, fuel, system, note
  if (!opts.show_system) cols -= 1;
  if (!opts.show_note) cols -= 1;
  if (opts.show_position) cols += 1;

  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;

  const float height = 240.0f;
  if (!ImGui::BeginTable(table_id, cols, flags, ImVec2(0, height))) return;

  ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 46.0f);
  ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthStretch, 240.0f);
  ImGui::TableSetupColumn("ETA (d)", ImGuiTableColumnFlags_WidthFixed, 72.0f);
  ImGui::TableSetupColumn("Δ (d)", ImGuiTableColumnFlags_WidthFixed, 62.0f);
  ImGui::TableSetupColumn("Fuel (t)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
  if (opts.show_system) ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed, 140.0f);
  if (opts.show_position) ImGui::TableSetupColumn("Pos (mkm)", ImGuiTableColumnFlags_WidthFixed, 120.0f);
  if (opts.show_note) ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 200.0f);
  ImGui::TableHeadersRow();

  for (int r = 0; r < rows; ++r) {
    const RowSpan& sp = spans[static_cast<std::size_t>(r)];
    const std::size_t start = sp.start;
    const std::size_t end = sp.end;

    const bool span_is_chain = opts.collapse_jump_chains && (end - start) > 1;

    const PlannedOrderStep stp = (end - start == 1) ? plan.steps[start] : aggregate_steps(plan.steps, start, end);

    ImGui::TableNextRow();

    // Index / range
    ImGui::TableSetColumnIndex(0);
    if (end - start == 1) {
      ImGui::Text("%d", static_cast<int>(start + 1));
    } else {
      ImGui::Text("%d-%d", static_cast<int>(start + 1), static_cast<int>(end));
    }

    // Order label
    ImGui::TableSetColumnIndex(1);

    std::string ord_str;
    if (span_is_chain) {
      ord_str = jump_chain_summary_label(sim, queue, plan.steps, start, end, opts.viewer_faction_id, opts.fog_of_war);
    } else {
      ord_str = order_to_ui_string(sim, queue[start], opts.viewer_faction_id, opts.fog_of_war);
    }

    std::string disp = ord_str;
    if (!stp.feasible) disp = "(!) " + disp;

    const std::string sel_id = std::string("##plan_row_") + std::to_string(static_cast<unsigned long long>(start));
    ImGui::Selectable((disp + sel_id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::TextUnformatted(ord_str.c_str());
      ImGui::Separator();
      ImGui::Text("ETA: +%.2f d (Δ %.2f)", stp.eta_days, stp.delta_days);
      if (fuel_capacity_tons > 1e-9) {
        ImGui::Text("Fuel: %.0f -> %.0f / %.0f", stp.fuel_before_tons, stp.fuel_after_tons, fuel_capacity_tons);
      } else {
        ImGui::Text("Fuel: %.0f -> %.0f", stp.fuel_before_tons, stp.fuel_after_tons);
      }

      if (span_is_chain) {
        ImGui::Separator();
        jump_chain_tooltip(sim, queue, start, end, opts.viewer_faction_id, opts.fog_of_war);
      }

      if (!stp.note.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted(stp.note.c_str());
      }
      ImGui::EndTooltip();
    }

    // ETA
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%.2f", stp.eta_days);

    // Delta
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%.2f", stp.delta_days);

    // Fuel
    ImGui::TableSetColumnIndex(4);
    if (fuel_capacity_tons > 1e-9) {
      ImGui::Text("%.0f/%.0f", stp.fuel_after_tons, fuel_capacity_tons);
    } else {
      ImGui::Text("%.0f", stp.fuel_after_tons);
    }
    if (!stp.feasible) {
      ImGui::SameLine();
      ImGui::TextDisabled("!");
    }

    int col = 5;
    if (opts.show_system) {
      ImGui::TableSetColumnIndex(col++);
      const std::string sys_label = system_label_fow(sim, stp.system_id, opts.viewer_faction_id, opts.fog_of_war);
      ImGui::TextUnformatted(sys_label.c_str());
    }
    if (opts.show_position) {
      ImGui::TableSetColumnIndex(col++);
      const std::string p = pos_compact(stp.position_mkm);
      ImGui::TextUnformatted(p.c_str());
    }
    if (opts.show_note) {
      ImGui::TableSetColumnIndex(col++);
      if (stp.note.empty()) {
        ImGui::TextDisabled("--");
      } else {
        const std::string first = note_first_line_compact(stp.note);
        ImGui::TextUnformatted(first.c_str());
        if (ImGui::IsItemHovered()) {
          ImGui::BeginTooltip();
          ImGui::TextUnformatted(stp.note.c_str());
          ImGui::EndTooltip();
        }
      }
    }
  }

  if (queue.size() > show_n_orders) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("... (%d more orders not shown)", static_cast<int>(queue.size() - show_n_orders));
  }

  ImGui::EndTable();
}

std::string order_plan_to_csv(const Simulation& sim, const std::vector<Order>& queue, const OrderPlan& plan,
                              const OrderPlanRenderOptions& opts) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(4);

  out << "index,order,eta_days,delta_days,fuel_before_tons,fuel_after_tons,feasible";
  if (opts.show_system) out << ",system_id,system";
  if (opts.show_position) out << ",pos_x_mkm,pos_y_mkm";
  if (opts.show_note) out << ",note";
  out << ",row_kind,index_end";
  out << "\n";

  if (!plan.ok) return out.str();

  const std::size_t n_orders = std::min<std::size_t>(queue.size(), plan.steps.size());
  const std::size_t show_n_orders =
      std::min<std::size_t>(n_orders, static_cast<std::size_t>(std::clamp(opts.max_rows, 1, 16384)));

  const auto spans = build_spans(queue, show_n_orders, opts.collapse_jump_chains);

  for (const RowSpan& sp : spans) {
    const std::size_t start = sp.start;
    const std::size_t end = sp.end;

    const bool is_chain = opts.collapse_jump_chains && (end - start) > 1;
    const PlannedOrderStep stp = (end - start == 1) ? plan.steps[start] : aggregate_steps(plan.steps, start, end);

    std::string label;
    if (is_chain) {
      label = jump_chain_summary_label(sim, queue, plan.steps, start, end, opts.viewer_faction_id, opts.fog_of_war);
    } else {
      label = order_to_ui_string(sim, queue[start], opts.viewer_faction_id, opts.fog_of_war);
    }

    out << (start + 1) << ',' << csv_escape(label) << ',' << stp.eta_days << ',' << stp.delta_days << ','
        << stp.fuel_before_tons << ',' << stp.fuel_after_tons << ',' << (stp.feasible ? 1 : 0);

    if (opts.show_system) {
      out << ',' << static_cast<unsigned long long>(stp.system_id) << ','
          << csv_escape(system_label_fow(sim, stp.system_id, opts.viewer_faction_id, opts.fog_of_war));
    }
    if (opts.show_position) {
      out << ',' << stp.position_mkm.x << ',' << stp.position_mkm.y;
    }
    if (opts.show_note) {
      out << ',' << csv_escape(stp.note);
    }

    out << ',' << (is_chain ? "jump_chain" : "order") << ',' << end;
    out << "\n";
  }

  return out.str();
}

std::string order_plan_to_json(const Simulation& sim, const std::vector<Order>& queue, const OrderPlan& plan,
                               const OrderPlanRenderOptions& opts, int indent) {
  using nebula4x::json::Array;
  using nebula4x::json::Object;

  Object root;
  root["ok"] = plan.ok;
  root["truncated"] = plan.truncated;
  if (!plan.truncated_reason.empty()) root["truncated_reason"] = plan.truncated_reason;
  root["collapsed_jump_chains"] = opts.collapse_jump_chains;
  root["start_fuel_tons"] = plan.start_fuel_tons;
  root["end_fuel_tons"] = plan.end_fuel_tons;
  root["total_eta_days"] = plan.total_eta_days;

  Array steps;
  if (plan.ok) {
    const std::size_t n_orders = std::min<std::size_t>(queue.size(), plan.steps.size());
    const std::size_t show_n_orders =
        std::min<std::size_t>(n_orders, static_cast<std::size_t>(std::clamp(opts.max_rows, 1, 16384)));

    const auto spans = build_spans(queue, show_n_orders, opts.collapse_jump_chains);
    steps.reserve(spans.size());

    for (const RowSpan& sp : spans) {
      const std::size_t start = sp.start;
      const std::size_t end = sp.end;

      const bool is_chain = opts.collapse_jump_chains && (end - start) > 1;
      const PlannedOrderStep stp = (end - start == 1) ? plan.steps[start] : aggregate_steps(plan.steps, start, end);

      Object row;
      row["index"] = static_cast<double>(start + 1);
      row["index_end"] = static_cast<double>(end);
      row["row_kind"] = is_chain ? "jump_chain" : "order";

      if (is_chain) {
        row["order"] = jump_chain_summary_label(sim, queue, plan.steps, start, end, opts.viewer_faction_id, opts.fog_of_war);

        // Include underlying legs as individual strings for tooling/debug.
        Array legs;
        legs.reserve(end - start);
        for (std::size_t i = start; i < end; ++i) {
          legs.push_back(order_to_ui_string(sim, queue[i], opts.viewer_faction_id, opts.fog_of_war));
        }
        row["legs"] = std::move(legs);
      } else {
        row["order"] = order_to_ui_string(sim, queue[start], opts.viewer_faction_id, opts.fog_of_war);
      }

      row["eta_days"] = stp.eta_days;
      row["delta_days"] = stp.delta_days;
      row["fuel_before_tons"] = stp.fuel_before_tons;
      row["fuel_after_tons"] = stp.fuel_after_tons;
      row["feasible"] = stp.feasible;
      row["system_id"] = static_cast<double>(static_cast<unsigned long long>(stp.system_id));

      if (opts.show_system) {
        row["system"] = system_label_fow(sim, stp.system_id, opts.viewer_faction_id, opts.fog_of_war);
      }
      if (opts.show_position) {
        row["pos_x_mkm"] = stp.position_mkm.x;
        row["pos_y_mkm"] = stp.position_mkm.y;
      }
      if (opts.show_note && !stp.note.empty()) {
        row["note"] = stp.note;
      }

      steps.push_back(std::move(row));
    }
  }

  root["steps"] = std::move(steps);
  return nebula4x::json::stringify(root, indent);
}

}  // namespace nebula4x::ui
