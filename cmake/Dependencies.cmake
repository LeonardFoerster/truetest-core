# cmake/Dependencies.cmake
#
# Single source of truth for third-party dependencies. Invokes FetchContent or
# find_package depending on the dep, and pins every version. Called once from
# the root CMakeLists.txt via tt_fetch_dependencies().
#
# The rule, per prerequisites.md §11:
#   * Small header/source-only libs  → FetchContent with pinned tag
#   * Large system libs (Boost, OpenSSL, PostgreSQL, SQLite) → find_package
#
# Public surface:
#   tt_fetch_dependencies()          # core deps (always fetched)
#   tt_fetch_tests_dependencies()    # GoogleTest
#   tt_fetch_bench_dependencies()    # Google Benchmark
#   tt_wire_optional_backends(target)   # optional ENABLE_* deps per target
#
# Why PUBLIC scope in tt_wire_optional_backends:
#   It is called exactly once — on `engine_core` (an OBJECT library). Every
#   executable / test / benchmark links `engine_core` via target_link_libraries,
#   which (CMake >= 3.12) propagates both the objects and PUBLIC usage
#   requirements (link libs, compile definitions, include dirs). This way
#   optional-backend sources are compiled once and the HAS_* macros are visible
#   to main.cpp (which is compiled into each executable separately).
# ─────────────────────────────────────────────────────────────────────────────

if(DEFINED _TT_DEPENDENCIES_INCLUDED)
    return()
endif()
set(_TT_DEPENDENCIES_INCLUDED TRUE)

include(FetchContent)

# ── tt_fetch_dependencies() ─────────────────────────────────────────────────
function(tt_fetch_dependencies)
    # CLI11 — header-only argument parser
    FetchContent_Declare(
        cli11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG        v2.4.2
    )
    FetchContent_MakeAvailable(cli11)

    # zstd — binary event-log compression
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.6
        SOURCE_SUBDIR  build/cmake
    )
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)

    # nlohmann/json — config-file parsing (NOT hot-path JSON)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endfunction()

# ── tt_fetch_tests_dependencies() ───────────────────────────────────────────
function(tt_fetch_tests_dependencies)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endfunction()

# ── tt_fetch_bench_dependencies() ───────────────────────────────────────────
function(tt_fetch_bench_dependencies)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.5
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endfunction()

# ── PostgreSQL helper (fetches vcpkg on platforms missing pg_config) ────────
function(_tt_ensure_vcpkg_for_postgres)
    find_program(_pg_config NAMES pg_config)
    if(_pg_config OR DEFINED CMAKE_TOOLCHAIN_FILE)
        return()
    endif()

    message(STATUS "pg_config not found — fetching vcpkg for PostgreSQL...")
    set(_vcpkg_dir "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg")

    if(NOT EXISTS "${_vcpkg_dir}/.git")
        execute_process(
            COMMAND git clone --depth 1 --branch 2025.04.09
                    https://github.com/microsoft/vcpkg.git "${_vcpkg_dir}"
            RESULT_VARIABLE _git_result)
        if(NOT _git_result EQUAL 0)
            message(FATAL_ERROR
                "vcpkg clone failed (${_git_result}). Install libpq manually.")
        endif()
    endif()

    set(_vcpkg_exe "${_vcpkg_dir}/vcpkg")
    if(WIN32)
        string(APPEND _vcpkg_exe ".exe")
    endif()

    if(NOT EXISTS "${_vcpkg_exe}")
        if(WIN32)
            execute_process(
                COMMAND           "${_vcpkg_dir}/bootstrap-vcpkg.bat" -disableMetrics
                WORKING_DIRECTORY "${_vcpkg_dir}"
                RESULT_VARIABLE   _vcpkg_result)
        else()
            execute_process(
                COMMAND           sh "${_vcpkg_dir}/bootstrap-vcpkg.sh" -disableMetrics
                WORKING_DIRECTORY "${_vcpkg_dir}"
                RESULT_VARIABLE   _vcpkg_result)
        endif()
        if(NOT _vcpkg_result EQUAL 0)
            message(FATAL_ERROR
                "vcpkg bootstrap failed (${_vcpkg_result}). "
                "Install libpq-dev / postgresql-libs manually.")
        endif()
    endif()

    set(CMAKE_TOOLCHAIN_FILE
        "${_vcpkg_dir}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain" FORCE)
    message(STATUS "vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endfunction()

# ── tt_wire_optional_backends(target) ───────────────────────────────────────
# Call this ONCE on engine_core (the OBJECT library). PUBLIC usage-requirements
# propagate to every executable / test / benchmark that links engine_core.
function(tt_wire_optional_backends target)
    # PostgreSQL backend
    if(ENABLE_POSTGRESQL)
        _tt_ensure_vcpkg_for_postgres()
        find_package(PostgreSQL REQUIRED)

        if(NOT TARGET pqxx)
            FetchContent_Declare(
                libpqxx
                GIT_REPOSITORY https://github.com/jtv/libpqxx.git
                GIT_TAG        7.9.2
            )
            set(SKIP_BUILD_AUDIT ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(libpqxx)
        endif()

        target_sources(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/data/pg_data_source.cpp)
        target_include_directories(${target} PUBLIC ${PostgreSQL_INCLUDE_DIRS})
        target_link_libraries(${target} PUBLIC pqxx ${PostgreSQL_LIBRARIES})
        target_compile_definitions(${target} PUBLIC HAS_POSTGRESQL)
        if(WIN32)
            target_link_libraries(${target} PUBLIC ws2_32 secur32)
        endif()
    endif()

    # Generic WebSocket data feed
    if(ENABLE_LIVE_DATA)
        find_package(Boost REQUIRED COMPONENTS system)
        target_sources(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/data/websocket_data_source.cpp)
        target_link_libraries(${target} PUBLIC Boost::system)
        target_compile_definitions(${target} PUBLIC HAS_LIVE_DATA)
    endif()

    # WebSocket UI
    if(ENABLE_WEB_UI)
        find_package(Boost REQUIRED)
        target_link_libraries(${target} PUBLIC Boost::headers)
        target_compile_definitions(${target} PUBLIC HAS_WEB_UI)
    endif()

    # Binance exchange provider
    if(ENABLE_BINANCE)
        find_package(Boost REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_sources(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/providers/binance/binance_register.cpp
            ${CMAKE_SOURCE_DIR}/src/providers/binance/binance_backfill.h)
        target_link_libraries(${target} PUBLIC
            Boost::headers OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(${target} PUBLIC HAS_BINANCE)
    endif()

    # SQLite persistence
    if(ENABLE_SQLITE)
        find_package(SQLite3 REQUIRED)
        target_sources(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/data/sqlite_store.cpp)
        target_link_libraries(${target} PUBLIC SQLite3::SQLite3)
        target_compile_definitions(${target} PUBLIC HAS_SQLITE)
    endif()

    # Debug instrumentation (Abseil)
    if(ENABLE_DEBUG)
        if(NOT TARGET absl::log)
            FetchContent_Declare(
                abseil-cpp
                GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
                GIT_TAG        20240722.0
            )
            set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
            set(BUILD_TESTING          OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(abseil-cpp)
        endif()

        target_sources(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/debug/hardware_info.cpp
            ${CMAKE_SOURCE_DIR}/src/debug/stage_timer.cpp
            ${CMAKE_SOURCE_DIR}/src/debug/memory_info.cpp
            ${CMAKE_SOURCE_DIR}/src/debug/debug_report.cpp)
        target_link_libraries(${target} PUBLIC
            absl::log absl::log_initialize absl::log_severity
            absl::log_sink absl::log_sink_registry
            absl::flags absl::flags_parse absl::strings absl::str_format)
        target_compile_definitions(${target} PUBLIC HAS_DEBUG)
    endif()
endfunction()
