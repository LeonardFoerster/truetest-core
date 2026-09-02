cmake_minimum_required(VERSION 3.22)

foreach(_required IN ITEMS GUARD_SOURCE_DIR GUARD_HEADER)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "source identity guard requires ${_required}")
    endif()
endforeach()
if(NOT EXISTS "${GUARD_HEADER}")
    message(FATAL_ERROR "source identity header is missing: ${GUARD_HEADER}")
endif()

if(NOT DEFINED GUARD_MODULE_DIR OR "${GUARD_MODULE_DIR}" STREQUAL "")
    set(GUARD_MODULE_DIR "${GUARD_SOURCE_DIR}/cmake")
endif()
include("${GUARD_MODULE_DIR}/SourceIdentity.cmake")
tt_compute_source_identity(
    "${GUARD_SOURCE_DIR}" ON
    _expected_sha _expected_dirty _expected_diff)
file(READ "${GUARD_HEADER}" _header)

function(read_string_macro macro output)
    string(REGEX MATCH
        "#define[ \t]+${macro}[ \t]+\"([^\"]*)\""
        _match "${_header}")
    if(NOT _match)
        message(FATAL_ERROR "source identity header lacks ${macro}")
    endif()
    set(${output} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

read_string_macro(TRUETEST_GIT_SHA _actual_sha)
read_string_macro(TRUETEST_GIT_DIRTY _actual_dirty)
read_string_macro(TRUETEST_GIT_DIFF_SHA256 _actual_diff)

if(NOT "${_actual_sha}" STREQUAL "${_expected_sha}")
    message(FATAL_ERROR "git commit SHA mismatch in generated source identity")
endif()
if(NOT "${_actual_dirty}" STREQUAL "${_expected_dirty}")
    message(FATAL_ERROR "git dirty-state mismatch in generated source identity")
endif()
if(NOT "${_actual_diff}" STREQUAL "${_expected_diff}")
    message(FATAL_ERROR "worktree diff SHA-256 mismatch in generated source identity")
endif()

message(STATUS "source identity header matches the current git worktree")
