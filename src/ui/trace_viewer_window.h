#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// In-process trace viewer for NEBULA4X_TRACE_SCOPE instrumentation.
//
// Uses nebula4x::trace::TraceRecorder and is intended as a lightweight
// performance profiler (frame/simulation hot spots, time-warp spikes, etc.).
void draw_trace_viewer_window(Simulation& sim, UIState& ui);

}  // namespace nebula4x::ui
