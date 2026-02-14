#include <iostream>
#include <string_view>

#ifndef NEBULA4X_VERSION
#define NEBULA4X_VERSION "unknown"
#endif

#ifndef NEBULA4X_UI_UNAVAILABLE_REASON
#define NEBULA4X_UI_UNAVAILABLE_REASON "UI dependencies are unavailable in this build."
#endif

namespace {

constexpr const char* kStatusCodeUiUnavailable = "N4X-UI-001";
constexpr const char* kStatusCodeUiRequiredUnavailable = "N4X-UI-002";
constexpr int kExitCodeOk = 0;
constexpr int kExitCodeUiRequiredUnavailable = 2;

bool has_flag(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (!arg) continue;
    if (flag == arg) return true;
  }
  return false;
}

void print_usage(const char* exe) {
  const char* name = (exe && *exe) ? exe : "nebula4x";
  std::cout << "Nebula4X UI launcher v" << NEBULA4X_VERSION << "\n\n";
  std::cout << "Usage: " << name << " [--help] [--version] [--require-ui]\n\n";
  std::cout << "This build does not include the interactive UI executable.\n";
  std::cout << "Reason: " << NEBULA4X_UI_UNAVAILABLE_REASON << "\n\n";
  std::cout << "To run the simulation in this build, use:\n";
  std::cout << "  nebula4x_cli --days 30\n\n";
  std::cout << "To build the full UI executable, install SDL2/ImGui dependencies\n";
  std::cout << "or configure with -DNEBULA4X_FETCH_DEPS=ON.\n";
  std::cout << "\nLauncher status codes:\n";
  std::cout << "  " << kStatusCodeUiUnavailable << " (exit " << kExitCodeOk
            << "): UI unavailable; informational fallback launch.\n";
  std::cout << "  " << kStatusCodeUiRequiredUnavailable << " (exit "
            << kExitCodeUiRequiredUnavailable
            << "): UI explicitly required via --require-ui but unavailable.\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (has_flag(argc, argv, "--version")) {
    std::cout << NEBULA4X_VERSION << "\n";
    return 0;
  }

  if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h") || has_flag(argc, argv, "/?")) {
    print_usage(argv[0]);
    return 0;
  }

  if (has_flag(argc, argv, "--require-ui")) {
    std::cerr << "[" << kStatusCodeUiRequiredUnavailable << "] "
              << "Nebula4X UI is unavailable in this build.\n";
    std::cerr << "Reason: " << NEBULA4X_UI_UNAVAILABLE_REASON << "\n";
    std::cerr << "Run with --help for details.\n";
    return kExitCodeUiRequiredUnavailable;
  }

  std::cerr << "[" << kStatusCodeUiUnavailable << "] "
            << "Nebula4X UI is unavailable in this build.\n";
  std::cerr << "Reason: " << NEBULA4X_UI_UNAVAILABLE_REASON << "\n";
  std::cerr << "Run with --help for details.\n";
  std::cerr << "Continuing with exit code " << kExitCodeOk
            << " so launcher runs cleanly in IDE/debug workflows.\n";
  return kExitCodeOk;
}
