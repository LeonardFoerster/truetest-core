cmake_minimum_required(VERSION 3.22)

project(truetest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_POSTGRESQL "Build with PostgreSQL storage backend" OFF)
option(ENABLE_LIVE_DATA "Build with WebSocket live data feed (requires Boost.Beast)" OFF)
option(ENABLE_DEBUG "Build with performance instrumentation (requires Abseil)" OFF)
option(ENABLE_WEB_UI "Build with WebSocket UI server (requires Boost.Beast)" OFF)
option(ENABLE_BINANCE "Build with Binance exchange provider" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# ── Core engine sources (no external dependencies) ────────────────────────────
set(SOURCES
    BacktestEngine/src/main.cpp
    BacktestEngine/src/core/engine.cpp
    BacktestEngine/src/analytics/analytics.cpp
    BacktestEngine/src/data/data_loader.cpp
    BacktestEngine/src/data/csv_data_source.cpp
    BacktestEngine/src/data/binary_cache_source.cpp
    BacktestEngine/src/data/tick_csv_data_source.cpp
    BacktestEngine/src/execution/portfolio.cpp
    BacktestEngine/src/orderbook/orderbook.cpp
    BacktestEngine/src/strategy/mean_reversion_strategy.cpp
    BacktestEngine/src/strategy/sma_strategy.cpp
    BacktestEngine/src/strategy/ma_crossover_strategy.cpp
    BacktestEngine/src/strategy/sma.cpp
    BacktestEngine/src/market_maker/market_maker.cpp
    BacktestEngine/src/risk/risk_manager.cpp
    BacktestEngine/src/providers/local/local_register.cpp
    BacktestEngine/src/utils/log/log.cpp
)

add_executable(truetest ${SOURCES})
target_include_directories(truetest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/BacktestEngine/src")

# ── Optional PostgreSQL backend ───────────────────────────────────────────────
if(ENABLE_POSTGRESQL)
    # Auto-fetch vcpkg when pg_config is not found on the system
    find_program(_pg_config NAMES pg_config)

    if(NOT _pg_config AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        message(STATUS "pg_config not found — fetching vcpkg to provide PostgreSQL...")
        set(_vcpkg_dir "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg")

        if(NOT EXISTS "${_vcpkg_dir}/.git")
            message(STATUS "Cloning vcpkg 2024.11.16...")
            execute_process(
                COMMAND git clone --depth 1 --branch 2024.11.16
                        https://github.com/microsoft/vcpkg.git
                        "${_vcpkg_dir}"
                RESULT_VARIABLE _git_result
            )
            if(NOT _git_result EQUAL 0)
                message(FATAL_ERROR "Failed to clone vcpkg (exit ${_git_result}). Ensure git is on PATH.")
            endif()
        endif()

        set(_vcpkg_exe "${_vcpkg_dir}/vcpkg")
        if(WIN32)
            string(APPEND _vcpkg_exe ".exe")
        endif()

        if(NOT EXISTS "${_vcpkg_exe}")
            message(STATUS "Bootstrapping vcpkg (one-time setup)...")
            if(WIN32)
                execute_process(
                    COMMAND           "${_vcpkg_dir}/bootstrap-vcpkg.bat" -disableMetrics
                    WORKING_DIRECTORY "${_vcpkg_dir}"
                    RESULT_VARIABLE   _vcpkg_result
                )
            else()
                execute_process(
                    COMMAND           sh "${_vcpkg_dir}/bootstrap-vcpkg.sh" -disableMetrics
                    WORKING_DIRECTORY "${_vcpkg_dir}"
                    RESULT_VARIABLE   _vcpkg_result
                )
            endif()
            if(NOT _vcpkg_result EQUAL 0)
                message(FATAL_ERROR
                    "vcpkg bootstrap failed (exit ${_vcpkg_result}).\n"
                    "As an alternative, install PostgreSQL dev libraries manually:\n"
                    "  Ubuntu/Debian : sudo apt-get install libpq-dev\n"
                    "  Arch Linux    : sudo pacman -S postgresql-libs\n"
                    "  macOS         : brew install postgresql\n"
                    "  Windows       : https://www.postgresql.org/download/")
            endif()
        endif()

        set(CMAKE_TOOLCHAIN_FILE
            "${_vcpkg_dir}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain" FORCE)

        if(WIN32)
            find_program(_ninja ninja)
            if(_ninja)
                set(ENV{VCPKG_CMAKE_GENERATOR} "Ninja")
            else()
                find_program(_nmake nmake)
                if(NOT _nmake)
                    message(WARNING
                        "Neither ninja nor nmake found on PATH.\n"
                        "vcpkg package builds will likely fail.\n"
                        "Fix: open an 'x64 Native Tools Command Prompt for VS' before running CMake,\n"
                        "     OR install Ninja: https://ninja-build.org/")
                endif()
            endif()
        endif()

        message(STATUS "vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
    endif()

    find_package(PostgreSQL REQUIRED)

    include(FetchContent)
    FetchContent_Declare(
        libpqxx
        GIT_REPOSITORY https://github.com/jtv/libpqxx.git
        GIT_TAG        7.9.2
    )
    set(SKIP_BUILD_AUDIT ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libpqxx)

    target_sources(truetest PRIVATE BacktestEngine/src/data/pg_data_source.cpp)
    target_include_directories(truetest PRIVATE ${PostgreSQL_INCLUDE_DIRS})
    target_link_libraries(truetest PRIVATE pqxx ${PostgreSQL_LIBRARIES})
    target_compile_definitions(truetest PRIVATE HAS_POSTGRESQL)

    if(WIN32)
        target_link_libraries(truetest PRIVATE ws2_32 secur32)
    endif()
endif()

# ── Optional live data feed (Boost.Beast WebSocket) ──────────────────────────
if(ENABLE_LIVE_DATA)
    find_package(Boost REQUIRED COMPONENTS system)

    target_sources(truetest PRIVATE BacktestEngine/src/data/websocket_data_source.cpp)
    target_link_libraries(truetest PRIVATE Boost::system)
    target_compile_definitions(truetest PRIVATE HAS_LIVE_DATA)
endif()

# ── Optional WebSocket UI server (Boost.Beast) ───────────────────────────────
if(ENABLE_WEB_UI)
    find_package(Boost REQUIRED)
    target_link_libraries(truetest PRIVATE Boost::headers)
    target_compile_definitions(truetest PRIVATE HAS_WEB_UI)
endif()

# ── Optional Binance exchange provider ────────────────────────────────────────
if(ENABLE_BINANCE)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)

    target_sources(truetest PRIVATE
        BacktestEngine/src/providers/binance/binance_register.cpp
    )
    target_link_libraries(truetest PRIVATE Boost::headers OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(truetest PRIVATE HAS_BINANCE)
endif()

# ── Optional debug instrumentation (Abseil) ──────────────────────────────────
if(ENABLE_DEBUG)
    include(FetchContent)
    FetchContent_Declare(
        abseil-cpp
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG        20240722.0
    )
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(abseil-cpp)

    target_sources(truetest PRIVATE
        BacktestEngine/src/debug/hardware_info.cpp
        BacktestEngine/src/debug/stage_timer.cpp
        BacktestEngine/src/debug/memory_info.cpp
        BacktestEngine/src/debug/debug_report.cpp
    )

    target_link_libraries(truetest PRIVATE
        absl::log
        absl::log_initialize
        absl::log_severity
        absl::log_sink
        absl::log_sink_registry
        absl::flags
        absl::flags_parse
        absl::strings
        absl::str_format
    )

    target_compile_definitions(truetest PRIVATE HAS_DEBUG)
endif()
# ──────────────────────────────────────────────────────────────────────────────

# ── Unit Tests (GoogleTest) ──────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    set(TEST_SOURCES
        tests/test_main.cpp
        tests/test_events.cpp
        tests/test_orderbook.cpp
        tests/test_portfolio.cpp
        tests/test_fee_model.cpp
        tests/test_latency_model.cpp
        tests/test_fill_model.cpp
        tests/test_sma.cpp
        tests/test_strategies.cpp
        tests/test_ring_buffer.cpp
        tests/test_bar_aggregator.cpp
        tests/test_analytics.cpp
        tests/test_market_maker.cpp
        tests/test_data_handler.cpp
        tests/test_execution_adapter.cpp
        tests/test_engine.cpp
        tests/test_order_id.cpp
        tests/test_price.cpp
        tests/test_orderbook_registry.cpp
        tests/test_order_types.cpp
        tests/test_object_pool.cpp
        tests/test_risk_manager.cpp
        tests/test_event_log.cpp
        tests/test_thread_preset.cpp
        tests/test_threading_correctness.cpp
        tests/test_engine_streaming.cpp
        tests/test_event_json.cpp
        tests/test_binance_parser.cpp
        tests/test_provider_transport.cpp
        tests/test_data_bridge.cpp
        tests/test_provider_event.cpp
        tests/test_provider_registry.cpp
        # Production sources (no main.cpp)
        BacktestEngine/src/core/engine.cpp
        BacktestEngine/src/analytics/analytics.cpp
        BacktestEngine/src/data/data_loader.cpp
        BacktestEngine/src/data/csv_data_source.cpp
        BacktestEngine/src/data/binary_cache_source.cpp
        BacktestEngine/src/data/tick_csv_data_source.cpp
        BacktestEngine/src/execution/portfolio.cpp
        BacktestEngine/src/orderbook/orderbook.cpp
        BacktestEngine/src/strategy/mean_reversion_strategy.cpp
        BacktestEngine/src/strategy/sma_strategy.cpp
        BacktestEngine/src/strategy/ma_crossover_strategy.cpp
        BacktestEngine/src/strategy/sma.cpp
        BacktestEngine/src/market_maker/market_maker.cpp
        BacktestEngine/src/risk/risk_manager.cpp
        BacktestEngine/src/providers/local/local_register.cpp
        BacktestEngine/src/utils/log/log.cpp
    )

    add_executable(truetest_tests ${TEST_SOURCES})
    target_include_directories(truetest_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/BacktestEngine/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_compile_definitions(truetest_tests PRIVATE
        TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_link_libraries(truetest_tests PRIVATE GTest::gtest)

    if(NOT MSVC)
        target_compile_options(truetest_tests PRIVATE -Wall -Wextra)
    endif()

    if(ENABLE_DEBUG)
        target_sources(truetest_tests PRIVATE
            BacktestEngine/src/debug/hardware_info.cpp
            BacktestEngine/src/debug/stage_timer.cpp
            BacktestEngine/src/debug/memory_info.cpp
            BacktestEngine/src/debug/debug_report.cpp
        )
        target_link_libraries(truetest_tests PRIVATE
            absl::log absl::log_initialize absl::log_severity
            absl::log_sink absl::log_sink_registry
            absl::strings absl::str_format
        )
        target_compile_definitions(truetest_tests PRIVATE HAS_DEBUG)
    endif()

    include(GoogleTest)
    gtest_discover_tests(truetest_tests)
endif()
# ──────────────────────────-───────────────────────────────────────────────────

# ── ThreadSanitizer ──────────────────────────────────────────────────────────
if(ENABLE_TSAN AND NOT MSVC)
    target_compile_options(truetest PRIVATE -fsanitize=thread)
    target_link_options(truetest PRIVATE -fsanitize=thread)
    if(BUILD_TESTS)
        target_compile_options(truetest_tests PRIVATE -fsanitize=thread)
        target_link_options(truetest_tests PRIVATE -fsanitize=thread)
    endif()
endif()

if(MSVC)
    target_compile_options(truetest PRIVATE /W4)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(truetest PRIVATE /D_ITERATOR_DEBUG_LEVEL=2)
    endif()
else()
    target_compile_options(truetest PRIVATE -Wall -Wextra)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(truetest PRIVATE _GLIBCXX_DEBUG)
        endif()
    endif()
endif()
