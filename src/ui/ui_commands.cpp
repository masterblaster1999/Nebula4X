#include "ui/ui_commands.h"

#include <utility>

namespace nebula4x::ui {

void UiCommandRegistry::clear() {
  commands_.clear();
}

void UiCommandRegistry::add(UiCommandSpec spec) {
  commands_.push_back(std::move(spec));
}

const UiCommandSpec* UiCommandRegistry::find(std::string_view id) const {
  for (const auto& c : commands_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

UiCommandRegistry build_default_ui_command_registry() {
  UiCommandRegistry r;
  // Intentionally empty for now; populated once the command console migrates.
  return r;
}

}  // namespace nebula4x::ui
