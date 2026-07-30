cmake_minimum_required(VERSION 3.22)

project(truetest LANGUAGES CXX VERSION 0.1.0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Build options ───────────────────────────────────────────────────────────
option(ENABLE_LIVE_DATA  "Build with WebSocket live data feed (requires Boost.Beast)" OFF)
option(ENABLE_DEBUG      "Build with performance instrumentation (requires Abseil)"   OFF)
option(ENABLE_BINANCE    "Build with Binance exchange provider"                       OFF)
option(ENABLE_BITGET     "Build with Bitget exchange provider"                        OFF)
option(ENABLE_BYBIT      "Build with Bybit exchange provider (V5 linear)"             OFF)
option(ENABLE_GATE       "Build with Gate.io exchange provider (USDT-M futures)"      OFF)
option(ENABLE_WEB        "Build with embedded web UI server (civetweb)"                 OFF)
option(ENABLE_QUESTDB    "Build with QuestDB persistence backend"                     OFF)
option(ENABLE_TSAN       "Enable ThreadSanitizer"                                     OFF)
option(ENABLE_ASAN       "Enable AddressSanitizer (mutually exclusive with TSAN)"     OFF)
option(ENABLE_UBSAN      "Enable UndefinedBehaviorSanitizer (mutex with TSAN)"        OFF)
option(ENABLE_NATIVE_OPT "Enable -march=native + unroll/omit-fp on all three engines (Release)" OFF)
option(BUILD_SHARED_LIB  "Build libtruetest shared library with C API for embedding"  OFF)
option(BUILD_TESTS       "Build unit tests"                                           OFF)
option(ENABLE_BENCHMARKS "Build performance benchmarks (Google Benchmark)"            OFF)

include(CompilerFlags)   # sets C++23, opt profiles, provides tt_apply_* helpers
include(Dependencies)    # provides tt_fetch_* and tt_wire_optional_backends
include(Sources)         # canonical ENGINE_CORE_SOURCES + TEST_SOURCES (explicit lists)

tt_fetch_dependencies()

# ── Generated build-audit header ────────────────────────────────────────────
# Captures commit SHA + build timestamp at configure time so every binary
# emits a reproducible AUDIT log line on startup. See docs/migration.md
# Step 11 notes and cmake/truetest_version.h.in for rationale.
execute_process(
    COMMAND           git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE   TRUETEST_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT TRUETEST_GIT_SHA)
    set(TRUETEST_GIT_SHA "unknown")
endif()

execute_process(
    COMMAND           git status --porcelain
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE   _tt_git_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(_tt_git_status)
    set(TRUETEST_GIT_DIRTY "dirty")
else()
    set(TRUETEST_GIT_DIRTY "clean")
endif()

string(TIMESTAMP TRUETEST_BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/truetest_version.h.in"
    "${CMAKE_BINARY_DIR}/generated/tt/truetest_version.h"
    @ONLY)

# ── Shared engine core (OBJECT library) ─────────────────────────────────────
# Source list lives in cmake/Sources.cmake (ENGINE_CORE_SOURCES).
# This keeps the one obvious registration point for core code.
add_library(engine_core OBJECT ${ENGINE_CORE_SOURCES})
# PIC required when the same objects link into truetest_shared (BUILD_SHARED_LIB).
# Harmless for the three engine executables; avoids R_X86_64_PC32 link failures.
set_target_properties(engine_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(engine_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(engine_core PUBLIC
    CLI11::CLI11 nlohmann_json::nlohmann_json libzstd_static)
tt_apply_common_flags(engine_core)
# Call ONCE here — PUBLIC usage-requirements propagate to everything that
# links engine_core (CMake ≥ 3.12 object-library linking semantics).
tt_wire_optional_backends(engine_core)

# ── Executable factory ──────────────────────────────────────────────────────
# Produces engine_backtest / engine_shadow / engine_live. Each binary owns
# its own translation unit under src/bin/<target>/main.cpp per Step 10's
# directive (prerequisites.md §10 "Binaries live under src/bin/"). The
# per-target main.cpp currently #includes the shared src/bin/main.inc so all
# three compile the same implementation with a distinct TT_TARGET define;
# as deepdive phases land, live-only code migrates out of the shared .inc
# into src/bin/engine_live/ directly, making the wrappers diverge.
function(_tt_add_engine_binary name target_id entry)
    add_executable(${name} ${entry})
    target_compile_definitions(${name} PRIVATE TT_TARGET=${target_id})
    target_link_libraries(${name} PRIVATE engine_core)
    tt_apply_common_flags(${name})
endfunction()

_tt_add_engine_binary(engine_backtest TT_TARGET_BACKTEST src/bin/engine_backtest/main.cpp)
_tt_add_engine_binary(engine_shadow   TT_TARGET_SHADOW   src/bin/engine_shadow/main.cpp)
_tt_add_engine_binary(engine_live     TT_TARGET_LIVE     src/bin/engine_live/main.cpp)

# Rich (ncurses) tabbed dashboard — shadow and live binaries only.
# engine_backtest keeps the existing ANSI ConsoleDashboard.
tt_wire_rich_tui(engine_shadow)
tt_wire_rich_tui(engine_live)

# Native microarchitecture tuning (ENABLE_NATIVE_OPT, default off).
# Applied to all three engines when opted in so Release perf builds are
# consistent; CI/portable builds leave the flag OFF.
tt_apply_live_flags(engine_backtest)
tt_apply_live_flags(engine_shadow)
tt_apply_live_flags(engine_live)

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

    # TEST_SOURCES (base list + conditional appends for QUESTDB/WEB) and the
    # engine.cpp LOC regression guard now live in cmake/Sources.cmake.
    # This is the single obvious place for test registration.
    add_executable(truetest_tests ${TEST_SOURCES})
    target_include_directories(truetest_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(truetest_tests PRIVATE
        TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(truetest_tests PRIVATE engine_core GTest::gtest)
    tt_apply_common_flags(truetest_tests)

    include(GoogleTest)
    gtest_discover_tests(truetest_tests)

    # CLI integration tests — exec engine_backtest / engine_live as subprocesses
    add_executable(truetest_cli_tests tests/test_cli.cpp)
    target_link_libraries(truetest_cli_tests PRIVATE GTest::gtest GTest::gtest_main)
    add_dependencies(truetest_cli_tests engine_backtest engine_live)
    if(ENABLE_QUESTDB)
        target_compile_definitions(truetest_cli_tests PRIVATE HAS_QUESTDB)
    endif()
    gtest_discover_tests(truetest_cli_tests
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
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
install(TARGETS engine_backtest engine_shadow engine_live
    RUNTIME DESTINATION bin)

# ── CPack packaging ─────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME              "truetest")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "TrueTest — modular C++23 backtesting / shadow / live trading engine")
set(CPACK_PACKAGE_CONTACT           "truetest@example.com")
set(CPACK_GENERATOR                 "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "TrueTest Maintainers")
set(CPACK_DEBIAN_PACKAGE_SECTION    "finance")
include(CPack)
