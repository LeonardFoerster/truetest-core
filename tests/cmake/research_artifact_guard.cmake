cmake_minimum_required(VERSION 3.22)

function(assert_no_engine_live_path root label)
    if(NOT IS_DIRECTORY "${root}")
        message(FATAL_ERROR "${label} does not exist: ${root}")
    endif()

    file(GLOB_RECURSE _candidates LIST_DIRECTORIES TRUE "${root}/*")
    foreach(_candidate IN LISTS _candidates)
        file(RELATIVE_PATH _relative_candidate "${root}" "${_candidate}")
        string(REPLACE "\\" "/" _relative_candidate "${_relative_candidate}")
        if(_relative_candidate MATCHES "engine_live")
            message(FATAL_ERROR
                "forbidden engine_live artifact in ${label}: ${_candidate}")
        endif()
    endforeach()
endfunction()

function(assert_no_live_compile_commands compile_commands label)
    if(NOT EXISTS "${compile_commands}")
        message(FATAL_ERROR
            "${label} compile_commands.json does not exist: ${compile_commands}")
    endif()

    file(READ "${compile_commands}" _commands)
    string(REPLACE "\\\\" "/" _commands_normalized "${_commands}")
    if(_commands_normalized MATCHES "src/bin/engine_live/main\\.cpp")
        message(FATAL_ERROR
            "${label} contains the live entry-point source")
    endif()
    if(_commands_normalized MATCHES
       "TT_TARGET=(TT_TARGET_LIVE|3)([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR
            "forbidden TT_TARGET_LIVE compile definition in ${label}")
    endif()
endfunction()

function(assert_research_bin_contents root label)
    if(NOT IS_DIRECTORY "${root}/bin")
        message(FATAL_ERROR "${label} has no bin directory: ${root}/bin")
    endif()

    set(_found_backtest FALSE)
    set(_found_shadow FALSE)
    file(GLOB _bin_entries LIST_DIRECTORIES TRUE "${root}/bin/*")
    foreach(_entry IN LISTS _bin_entries)
        get_filename_component(_name "${_entry}" NAME)
        if(_name MATCHES "^engine_backtest(\\.exe)?$")
            set(_found_backtest TRUE)
        elseif(_name MATCHES "^engine_shadow(\\.exe)?$")
            set(_found_shadow TRUE)
        elseif(_name STREQUAL "truetest.dll")
            # BUILD_SHARED_LIB is a research-safe optional artifact on Windows.
        else()
            message(FATAL_ERROR
                "unexpected executable or wrapper in ${label}: ${_entry}")
        endif()
    endforeach()

    if(NOT _found_backtest OR NOT _found_shadow)
        message(FATAL_ERROR
            "${label} must contain engine_backtest and engine_shadow only")
    endif()
endfunction()

# Scanner-only mode is used by the negative self-test. It deliberately shares
# the production scanner so a passing self-test cannot drift from the guard.
if(DEFINED GUARD_SCAN_ROOT)
    assert_no_engine_live_path("${GUARD_SCAN_ROOT}" "guard probe")
    if(DEFINED GUARD_SCAN_COMPILE_COMMANDS)
        assert_no_live_compile_commands(
            "${GUARD_SCAN_COMPILE_COMMANDS}" "guard probe")
    endif()
    if(DEFINED GUARD_SCAN_BIN_ROOT)
        assert_research_bin_contents("${GUARD_SCAN_BIN_ROOT}" "guard probe")
    endif()
    return()
endif()

foreach(_required IN ITEMS
        GUARD_BUILD_DIR GUARD_BACKTEST GUARD_SHADOW
        GUARD_CPACK_COMMAND)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "research artifact guard requires ${_required}")
    endif()
endforeach()

file(READ "${GUARD_BUILD_DIR}/CMakeCache.txt" _cache)
string(FIND "${_cache}" "TRUETEST_RESEARCH_ONLY:BOOL=ON" _research_option)
if(_research_option EQUAL -1)
    message(FATAL_ERROR
        "artifact guard refuses a build without TRUETEST_RESEARCH_ONLY:BOOL=ON")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${GUARD_BUILD_DIR}" --target help
    RESULT_VARIABLE _help_result
    OUTPUT_VARIABLE _help_output
    ERROR_VARIABLE _help_error)
if(NOT _help_result EQUAL 0)
    message(FATAL_ERROR
        "failed to query research build targets: ${_help_error}")
endif()
if("${_help_output}\n${_help_error}" MATCHES "engine_live")
    message(FATAL_ERROR "engine_live is a known research build target")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${GUARD_BUILD_DIR}"
            --target engine_live
    RESULT_VARIABLE _live_build_result
    OUTPUT_VARIABLE _live_build_output
    ERROR_VARIABLE _live_build_error)
if(_live_build_result EQUAL 0)
    message(FATAL_ERROR
        "research build unexpectedly accepted --target engine_live")
endif()

foreach(_binary IN ITEMS GUARD_BACKTEST GUARD_SHADOW)
    if(NOT EXISTS "${${_binary}}")
        message(FATAL_ERROR
            "required research binary was not built: ${${_binary}}")
    endif()
endforeach()

assert_no_engine_live_path("${GUARD_BUILD_DIR}" "research build tree")

set(_compile_commands "${GUARD_BUILD_DIR}/compile_commands.json")
if(GUARD_EXPECT_COMPILE_COMMANDS)
    assert_no_live_compile_commands(
        "${_compile_commands}" "research compile_commands.json")
elseif(EXISTS "${_compile_commands}")
    assert_no_live_compile_commands(
        "${_compile_commands}" "research compile_commands.json")
endif()

set(_guard_root "${GUARD_BUILD_DIR}/research-artifact-guard")
set(_stage "${_guard_root}/install")
set(_packages "${_guard_root}/packages")
set(_source_packages "${_guard_root}/source-packages")
file(REMOVE_RECURSE "${_guard_root}")
file(MAKE_DIRECTORY "${_stage}" "${_packages}" "${_source_packages}")

set(_install_command
    "${CMAKE_COMMAND}" --install "${GUARD_BUILD_DIR}" --prefix "${_stage}")
if(DEFINED GUARD_CONFIG AND NOT "${GUARD_CONFIG}" STREQUAL "")
    list(APPEND _install_command --config "${GUARD_CONFIG}")
endif()
execute_process(
    COMMAND ${_install_command}
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "research staging install failed: ${_install_error}")
endif()

assert_no_engine_live_path("${_stage}" "research install staging")
assert_research_bin_contents("${_stage}" "research install staging")

set(_manifest "${GUARD_BUILD_DIR}/install_manifest.txt")
if(NOT EXISTS "${_manifest}")
    message(FATAL_ERROR "research install did not produce install_manifest.txt")
endif()
file(READ "${_manifest}" _manifest_content)
if(_manifest_content MATCHES "engine_live")
    message(FATAL_ERROR "install_manifest.txt contains engine_live")
endif()

set(_package_command
    "${GUARD_CPACK_COMMAND}"
    --config "${GUARD_BUILD_DIR}/CPackConfig.cmake"
    -B "${_packages}")
if(DEFINED GUARD_CONFIG AND NOT "${GUARD_CONFIG}" STREQUAL "")
    list(APPEND _package_command -C "${GUARD_CONFIG}")
endif()
execute_process(
    COMMAND ${_package_command}
    WORKING_DIRECTORY "${GUARD_BUILD_DIR}"
    RESULT_VARIABLE _package_result
    OUTPUT_VARIABLE _package_output
    ERROR_VARIABLE _package_error)
if(NOT _package_result EQUAL 0)
    message(FATAL_ERROR
        "research TGZ package failed: ${_package_error}")
endif()

file(GLOB _package_files LIST_DIRECTORIES FALSE "${_packages}/*")
list(LENGTH _package_files _package_file_count)
if(NOT _package_file_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one audited research package, found ${_package_file_count}")
endif()
list(GET _package_files 0 _archive)
if(NOT _archive MATCHES "\\.tar\\.gz$")
    message(FATAL_ERROR
        "research package is not the audited TGZ format: ${_archive}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${_archive}"
    RESULT_VARIABLE _tar_result
    OUTPUT_VARIABLE _tar_listing
    ERROR_VARIABLE _tar_error)
if(NOT _tar_result EQUAL 0)
    message(FATAL_ERROR "cannot inspect research TGZ: ${_tar_error}")
endif()
if(_tar_listing MATCHES "engine_live")
    message(FATAL_ERROR "research TGZ contains engine_live")
endif()

set(_packaged_backtest FALSE)
set(_packaged_shadow FALSE)
string(REPLACE "\r\n" "\n" _tar_listing "${_tar_listing}")
string(REPLACE "\n" ";" _tar_entries "${_tar_listing}")
foreach(_tar_entry IN LISTS _tar_entries)
    if(_tar_entry MATCHES "(^|/)bin/([^/]+)$")
        set(_packaged_name "${CMAKE_MATCH_2}")
        if(_packaged_name MATCHES "^engine_backtest(\\.exe)?$")
            set(_packaged_backtest TRUE)
        elseif(_packaged_name MATCHES "^engine_shadow(\\.exe)?$")
            set(_packaged_shadow TRUE)
        elseif(_packaged_name STREQUAL "truetest.dll")
            # BUILD_SHARED_LIB is a research-safe optional artifact on Windows.
        else()
            message(FATAL_ERROR
                "unexpected executable or wrapper in research TGZ: ${_packaged_name}")
        endif()
    endif()
endforeach()
if(NOT _packaged_backtest OR NOT _packaged_shadow)
    message(FATAL_ERROR
        "research TGZ must contain engine_backtest and engine_shadow")
endif()
foreach(_packaged_binary IN ITEMS engine_backtest engine_shadow)
    string(FIND "${_tar_listing}" "/bin/${_packaged_binary}"
        _packaged_binary_index)
    if(_packaged_binary_index EQUAL -1)
        message(FATAL_ERROR
            "research TGZ omitted ${_packaged_binary}")
    endif()
endforeach()

# A command-line -G override must not broaden the audited Research package
# format. The CPack project policy is evaluated after CPack resolves the actual
# generator, so this also exercises the enforcement rather than just config.
set(_denied_packages "${_guard_root}/denied-packages")
file(MAKE_DIRECTORY "${_denied_packages}")
set(_denied_package_command
    "${GUARD_CPACK_COMMAND}"
    --config "${GUARD_BUILD_DIR}/CPackConfig.cmake"
    -G DEB -B "${_denied_packages}")
if(DEFINED GUARD_CONFIG AND NOT "${GUARD_CONFIG}" STREQUAL "")
    list(APPEND _denied_package_command -C "${GUARD_CONFIG}")
endif()
execute_process(
    COMMAND ${_denied_package_command}
    WORKING_DIRECTORY "${GUARD_BUILD_DIR}"
    RESULT_VARIABLE _denied_package_result
    OUTPUT_VARIABLE _denied_package_output
    ERROR_VARIABLE _denied_package_error)
if(_denied_package_result EQUAL 0)
    message(FATAL_ERROR
        "research CPack unexpectedly accepted the unaudited DEB generator")
endif()
if(NOT "${_denied_package_output}\n${_denied_package_error}" MATCHES
       "permits only the audited TGZ CPack generator")
    message(FATAL_ERROR
        "research DEB override failed for an unexpected reason")
endif()
file(GLOB _denied_package_files LIST_DIRECTORIES FALSE
    "${_denied_packages}/*")
if(_denied_package_files)
    message(FATAL_ERROR
        "rejected research generator left a package artifact: ${_denied_package_files}")
endif()

execute_process(
    COMMAND "${GUARD_CPACK_COMMAND}"
            --config "${GUARD_BUILD_DIR}/CPackSourceConfig.cmake"
            -B "${_source_packages}"
    WORKING_DIRECTORY "${GUARD_BUILD_DIR}"
    RESULT_VARIABLE _source_package_result
    OUTPUT_VARIABLE _source_package_output
    ERROR_VARIABLE _source_package_error)
if(_source_package_result EQUAL 0)
    message(FATAL_ERROR
        "research build unexpectedly generated a CPack source package")
endif()
if(NOT "${_source_package_output}\n${_source_package_error}" MATCHES
       "source packaging is disabled for TRUETEST_RESEARCH_ONLY=ON")
    message(FATAL_ERROR
        "research source packaging failed for an unexpected reason")
endif()
file(GLOB_RECURSE _source_archives LIST_DIRECTORIES FALSE
    "${_source_packages}/*.7z"
    "${_source_packages}/*.tar"
    "${_source_packages}/*.tar.bz2"
    "${_source_packages}/*.tar.gz"
    "${_source_packages}/*.tar.xz"
    "${_source_packages}/*.zip")
if(_source_archives)
    message(FATAL_ERROR
        "research source-package refusal left a distribution artifact: ${_source_archives}")
endif()

message(STATUS
    "research artifact guard passed: no live target/TT_TARGET/TU, wrapper, staged binary, manifest/package member, generator override, or source-package path")
