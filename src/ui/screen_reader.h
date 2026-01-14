#pragma once

#include <memory>
#include <string>
#include <vector>

namespace nebula4x::ui {

// Simple narration/screen-reader layer.
//
// Dear ImGui is an immediate-mode UI and does not expose a native accessibility
// tree. This module provides an in-game narration channel (TTS when available)
// plus a small "observe" API that UI code can opt into.
struct ScreenReaderHistoryEntry {
  double time_s{0.0};
  std::string text;
};

class ScreenReader {
 public:
  static ScreenReader& instance();

  ScreenReader(const ScreenReader&) = delete;
  ScreenReader& operator=(const ScreenReader&) = delete;

  // --- Global settings ---
  void set_enabled(bool enabled);
  bool enabled() const;

  // UI-controlled voice parameters.
  //
  // Backends interpret these as best-effort.
  // - rate:   0.50 .. 2.00 (1.0 = normal)
  // - volume: 0.00 .. 1.00
  void set_rate(float rate);
  void set_volume(float volume);
  void set_hover_delay(float seconds);

  void set_speak_focus(bool v);
  void set_speak_hover(bool v);
  void set_speak_windows(bool v);
  void set_speak_toasts(bool v);
  void set_speak_selection(bool v);

  bool speak_focus() const;
  bool speak_hover() const;
  bool speak_windows() const;
  bool speak_toasts() const;
  bool speak_selection() const;

  // Speak immediately (queued to a worker thread).
  // If interrupt==true, pending speech is dropped and this is prioritized.
  void speak(std::string text, bool interrupt = false);
  void repeat_last();

  // Convenience: announce a HUD toast (warn/error).
  // Uses a higher priority than normal focus narration so important events
  // are less likely to be delayed behind UI chrome.
  void announce_toast(std::string text);

  // ImGui helpers.
  // Call once per frame from the UI thread.
  void begin_frame();
  // Call inside an ImGui window (after Begin) to announce focus.
  void observe_window(const char* window_title);
  // Call immediately after drawing an ImGui item to optionally announce focus/hover.
  void observe_item(const char* label, const char* hint = nullptr);

  // History (UI can display / copy this).
  // Returns a thread-safe snapshot copy.
  std::vector<ScreenReaderHistoryEntry> history_snapshot() const;
  void clear_history();

 private:
  ScreenReader();
  ~ScreenReader();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nebula4x::ui
