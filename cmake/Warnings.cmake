function(nebula4x_set_project_warnings target warnings_as_errors)
  if(MSVC)
    # /MP enables parallel compilation of translation units within a target.
    # This significantly improves build responsiveness on large projects.
    # /FS serializes writes to PDBs and avoids intermittent file-lock races
    # when many compiler processes are active.
    target_compile_options(${target} PRIVATE /W4 /MP /FS)
    if(NEBULA4X_MSVC_PROGRESS_OUTPUT)
      # /Bt+ prints front-end/back-end timings per file so long builds show
      # continuous progress instead of looking stalled.
      target_compile_options(${target} PRIVATE /Bt+)
      get_target_property(_n4x_target_type ${target} TYPE)
      if(_n4x_target_type STREQUAL "EXECUTABLE" OR
         _n4x_target_type STREQUAL "SHARED_LIBRARY" OR
         _n4x_target_type STREQUAL "MODULE_LIBRARY")
        target_link_options(${target} PRIVATE /time+)
      endif()
    endif()
    if(${warnings_as_errors})
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(${warnings_as_errors})
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
