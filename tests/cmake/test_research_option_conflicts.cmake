cmake_minimum_required(VERSION 3.22)

foreach(_required IN ITEMS GUARD_SOURCE_DIR GUARD_BUILD_DIR GUARD_PROBE_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "research option-conflict test requires ${_required}")
    endif()
endforeach()

set(_build_root "${GUARD_BUILD_DIR}")
cmake_path(ABSOLUTE_PATH _build_root NORMALIZE)
set(_probe_root "${GUARD_PROBE_DIR}")
cmake_path(ABSOLUTE_PATH _probe_root NORMALIZE)
cmake_path(IS_PREFIX _build_root "${_probe_root}" NORMALIZE _probe_is_inside_build)
get_filename_component(_probe_name "${_probe_root}" NAME)
if(NOT _probe_is_inside_build
   OR NOT _probe_name STREQUAL "research-option-conflicts")
    message(FATAL_ERROR
        "refusing unsafe research option-conflict directory: ${_probe_root}")
endif()

file(REMOVE_RECURSE "${_probe_root}")
file(MAKE_DIRECTORY "${_probe_root}")

foreach(_conflicting_option IN ITEMS ENABLE_BINANCE ENABLE_BITGET)
    set(_conflict_build "${_probe_root}/${_conflicting_option}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${GUARD_SOURCE_DIR}"
            -B "${_conflict_build}"
            -DTRUETEST_RESEARCH_ONLY=ON
            "-D${_conflicting_option}=ON"
        RESULT_VARIABLE _configure_result
        OUTPUT_VARIABLE _configure_output
        ERROR_VARIABLE _configure_error)

    if(_configure_result EQUAL 0)
        message(FATAL_ERROR
            "research configure accepted conflicting ${_conflicting_option}=ON")
    endif()
    if(NOT "${_configure_output}\n${_configure_error}" MATCHES
           "TRUETEST_RESEARCH_ONLY=ON is incompatible with live-capable venue")
        message(FATAL_ERROR
            "${_conflicting_option} configure failed for an unexpected reason: "
            "${_configure_output}${_configure_error}")
    endif()
endforeach()

set(_stale_build "${_probe_root}/stale-build")
file(MAKE_DIRECTORY "${_stale_build}")
file(WRITE "${_stale_build}/engine_live" "stale forbidden binary\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${GUARD_SOURCE_DIR}"
        -B "${_stale_build}"
        -DTRUETEST_RESEARCH_ONLY=ON
    RESULT_VARIABLE _stale_result
    OUTPUT_VARIABLE _stale_output
    ERROR_VARIABLE _stale_error)
if(_stale_result EQUAL 0)
    message(FATAL_ERROR
        "research configure accepted a build tree with stale engine_live")
endif()
if(NOT "${_stale_output}\n${_stale_error}" MATCHES
       "found a stale engine_live artifact")
    message(FATAL_ERROR
        "stale-artifact configure failed for an unexpected reason: "
        "${_stale_output}${_stale_error}")
endif()

file(REMOVE_RECURSE "${_probe_root}")
message(STATUS
    "research configure rejected live-capable venues and a stale live build tree")
