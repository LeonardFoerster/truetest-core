cmake_minimum_required(VERSION 3.22)

if(TRUETEST_BUILD_PROFILE STREQUAL "linux-deterministic-v1")
    # Compiler/linker flags affect project()'s ABI probes. Validate the exact
    # profile values before those probes can consume injected inputs.
    function(_tt_require_preproject_profile_flag _variable _expected)
        if(NOT "${${_variable}}" STREQUAL "${_expected}")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids custom global/config-specific "
                "build flags in '${_variable}'")
        endif()
    endfunction()
    _tt_require_preproject_profile_flag(CMAKE_C_FLAGS "")
    _tt_require_preproject_profile_flag(
        CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
    _tt_require_preproject_profile_flag(CMAKE_CXX_FLAGS "")
    _tt_require_preproject_profile_flag(
        CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    foreach(_tt_preproject_empty_flag IN ITEMS
            CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_RELEASE
            CMAKE_SHARED_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS_RELEASE
            CMAKE_MODULE_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS_RELEASE
            CMAKE_STATIC_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS_RELEASE)
        _tt_require_preproject_profile_flag(
            ${_tt_preproject_empty_flag} "")
    endforeach()

    # These hooks execute during or immediately after project() and can change
    # compiler rules before the audited profile has a chance to inspect them.
    # Deterministic v1 therefore admits only the checked-in CMake modules.
    foreach(_tt_preproject_override IN ITEMS
            CMAKE_PROJECT_TOP_LEVEL_INCLUDES
            CMAKE_PROJECT_INCLUDE CMAKE_PROJECT_INCLUDE_BEFORE
            CMAKE_PROJECT_truetest_INCLUDE CMAKE_PROJECT_truetest_INCLUDE_BEFORE
            CMAKE_USER_MAKE_RULES_OVERRIDE
            CMAKE_USER_MAKE_RULES_OVERRIDE_C
            CMAKE_USER_MAKE_RULES_OVERRIDE_CXX
            CMAKE_MODULE_PATH CMAKE_TOOLCHAIN_FILE
            CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN
            CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
            CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK
            CMAKE_FIND_ROOT_PATH CMAKE_PREFIX_PATH CMAKE_PROGRAM_PATH
            CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
            CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
            CMAKE_C_STANDARD_LINK_DIRECTORIES
            CMAKE_CXX_STANDARD_LINK_DIRECTORIES
            CMAKE_C_STANDARD_LIBRARIES CMAKE_CXX_STANDARD_LIBRARIES
            CMAKE_C_COMPILE_OBJECT CMAKE_CXX_COMPILE_OBJECT
            CMAKE_C_LINK_EXECUTABLE CMAKE_CXX_LINK_EXECUTABLE
            CMAKE_C_CREATE_SHARED_LIBRARY
            CMAKE_CXX_CREATE_SHARED_LIBRARY
            CMAKE_C_CREATE_SHARED_MODULE
            CMAKE_CXX_CREATE_SHARED_MODULE
            CMAKE_C_CREATE_STATIC_LIBRARY
            CMAKE_CXX_CREATE_STATIC_LIBRARY
            CMAKE_C_ARCHIVE_CREATE CMAKE_C_ARCHIVE_APPEND
            CMAKE_C_ARCHIVE_FINISH CMAKE_CXX_ARCHIVE_CREATE
            CMAKE_CXX_ARCHIVE_APPEND CMAKE_CXX_ARCHIVE_FINISH
            CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS
            CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS
            CMAKE_C_USE_RESPONSE_FILE_FOR_INCLUDES
            CMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES
            CMAKE_C_USE_RESPONSE_FILE_FOR_LIBRARIES
            CMAKE_CXX_USE_RESPONSE_FILE_FOR_LIBRARIES
            CMAKE_C_RESPONSE_FILE_LINK_FLAG
            CMAKE_CXX_RESPONSE_FILE_LINK_FLAG
            CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR
            CMAKE_CROSSCOMPILING_EMULATOR)
        if(DEFINED ${_tt_preproject_override}
           AND NOT "${${_tt_preproject_override}}" STREQUAL "")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids pre-project CMake "
                "injection '${_tt_preproject_override}'")
        endif()
    endforeach()
    foreach(_tt_build_environment IN ITEMS
            CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH COMPILER_PATH
            LIBRARY_PATH GCC_EXEC_PREFIX LD_PRELOAD LD_AUDIT
            LD_LIBRARY_PATH LD_HWCAP_MASK GLIBC_TUNABLES)
        if(NOT "$ENV{${_tt_build_environment}}" STREQUAL "")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids inherited build "
                "environment '${_tt_build_environment}'")
        endif()
    endforeach()
endif()

project(truetest LANGUAGES C CXX VERSION 0.1.0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Build options ───────────────────────────────────────────────────────────
option(ENABLE_LIVE_DATA  "Deprecated no-op; enable a concrete venue provider instead" OFF)
option(ENABLE_DEBUG      "Build with performance instrumentation (requires Abseil)"   OFF)
option(ENABLE_BINANCE    "Build with Binance exchange provider"                       OFF)
option(ENABLE_BITGET     "Build with Bitget UTA futures provider"                     OFF)
option(ENABLE_BITUNIX    "Build with Bitunix futures provider (MD/shadow Phase 0–1)"  OFF)
option(ENABLE_WEB        "Build with embedded web UI server (civetweb)"                 OFF)
option(ENABLE_IMGUI      "Build with in-process ImGui strategy desk (GLFW+OpenGL3)"     OFF)
option(ENABLE_QUESTDB    "Build with QuestDB persistence backend"                     OFF)
option(ENABLE_TSAN       "Enable ThreadSanitizer"                                     OFF)
option(ENABLE_ASAN       "Enable AddressSanitizer (mutually exclusive with TSAN)"     OFF)
option(ENABLE_UBSAN      "Enable UndefinedBehaviorSanitizer (mutually exclusive with TSAN)" OFF)
option(ENABLE_LTO        "Enable link-time optimization for first-party Release targets"   ON)
option(ENABLE_NATIVE_OPT "Enable -march=native + unroll/omit-fp on all configured engines (Release)" OFF)
option(BUILD_SHARED_LIB  "Build libtruetest shared library with C API for embedding"  OFF)
option(BUILD_TESTS       "Build unit tests"                                           OFF)
option(ENABLE_BENCHMARKS "Build performance benchmarks (Google Benchmark)"            OFF)
option(TRUETEST_RESEARCH_ONLY
    "Build a research distribution without defining or packaging engine_live"         OFF)
option(TRUETEST_DETERMINISTIC_BUILD
    "Internal deterministic-profile switch; select it through the linux-deterministic preset" OFF)
set(TRUETEST_BUILD_PROFILE "default" CACHE STRING
    "Stable build-profile identifier embedded in deterministic run manifests")
set(TRUETEST_CONTAINER_DIGEST "" CACHE STRING
    "Optional immutable container/toolchain digest for hermetic build profiles")

if(TRUETEST_DETERMINISTIC_BUILD AND (ENABLE_LTO OR ENABLE_NATIVE_OPT))
    message(FATAL_ERROR
        "TRUETEST_DETERMINISTIC_BUILD requires ENABLE_LTO=OFF and "
        "ENABLE_NATIVE_OPT=OFF")
endif()

if(TRUETEST_DETERMINISTIC_BUILD
   AND NOT TRUETEST_BUILD_PROFILE STREQUAL "linux-deterministic-v1")
    message(FATAL_ERROR
        "TRUETEST_DETERMINISTIC_BUILD requires the complete "
        "linux-deterministic-v1 profile; use the linux-deterministic preset")
endif()

if(TRUETEST_BUILD_PROFILE STREQUAL "linux-deterministic-v1")
    if(NOT TRUETEST_DETERMINISTIC_BUILD
       OR NOT TRUETEST_RESEARCH_ONLY
       OR NOT BUILD_TESTS OR NOT CMAKE_EXPORT_COMPILE_COMMANDS
       OR ENABLE_LTO OR ENABLE_NATIVE_OPT
       OR CMAKE_INTERPROCEDURAL_OPTIMIZATION
       OR CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE
       OR CMAKE_UNITY_BUILD
       OR ENABLE_LIVE_DATA OR ENABLE_DEBUG OR ENABLE_WEB OR ENABLE_IMGUI
       OR ENABLE_QUESTDB OR ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN
       OR BUILD_SHARED_LIB OR ENABLE_BENCHMARKS
       OR ENABLE_BINANCE OR ENABLE_BITGET OR ENABLE_BITUNIX
       OR FETCHCONTENT_FULLY_DISCONNECTED
       OR NOT FETCHCONTENT_UPDATES_DISCONNECTED
       OR NOT CMAKE_BUILD_TYPE STREQUAL "Release"
       OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
        message(FATAL_ERROR
            "linux-deterministic-v1 profile invariants were overridden; "
            "use the checked-in preset without weakening its cache values")
    endif()
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU"
       OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "linux-deterministic-v1 currently requires the audited GNU C/C++ "
            "driver family")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR
            "linux-deterministic-v1 currently supports only audited x86_64 "
            "floating-point control")
    endif()
    foreach(_tt_system_tool IN ITEMS cmake git ninja)
        if(NOT EXISTS "/usr/bin/${_tt_system_tool}")
            message(FATAL_ERROR
                "linux-deterministic-v1 requires /usr/bin/${_tt_system_tool}")
        endif()
    endforeach()
    file(REAL_PATH "/usr/bin/cmake" _tt_audited_cmake_executable)
    file(REAL_PATH "/usr/bin/git" _tt_audited_git_executable)
    file(REAL_PATH "/usr/bin/ninja" _tt_audited_ninja_executable)
    file(REAL_PATH "${CMAKE_COMMAND}" _tt_actual_cmake_executable)
    file(REAL_PATH "${CMAKE_MAKE_PROGRAM}" _tt_actual_build_executor)
    if(NOT _tt_actual_cmake_executable STREQUAL _tt_audited_cmake_executable
       OR NOT _tt_actual_build_executor STREQUAL _tt_audited_ninja_executable)
        message(FATAL_ERROR
            "linux-deterministic-v1 requires canonical /usr/bin/cmake and "
            "/usr/bin/ninja build executors")
    endif()
    file(SHA256 "${_tt_audited_cmake_executable}"
        _tt_audited_cmake_sha256)
    file(SHA256 "${_tt_audited_git_executable}" _tt_audited_git_sha256)
    file(SHA256 "${_tt_audited_ninja_executable}"
        _tt_audited_ninja_sha256)
    file(REAL_PATH "/usr/bin/cc" _tt_expected_c_compiler)
    file(REAL_PATH "/usr/bin/c++" _tt_expected_cxx_compiler)
    file(REAL_PATH "${CMAKE_C_COMPILER}" _tt_actual_c_compiler)
    file(REAL_PATH "${CMAKE_CXX_COMPILER}" _tt_actual_cxx_compiler)
    if(NOT _tt_actual_c_compiler STREQUAL _tt_expected_c_compiler
       OR NOT _tt_actual_cxx_compiler STREQUAL _tt_expected_cxx_compiler)
        message(FATAL_ERROR
            "linux-deterministic-v1 requires the canonical /usr/bin/cc and "
            "/usr/bin/c++ system compiler drivers")
    endif()
    if(DEFINED CMAKE_LINKER_TYPE
       AND NOT CMAKE_LINKER_TYPE STREQUAL ""
       AND NOT CMAKE_LINKER_TYPE STREQUAL "DEFAULT")
        message(FATAL_ERROR
            "linux-deterministic-v1 profile forbids alternate "
            "CMAKE_LINKER_TYPE selection")
    endif()
    foreach(_tt_compiler_arg1 IN ITEMS
            CMAKE_C_COMPILER_ARG1 CMAKE_CXX_COMPILER_ARG1)
        if(DEFINED ${_tt_compiler_arg1}
           AND NOT "${${_tt_compiler_arg1}}" STREQUAL "")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids implicit compiler "
                "argument '${_tt_compiler_arg1}'")
        endif()
    endforeach()
    foreach(_tt_implicit_override IN ITEMS
            CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
            CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
            CMAKE_C_STANDARD_LINK_DIRECTORIES
            CMAKE_CXX_STANDARD_LINK_DIRECTORIES
            CMAKE_C_STANDARD_LIBRARIES CMAKE_CXX_STANDARD_LIBRARIES)
        if(DEFINED ${_tt_implicit_override}
           AND NOT "${${_tt_implicit_override}}" STREQUAL "")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids implicit global "
                "compile/link input '${_tt_implicit_override}'")
        endif()
    endforeach()
    foreach(_tt_driver IN ITEMS CMAKE_C_COMPILER CMAKE_CXX_COMPILER)
        if(NOT EXISTS "${${_tt_driver}}")
            message(FATAL_ERROR
                "linux-deterministic-v1 compiler driver is missing: "
                "${${_tt_driver}}")
        endif()
        file(READ "${${_tt_driver}}" _tt_driver_magic
            OFFSET 0 LIMIT 4 HEX)
        if(NOT _tt_driver_magic STREQUAL "7f454c46")
            message(FATAL_ERROR
                "linux-deterministic-v1 requires direct ELF compiler drivers; "
                "scripts and mutable compiler wrappers are forbidden")
        endif()
    endforeach()

    string(TOUPPER "${CMAKE_BUILD_TYPE}" _tt_profile_build_type_upper)
    set(_tt_profile_user_flags
        "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_tt_profile_build_type_upper}} "
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_tt_profile_build_type_upper}} "
        "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${_tt_profile_build_type_upper}} "
        "${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS_${_tt_profile_build_type_upper}} "
        "${CMAKE_MODULE_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS_${_tt_profile_build_type_upper}}")
    foreach(_tt_forbidden_flag IN ITEMS
            "-Ofast" "-ffast-math" "-funsafe-math-optimizations"
            "-ffinite-math-only" "-fassociative-math"
            "-freciprocal-math" "-fno-signed-zeros"
            "-fno-trapping-math" "-ffp-contract=fast"
            "-ffp-contract=on" "-flto" "-march=" "-mtune=" "-mcpu="
            "-fuse-ld=" "--ld-path=")
        string(FIND "${_tt_profile_user_flags}"
            "${_tt_forbidden_flag}" _tt_forbidden_flag_position)
        if(NOT _tt_forbidden_flag_position EQUAL -1)
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids user flag "
                "'${_tt_forbidden_flag}' in global/config-specific compile "
                "or linker flags")
        endif()
    endforeach()
    if(_tt_profile_user_flags MATCHES "(^|[ \t])-B[^ \t]*([ \t]|$)")
        message(FATAL_ERROR
            "linux-deterministic-v1 profile forbids compiler-driver -B "
            "toolchain overrides")
    endif()

    # The v1 profile has no contract for hashing arbitrary external files named
    # by user flags (for example -include, @response, -L, or loose objects).
    # Admit only the exact checked-in preset values, so every compile/link input
    # is either repository/dependency content or an audited toolchain artifact.
    function(_tt_require_profile_flag _variable _expected)
        if(NOT "${${_variable}}" STREQUAL "${_expected}")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids custom global/config-specific "
                "build flags in '${_variable}'")
        endif()
    endfunction()
    _tt_require_profile_flag(CMAKE_C_FLAGS "")
    _tt_require_profile_flag(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
    _tt_require_profile_flag(CMAKE_CXX_FLAGS "")
    _tt_require_profile_flag(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    _tt_require_profile_flag(CMAKE_EXE_LINKER_FLAGS "")
    _tt_require_profile_flag(CMAKE_EXE_LINKER_FLAGS_RELEASE "")
    _tt_require_profile_flag(CMAKE_SHARED_LINKER_FLAGS "")
    _tt_require_profile_flag(CMAKE_SHARED_LINKER_FLAGS_RELEASE "")
    _tt_require_profile_flag(CMAKE_MODULE_LINKER_FLAGS "")
    _tt_require_profile_flag(CMAKE_MODULE_LINKER_FLAGS_RELEASE "")
    _tt_require_profile_flag(CMAKE_STATIC_LINKER_FLAGS "")
    _tt_require_profile_flag(CMAKE_STATIC_LINKER_FLAGS_RELEASE "")

    foreach(_tt_launcher IN ITEMS
            CMAKE_C_COMPILER_LAUNCHER CMAKE_C_LINKER_LAUNCHER
            CMAKE_CXX_COMPILER_LAUNCHER CMAKE_CXX_LINKER_LAUNCHER
            CMAKE_RULE_LAUNCH_COMPILE CMAKE_RULE_LAUNCH_LINK)
        if(DEFINED ${_tt_launcher} AND NOT "${${_tt_launcher}}" STREQUAL "")
            message(FATAL_ERROR
                "linux-deterministic-v1 profile forbids build launcher "
                "'${_tt_launcher}'")
        endif()
    endforeach()

    get_cmake_property(_tt_profile_cache_variables CACHE_VARIABLES)
    foreach(_tt_cache_variable IN LISTS _tt_profile_cache_variables)
        if(_tt_cache_variable MATCHES "^FETCHCONTENT_SOURCE_DIR_")
            if(NOT "${${_tt_cache_variable}}" STREQUAL "")
                message(FATAL_ERROR
                    "linux-deterministic-v1 profile forbids local FetchContent "
                    "source override '${_tt_cache_variable}' because the "
                    "embedded dependency identity would no longer describe "
                    "the built source")
            endif()
        endif()
    endforeach()
    if(DEFINED FETCHCONTENT_BASE_DIR
       AND NOT FETCHCONTENT_BASE_DIR STREQUAL "${CMAKE_BINARY_DIR}/_deps")
        message(FATAL_ERROR
            "linux-deterministic-v1 profile forbids FetchContent resolution "
            "override 'FETCHCONTENT_BASE_DIR'")
    endif()
    if(DEFINED FETCHCONTENT_TRY_FIND_PACKAGE_MODE
       AND NOT FETCHCONTENT_TRY_FIND_PACKAGE_MODE STREQUAL "")
        message(FATAL_ERROR
            "linux-deterministic-v1 profile forbids FetchContent resolution "
            "override 'FETCHCONTENT_TRY_FIND_PACKAGE_MODE'")
    endif()
endif()

if(ENABLE_LIVE_DATA)
    message(DEPRECATION
        "ENABLE_LIVE_DATA no longer enables a data source and is ignored; "
        "use ENABLE_BINANCE, ENABLE_BITGET, or ENABLE_BITUNIX")
endif()

if(TRUETEST_RESEARCH_ONLY AND (ENABLE_BINANCE OR ENABLE_BITGET))
    set(_tt_research_conflicts "")
    foreach(_tt_option IN ITEMS ENABLE_BINANCE ENABLE_BITGET)
        if(${_tt_option})
            list(APPEND _tt_research_conflicts "${_tt_option}")
        endif()
    endforeach()
    list(JOIN _tt_research_conflicts ", " _tt_research_conflicts_text)
    message(FATAL_ERROR
        "TRUETEST_RESEARCH_ONLY=ON is incompatible with live-capable venue "
        "backend generation (${_tt_research_conflicts_text}); these backends "
        "currently compile real order transports into shared engine_core.")
endif()

if(TRUETEST_RESEARCH_ONLY)
    file(GLOB_RECURSE _tt_stale_live_artifacts LIST_DIRECTORIES TRUE
        "${CMAKE_BINARY_DIR}/*")
    foreach(_tt_stale_candidate IN LISTS _tt_stale_live_artifacts)
        file(RELATIVE_PATH _tt_stale_relative
            "${CMAKE_BINARY_DIR}" "${_tt_stale_candidate}")
        string(REPLACE "\\" "/" _tt_stale_relative "${_tt_stale_relative}")
        if(_tt_stale_relative MATCHES "engine_live")
            message(FATAL_ERROR
                "TRUETEST_RESEARCH_ONLY=ON found a stale engine_live artifact "
                "in the build tree (${_tt_stale_candidate}); use a clean, "
                "dedicated Research build directory.")
        endif()
    endforeach()
endif()

include(CompilerFlags)   # sets C++23, opt profiles, provides tt_apply_* helpers
include(Dependencies)    # provides tt_fetch_* and tt_wire_optional_backends
include(Sources)         # canonical ENGINE_CORE_SOURCES + TEST_SOURCES (explicit lists)

tt_fetch_dependencies()

if(TRUETEST_DETERMINISTIC_BUILD)
    FetchContent_GetProperties(cli11 SOURCE_DIR _tt_cli11_source_dir)
    FetchContent_GetProperties(zstd SOURCE_DIR _tt_zstd_source_dir)
    FetchContent_GetProperties(
        nlohmann_json SOURCE_DIR _tt_nlohmann_source_dir)
    add_custom_target(truetest_dependency_identity ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DTT_DEPENDENCY_NAME=cli11"
            "-DTT_DEPENDENCY_SOURCE_DIR=${_tt_cli11_source_dir}"
            "-DTT_EXPECTED_COMMIT=6c7b07a878ad834957b98d0f9ce1dbe0cb204fc9"
            "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
            "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyDependencyIdentity.cmake"
        COMMAND "${CMAKE_COMMAND}"
            "-DTT_DEPENDENCY_NAME=zstd"
            "-DTT_DEPENDENCY_SOURCE_DIR=${_tt_zstd_source_dir}"
            "-DTT_EXPECTED_COMMIT=794ea1b0afca0f020f4e57b6732332231fb23c70"
            "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
            "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyDependencyIdentity.cmake"
        COMMAND "${CMAKE_COMMAND}"
            "-DTT_DEPENDENCY_NAME=nlohmann-json"
            "-DTT_DEPENDENCY_SOURCE_DIR=${_tt_nlohmann_source_dir}"
            "-DTT_EXPECTED_COMMIT=9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03"
            "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
            "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyDependencyIdentity.cmake"
        VERBATIM)
endif()

if(ENABLE_NATIVE_OPT)
    set(TRUETEST_MARCH "native")
else()
    set(TRUETEST_MARCH "portable-baseline")
endif()
if(TRUETEST_DETERMINISTIC_BUILD)
    set(TRUETEST_FP_FLAGS "-fno-fast-math -ffp-contract=off")
else()
    set(TRUETEST_FP_FLAGS "compiler-default")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _tt_build_type_upper)
if(CMAKE_C_STANDARD)
    set(_tt_c_standard_identity "${CMAKE_C_STANDARD}")
else()
    set(_tt_c_standard_identity "compiler-default")
endif()
set(_tt_config_cxx_flags "${CMAKE_CXX_FLAGS_${_tt_build_type_upper}}")
set(_tt_config_c_flags "${CMAKE_C_FLAGS_${_tt_build_type_upper}}")
set(_tt_config_exe_linker_flags
    "${CMAKE_EXE_LINKER_FLAGS_${_tt_build_type_upper}}")
set(_tt_compiler_binary_sha256 "unavailable")
if(EXISTS "${CMAKE_CXX_COMPILER}")
    file(SHA256 "${CMAKE_CXX_COMPILER}" _tt_compiler_binary_sha256)
endif()
set(_tt_c_compiler_binary_sha256 "unavailable")
if(CMAKE_C_COMPILER AND EXISTS "${CMAKE_C_COMPILER}")
    file(SHA256 "${CMAKE_C_COMPILER}" _tt_c_compiler_binary_sha256)
endif()
set(_tt_linker_binary_sha256 "unavailable")
if(CMAKE_LINKER AND EXISTS "${CMAKE_LINKER}")
    file(SHA256 "${CMAKE_LINKER}" _tt_linker_binary_sha256)
endif()
set(_tt_archiver_binary_sha256 "unavailable")
if(CMAKE_AR AND EXISTS "${CMAKE_AR}")
    file(SHA256 "${CMAKE_AR}" _tt_archiver_binary_sha256)
endif()
set(_tt_ranlib_binary_sha256 "unavailable")
if(CMAKE_RANLIB AND EXISTS "${CMAKE_RANLIB}")
    file(SHA256 "${CMAKE_RANLIB}" _tt_ranlib_binary_sha256)
endif()
set(_tt_toolchain_file_sha256 "not-configured")
if(CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    file(SHA256 "${CMAKE_TOOLCHAIN_FILE}" _tt_toolchain_file_sha256)
endif()

function(_tt_resolve_gnu_driver_program compiler program
         out_path out_sha256)
    execute_process(
        COMMAND "${compiler}" "-print-prog-name=${program}"
        RESULT_VARIABLE _tt_program_result
        OUTPUT_VARIABLE _tt_program_path
        ERROR_VARIABLE _tt_program_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _tt_program_result EQUAL 0 OR _tt_program_path STREQUAL "")
        message(FATAL_ERROR
            "cannot resolve GNU driver program '${program}': "
            "${_tt_program_error}")
    endif()
    if(NOT IS_ABSOLUTE "${_tt_program_path}")
        find_program(_tt_program_absolute
            NAMES "${_tt_program_path}" REQUIRED NO_CACHE)
        set(_tt_program_path "${_tt_program_absolute}")
    endif()
    file(REAL_PATH "${_tt_program_path}" _tt_program_path)
    if(NOT EXISTS "${_tt_program_path}")
        message(FATAL_ERROR
            "resolved GNU driver program does not exist: ${_tt_program_path}")
    endif()
    file(SHA256 "${_tt_program_path}" _tt_program_sha256)
    set(${out_path} "${_tt_program_path}" PARENT_SCOPE)
    set(${out_sha256} "${_tt_program_sha256}" PARENT_SCOPE)
endfunction()

set(_tt_cc1_path "unavailable")
set(_tt_cc1_sha256 "unavailable")
set(_tt_cc1plus_path "unavailable")
set(_tt_cc1plus_sha256 "unavailable")
set(_tt_collect2_path "unavailable")
set(_tt_collect2_sha256 "unavailable")
set(_tt_assembler_path "unavailable")
set(_tt_assembler_sha256 "unavailable")
set(_tt_driver_linker_path "unavailable")
set(_tt_driver_linker_sha256 "unavailable")
if(TRUETEST_DETERMINISTIC_BUILD)
    _tt_resolve_gnu_driver_program(
        "${CMAKE_C_COMPILER}" cc1 _tt_cc1_path _tt_cc1_sha256)
    _tt_resolve_gnu_driver_program(
        "${CMAKE_CXX_COMPILER}" cc1plus
        _tt_cc1plus_path _tt_cc1plus_sha256)
    _tt_resolve_gnu_driver_program(
        "${CMAKE_CXX_COMPILER}" collect2
        _tt_collect2_path _tt_collect2_sha256)
    _tt_resolve_gnu_driver_program(
        "${CMAKE_CXX_COMPILER}" as
        _tt_assembler_path _tt_assembler_sha256)
    _tt_resolve_gnu_driver_program(
        "${CMAKE_CXX_COMPILER}" ld
        _tt_driver_linker_path _tt_driver_linker_sha256)
endif()
set(TRUETEST_EFFECTIVE_BUILD_FLAGS
    "build_type=${CMAKE_BUILD_TYPE};generator=${CMAKE_GENERATOR};cmake=${CMAKE_VERSION};cmake_sha256=${_tt_audited_cmake_sha256};build_executor_sha256=${_tt_audited_ninja_sha256};git_sha256=${_tt_audited_git_sha256};c_standard=${_tt_c_standard_identity};c_compiler_id=${CMAKE_C_COMPILER_ID};c_compiler_version=${CMAKE_C_COMPILER_VERSION};c_compiler_sha256=${_tt_c_compiler_binary_sha256};cc1_sha256=${_tt_cc1_sha256};c_flags=${CMAKE_C_FLAGS};c_flags_config=${_tt_config_c_flags};cxx_standard=23;cxx_compiler_id=${CMAKE_CXX_COMPILER_ID};cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION};cxx_compiler_sha256=${_tt_compiler_binary_sha256};cc1plus_sha256=${_tt_cc1plus_sha256};collect2_sha256=${_tt_collect2_sha256};assembler_sha256=${_tt_assembler_sha256};driver_linker_sha256=${_tt_driver_linker_sha256};linker_sha256=${_tt_linker_binary_sha256};archiver_sha256=${_tt_archiver_binary_sha256};ranlib_sha256=${_tt_ranlib_binary_sha256};toolchain_file_sha256=${_tt_toolchain_file_sha256};cxx_flags=${CMAKE_CXX_FLAGS};cxx_flags_config=${_tt_config_cxx_flags};exe_linker_flags=${CMAKE_EXE_LINKER_FLAGS};exe_linker_flags_config=${_tt_config_exe_linker_flags};lto=${ENABLE_LTO};native=${ENABLE_NATIVE_OPT};deterministic=${TRUETEST_DETERMINISTIC_BUILD};research_only=${TRUETEST_RESEARCH_ONLY};asan=${ENABLE_ASAN};ubsan=${ENABLE_UBSAN};tsan=${ENABLE_TSAN};debug=${ENABLE_DEBUG};web=${ENABLE_WEB};imgui=${ENABLE_IMGUI};shared=${BUILD_SHARED_LIB};benchmarks=${ENABLE_BENCHMARKS};binance=${ENABLE_BINANCE};bitget=${ENABLE_BITGET};bitunix=${ENABLE_BITUNIX};questdb=${ENABLE_QUESTDB};fetch_fully_disconnected=${FETCHCONTENT_FULLY_DISCONNECTED};fetch_updates_disconnected=${FETCHCONTENT_UPDATES_DISCONNECTED}")
if(NOT TRUETEST_CONTAINER_DIGEST)
    set(TRUETEST_CONTAINER_DIGEST "not-hermetic")
endif()
if(TRUETEST_DETERMINISTIC_BUILD)
    # Configure wall time is observational state. Embedding it would make two
    # otherwise identical deterministic-profile builds produce different
    # executables and therefore different build identities.
    set(TRUETEST_BUILD_TIMESTAMP "deterministic-build-no-wall-clock")
else()
    string(TIMESTAMP TRUETEST_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
endif()

# ── Shared engine core (OBJECT library) ─────────────────────────────────────
# Source list lives in cmake/Sources.cmake (ENGINE_CORE_SOURCES).
# This keeps the one obvious registration point for core code.
add_library(engine_core OBJECT ${ENGINE_CORE_SOURCES})
# PIC always on so BUILD_SHARED_LIB can link engine_core objects into
# libtruetest.so without R_X86_64_PC32 failures. Static engine_* binaries
# are unaffected (PIC objects link fine into executables).
set_target_properties(engine_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(engine_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(engine_core PUBLIC
    CLI11::CLI11 nlohmann_json::nlohmann_json libzstd_static)
tt_apply_common_flags(engine_core)
# Native tuning must cover the shared engine objects as well as the small
# per-binary entry points; otherwise ENABLE_NATIVE_OPT leaves almost all engine
# code at the portable baseline.
tt_apply_live_flags(engine_core)
# Call ONCE here — PUBLIC usage-requirements propagate to everything that
# links engine_core (CMake ≥ 3.12 object-library linking semantics).
tt_wire_optional_backends(engine_core)

# ── Generated build/source audit headers ───────────────────────────────────
# Toolchain values are fixed at configure time. Git commit/dirty/diff identity
# is regenerated before every first-party build so an ordinary source edit +
# rebuild cannot leave a stale source fingerprint embedded in the binary.
if(DEFINED OPENSSL_VERSION AND OPENSSL_VERSION)
    set(TRUETEST_DEP_OPENSSL "${OPENSSL_VERSION}")
else()
    set(TRUETEST_DEP_OPENSSL "not-linked")
endif()
if(DEFINED Boost_VERSION_STRING AND Boost_VERSION_STRING)
    set(TRUETEST_DEP_BOOST "${Boost_VERSION_STRING}")
else()
    set(TRUETEST_DEP_BOOST "not-linked")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/truetest_version.h.in"
    "${CMAKE_BINARY_DIR}/generated/tt/truetest_version.h"
    @ONLY)

set(_tt_source_identity_header
    "${CMAKE_BINARY_DIR}/generated/tt/truetest_source_identity.h")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DTT_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DTT_OUTPUT_HEADER=${_tt_source_identity_header}"
        "-DTT_TEMPLATE=${CMAKE_SOURCE_DIR}/cmake/truetest_source_identity.h.in"
        "-DTT_REQUIRE_EXACT=${TRUETEST_DETERMINISTIC_BUILD}"
        "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
        "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateSourceIdentity.cmake"
    RESULT_VARIABLE _tt_source_identity_configure_result)
if(NOT _tt_source_identity_configure_result EQUAL 0)
    message(FATAL_ERROR "failed to generate configure-time source identity")
endif()
add_custom_target(truetest_source_identity ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DTT_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DTT_OUTPUT_HEADER=${_tt_source_identity_header}"
        "-DTT_TEMPLATE=${CMAKE_SOURCE_DIR}/cmake/truetest_source_identity.h.in"
        "-DTT_REQUIRE_EXACT=${TRUETEST_DETERMINISTIC_BUILD}"
        "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
        "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateSourceIdentity.cmake"
    BYPRODUCTS "${_tt_source_identity_header}"
    VERBATIM)
add_dependencies(engine_core truetest_source_identity)
if(TRUETEST_DETERMINISTIC_BUILD)
    add_custom_target(truetest_toolchain_identity ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DTT_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DTT_C_COMPILER_SHA256=${_tt_c_compiler_binary_sha256}"
            "-DTT_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DTT_COMPILER_SHA256=${_tt_compiler_binary_sha256}"
            "-DTT_LINKER=${CMAKE_LINKER}"
            "-DTT_LINKER_SHA256=${_tt_linker_binary_sha256}"
            "-DTT_ARCHIVER=${CMAKE_AR}"
            "-DTT_ARCHIVER_SHA256=${_tt_archiver_binary_sha256}"
            "-DTT_RANLIB=${CMAKE_RANLIB}"
            "-DTT_RANLIB_SHA256=${_tt_ranlib_binary_sha256}"
            "-DTT_CC1=${_tt_cc1_path}"
            "-DTT_CC1_SHA256=${_tt_cc1_sha256}"
            "-DTT_CC1PLUS=${_tt_cc1plus_path}"
            "-DTT_CC1PLUS_SHA256=${_tt_cc1plus_sha256}"
            "-DTT_COLLECT2=${_tt_collect2_path}"
            "-DTT_COLLECT2_SHA256=${_tt_collect2_sha256}"
            "-DTT_ASSEMBLER=${_tt_assembler_path}"
            "-DTT_ASSEMBLER_SHA256=${_tt_assembler_sha256}"
            "-DTT_DRIVER_LINKER=${_tt_driver_linker_path}"
            "-DTT_DRIVER_LINKER_SHA256=${_tt_driver_linker_sha256}"
            "-DTT_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DTT_TOOLCHAIN_FILE_SHA256=${_tt_toolchain_file_sha256}"
            "-DTT_CMAKE=${_tt_audited_cmake_executable}"
            "-DTT_CMAKE_SHA256=${_tt_audited_cmake_sha256}"
            "-DTT_GIT=${_tt_audited_git_executable}"
            "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
            "-DTT_BUILD_EXECUTOR=${_tt_audited_ninja_executable}"
            "-DTT_BUILD_EXECUTOR_SHA256=${_tt_audited_ninja_sha256}"
            -P "${CMAKE_SOURCE_DIR}/cmake/VerifyToolchainIdentity.cmake"
        VERBATIM)
    add_dependencies(engine_core truetest_toolchain_identity)
    add_dependencies(engine_core truetest_dependency_identity)
endif()

# ── Executable factory ──────────────────────────────────────────────────────
# Produces engine_backtest / engine_shadow / engine_live. Each binary owns
# its own translation unit under src/bin/<target>/main.cpp per Step 10's
# directive (prerequisites.md §10 "Binaries live under src/bin/"). The
# per-target main.cpp currently #includes the shared src/bin/main.inc so all
# three compile the same implementation with a distinct TT_TARGET define;
# as deepdive phases land, live-only code migrates out of the shared .inc
# into src/bin/engine_live/ directly, making the wrappers diverge.
set(_tt_live_entry_source src/bin/engine_live/main.cpp)
function(_tt_add_engine_binary name target_id entry)
    if(TRUETEST_RESEARCH_ONLY AND target_id STREQUAL "TT_TARGET_LIVE")
        message(FATAL_ERROR
            "TRUETEST_RESEARCH_ONLY=ON is incompatible with live-target generation.")
    endif()
    add_executable(${name} ${entry})
    add_dependencies(${name} truetest_source_identity)
    target_compile_definitions(${name} PRIVATE TT_TARGET=${target_id})
    target_link_libraries(${name} PRIVATE engine_core)
    tt_apply_common_flags(${name})
endfunction()

_tt_add_engine_binary(engine_backtest TT_TARGET_BACKTEST src/bin/engine_backtest/main.cpp)
_tt_add_engine_binary(engine_shadow   TT_TARGET_SHADOW   src/bin/engine_shadow/main.cpp)
if(NOT TRUETEST_RESEARCH_ONLY)
    _tt_add_engine_binary(engine_live TT_TARGET_LIVE "${_tt_live_entry_source}")
endif()

# Rich (ncurses) tabbed dashboard — shadow and live binaries only.
# engine_backtest keeps the existing ANSI ConsoleDashboard.
tt_wire_rich_tui(engine_shadow)
if(NOT TRUETEST_RESEARCH_ONLY)
    tt_wire_rich_tui(engine_live)
endif()

# ImGui personal strategy desk (optional; GLFW + OpenGL3 + ImPlot).
# Wired on shadow/live for Monitor/Execute; research launcher is Phase 4+.
if(ENABLE_IMGUI)
    tt_wire_imgui_desk(engine_shadow)
    if(NOT TRUETEST_RESEARCH_ONLY)
        tt_wire_imgui_desk(engine_live)
    endif()
endif()

# Native microarchitecture tuning. Gated on ENABLE_NATIVE_OPT (default off);
# when the user opts in they want it across the board, not just on live.
# CI and portable builds keep ENABLE_NATIVE_OPT=OFF and are unaffected.
tt_apply_live_flags(engine_backtest)
tt_apply_live_flags(engine_shadow)
if(NOT TRUETEST_RESEARCH_ONLY)
    tt_apply_live_flags(engine_live)
endif()

# ── Shared library (C API for language bindings) ────────────────────────────
if(BUILD_SHARED_LIB)
    add_library(truetest_shared SHARED
        src/api/truetest_api.cpp)
    set_target_properties(truetest_shared PROPERTIES
        OUTPUT_NAME truetest
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET     hidden
        VISIBILITY_INLINES_HIDDEN ON
        VERSION                   ${PROJECT_VERSION}
        SOVERSION                 0)
    target_include_directories(truetest_shared PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/api")
    target_link_libraries(truetest_shared PRIVATE engine_core)
    target_compile_definitions(truetest_shared PRIVATE TRUETEST_API_BUILDING)
    tt_apply_common_flags(truetest_shared)

    install(TARGETS truetest_shared
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)
    install(FILES src/api/truetest_api.h
        DESTINATION include/truetest)
endif()

# ── Unit tests (GoogleTest) ─────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    tt_fetch_tests_dependencies()

    if(TRUETEST_DETERMINISTIC_BUILD)
        FetchContent_GetProperties(
            googletest SOURCE_DIR _tt_googletest_source_dir)
        add_custom_target(truetest_test_dependency_identity ALL
            COMMAND "${CMAKE_COMMAND}"
                "-DTT_DEPENDENCY_NAME=googletest"
                "-DTT_DEPENDENCY_SOURCE_DIR=${_tt_googletest_source_dir}"
                "-DTT_EXPECTED_COMMIT=b514bdc898e2951020cbdca1304b75f5950d1f59"
                "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
                "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
                -P "${CMAKE_SOURCE_DIR}/cmake/VerifyDependencyIdentity.cmake"
            VERBATIM)
    endif()

    # TEST_SOURCES (base list + conditional appends for QUESTDB/WEB) and the
    # engine.cpp LOC regression guard now live in cmake/Sources.cmake.
    # This is the single obvious place for test registration.
    add_executable(truetest_tests ${TEST_SOURCES})
    target_include_directories(truetest_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(truetest_tests PRIVATE
        TRUETEST_API_BUILDING
        TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(truetest_tests PRIVATE engine_core GTest::gtest)
    if(TRUETEST_DETERMINISTIC_BUILD)
        add_dependencies(
            truetest_tests truetest_test_dependency_identity)
    endif()
    tt_apply_common_flags(truetest_tests)

    include(GoogleTest)
    gtest_discover_tests(truetest_tests)

    # CLI integration tests execute only binaries from this configured tree.
    add_executable(truetest_cli_tests tests/test_cli.cpp)
    target_link_libraries(truetest_cli_tests PRIVATE
        engine_core GTest::gtest GTest::gtest_main)
    if(TRUETEST_DETERMINISTIC_BUILD)
        add_dependencies(
            truetest_cli_tests truetest_test_dependency_identity)
        add_library(truetest_deterministic_preload_fixture MODULE
            tests/fixtures/deterministic_preload_fixture.cpp)
        set_target_properties(truetest_deterministic_preload_fixture PROPERTIES
            PREFIX "")
        tt_apply_common_flags(truetest_deterministic_preload_fixture)
        add_dependencies(truetest_cli_tests
            truetest_deterministic_preload_fixture)
        target_compile_definitions(truetest_cli_tests PRIVATE
            TRUETEST_DETERMINISTIC_BUILD_PROFILE=1
            "TRUETEST_PRELOAD_FIXTURE=\"$<TARGET_FILE:truetest_deterministic_preload_fixture>\""
            "TRUETEST_DYNAMIC_LOADER=\"/lib64/ld-linux-x86-64.so.2\"")
    endif()
    add_dependencies(truetest_cli_tests engine_backtest)
    if(TRUETEST_RESEARCH_ONLY)
        add_dependencies(truetest_cli_tests engine_shadow)
        target_compile_definitions(truetest_cli_tests PRIVATE
            TRUETEST_RESEARCH_ONLY=1)
    else()
        add_dependencies(truetest_cli_tests engine_live)
    endif()
    target_compile_definitions(truetest_cli_tests PRIVATE
        TRUETEST_ENGINE_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    if(ENABLE_QUESTDB)
        target_compile_definitions(truetest_cli_tests PRIVATE HAS_QUESTDB)
    endif()
    tt_apply_common_flags(truetest_cli_tests)
    gtest_discover_tests(truetest_cli_tests
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME launcher_contract
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_launchers.sh")
    set_tests_properties(launcher_contract PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME live_safety_freeze_contract
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_live_safety_freeze.sh")
    set_tests_properties(live_safety_freeze_contract PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME repro_baseline_contract
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_capture_repro_baseline.sh")
    set_tests_properties(repro_baseline_contract PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME deterministic_entropy_contract
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_deterministic_entropy_guard.sh")
    set_tests_properties(deterministic_entropy_contract PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        LABELS "determinism")
    add_test(NAME deterministic_build_profile_contract
        COMMAND "${CMAKE_COMMAND}"
            "-DGUARD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DGUARD_BUILD_DIR=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/tests/cmake/deterministic_profile_contract.cmake")
    set_tests_properties(deterministic_build_profile_contract PROPERTIES
        LABELS "determinism;build-security")
    if(TRUETEST_DETERMINISTIC_BUILD)
        add_test(NAME source_identity_contract
            COMMAND "${CMAKE_COMMAND}"
                "-DGUARD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DGUARD_HEADER=${_tt_source_identity_header}"
                "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
                "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/source_identity_guard.cmake")
        set_tests_properties(source_identity_contract PROPERTIES
            LABELS "determinism;build-security")
        add_test(NAME source_identity_contract_negative
            COMMAND "${CMAKE_COMMAND}"
                "-DGUARD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DGUARD_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DGUARD_SCRIPT=${CMAKE_SOURCE_DIR}/tests/cmake/source_identity_guard.cmake"
                "-DTT_GIT_EXECUTABLE=${_tt_audited_git_executable}"
                "-DTT_GIT_SHA256=${_tt_audited_git_sha256}"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_source_identity_guard_negative.cmake")
        set_tests_properties(source_identity_contract_negative PROPERTIES
            LABELS "determinism;build-security"
            RUN_SERIAL TRUE)
    endif()
    add_test(NAME sensitivity_oos_contract
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_capture_sensitivity_oos.sh"
            "$<TARGET_FILE:engine_backtest>")
    set_tests_properties(sensitivity_oos_contract PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME observability_evidence_contract
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_capture_observability_evidence.sh"
            "$<TARGET_FILE:truetest_tests>")
    set_tests_properties(observability_evidence_contract PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(TRUETEST_RESEARCH_ONLY)
        add_test(NAME research_artifact_guard
            COMMAND "${CMAKE_COMMAND}"
                "-DGUARD_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DGUARD_BACKTEST=$<TARGET_FILE:engine_backtest>"
                "-DGUARD_SHADOW=$<TARGET_FILE:engine_shadow>"
                "-DGUARD_CPACK_COMMAND=${CMAKE_CPACK_COMMAND}"
                "-DGUARD_CONFIG=$<CONFIG>"
                "-DGUARD_EXPECT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/research_artifact_guard.cmake")
        set_tests_properties(research_artifact_guard PROPERTIES
            LABELS "build-security"
            RUN_SERIAL TRUE)
        add_test(NAME research_artifact_guard_negative
            COMMAND "${CMAKE_COMMAND}"
                "-DGUARD_SCRIPT=${CMAKE_SOURCE_DIR}/tests/cmake/research_artifact_guard.cmake"
                "-DGUARD_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DGUARD_PROBE_DIR=${CMAKE_BINARY_DIR}/research-artifact-guard-negative"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_research_artifact_guard_negative.cmake")
        set_tests_properties(research_artifact_guard_negative PROPERTIES
            LABELS "build-security"
            RUN_SERIAL TRUE)
        add_test(NAME research_option_conflicts
            COMMAND "${CMAKE_COMMAND}"
                "-DGUARD_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DGUARD_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DGUARD_PROBE_DIR=${CMAKE_BINARY_DIR}/research-option-conflicts"
                -P "${CMAKE_SOURCE_DIR}/tests/cmake/test_research_option_conflicts.cmake")
        set_tests_properties(research_option_conflicts PROPERTIES
            LABELS "build-security"
            RUN_SERIAL TRUE)
    endif()
endif()

# ── Benchmarks (Google Benchmark) ───────────────────────────────────────────
if(ENABLE_BENCHMARKS)
    tt_fetch_bench_dependencies()

    add_executable(truetest_benchmarks benchmarks/bench_main.cpp)
    target_link_libraries(truetest_benchmarks PRIVATE
        engine_core benchmark::benchmark)
    tt_apply_common_flags(truetest_benchmarks)
endif()

# ── Web UI frontend bundle (ENABLE_WEB) ─────────────────────────────────────
# Optional convenience target: builds the React/Vite SPA into src/web/assets/.
# Deliberately NOT part of ALL so the C++ build stays node-free. Run explicitly:
#   cmake --build build --target web_assets
if(ENABLE_WEB)
    find_program(NPM_EXECUTABLE npm)
    if(NPM_EXECUTABLE)
        add_custom_target(web_assets
            COMMAND ${NPM_EXECUTABLE} ci
            COMMAND ${NPM_EXECUTABLE} run build
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/web/frontend
            COMMENT "Building TrueTest web UI bundle → src/web/assets/"
            VERBATIM)
    else()
        message(STATUS "ENABLE_WEB: npm not found — 'web_assets' target unavailable "
                       "(build the SPA manually or install Node).")
    endif()
endif()

# ── Install rules ───────────────────────────────────────────────────────────
install(TARGETS engine_backtest engine_shadow
    RUNTIME DESTINATION bin)
if(NOT TRUETEST_RESEARCH_ONLY)
    install(TARGETS engine_live
        RUNTIME DESTINATION bin)
endif()

# ── CPack packaging ─────────────────────────────────────────────────────────
if(TRUETEST_RESEARCH_ONLY)
    set(CPACK_PACKAGE_NAME          "truetest-research")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "TrueTest — research-only C++23 backtesting and shadow engine")
    # The research distribution intentionally exposes one audited, portable
    # binary format. A CPack project policy also rejects command-line -G
    # overrides so an unaudited generator cannot silently broaden the profile.
    set(CPACK_GENERATOR             "TGZ")
    set(_tt_research_cpack_policy
        "${CMAKE_BINARY_DIR}/TruetestResearchCPackPolicy.cmake")
    file(WRITE "${_tt_research_cpack_policy}" [=[
if(NOT CPACK_GENERATOR STREQUAL "TGZ")
    message(FATAL_ERROR
        "TRUETEST_RESEARCH_ONLY=ON permits only the audited TGZ CPack generator.")
endif()
]=])
    set(CPACK_PROJECT_CONFIG_FILE "${_tt_research_cpack_policy}")
else()
    set(CPACK_PACKAGE_NAME          "truetest")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "TrueTest — modular C++23 backtesting / shadow / live trading engine")
    set(CPACK_GENERATOR             "TGZ;DEB")
endif()
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT           "truetest@example.com")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "TrueTest Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION    "finance")
include(CPack)
if(TRUETEST_RESEARCH_ONLY)
    # include(CPack) always creates package_source and treats an empty source
    # generator as "use defaults". Replace only the generated source-package
    # config with a loud refusal; binary CPackConfig.cmake remains available.
    file(WRITE "${CMAKE_BINARY_DIR}/CPackSourceConfig.cmake"
        "message(FATAL_ERROR \"CPack source packaging is disabled for TRUETEST_RESEARCH_ONLY=ON; retained live source is not a research distribution.\")\n")
endif()

# ── Research-only graph invariant ───────────────────────────────────────────
# Keep this assertion after every first-party and CPack target declaration. It
# rejects the canonical name/source and, independently, any cosmetically
# renamed executable that is compiled with live TT_TARGET semantics.
if(TRUETEST_RESEARCH_ONLY)
    if(TARGET engine_live)
        message(FATAL_ERROR
            "TRUETEST_RESEARCH_ONLY=ON is incompatible with live-target generation.")
    endif()

    function(_tt_collect_first_party_targets directory output_variable)
        get_property(_directory_targets DIRECTORY "${directory}"
            PROPERTY BUILDSYSTEM_TARGETS)
        set(_collected_targets ${_directory_targets})
        get_property(_child_directories DIRECTORY "${directory}"
            PROPERTY SUBDIRECTORIES)
        foreach(_child_directory IN LISTS _child_directories)
            file(RELATIVE_PATH _child_relative
                "${CMAKE_SOURCE_DIR}" "${_child_directory}")
            if(NOT _child_relative MATCHES "^\\.\\.")
                _tt_collect_first_party_targets(
                    "${_child_directory}" _child_targets)
                list(APPEND _collected_targets ${_child_targets})
            endif()
        endforeach()
        set(${output_variable} "${_collected_targets}" PARENT_SCOPE)
    endfunction()

    _tt_collect_first_party_targets("${CMAKE_CURRENT_SOURCE_DIR}"
        _tt_research_targets)
    list(REMOVE_DUPLICATES _tt_research_targets)

    set(_tt_output_properties OUTPUT_NAME RUNTIME_OUTPUT_NAME)
    set(_tt_output_configs
        ${CMAKE_CONFIGURATION_TYPES}
        ${CMAKE_BUILD_TYPE}
        Debug Release RelWithDebInfo MinSizeRel)
    list(REMOVE_DUPLICATES _tt_output_configs)
    foreach(_tt_config IN LISTS _tt_output_configs)
        if(NOT _tt_config STREQUAL "")
            string(TOUPPER "${_tt_config}" _tt_config_upper)
            list(APPEND _tt_output_properties
                "OUTPUT_NAME_${_tt_config_upper}"
                "RUNTIME_OUTPUT_NAME_${_tt_config_upper}")
        endif()
    endforeach()

    foreach(_tt_target IN LISTS _tt_research_targets)
        if(_tt_target MATCHES "engine_live")
            message(FATAL_ERROR
                "TRUETEST_RESEARCH_ONLY=ON forbids live-named target ${_tt_target}")
        endif()

        foreach(_tt_property IN LISTS _tt_output_properties)
            get_target_property(_tt_property_value
                "${_tt_target}" "${_tt_property}")
            if(_tt_property_value
               AND NOT _tt_property_value MATCHES "-NOTFOUND$"
               AND _tt_property_value MATCHES "engine_live")
                message(FATAL_ERROR
                    "TRUETEST_RESEARCH_ONLY=ON forbids ${_tt_target} ${_tt_property}=${_tt_property_value}")
            endif()
        endforeach()

        foreach(_tt_property IN ITEMS
                COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS
                COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS)
            get_target_property(_tt_property_value
                "${_tt_target}" "${_tt_property}")
            if(_tt_property_value
               AND NOT _tt_property_value MATCHES "-NOTFOUND$"
               AND _tt_property_value MATCHES
                   "TT_TARGET=(TT_TARGET_LIVE|3)([^A-Za-z0-9_]|$)")
                message(FATAL_ERROR
                    "TRUETEST_RESEARCH_ONLY=ON forbids TT_TARGET_LIVE on ${_tt_target}")
            endif()
        endforeach()

        get_target_property(_tt_sources "${_tt_target}" SOURCES)
        foreach(_tt_source IN LISTS _tt_sources)
            string(REPLACE "\\" "/" _tt_source_normalized "${_tt_source}")
            if(_tt_source_normalized MATCHES "(^|/)src/bin/engine_live/"
               OR _tt_source_normalized STREQUAL "${_tt_live_entry_source}")
                message(FATAL_ERROR
                    "TRUETEST_RESEARCH_ONLY=ON forbids live entry-point source ${_tt_source} in ${_tt_target}")
            endif()
        endforeach()
    endforeach()
endif()
