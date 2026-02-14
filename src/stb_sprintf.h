#ifndef NEBULA4X_STB_SPRINTF_H
#define NEBULA4X_STB_SPRINTF_H

// Minimal compatibility shim for ImGui's IMGUI_USE_STB_SPRINTF path.
// This project does not vendor upstream stb_sprintf.h, so we provide the
// small API surface ImGui needs by delegating to the C runtime formatter.

#include <cstdarg>
#include <cstdio>

inline int stbsp_vsnprintf(char* buf, int count, const char* fmt, va_list args) {
  if (!fmt) return -1;
  if (!buf || count <= 0) {
    char sink[1] = {0};
    return std::vsnprintf(sink, 0, fmt, args);
  }
  return std::vsnprintf(buf, static_cast<std::size_t>(count), fmt, args);
}

inline int stbsp_snprintf(char* buf, int count, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int rc = stbsp_vsnprintf(buf, count, fmt, args);
  va_end(args);
  return rc;
}

#endif // NEBULA4X_STB_SPRINTF_H
