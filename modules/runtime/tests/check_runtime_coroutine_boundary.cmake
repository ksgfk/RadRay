if (NOT DEFINED RADRAY_RUNTIME_DIR)
    message(FATAL_ERROR "RADRAY_RUNTIME_DIR is required")
endif()

file(GLOB_RECURSE _runtime_files
    "${RADRAY_RUNTIME_DIR}/include/*.h"
    "${RADRAY_RUNTIME_DIR}/src/*.cpp")

set(_violations)
foreach (_file IN LISTS _runtime_files)
    file(READ "${_file}" _content)
    if (_content MATCHES "stdexec::|exec::")
        list(APPEND _violations "${_file}")
    endif()
endforeach()

if (_violations)
    list(JOIN _violations "\n  " _message)
    message(FATAL_ERROR "Runtime code must use radray/coroutine.h aliases instead of stdexec::/exec:: directly:\n  ${_message}")
endif()
