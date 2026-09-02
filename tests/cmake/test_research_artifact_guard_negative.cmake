cmake_minimum_required(VERSION 3.22)

foreach(_required IN ITEMS GUARD_SCRIPT GUARD_BUILD_DIR GUARD_PROBE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "negative guard test requires ${_required}")
    endif()
endforeach()

set(_build_root "${GUARD_BUILD_DIR}")
cmake_path(ABSOLUTE_PATH _build_root NORMALIZE)
set(_probe_root "${GUARD_PROBE_DIR}")
cmake_path(ABSOLUTE_PATH _probe_root NORMALIZE)
cmake_path(IS_PREFIX _build_root "${_probe_root}" NORMALIZE _probe_is_inside_build)
get_filename_component(_probe_name "${_probe_root}" NAME)
if(NOT _probe_is_inside_build
   OR NOT _probe_name STREQUAL "research-artifact-guard-negative")
    message(FATAL_ERROR
        "refusing unsafe negative-guard probe directory: ${_probe_root}")
endif()

function(assert_guard_rejects label expected_error)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${GUARD_SCRIPT}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)

    if(_result EQUAL 0)
        message(FATAL_ERROR
            "research artifact guard accepted ${label}")
    endif()
    if(NOT "${_output}\n${_error}" MATCHES "${expected_error}")
        message(FATAL_ERROR
            "guard rejected ${label} for an unexpected reason: ${_output}${_error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${_probe_root}")
file(MAKE_DIRECTORY "${_probe_root}/bin")
file(WRITE "${_probe_root}/bin/engine_live" "forbidden probe\n")

assert_guard_rejects(
    "an injected engine_live artifact"
    "forbidden engine_live artifact in guard probe"
    "-DGUARD_SCAN_ROOT=${_probe_root}")

file(REMOVE_RECURSE "${_probe_root}")
file(MAKE_DIRECTORY "${_probe_root}")
set(_fake_commands "${_probe_root}/compile_commands.json")
file(WRITE "${_fake_commands}" [=[
[
  {
    "directory": "/tmp",
    "command": "c++ -DTT_TARGET=TT_TARGET_LIVE -c renamed_entry.cpp",
    "file": "renamed_entry.cpp"
  }
]
]=])

assert_guard_rejects(
    "an injected TT_TARGET_LIVE compile command"
    "forbidden TT_TARGET_LIVE compile definition"
    "-DGUARD_SCAN_ROOT=${_probe_root}"
    "-DGUARD_SCAN_COMPILE_COMMANDS=${_fake_commands}")

file(REMOVE_RECURSE "${_probe_root}")
file(MAKE_DIRECTORY "${_probe_root}/bin")
file(WRITE "${_probe_root}/bin/engine_backtest" "allowed probe\n")
file(WRITE "${_probe_root}/bin/engine_shadow" "allowed probe\n")
file(WRITE "${_probe_root}/bin/research_exec" "renamed forbidden probe\n")

assert_guard_rejects(
    "an injected renamed executable or wrapper"
    "unexpected executable or wrapper in guard probe"
    "-DGUARD_SCAN_ROOT=${_probe_root}"
    "-DGUARD_SCAN_BIN_ROOT=${_probe_root}")

file(REMOVE_RECURSE "${_probe_root}")

message(STATUS
    "research artifact guard rejected live-name, TT_TARGET_LIVE, and renamed-wrapper probes")
