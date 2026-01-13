#include "ui/screen_reader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <imgui.h>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <sapi.h>
#else
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include <cerrno>
  #include <filesystem>
#endif

namespace nebula4x::ui {
namespace {

std::string trim_copy(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::string spoken_label_from_imgui_label(const char* label) {
  if (!label) return {};
  std::string s(label);
  // Strip the "##id" suffix (ImGui ID separators).
  const std::size_t pos = s.find("##");
  if (pos != std::string::npos) s = s.substr(0, pos);
  return trim_copy(s);
}

std::string clamp_text_for_speech(std::string s, std::size_t max_len = 240) {
  s = trim_copy(s);
  if (s.size() <= max_len) return s;
  s.resize(max_len);
  s += "…";
  return s;
}

}  // namespace

struct ScreenReader::Impl {
  struct Utterance {
    int priority{0};
    bool interrupt{false};
    std::string text;
  };

  // Settings (read by both UI thread and worker thread).
  std::atomic<bool> enabled{false};
  std::atomic<float> rate{1.0f};
  std::atomic<float> volume{1.0f};
  std::atomic<float> hover_delay_s{0.65f};

  std::atomic<bool> speak_focus{true};
  std::atomic<bool> speak_hover{false};
  std::atomic<bool> speak_windows{true};
  std::atomic<bool> speak_toasts{true};
  std::atomic<bool> speak_selection{true};

  // Queue / worker.
  std::mutex m;
  std::condition_variable cv;
  std::deque<Utterance> q;
  bool stop{false};
  std::thread worker;

  // Dedup and last spoken.
  std::string last_spoken;
  std::chrono::steady_clock::time_point last_spoken_t{};

  // History (UI thread reads; mutate under mutex).
  std::vector<ScreenReaderHistoryEntry> hist;
  std::size_t hist_cap{250};

  // Wall-clock for history timestamps (independent of ImGui; thread-safe).
  std::chrono::steady_clock::time_point start_t{std::chrono::steady_clock::now()};

  // ImGui observe state (UI thread only).
  std::string last_focused_item;
  std::string last_focused_window;
  std::string hover_label;
  double hover_start_t{0.0};
  bool hover_announced{false};

#if defined(_WIN32)
  // Windows SAPI (lives on the worker thread).
  ISpVoice* sapi_voice{nullptr};
  bool sapi_ready{false};
#endif

  Impl() {
    worker = std::thread([this]() { this->worker_loop(); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lk(m);
      stop = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
  }

  void enqueue(std::string text, bool interrupt, int priority) {
    text = clamp_text_for_speech(std::move(text));
    if (text.empty()) return;

    // Cheap dedupe at enqueue time.
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lk(m);
      if (text == last_spoken) {
        const auto dt = now - last_spoken_t;
        if (dt < std::chrono::milliseconds(250)) return;
      }

      if (interrupt) {
        q.clear();
      }

      q.push_back(Utterance{priority, interrupt, std::move(text)});
    }
    cv.notify_one();
  }

  void record_history(const std::string& text) {
    std::lock_guard<std::mutex> lk(m);
    ScreenReaderHistoryEntry e;
    const auto now = std::chrono::steady_clock::now();
    e.time_s = std::chrono::duration<double>(now - start_t).count();
    e.text = text;
    hist.push_back(std::move(e));
    if (hist.size() > hist_cap) {
      hist.erase(hist.begin(), hist.begin() + (hist.size() - hist_cap));
    }
  }

  void worker_loop() {
#if defined(_WIN32)
    // Initialize COM on this thread.
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Create SAPI voice via ProgID so we don't need sapiuuid.lib at link time.
    CLSID clsid;
    if (SUCCEEDED(CLSIDFromProgID(L"SAPI.SpVoice", &clsid))) {
      void* p = nullptr;
      if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, __uuidof(ISpVoice), &p))) {
        sapi_voice = static_cast<ISpVoice*>(p);
        sapi_ready = true;
      }
    }
#endif

    for (;;) {
      Utterance u;
      {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&]() { return stop || !q.empty(); });
        if (stop) break;

        // Pop highest-priority utterance.
        auto best = q.begin();
        for (auto it = q.begin(); it != q.end(); ++it) {
          if (it->priority > best->priority) best = it;
        }
        u = std::move(*best);
        q.erase(best);
      }

      if (!enabled.load()) continue;

      // Update last spoken (under mutex).
      {
        std::lock_guard<std::mutex> lk(m);
        last_spoken = u.text;
        last_spoken_t = std::chrono::steady_clock::now();
      }

      // Speak.
      const float r = std::clamp(rate.load(), 0.5f, 2.0f);
      const float v = std::clamp(volume.load(), 0.0f, 1.0f);

#if defined(_WIN32)
      if (sapi_ready && sapi_voice) {
        // SAPI rate is roughly -10..10.
        const int sapi_rate = static_cast<int>(std::lround((r - 1.0f) * 10.0f));
        const USHORT sapi_vol = static_cast<USHORT>(std::lround(v * 100.0f));
        sapi_voice->SetRate(std::clamp(sapi_rate, -10, 10));
        sapi_voice->SetVolume(sapi_vol);

        // UTF-8 -> UTF-16.
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, u.text.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
          std::wstring w;
          w.resize(static_cast<std::size_t>(wlen));
          MultiByteToWideChar(CP_UTF8, 0, u.text.c_str(), -1, w.data(), wlen);
          // Synchronous speak (blocks worker thread only).
          (void)sapi_voice->Speak(w.c_str(), SPF_DEFAULT, nullptr);
        }
      }
#else
      // POSIX: spawn a platform TTS tool (best-effort).
      const auto spawn_and_wait = [&](const char* prog) {
        pid_t pid = fork();
        if (pid == 0) {
          // Child
          execlp(prog, prog, u.text.c_str(), (char*)nullptr);
          _exit(127);
        }
        if (pid > 0) {
          int status = 0;
          (void)waitpid(pid, &status, 0);
        }
      };

  #if defined(__APPLE__)
      spawn_and_wait("say");
  #else
      // Prefer spd-say (speech-dispatcher) when present.
      bool used = false;
      try {
        if (std::filesystem::exists("/usr/bin/spd-say") || std::filesystem::exists("/bin/spd-say")) {
          spawn_and_wait("spd-say");
          used = true;
        }
      } catch (...) {
      }
      if (!used) {
        spawn_and_wait("espeak");
      }
  #endif
#endif

      // Record history after we attempted to speak.
      record_history(u.text);
    }

#if defined(_WIN32)
    if (sapi_voice) {
      sapi_voice->Release();
      sapi_voice = nullptr;
    }
    CoUninitialize();
#endif
  }
};

ScreenReader& ScreenReader::instance() {
  static ScreenReader sr;
  return sr;
}

ScreenReader::ScreenReader() : impl_(std::make_unique<Impl>()) {}
ScreenReader::~ScreenReader() = default;

void ScreenReader::set_enabled(bool enabled) { impl_->enabled.store(enabled); }
bool ScreenReader::enabled() const { return impl_->enabled.load(); }

void ScreenReader::set_rate(float rate) { impl_->rate.store(std::clamp(rate, 0.5f, 2.0f)); }
void ScreenReader::set_volume(float volume) { impl_->volume.store(std::clamp(volume, 0.0f, 1.0f)); }
void ScreenReader::set_hover_delay(float seconds) { impl_->hover_delay_s.store(std::clamp(seconds, 0.0f, 5.0f)); }

void ScreenReader::set_speak_focus(bool v) { impl_->speak_focus.store(v); }
void ScreenReader::set_speak_hover(bool v) { impl_->speak_hover.store(v); }
void ScreenReader::set_speak_windows(bool v) { impl_->speak_windows.store(v); }
void ScreenReader::set_speak_toasts(bool v) { impl_->speak_toasts.store(v); }
void ScreenReader::set_speak_selection(bool v) { impl_->speak_selection.store(v); }

bool ScreenReader::speak_focus() const { return impl_->speak_focus.load(); }
bool ScreenReader::speak_hover() const { return impl_->speak_hover.load(); }
bool ScreenReader::speak_windows() const { return impl_->speak_windows.load(); }
bool ScreenReader::speak_toasts() const { return impl_->speak_toasts.load(); }
bool ScreenReader::speak_selection() const { return impl_->speak_selection.load(); }

void ScreenReader::speak(std::string text, bool interrupt) {
  impl_->enqueue(std::move(text), interrupt, interrupt ? 100 : 10);
}

void ScreenReader::repeat_last() {
  std::string t;
  {
    std::lock_guard<std::mutex> lk(impl_->m);
    t = impl_->last_spoken;
  }
  if (!t.empty()) impl_->enqueue(std::move(t), true, 110);
}

void ScreenReader::begin_frame() {
  // Reset hover state when no item is hovered.
  if (!enabled()) return;
  if (!ImGui::IsAnyItemHovered()) {
    impl_->hover_label.clear();
    impl_->hover_start_t = 0.0;
    impl_->hover_announced = false;
  }
}

void ScreenReader::observe_window(const char* window_title) {
  if (!enabled()) return;
  if (!impl_->speak_windows.load()) return;
  if (!window_title) return;

  const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  if (!focused) return;

  const std::string title = spoken_label_from_imgui_label(window_title);
  if (title.empty()) return;

  if (title != impl_->last_focused_window) {
    impl_->last_focused_window = title;
    speak(title, false);
  }
}

void ScreenReader::observe_item(const char* label, const char* hint) {
  if (!enabled()) return;

  const std::string spoken = spoken_label_from_imgui_label(label);
  if (spoken.empty()) return;

  // Focus narration (keyboard/gamepad navigation).
  if (impl_->speak_focus.load() && ImGui::IsItemFocused()) {
    if (spoken != impl_->last_focused_item) {
      impl_->last_focused_item = spoken;
      if (hint && hint[0]) {
        speak(spoken + ": " + clamp_text_for_speech(std::string(hint), 180), false);
      } else {
        speak(spoken, false);
      }
    }
  }

  // Hover narration (mouse).
  if (impl_->speak_hover.load() && ImGui::IsItemHovered()) {
    if (spoken != impl_->hover_label) {
      impl_->hover_label = spoken;
      impl_->hover_start_t = ImGui::GetTime();
      impl_->hover_announced = false;
    }

    const float delay = std::max(0.0f, impl_->hover_delay_s.load());
    if (!impl_->hover_announced && (ImGui::GetTime() - impl_->hover_start_t) >= delay) {
      impl_->hover_announced = true;
      if (hint && hint[0]) {
        speak(spoken + ": " + clamp_text_for_speech(std::string(hint), 180), false);
      } else {
        speak(spoken, false);
      }
    }
  }
}

std::vector<ScreenReaderHistoryEntry> ScreenReader::history_snapshot() const {
  std::lock_guard<std::mutex> lk(impl_->m);
  return impl_->hist;
}

void ScreenReader::clear_history() {
  std::lock_guard<std::mutex> lk(impl_->m);
  impl_->hist.clear();
}

}  // namespace nebula4x::ui
