# Apply a patch file to the current source tree in a robust, idempotent way.
# Usage: cmake -DPATCH_FILE="<path-to-patch>" -P apply_patch.cmake

if(NOT DEFINED PATCH_FILE OR "${PATCH_FILE}" STREQUAL "")
  # Fallback to patch file located next to this script
  set(PATCH_FILE "${CMAKE_CURRENT_LIST_DIR}/no-zlib-shared.patch")
endif()

if(NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
endif()

# Helper to run a git command and capture output
function(_run_git out_var)
  execute_process(
    COMMAND git --no-pager ${ARGN}
    RESULT_VARIABLE _res
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
  if(NOT _res EQUAL 0)
    set(_msg "git ${ARGN} failed with code ${_res}.\nSTDOUT: ${_out}\nSTDERR: ${_err}")
    set(${out_var} "__ERROR__:${_msg}" PARENT_SCOPE)
  else()
    set(${out_var} "${_out}" PARENT_SCOPE)
  endif()
endfunction()

# First, check that git is available
execute_process(COMMAND git --version RESULT_VARIABLE _git_ok OUTPUT_QUIET ERROR_QUIET)
if(NOT _git_ok EQUAL 0)
  message(FATAL_ERROR "'git' is required to apply patches but was not found in PATH.")
endif()

# Determine if patch is applicable, already applied, or conflicting
set(_check_out "")
_run_git(_check_out apply --check --whitespace=nowarn "${PATCH_FILE}")
if(_check_out MATCHES "^__ERROR__:")
  # Not directly applicable; see if it's already applied
  set(_reverse_check "")
  _run_git(_reverse_check apply --reverse --check --whitespace=nowarn "${PATCH_FILE}")
  if(_reverse_check MATCHES "^__ERROR__:")
    string(REPLACE "__ERROR__:" "" _err_msg "${_reverse_check}")
    message(FATAL_ERROR "Failed to apply patch and it does not appear to be already applied.\n${_err_msg}")
  else()
    message(STATUS "Patch already applied; skipping: ${PATCH_FILE}")
    return()
  endif()
endif()

# Apply the patch
set(_apply_out "")
_run_git(_apply_out apply --whitespace=nowarn "${PATCH_FILE}")
if(_apply_out MATCHES "^__ERROR__:")
  string(REPLACE "__ERROR__:" "" _err_msg "${_apply_out}")
  message(FATAL_ERROR "Failed to apply patch.\n${_err_msg}")
endif()

message(STATUS "Successfully applied patch: ${PATCH_FILE}")
