cmake_minimum_required(VERSION 3.22)

foreach(required IN ITEMS
        TT_DEPENDENCY_NAME TT_DEPENDENCY_SOURCE_DIR TT_EXPECTED_COMMIT
        TT_GIT_EXECUTABLE TT_GIT_SHA256)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR
            "deterministic dependency verification requires ${required}")
    endif()
endforeach()

if(DEFINED TT_GIT_EXECUTABLE AND NOT TT_GIT_EXECUTABLE STREQUAL "")
    set(git_executable "${TT_GIT_EXECUTABLE}")
else()
    find_program(git_executable git REQUIRED)
endif()
if(NOT EXISTS "${git_executable}")
    message(FATAL_ERROR "deterministic dependency git executable is missing")
endif()
if(DEFINED TT_GIT_SHA256 AND NOT TT_GIT_SHA256 STREQUAL "")
    file(SHA256 "${git_executable}" actual_git_sha256)
    if(NOT actual_git_sha256 STREQUAL TT_GIT_SHA256)
        message(FATAL_ERROR
            "deterministic dependency git executable changed after configure")
    endif()
endif()
execute_process(
    COMMAND "${git_executable}" rev-parse --verify HEAD
    WORKING_DIRECTORY "${TT_DEPENDENCY_SOURCE_DIR}"
    RESULT_VARIABLE head_result
    OUTPUT_VARIABLE actual_head
    ERROR_VARIABLE head_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND "${git_executable}" status --porcelain --untracked-files=all
    WORKING_DIRECTORY "${TT_DEPENDENCY_SOURCE_DIR}"
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE worktree_status
    ERROR_VARIABLE status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND "${git_executable}" ls-files -v
    WORKING_DIRECTORY "${TT_DEPENDENCY_SOURCE_DIR}"
    RESULT_VARIABLE index_flags_result
    OUTPUT_VARIABLE index_flags
    ERROR_VARIABLE index_flags_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT head_result EQUAL 0 OR NOT status_result EQUAL 0
   OR NOT index_flags_result EQUAL 0
   OR NOT actual_head STREQUAL "${TT_EXPECTED_COMMIT}"
   OR NOT worktree_status STREQUAL ""
   OR index_flags MATCHES "(^|\n)[a-zS] ")
    message(FATAL_ERROR
        "deterministic dependency '${TT_DEPENDENCY_NAME}' changed after "
        "configure: expected clean ${TT_EXPECTED_COMMIT}, "
        "HEAD='${actual_head}', status='${worktree_status}', "
        "errors='${head_error}${status_error}${index_flags_error}'")
endif()
