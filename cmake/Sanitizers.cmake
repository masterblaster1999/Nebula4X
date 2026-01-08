# Sanitizer support helpers.
#
# Intended for local bug-hunting builds.
#
# CMake options expected (defined in the top-level CMakeLists.txt):
#   NEBULA4X_ENABLE_ASAN
#   NEBULA4X_ENABLE_UBSAN
#   NEBULA4X_ENABLE_TSAN
#   NEBULA4X_ENABLE_LSAN
#   NEBULA4X_ENABLE_GLIBCXX_ASSERTIONS

include_guard(GLOBAL)

function(nebula4x_enable_sanitizers target)
  if(MSVC)
    # MSVC has limited sanitizer support and uses different flags. For now, keep this a no-op.
    return()
  endif()

  # Allow enabling libstdc++ runtime assertions even without sanitizers.
  if(NEBULA4X_ENABLE_GLIBCXX_ASSERTIONS)
    target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
  endif()

  set(_sanitizers "")
  if(NEBULA4X_ENABLE_ASAN)
    list(APPEND _sanitizers address)
  endif()
  if(NEBULA4X_ENABLE_UBSAN)
    list(APPEND _sanitizers undefined)
  endif()
  if(NEBULA4X_ENABLE_TSAN)
    list(APPEND _sanitizers thread)
  endif()
  if(NEBULA4X_ENABLE_LSAN)
    list(APPEND _sanitizers leak)
  endif()

  if(NOT _sanitizers)
    return()
  endif()

  # Invalid combinations.
  if(NEBULA4X_ENABLE_TSAN AND (NEBULA4X_ENABLE_ASAN OR NEBULA4X_ENABLE_LSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan/LSan")
  endif()

  string(JOIN "," _san_list ${_sanitizers})

  # Compile + link flags.
  target_compile_options(${target} PRIVATE
    -fsanitize=${_san_list}
    -fno-omit-frame-pointer
  )
  target_link_options(${target} PRIVATE
    -fsanitize=${_san_list}
    -fno-omit-frame-pointer
  )

  # Make UBSan failures crash immediately; improves signal quality.
  if(NEBULA4X_ENABLE_UBSAN)
    target_compile_options(${target} PRIVATE -fno-sanitize-recover=all)
    target_link_options(${target} PRIVATE -fno-sanitize-recover=all)
  endif()

  target_compile_definitions(${target} PRIVATE NEBULA4X_SANITIZERS_ENABLED=1)
endfunction()
