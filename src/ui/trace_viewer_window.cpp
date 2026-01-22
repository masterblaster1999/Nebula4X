#include "ui/trace_viewer_window.h"

#include "ui/imgui_includes.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"

namespace nebula4x::ui {
namespace {

using nebula4x::trace::TraceEvent;
using nebula4x::trace::TraceRecorder;

static bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;

  auto lower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };

  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool ok = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (lower(haystack[i + j]) != lower(needle[j])) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

static std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

static std::uint32_t hash_string(std::string_view s) {
  std::uint32_t h = 2166136261u;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 16777619u;
  }
  return hash_u32(h);
}

static ImU32 cat_color(std::string_view cat) {
  // Stable pseudo-palette derived from HSV.
  const std::uint32_t h = hash_string(cat);
  const float hue = static_cast<float>(h % 360u) / 360.0f;
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, r, g, b);
  return IM_COL32(static_cast<int>(r * 255.0f), static_cast<int>(g * 255.0f), static_cast<int>(b * 255.0f), 200);
}

struct AggRow {
  std::string key;
  std::uint64_t total_us{0};
  std::uint64_t max_us{0};
  std::uint32_t count{0};
};

struct TraceViewerCache {
  std::vector<TraceEvent> snapshot;
  std::size_t last_total_count{0};
  double last_refresh_time{0.0};
  std::string last_filter_text;
  std::string last_filter_cat;
  bool last_show_metadata{false};

  // Derived per-frame.
  std::vector<const TraceEvent*> filtered;
  std::vector<AggRow> top;
};

static TraceViewerCache g_cache;

static void rebuild_filtered(const std::string& filter_text, const std::string& filter_cat,
                             bool show_metadata) {
  g_cache.filtered.clear();
  g_cache.filtered.reserve(g_cache.snapshot.size());

  for (const auto& e : g_cache.snapshot) {
    if (!show_metadata && e.ph == 'M') continue;
    if (!filter_cat.empty() && !contains_case_insensitive(e.cat, filter_cat)) continue;
    if (!filter_text.empty() && !contains_case_insensitive(e.name, filter_text)) continue;
    g_cache.filtered.push_back(&e);
  }
}

static void rebuild_top() {
  std::unordered_map<std::string, AggRow> acc;
  acc.reserve(256);

  for (const TraceEvent* e : g_cache.filtered) {
    if (!e || e->ph != 'X') continue;
    std::string key = e->cat.empty() ? e->name : (e->cat + ":" + e->name);
    auto& row = acc[key];
    if (row.key.empty()) row.key = key;
    row.count += 1;
    row.total_us += e->dur_us;
    row.max_us = std::max(row.max_us, e->dur_us);
  }

  g_cache.top.clear();
  g_cache.top.reserve(acc.size());
  for (auto& [_, row] : acc) g_cache.top.push_back(std::move(row));

  std::sort(g_cache.top.begin(), g_cache.top.end(), [](const AggRow& a, const AggRow& b) {
    return a.total_us > b.total_us;
  });
}

static void draw_timeline(UIState& ui, const std::vector<const TraceEvent*>& filtered) {
  // Only data events.
  std::vector<const TraceEvent*> events;
  events.reserve(filtered.size());
  std::uint64_t max_ts = 0;
  std::uint64_t min_ts = 0;
  bool have = false;
  for (const TraceEvent* e : filtered) {
    if (!e || e->ph != 'X') continue;
    events.push_back(e);
    const std::uint64_t end = e->ts_us + e->dur_us;
    max_ts = std::max(max_ts, end);
    if (!have) {
      min_ts = e->ts_us;
      have = true;
    } else {
      min_ts = std::min(min_ts, e->ts_us);
    }
  }

  if (events.empty()) {
    ImGui::TextDisabled("(no trace events yet)");
    return;
  }

  // Clamp and keep UI sane.
  ui.trace_viewer_window_ms = std::clamp(ui.trace_viewer_window_ms, 10.0f, 60000.0f);
  const double span_us = static_cast<double>(ui.trace_viewer_window_ms) * 1000.0;
  const std::uint64_t end_us = max_ts;
  std::uint64_t start_us = (end_us > static_cast<std::uint64_t>(span_us))
                               ? (end_us - static_cast<std::uint64_t>(span_us))
                               : min_ts;
  if (!ui.trace_viewer_follow_tail) {
    // If not following tail, keep a stable view anchored at the oldest timestamp.
    start_us = min_ts;
  }
  const double denom = std::max(1.0, static_cast<double>(end_us > start_us ? (end_us - start_us) : 1));

  // Thread lanes.
  std::unordered_map<std::uint32_t, int> lane;
  lane.reserve(16);

  std::vector<std::uint32_t> tids;
  tids.reserve(16);
  for (const TraceEvent* e : events) {
    const std::uint64_t e_end = e->ts_us + e->dur_us;
    if (e_end < start_us) continue;
    if (e->ts_us > end_us) continue;
    if (lane.contains(e->tid)) continue;
    lane[e->tid] = static_cast<int>(tids.size());
    tids.push_back(e->tid);
    if (tids.size() >= 20) break; // keep the view compact
  }

  const float row_h = 18.0f;
  const float canvas_h = std::max(120.0f, row_h * static_cast<float>(tids.size()) + 22.0f);

  ImGui::BeginChild("##trace_timeline", ImVec2(0, canvas_h), true, ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
  const float width = std::max(50.0f, ImGui::GetContentRegionAvail().x);
  const float height = std::max(50.0f, row_h * static_cast<float>(tids.size()));
  ImGui::InvisibleButton("##trace_canvas", ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 canvas_max = ImGui::GetItemRectMax();

  // Draw grid / lanes.
  for (std::size_t i = 0; i < tids.size(); ++i) {
    const float y0 = canvas_min.y + static_cast<float>(i) * row_h;
    dl->AddLine(ImVec2(canvas_min.x, y0), ImVec2(canvas_max.x, y0), IM_COL32(255, 255, 255, 20));
    dl->AddText(ImVec2(canvas_min.x + 4.0f, y0 + 2.0f), IM_COL32(255, 255, 255, 120),
                (std::string("tid ") + std::to_string(tids[i])).c_str());
  }

  struct Drawn {
    ImVec2 a;
    ImVec2 b;
    const TraceEvent* e{nullptr};
  };
  std::vector<Drawn> drawn;
  drawn.reserve(512);

  for (const TraceEvent* e : events) {
    if (!e) continue;
    const std::uint64_t e_start = e->ts_us;
    const std::uint64_t e_end = e->ts_us + e->dur_us;
    if (e_end < start_us) continue;
    if (e_start > end_us) continue;

    auto it = lane.find(e->tid);
    if (it == lane.end()) continue;
    const int row = it->second;

    const double x0f = (static_cast<double>(e_start - start_us) / denom);
    const double x1f = (static_cast<double>(std::min(e_end, end_us) - start_us) / denom);
    float x0 = canvas_min.x + static_cast<float>(x0f) * width;
    float x1 = canvas_min.x + static_cast<float>(x1f) * width;
    if (x1 < x0 + 1.0f) x1 = x0 + 1.0f;

    const float y0 = canvas_min.y + static_cast<float>(row) * row_h + 2.0f;
    const float y1 = y0 + row_h - 4.0f;

    const ImU32 col = cat_color(e->cat);
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 80));
    drawn.push_back(Drawn{ImVec2(x0, y0), ImVec2(x1, y1), e});
  }

  if (hovered) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const auto& d : drawn) {
      if (!d.e) continue;
      if (mouse.x < d.a.x || mouse.x > d.b.x || mouse.y < d.a.y || mouse.y > d.b.y) continue;
      ImGui::BeginTooltip();
      ImGui::Text("%s", d.e->name.c_str());
      if (!d.e->cat.empty()) ImGui::TextDisabled("%s", d.e->cat.c_str());
      ImGui::Separator();
      ImGui::Text("dur: %.3f ms", static_cast<double>(d.e->dur_us) / 1000.0);
      ImGui::Text("ts:  %.3f ms", static_cast<double>(d.e->ts_us) / 1000.0);
      if (!d.e->args.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("args:");
        int shown = 0;
        for (const auto& [k, v] : d.e->args) {
          if (shown++ >= 6) {
            ImGui::TextDisabled("...");
            break;
          }
          const std::string val = v.string_value("");
          if (!val.empty()) {
            ImGui::BulletText("%s = %s", k.c_str(), val.c_str());
          } else {
            const double num = v.number_value(0.0);
            if (v.is_number()) {
              ImGui::BulletText("%s = %.3f", k.c_str(), num);
            } else if (v.is_bool()) {
              ImGui::BulletText("%s = %s", k.c_str(), v.bool_value(false) ? "true" : "false");
            } else {
              ImGui::BulletText("%s", k.c_str());
            }
          }
        }
      }
      ImGui::EndTooltip();
      break;
    }
  }

  ImGui::EndChild();
}

} // namespace

void draw_trace_viewer_window(Simulation& /*sim*/, UIState& ui) {
  if (!ui.show_trace_viewer_window) return;

  bool open = ui.show_trace_viewer_window;
  if (!ImGui::Begin("Trace Viewer", &open)) {
    ImGui::End();
    ui.show_trace_viewer_window = open;
    return;
  }
  ui.show_trace_viewer_window = open;

  TraceRecorder& rec = TraceRecorder::instance();

  // Keep the recorder limit synced (cheap; guarded by mutex).
  ui.trace_viewer_max_events = std::clamp(ui.trace_viewer_max_events, 0, 500000);
  if (rec.max_events() != static_cast<std::size_t>(ui.trace_viewer_max_events)) {
    rec.set_max_events(static_cast<std::size_t>(ui.trace_viewer_max_events));
  }

  const bool recording = rec.enabled();
  ImGui::Text("Recorder: %s", recording ? "ON" : "OFF");
  ImGui::SameLine();
  ImGui::TextDisabled("events: %zu (%zu data)", rec.total_event_count(), rec.data_event_count());

  if (recording) {
    if (ImGui::Button("Stop")) rec.stop();
  } else {
    if (ImGui::Button("Start")) rec.start("nebula4x");
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) rec.clear();

  ImGui::Separator();

  ImGui::Checkbox("Autostart on launch", &ui.trace_viewer_autostart);
  ImGui::SameLine();
  ImGui::Checkbox("Auto-refresh", &ui.trace_viewer_auto_refresh);
  ImGui::SameLine();
  ImGui::Checkbox("Follow tail", &ui.trace_viewer_follow_tail);

  ImGui::InputInt("Max data events", &ui.trace_viewer_max_events);
  ui.trace_viewer_max_events = std::clamp(ui.trace_viewer_max_events, 0, 500000);

  ImGui::SliderFloat("Refresh (sec)", &ui.trace_viewer_refresh_sec, 0.05f, 2.0f, "%.2f");
  ui.trace_viewer_refresh_sec = std::clamp(ui.trace_viewer_refresh_sec, 0.05f, 2.0f);
  ImGui::SliderFloat("Window (ms)", &ui.trace_viewer_window_ms, 10.0f, 60000.0f, "%.0f");

  ImGui::Spacing();
  ImGui::TextDisabled("Export (Chrome/Perfetto trace JSON):");
  ImGui::SetNextItemWidth(-90.0f);
  ImGui::InputText("##trace_export_path", &ui.trace_viewer_export_path);
  ImGui::SameLine();
  if (ImGui::Button("Write")) {
    try {
      const std::string payload = rec.to_json_string(2);
      nebula4x::write_text_file(ui.trace_viewer_export_path, payload);
      nebula4x::log::info("Trace written to " + ui.trace_viewer_export_path);
    } catch (const std::exception& e) {
      nebula4x::log::error(std::string("Trace export failed: ") + e.what());
    }
  }

  static bool show_metadata = false;
  static std::string filter_text;
  static std::string filter_cat;

  ImGui::Separator();

  ImGui::Checkbox("Show metadata", &show_metadata);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(240.0f);
  ImGui::InputText("Filter name", &filter_text);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180.0f);
  ImGui::InputText("Filter cat", &filter_cat);

  // Refresh snapshot if needed.
  const std::size_t total = rec.total_event_count();
  const double now = ImGui::GetTime();
  const bool refresh_due = ui.trace_viewer_auto_refresh && (now - g_cache.last_refresh_time) >= ui.trace_viewer_refresh_sec;
  const bool count_changed = total != g_cache.last_total_count;
  if (refresh_due || count_changed) {
    rec.snapshot(&g_cache.snapshot);
    g_cache.last_total_count = total;
    g_cache.last_refresh_time = now;
  }

  const bool filter_changed = (filter_text != g_cache.last_filter_text) || (filter_cat != g_cache.last_filter_cat) ||
                              (show_metadata != g_cache.last_show_metadata);
  if (filter_changed || refresh_due || count_changed) {
    g_cache.last_filter_text = filter_text;
    g_cache.last_filter_cat = filter_cat;
    g_cache.last_show_metadata = show_metadata;
    rebuild_filtered(filter_text, filter_cat, show_metadata);
    rebuild_top();
  }

  // --- Timeline ---
  ImGui::SeparatorText("Timeline");
  draw_timeline(ui, g_cache.filtered);

  // --- Hot spots ---
  ImGui::SeparatorText("Hot spots (by total time)");
  const int top_n = 12;
  const int n = std::min<int>(static_cast<int>(g_cache.top.size()), top_n);
  for (int i = 0; i < n; ++i) {
    const auto& r = g_cache.top[static_cast<std::size_t>(i)];
    ImGui::BulletText("%s: %.2f ms (%u calls, max %.2f ms)", r.key.c_str(),
                      static_cast<double>(r.total_us) / 1000.0, r.count,
                      static_cast<double>(r.max_us) / 1000.0);
  }

  // --- Event table ---
  ImGui::SeparatorText("Events");
  static int sort_mode = 0; // 0 time, 1 duration
  static bool sort_desc = true;
  ImGui::RadioButton("Time", &sort_mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Duration", &sort_mode, 1);
  ImGui::SameLine();
  ImGui::Checkbox("Desc", &sort_desc);

  std::vector<const TraceEvent*> view = g_cache.filtered;
  if (sort_mode == 1) {
    std::sort(view.begin(), view.end(), [&](const TraceEvent* a, const TraceEvent* b) {
      const std::uint64_t da = a ? a->dur_us : 0;
      const std::uint64_t db = b ? b->dur_us : 0;
      return sort_desc ? (da > db) : (da < db);
    });
  } else {
    std::sort(view.begin(), view.end(), [&](const TraceEvent* a, const TraceEvent* b) {
      const std::uint64_t ta = a ? a->ts_us : 0;
      const std::uint64_t tb = b ? b->ts_us : 0;
      return sort_desc ? (ta > tb) : (ta < tb);
    });
  }

  if (ImGui::BeginTable("##trace_events", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
                                         ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn("t (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("dur (ms)", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("cat", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("tid", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableHeadersRow();

    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(view.size()));
    while (clip.Step()) {
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const TraceEvent* e = view[static_cast<std::size_t>(i)];
        if (!e) continue;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%.3f", static_cast<double>(e->ts_us) / 1000.0);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f", static_cast<double>(e->dur_us) / 1000.0);
        ImGui::TableSetColumnIndex(2);
        if (!e->cat.empty()) {
          ImGui::TextUnformatted(e->cat.c_str());
        } else {
          ImGui::TextDisabled("-");
        }
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(e->name.c_str());
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%u", e->tid);

        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", e->name.c_str());
        }
      }
    }
    ImGui::EndTable();
  }

  ImGui::End();
}

}  // namespace nebula4x::ui
