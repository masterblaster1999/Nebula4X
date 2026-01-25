#include "ui/watchboard_alerts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"

#include "ui/game_json_cache.h"
#include "ui/json_watch_eval.h"
#include "ui/notifications.h"
#include "ui/screen_reader.h"

namespace nebula4x::ui {

namespace {

constexpr std::uint64_t kCustomToastSeqBase = 0x8000000000000000ull;

std::string format_number(const double x) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", x);
  return std::string(buf);
}

std::int64_t sim_tick_hours(const GameState& st) {
  const std::int64_t day = st.date.days_since_epoch();
  const int hod = std::clamp(st.hour_of_day, 0, 23);
  return day * 24 + static_cast<std::int64_t>(hod);
}

EventLevel toast_level_from_cfg(const int lvl) {
  switch (std::clamp(lvl, 0, 2)) {
    case 2: return EventLevel::Error;
    case 1: return EventLevel::Warn;
    default: return EventLevel::Info;
  }
}

const char* alert_mode_name(const int mode) {
  switch (std::clamp(mode, 0, 4)) {
    case 0: return "cross above";
    case 1: return "cross below";
    case 2: return "change (abs)";
    case 3: return "change (%)";
    case 4: return "changed";
    default: return "alert";
  }
}

struct AlertRt {
  std::string last_path;
  bool last_is_query{false};
  int last_query_op{0};

  bool last_alert_enabled{false};

  bool has_last{false};
  bool last_numeric{false};
  double last_num{0.0};
  std::string last_display;

  std::int64_t last_tick{-1};
  double last_fire_time_s{0.0};
};

std::unordered_map<std::uint64_t, AlertRt> g_rt;
std::uint64_t g_last_state_generation = 0;
std::int64_t g_last_tick = -1;

} // namespace

void update_watchboard_alert_toasts(const Simulation& sim, UIState& ui, HUDState& hud) {
  // The watchboard can emit two user-facing signals:
  //   - transient HUD toasts
  //   - persistent Notification Center entries
  //
  // Historically we suppressed evaluation when toasts were disabled to avoid
  // building a hidden backlog. With the Notification Center, users may want
  // alerts *without* pop-up toasts, so we evaluate when either sink is enabled.
  const bool emit_toasts = ui.show_event_toasts;
  const bool emit_inbox = ui.notifications_capture_watchboard_alerts;
  if (!emit_toasts && !emit_inbox) return;

  // Reset state when a new game state is loaded.
  if (g_last_state_generation != sim.state_generation()) {
    g_last_state_generation = sim.state_generation();
    g_last_tick = -1;
    g_rt.clear();
  }

  bool any_alerts = false;
  for (const auto& w : ui.json_watch_items) {
    if (w.alert_enabled) {
      any_alerts = true;
      break;
    }
  }
  if (!any_alerts) return;

  const auto& st = sim.state();
  const std::int64_t tick = sim_tick_hours(st);
  const bool tick_changed = (tick != g_last_tick);

  const double now_s = ImGui::GetTime();

  // Ensure the JSON cache is available.
  // - If the sim tick changed: force a refresh to capture new state.
  // - If no cache exists yet: one-time forced build so alerts can evaluate.
  auto& cache = game_json_cache();
  const bool have_doc = cache.loaded && cache.root;
  if (tick_changed || !have_doc) {
    ensure_game_json_cache(sim, now_s, /*min_refresh_sec=*/0.0, /*force=*/true);
  }

  if (!cache.loaded || !cache.root) {
    return; // can't evaluate
  }

  const nebula4x::json::Value& root = *cache.root;

  int emitted_this_tick = 0;
  constexpr int kMaxEmitsPerUpdate = 6;

  for (const auto& cfg : ui.json_watch_items) {
    if (!cfg.alert_enabled) {
      // Track toggle transitions so enabling doesn't instantly fire due to stale last values.
      auto& rt = g_rt[cfg.id];
      rt.last_alert_enabled = false;
      continue;
    }

    auto& rt = g_rt[cfg.id];

    const bool config_changed = (rt.last_path != cfg.path || rt.last_is_query != cfg.is_query ||
                                 rt.last_query_op != cfg.query_op);

    if (config_changed) {
      rt.last_path = cfg.path;
      rt.last_is_query = cfg.is_query;
      rt.last_query_op = cfg.query_op;

      // Reset signal memory so we don't mis-fire.
      rt.has_last = false;
      rt.last_display.clear();
      rt.last_numeric = false;
      rt.last_num = 0.0;
      rt.last_tick = -1;
      rt.last_fire_time_s = 0.0;
    }

    if (!rt.last_alert_enabled) {
      rt.last_alert_enabled = true;
      rt.has_last = false;
      rt.last_tick = -1;
    }

    // Only evaluate when the sim tick changes or when the config changed / has no baseline.
    const bool should_eval = tick_changed || config_changed || !rt.has_last;
    if (!should_eval) continue;

    // Avoid double-evaluating a pin multiple times during the same tick.
    if (rt.last_tick == tick && !config_changed) continue;

    JsonWatchEvalOptions eval_opts;
    eval_opts.collect_samples = false;
    eval_opts.max_preview_chars = 96;
    const JsonWatchEvalResult cur = eval_json_watch(root, cfg, ui, eval_opts);

    // Update baseline even on failures.
    rt.last_tick = tick;

    if (!cur.ok) {
      rt.has_last = false;
      rt.last_display = cur.display;
      rt.last_numeric = false;
      rt.last_num = 0.0;
      continue;
    }

    const int mode = std::clamp(cfg.alert_mode, 0, 4);
    bool should_fire = false;
    // Snapshot previous baseline for messaging.
    const bool had_last = rt.has_last;
    const bool prev_numeric = rt.last_numeric;
    const double prev_num = rt.last_num;
    const std::string prev_display = rt.last_display;

    // Compare against baseline.
    if (had_last) {
      const double cooldown = std::max(0.0f, cfg.alert_cooldown_sec);
      if (cooldown > 0.0 && (now_s - rt.last_fire_time_s) < cooldown) {
        // Debounced.
      } else {
        if (mode == 0 || mode == 1 || mode == 2 || mode == 3) {
          // Numeric modes.
          if (cur.numeric && prev_numeric) {
            const double thr = cfg.alert_threshold;
            const double d = cfg.alert_delta;

            switch (mode) {
              case 0: // cross above
                should_fire = (prev_num <= thr) && (cur.value > thr);
                break;
              case 1: // cross below
                should_fire = (prev_num >= thr) && (cur.value < thr);
                break;
              case 2: // abs delta
                if (d > 0.0) should_fire = std::fabs(cur.value - prev_num) >= d;
                break;
              case 3: // percent delta
                if (d > 0.0 && std::fabs(prev_num) > 1e-9) {
                  should_fire = std::fabs((cur.value - prev_num) / prev_num) >= d;
                }
                break;
              default:
                break;
            }
          }
        } else if (mode == 4) {
          // Any change (string/number).
          if (cur.numeric && prev_numeric) {
            should_fire = std::fabs(cur.value - prev_num) > 1e-9;
          } else {
            should_fire = (cur.display != prev_display);
          }
        }
      }
    }

    // Update baseline.
    rt.has_last = true;
    rt.last_numeric = cur.numeric;
    rt.last_num = cur.value;
    rt.last_display = cur.display;

    if (!should_fire) continue;
    if (emitted_this_tick >= kMaxEmitsPerUpdate) break;

    const EventLevel lvl = toast_level_from_cfg(cfg.alert_toast_level);

    // Build toast message.
    const std::string label = cfg.label.empty() ? cfg.path : cfg.label;

    std::string msg;
    if (!had_last) {
      msg = label + " " + alert_mode_name(mode) + " (now " + cur.display + ")";
    } else if (mode == 0 || mode == 1) {
      msg = label + " " + alert_mode_name(mode) + " " + format_number(cfg.alert_threshold) +
            " (was " + prev_display + ", now " + cur.display + ")";
    } else if (mode == 2 && cur.numeric && prev_numeric) {
      const double diff = cur.value - prev_num;
      msg = label + " change " + format_number(diff) +
            " (|Δ|>= " + format_number(cfg.alert_delta) + ", was " + prev_display + ", now " + cur.display + ")";
    } else if (mode == 3 && cur.numeric && prev_numeric && std::fabs(prev_num) > 1e-9) {
      const double pct = (cur.value - prev_num) / prev_num * 100.0;
      msg = label + " change " + format_number(pct) + "%" +
            " (|Δ|>= " + format_number(cfg.alert_delta * 100.0) + "%, was " + prev_display + ", now " + cur.display + ")";
    } else {
      msg = label + " changed (was " + prev_display + ", now " + cur.display + ")";
    }

    const std::uint64_t seq = kCustomToastSeqBase | (hud.next_custom_toast_seq++);
    const std::int64_t day = st.date.days_since_epoch();
    const int hour = st.hour_of_day;

    if (emit_inbox) {
      notifications_push_watchboard_alert(ui, seq, day, hour, static_cast<int>(lvl), msg, cfg.id, cfg.label, cfg.path,
                                         cur.rep_ptr);
    }

    if (emit_toasts) {
      EventToast t;
      t.seq = seq;
      t.day = day;
      t.level = lvl;
      t.category = EventCategory::General;
      t.custom = true;
      t.watch_id = cfg.id;
      t.watch_label = cfg.label;
      t.watch_path = cfg.path;
      t.watch_rep_ptr = cur.rep_ptr;
      t.message = std::move(msg);
      t.created_time_s = now_s;

      hud.toasts.push_back(std::move(t));

      // Keep toast list bounded (matches update_event_toasts cap).
      constexpr std::size_t kMaxToastsTotal = 10;
      if (hud.toasts.size() > kMaxToastsTotal) {
        hud.toasts.erase(hud.toasts.begin(), hud.toasts.begin() + (hud.toasts.size() - kMaxToastsTotal));
      }

      if (ui.screen_reader_enabled && ui.screen_reader_speak_toasts) {
        ScreenReader::instance().announce_toast("Alert: " + hud.toasts.back().message);
      }
    }

    rt.last_fire_time_s = now_s;
    emitted_this_tick++;
  }

  if (tick_changed) g_last_tick = tick;
}

} // namespace nebula4x::ui
