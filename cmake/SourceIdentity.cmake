include_guard(GLOBAL)

function(tt_compute_source_identity source_dir require_exact
         out_sha out_dirty out_diff_sha256)
    if(require_exact
       AND (NOT DEFINED TT_GIT_EXECUTABLE OR TT_GIT_EXECUTABLE STREQUAL ""
            OR NOT DEFINED TT_GIT_SHA256 OR TT_GIT_SHA256 STREQUAL ""))
        message(FATAL_ERROR
            "deterministic source identity requires audited Git path and hash")
    endif()
    if(DEFINED TT_GIT_EXECUTABLE AND NOT TT_GIT_EXECUTABLE STREQUAL "")
        set(_tt_git_executable "${TT_GIT_EXECUTABLE}")
    else()
        find_program(_tt_git_executable git)
    endif()
    if(NOT _tt_git_executable)
        if(require_exact)
            message(FATAL_ERROR
                "deterministic build identity requires the git executable")
        endif()
        set(${out_sha} "unknown" PARENT_SCOPE)
        set(${out_dirty} "unknown" PARENT_SCOPE)
        set(${out_diff_sha256} "unknown" PARENT_SCOPE)
        return()
    endif()
    if(NOT EXISTS "${_tt_git_executable}")
        message(FATAL_ERROR
            "deterministic source identity git executable is missing")
    endif()
    if(DEFINED TT_GIT_SHA256 AND NOT TT_GIT_SHA256 STREQUAL "")
        file(SHA256 "${_tt_git_executable}" _tt_git_actual_sha256)
        if(NOT _tt_git_actual_sha256 STREQUAL TT_GIT_SHA256)
            message(FATAL_ERROR
                "deterministic source identity git executable changed")
        endif()
    endif()

    execute_process(
        COMMAND "${_tt_git_executable}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _tt_sha_result
        OUTPUT_VARIABLE _tt_sha
        ERROR_VARIABLE _tt_sha_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND "${_tt_git_executable}" status --porcelain --untracked-files=all
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _tt_status_result
        OUTPUT_VARIABLE _tt_status
        ERROR_VARIABLE _tt_status_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND "${_tt_git_executable}" ls-files -v
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _tt_index_flags_result
        OUTPUT_VARIABLE _tt_index_flags
        ERROR_VARIABLE _tt_index_flags_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(LENGTH "${_tt_sha}" _tt_sha_length)
    if(NOT _tt_sha_result EQUAL 0 OR NOT _tt_status_result EQUAL 0
       OR NOT _tt_index_flags_result EQUAL 0
       OR (NOT _tt_sha_length EQUAL 40 AND NOT _tt_sha_length EQUAL 64)
       OR NOT _tt_sha MATCHES "^[0-9a-fA-F]+$")
        if(require_exact)
            message(FATAL_ERROR
                "cannot compute deterministic git source identity: "
                "${_tt_sha_error}${_tt_status_error}${_tt_index_flags_error}")
        endif()
        set(${out_sha} "unknown" PARENT_SCOPE)
        set(${out_dirty} "unknown" PARENT_SCOPE)
        set(${out_diff_sha256} "unknown" PARENT_SCOPE)
        return()
    endif()

    if(_tt_index_flags MATCHES "(^|\n)[a-zS] ")
        message(FATAL_ERROR
            "deterministic source identity forbids assume-unchanged or "
            "skip-worktree index flags")
    endif()

    string(TOLOWER "${_tt_sha}" _tt_sha)
    if(NOT _tt_status)
        set(${out_sha} "${_tt_sha}" PARENT_SCOPE)
        set(${out_dirty} "clean" PARENT_SCOPE)
        set(${out_diff_sha256} "none" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND "${_tt_git_executable}" diff --binary --no-ext-diff HEAD -- .
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _tt_diff_result
        OUTPUT_VARIABLE _tt_tracked_diff
        ERROR_VARIABLE _tt_diff_error)
    execute_process(
        COMMAND "${_tt_git_executable}" ls-files --others --exclude-standard
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE _tt_untracked_result
        OUTPUT_VARIABLE _tt_untracked_text
        ERROR_VARIABLE _tt_untracked_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _tt_diff_result EQUAL 0 OR NOT _tt_untracked_result EQUAL 0)
        message(FATAL_ERROR
            "cannot hash deterministic git worktree state: "
            "${_tt_diff_error}${_tt_untracked_error}")
    endif()

    string(SHA256 _tt_tracked_sha256 "${_tt_tracked_diff}")
    string(REPLACE "\n" ";" _tt_untracked_files "${_tt_untracked_text}")
    list(SORT _tt_untracked_files)
    set(_tt_untracked_identity "")
    foreach(_tt_untracked IN LISTS _tt_untracked_files)
        if(_tt_untracked STREQUAL "")
            continue()
        endif()
        set(_tt_untracked_path "${source_dir}/${_tt_untracked}")
        if(NOT EXISTS "${_tt_untracked_path}"
           OR IS_DIRECTORY "${_tt_untracked_path}")
            message(FATAL_ERROR
                "cannot hash untracked source identity entry: ${_tt_untracked}")
        endif()
        file(SHA256 "${_tt_untracked_path}" _tt_untracked_sha256)
        string(LENGTH "${_tt_untracked}" _tt_name_length)
        string(APPEND _tt_untracked_identity
            "${_tt_name_length}:${_tt_untracked}:${_tt_untracked_sha256}\n")
    endforeach()
    string(SHA256 _tt_diff_sha256
        "tracked:${_tt_tracked_sha256}\nuntracked:\n${_tt_untracked_identity}")

    set(${out_sha} "${_tt_sha}" PARENT_SCOPE)
    set(${out_dirty} "dirty" PARENT_SCOPE)
    set(${out_diff_sha256} "${_tt_diff_sha256}" PARENT_SCOPE)
endfunction()
