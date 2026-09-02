cmake_minimum_required(VERSION 3.22)

foreach(_required IN ITEMS GUARD_SOURCE_DIR GUARD_BUILD_DIR GUARD_SCRIPT
        TT_GIT_EXECUTABLE TT_GIT_SHA256)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "source identity negative test requires ${_required}")
    endif()
endforeach()

set(_probe "${GUARD_BUILD_DIR}/source-identity-negative")
cmake_path(ABSOLUTE_PATH _probe NORMALIZE)
set(_build "${GUARD_BUILD_DIR}")
cmake_path(ABSOLUTE_PATH _build NORMALIZE)
cmake_path(IS_PREFIX _build "${_probe}" NORMALIZE _inside_build)
if(NOT _inside_build)
    message(FATAL_ERROR "refusing unsafe source identity probe path")
endif()
file(REMOVE_RECURSE "${_probe}")
file(MAKE_DIRECTORY "${_probe}/repo" "${_probe}/generated")

function(run_checked)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "source identity negative fixture command failed: ${_output}${_error}")
    endif()
endfunction()

run_checked(git init -q "${_probe}/repo")
run_checked(git -C "${_probe}/repo" config user.name "TrueTest Test")
run_checked(git -C "${_probe}/repo" config user.email "test@invalid.local")
file(WRITE "${_probe}/repo/input.txt" "committed\n")
run_checked(git -C "${_probe}/repo" add input.txt)
run_checked(git -C "${_probe}/repo" commit -q -m baseline)
file(WRITE "${_probe}/repo/input.txt" "dirty-state-a\n")

set(_header "${_probe}/generated/truetest_source_identity.h")
run_checked(
    "${CMAKE_COMMAND}"
    "-DTT_SOURCE_DIR=${_probe}/repo"
    "-DTT_OUTPUT_HEADER=${_header}"
    "-DTT_TEMPLATE=${GUARD_SOURCE_DIR}/cmake/truetest_source_identity.h.in"
    "-DTT_GIT_EXECUTABLE=${TT_GIT_EXECUTABLE}"
    "-DTT_GIT_SHA256=${TT_GIT_SHA256}"
    -DTT_REQUIRE_EXACT=ON
    -P "${GUARD_SOURCE_DIR}/cmake/GenerateSourceIdentity.cmake")

file(WRITE "${_probe}/repo/input.txt" "dirty-state-b\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DGUARD_SOURCE_DIR=${_probe}/repo"
        "-DGUARD_HEADER=${_header}"
        "-DGUARD_MODULE_DIR=${GUARD_SOURCE_DIR}/cmake"
        "-DTT_GIT_EXECUTABLE=${TT_GIT_EXECUTABLE}"
        "-DTT_GIT_SHA256=${TT_GIT_SHA256}"
        -P "${GUARD_SCRIPT}"
    RESULT_VARIABLE _guard_result
    OUTPUT_VARIABLE _guard_output
    ERROR_VARIABLE _guard_error)
if(_guard_result EQUAL 0)
    message(FATAL_ERROR "source identity guard accepted a stale diff hash")
endif()
if(NOT "${_guard_output}\n${_guard_error}" MATCHES
       "worktree diff SHA-256 mismatch")
    message(FATAL_ERROR
        "source identity guard failed for an unexpected reason: "
        "${_guard_output}${_guard_error}")
endif()

file(WRITE "${_probe}/repo/input.txt" "committed\n")
run_checked(git -C "${_probe}/repo" update-index --assume-unchanged input.txt)
file(WRITE "${_probe}/repo/input.txt" "hidden-dirty-state\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DTT_SOURCE_DIR=${_probe}/repo"
        "-DTT_OUTPUT_HEADER=${_header}"
        "-DTT_TEMPLATE=${GUARD_SOURCE_DIR}/cmake/truetest_source_identity.h.in"
        "-DTT_GIT_EXECUTABLE=${TT_GIT_EXECUTABLE}"
        "-DTT_GIT_SHA256=${TT_GIT_SHA256}"
        -DTT_REQUIRE_EXACT=ON
        -P "${GUARD_SOURCE_DIR}/cmake/GenerateSourceIdentity.cmake"
    RESULT_VARIABLE _hidden_source_result
    OUTPUT_VARIABLE _hidden_source_output
    ERROR_VARIABLE _hidden_source_error)
if(_hidden_source_result EQUAL 0
   OR NOT "${_hidden_source_output}${_hidden_source_error}"
          MATCHES "forbids assume-unchanged or skip-worktree")
    message(FATAL_ERROR
        "source identity accepted assume-unchanged tracked content: "
        "${_hidden_source_output}${_hidden_source_error}")
endif()

execute_process(
    COMMAND git -C "${_probe}/repo" rev-parse HEAD
    OUTPUT_VARIABLE _dependency_head
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _dependency_head_result)
if(NOT _dependency_head_result EQUAL 0)
    message(FATAL_ERROR "cannot resolve dependency fixture HEAD")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DTT_DEPENDENCY_NAME=fixture
        "-DTT_DEPENDENCY_SOURCE_DIR=${_probe}/repo"
        "-DTT_EXPECTED_COMMIT=${_dependency_head}"
        "-DTT_GIT_EXECUTABLE=${TT_GIT_EXECUTABLE}"
        "-DTT_GIT_SHA256=${TT_GIT_SHA256}"
        -P "${GUARD_SOURCE_DIR}/cmake/VerifyDependencyIdentity.cmake"
    RESULT_VARIABLE _hidden_dependency_result
    OUTPUT_VARIABLE _hidden_dependency_output
    ERROR_VARIABLE _hidden_dependency_error)
if(_hidden_dependency_result EQUAL 0)
    message(FATAL_ERROR
        "dependency identity accepted assume-unchanged tracked content")
endif()

file(REMOVE_RECURSE "${_probe}")
message(STATUS "source identity guard rejected a stale worktree diff hash")
