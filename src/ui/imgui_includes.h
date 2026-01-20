#pragma once

// Centralized ImGui includes for Nebula4X UI code.
//
// Keeping this in a single header avoids "include roulette" across windows
// (some need ImVec2/ImDrawList, others need std::string InputText helpers).

#include <imgui.h>

// Optional helpers: std::string overloads for InputText/Combo, etc.
#include <misc/cpp/imgui_stdlib.h>
