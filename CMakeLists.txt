cmake_minimum_required(VERSION 3.22)

project(truetest LANGUAGES CXX VERSION 0.1.0)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Build options ───────────────────────────────────────────────────────────
option(ENABLE_LIVE_DATA  "Build with WebSocket live data feed (requires Boost.Beast)" OFF)
option(ENABLE_DEBUG      "Build with performance instrumentation (requires Abseil)"   OFF)
option(ENABLE_BINANCE    "Build with Binance exchange provider"                       OFF)
option(ENABLE_WEB        "Build with embedded web UI server (civetweb)"                 OFF)
option(ENABLE_QUESTDB    "Build with QuestDB persistence backend"                     OFF)
option(ENABLE_TSAN       "Enable ThreadSanitizer"                                     OFF)
option(ENABLE_ASAN       "Enable AddressSanitizer (mutually exclusive with TSAN)"     OFF)
option(ENABLE_UBSAN      "Enable UndefinedBehaviorSanitizer (mutex with TSAN)"        OFF)
option(ENABLE_NATIVE_OPT "Enable -march=native + unroll/omit-fp on engine_live only"  OFF)
option(BUILD_SHARED_LIB  "Build libtruetest shared library with C API for embedding"  OFF)
option(BUILD_TESTS       "Build unit tests"                                           OFF)
option(ENABLE_BENCHMARKS "Build performance benchmarks (Google Benchmark)"            OFF)

include(CompilerFlags)   # sets C++23, opt profiles, provides tt_apply_* helpers
include(Dependencies)    # provides tt_fetch_* and tt_wire_optional_backends

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
# Every source file that is identical across the three binaries lives here.
# main.cpp is intentionally NOT part of engine_core — each executable owns
# its own copy, compiled with a distinct TT_TARGET define.
set(ENGINE_CORE_SOURCES
    src/engine/engine.cpp
    src/analytics/adverse_selection_tracker.cpp
    src/analytics/analytics.cpp
    src/analytics/ascii_widgets.cpp
    src/analytics/report_generator.cpp
    src/data/data_loader.cpp
    src/data/csv_data_source.cpp
    src/data/binary_cache_source.cpp
    src/data/tick_csv_data_source.cpp
    src/execution/portfolio.cpp
    src/orderbook/orderbook.cpp
    src/strategy/mean_reversion_strategy.cpp
    src/strategy/sma_strategy.cpp
    src/strategy/ma_crossover_strategy.cpp
    src/strategy/hedge_demo_strategy.cpp
    src/strategy/breakout_strategy.cpp
    src/strategy/larry_connor_strategy.cpp
    src/strategy/coiled_spring_strategy.cpp
    src/strategy/sma.cpp
    src/strategy/adaptive_hybrid_config.cpp
    src/strategy/adaptive_hybrid_strategy.cpp
    src/strategy/structure_continuation_strategy.cpp
    src/market_maker/market_maker.cpp
    src/risk/risk_manager.cpp
    src/risk/maintenance_margin_table.cpp
    src/providers/local/local_register.cpp
    src/ui/console_dashboard.cpp
    src/exits/exit_manager.cpp

    # Monte Carlo / synthetic simulation (integrated from monte-carlo branch)
    src/simulation/imonte_carlo_generator.cpp
    src/simulation/monte_carlo_controller.cpp
    src/simulation/monte_carlo_reporter.cpp
    src/simulation/generators/gbm_generator.cpp
    src/providers/synthetic/synthetic_transport.cpp
    src/providers/synthetic/synthetic_provider.cpp
    src/providers/synthetic/synthetic_register.cpp
)

add_library(engine_core OBJECT ${ENGINE_CORE_SOURCES})
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

# Native microarchitecture tuning. Gated on ENABLE_NATIVE_OPT (default off);
# when the user opts in they want it across the board, not just on live.
# CI and portable builds keep ENABLE_NATIVE_OPT=OFF and are unaffected.
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

    set(TEST_SOURCES
        tests/test_main.cpp
        tests/test_adverse_selection.cpp
        tests/test_cancel_race.cpp
        tests/test_queue_model.cpp
        tests/test_queue_aware_adapter.cpp
        tests/test_impact_model.cpp
        tests/test_events.cpp
        tests/test_orderbook.cpp
        tests/test_portfolio.cpp
        tests/test_fee_model.cpp
        tests/test_latency_model.cpp
        tests/test_fill_model.cpp
        tests/test_sma.cpp
        tests/test_ema.cpp
        tests/test_rsi.cpp
        tests/test_stochastic.cpp
        tests/test_swing_detector.cpp
        tests/test_ema_regime.cpp
        tests/test_bollinger.cpp
        tests/test_strategies.cpp
        tests/test_adaptive_hybrid.cpp
        tests/test_monte_carlo_generators.cpp
        tests/test_monte_carlo_controller.cpp
        tests/test_ring_buffer.cpp
        tests/test_bar_aggregator.cpp
        tests/test_analytics.cpp
        tests/test_report_generator.cpp
        tests/test_market_maker.cpp
        tests/test_data_handler.cpp
        tests/test_date_parse.cpp
        tests/test_trade_tape_shadow_adapter.cpp
        tests/test_exit_manager.cpp
        tests/test_engine_brackets.cpp
        tests/test_bracket_adapter.cpp
        tests/test_binance_oco_bracket_adapter.cpp
        tests/test_bridge_unknown_fill.cpp
        tests/test_hybrid_executor.cpp
        tests/test_binance_combined_parser.cpp
        tests/test_provider_event_stream_contract.cpp
        tests/test_engine_l2_ingestion.cpp
        tests/test_execution_adapter.cpp
        tests/test_realistic_fills.cpp
        tests/test_queue_position_model.cpp
        tests/test_trade_tape_shadow_queue.cpp
        tests/test_walked_book_impact.cpp
        tests/test_execution_bridge.cpp
        tests/test_engine.cpp
        tests/test_engine_lookahead.cpp
        tests/test_engine_instrument_spec.cpp
        tests/test_instrument.cpp
        tests/test_rate_limiter.cpp
        tests/test_client_order_id.cpp
        tests/test_live_safety.cpp
        tests/test_order_id.cpp
        tests/test_price.cpp
        tests/test_orderbook_registry.cpp
        tests/test_order_types.cpp
        tests/test_object_pool.cpp
        tests/helpers/alloc_counter.cpp
        tests/test_hotpath_allocs.cpp
        tests/test_hotpath_alloc_matrix.cpp
        tests/test_hotpath_pool_prewarm.cpp
        tests/test_control_block_pool.cpp
        tests/test_symbol_table.cpp
        tests/test_orderbook_order_pool.cpp
        tests/test_deferred_return_queue.cpp
        tests/test_risk_manager.cpp
        tests/test_event_log.cpp
        tests/test_thread_preset.cpp
        tests/test_threading_correctness.cpp
        tests/test_engine_streaming.cpp
        tests/test_engine_integration.cpp
        tests/test_golden_regression.cpp
        tests/test_binance_parser.cpp
        tests/test_binance_order_encoder.cpp
        tests/test_binance_rest_order_transport.cpp
        tests/test_binance_user_data_parser.cpp
        tests/test_binance_user_data_transport.cpp
        tests/test_binance_rest_client_time.cpp
        tests/test_binance_rest_client_rate_limit.cpp
        tests/test_binance_user_data_transport_keepalive.cpp
        tests/test_binance_user_data_transport_reconnect.cpp
        tests/test_binance_endpoints.cpp
        tests/test_binance_testnet_live.cpp
        tests/test_binance_futures_order_encoder.cpp
        tests/test_binance_futures_user_data_parser.cpp
        tests/test_binance_futures_register.cpp
        tests/test_binance_futures_reconciler.cpp
        tests/test_binance_futures_kill_switch.cpp
        tests/test_binance_futures_bracket_adapter.cpp
        tests/test_binance_futures_testnet_live.cpp
        tests/test_binance_futures_safety.cpp
        tests/test_futures_risk_check.cpp
        tests/test_engine_venue_risk_check.cpp
        tests/test_worker_watchdog.cpp
        tests/test_binance_futures_dead_mans_switch.cpp
        tests/test_provider_transport.cpp
        tests/test_data_bridge.cpp
        tests/test_provider_event.cpp
        tests/test_provider_registry.cpp
        tests/test_provider_engine_wiring.cpp
    )

    if(ENABLE_QUESTDB)
        list(APPEND TEST_SOURCES
            tests/test_questdb_http_client.cpp
            tests/test_questdb_ilp_writer.cpp
            tests/test_questdb_schema.cpp
            tests/test_questdb_run_tag.cpp
            tests/test_questdb_store.cpp
            tests/test_questdb_integration.cpp)
    endif()

    add_executable(truetest_tests ${TEST_SOURCES})
    target_include_directories(truetest_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(truetest_tests PRIVATE
        TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(truetest_tests PRIVATE engine_core GTest::gtest)
    tt_apply_common_flags(truetest_tests)

    include(GoogleTest)
    gtest_discover_tests(truetest_tests)

    # CLI integration tests — exec engine_backtest as subprocess
    add_executable(truetest_cli_tests tests/test_cli.cpp)
    target_link_libraries(truetest_cli_tests PRIVATE GTest::gtest GTest::gtest_main)
    add_dependencies(truetest_cli_tests engine_backtest)
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
