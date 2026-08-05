# cmake/Dependencies.cmake
#
# Single source of truth for third-party dependencies. Invokes FetchContent or
# find_package depending on the dep, and pins every version. Called once from
# the root CMakeLists.txt via tt_fetch_dependencies().
#
# The rule, per prerequisites.md §11:
#   * Small header/source-only libs  → FetchContent with pinned tag
#   * Large system libs (Boost, OpenSSL) → find_package
#
# Public surface:
#   tt_fetch_dependencies()          # core deps (always fetched)
#   tt_fetch_tests_dependencies()    # GoogleTest
#   tt_fetch_bench_dependencies()    # Google Benchmark
#   tt_wire_optional_backends(target)   # optional ENABLE_* deps per target
#   tt_wire_rich_tui(target)            # ncurses-backed tabbed dashboard
#                                       # (call on engine_shadow / engine_live)
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

# ── tt_wire_optional_backends(target) ───────────────────────────────────────
# Call this ONCE on engine_core (the OBJECT library). PUBLIC usage-requirements
# propagate to every executable / test / benchmark that links engine_core.
function(tt_wire_optional_backends target)
    set(_src "${CMAKE_SOURCE_DIR}/src")

    # HAS_LIVE_DATA remains available for venue live transports under providers/.
    # Generic WebSocketDataSource was removed (docs/internal/data-pipeline.md#D-07 — unwired dead end).
    # Boost.System is header-only in modern Boost (no boost_system package / Boost::system
    # target on 1.89+); use Boost::headers like the venue providers below.
    if(ENABLE_LIVE_DATA)
        find_package(Boost CONFIG REQUIRED)
        target_link_libraries(${target} PUBLIC Boost::headers)
        target_compile_definitions(${target} PUBLIC HAS_LIVE_DATA)
    endif()

    # Binance exchange provider
    if(ENABLE_BINANCE)
        find_package(Boost CONFIG REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_sources(${target} PRIVATE
            ${_src}/providers/binance/binance_register.cpp
            ${_src}/providers/binance/binance_futures_register.cpp)
        # Note: binance_backfill.h is a header-only include (not a source)
        target_link_libraries(${target} PUBLIC
            Boost::headers OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(${target} PUBLIC HAS_BINANCE)
    endif()

    # Bitget UTA USDT-M futures provider
    if(ENABLE_BITGET)
        find_package(Boost CONFIG REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_sources(${target} PRIVATE
            ${_src}/providers/bitget/bitget_futures_register.cpp)
        target_link_libraries(${target} PUBLIC
            Boost::headers OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(${target} PUBLIC HAS_BITGET)
    endif()

    # Bitunix futures provider (Phase 0–1 MD/shadow)
    if(ENABLE_BITUNIX)
        find_package(Boost CONFIG REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_sources(${target} PRIVATE
            ${_src}/providers/bitunix/bitunix_futures_register.cpp)
        target_link_libraries(${target} PUBLIC
            Boost::headers OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(${target} PUBLIC HAS_BITUNIX)
    endif()

    # QuestDB persistence (raw POSIX sockets, zero external deps).
    if(ENABLE_QUESTDB)
        target_sources(${target} PRIVATE
            ${_src}/data/questdb/tcp_client.cpp
            ${_src}/data/questdb/http_client.cpp
            ${_src}/data/questdb/ilp_writer.cpp
            ${_src}/data/questdb/schema.cpp
            ${_src}/data/questdb/run_tag.cpp
            ${_src}/data/questdb/store.cpp)
        target_compile_definitions(${target} PUBLIC HAS_QUESTDB)
    endif()

    # Embedded web UI server (civetweb — small C HTTP+WS server, no Boost).
    # Off the hot path: the snapshot/report serializers may use whatever they
    # like and the WS server runs on its own thread, polling the same
    # snapshot_dashboard() seam the ncurses TUI uses.
    if(ENABLE_WEB)
        if(NOT TARGET civetweb-c-library)
            FetchContent_Declare(
                civetweb
                GIT_REPOSITORY https://github.com/civetweb/civetweb.git
                GIT_TAG        v1.16
            )
            set(CIVETWEB_ENABLE_WEBSOCKETS        ON  CACHE BOOL "" FORCE)
            set(CIVETWEB_ENABLE_SSL               OFF CACHE BOOL "" FORCE)  # localhost; TLS via reverse proxy
            set(CIVETWEB_ENABLE_CXX               OFF CACHE BOOL "" FORCE)  # we use the C API directly
            set(CIVETWEB_BUILD_TESTING            OFF CACHE BOOL "" FORCE)
            set(CIVETWEB_ENABLE_SERVER_EXECUTABLE OFF CACHE BOOL "" FORCE)
            set(CIVETWEB_INSTALL_EXECUTABLE       OFF CACHE BOOL "" FORCE)
            set(CIVETWEB_ENABLE_ASAN              OFF CACHE BOOL "" FORCE)
            # civetweb v1.16 declares cmake_minimum_required < 3.5, which CMake 4
            # rejects. Scope the compatibility shim to this subproject only.
            set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
            FetchContent_MakeAvailable(civetweb)
        endif()
        target_sources(${target} PRIVATE
            ${_src}/web/snapshot_json.cpp
            ${_src}/web/report_json.cpp
            ${_src}/web/web_server.cpp)
        target_link_libraries(${target} PUBLIC civetweb-c-library)
        target_compile_definitions(${target} PUBLIC HAS_WEB)
    endif()

    # Rich (ncurses) TUI dashboard for shadow/live binaries. Wired here for
    # consistency with the optional-backend pattern, but the tt_wire_rich_tui
    # function below is the actual entry point — engine_core is target-agnostic
    # and must not link Curses (engine_backtest binary doesn't ship the rich TUI).

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
            ${_src}/debug/hardware_info.cpp
            ${_src}/debug/stage_timer.cpp
            ${_src}/debug/memory_info.cpp
            ${_src}/debug/debug_report.cpp)
        target_link_libraries(${target} PUBLIC
            absl::log absl::log_initialize absl::log_severity
            absl::log_sink absl::log_sink_registry
            absl::flags absl::flags_parse absl::strings absl::str_format)
        target_compile_definitions(${target} PUBLIC HAS_DEBUG)
    endif()
endfunction()

# ── tt_wire_rich_tui(target) ────────────────────────────────────────────────
# Adds the ncurses-backed tabbed dashboard sources and links Curses.
# Call ONLY on engine_shadow and engine_live; engine_backtest must not link
# Curses, so the rich-TUI sources are added per-binary, not into engine_core.
function(tt_wire_rich_tui target)
    set(CURSES_NEED_NCURSES TRUE)
    set(CURSES_NEED_WIDE    TRUE)
    find_package(Curses REQUIRED)

    set(_src "${CMAKE_SOURCE_DIR}/src")
    target_sources(${target} PRIVATE
        ${_src}/ui/tabbed_dashboard.cpp
        ${_src}/ui/tui_style.cpp
        ${_src}/ui/tui_prefs.cpp
        ${_src}/ui/toast.cpp
        ${_src}/ui/overlays.cpp
        ${_src}/ui/panels/overview_panel.cpp
        ${_src}/ui/panels/positions_panel.cpp
        ${_src}/ui/panels/orders_panel.cpp
        ${_src}/ui/panels/risk_panel.cpp
        ${_src}/ui/panels/brackets_panel.cpp
        ${_src}/ui/panels/strategy_panel.cpp
        ${_src}/ui/panels/health_panel.cpp
        ${_src}/ui/panels/debug_panel.cpp
        ${_src}/ui/panels/l2_panel.cpp)
    target_include_directories(${target} PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ${CURSES_LIBRARIES})
    target_compile_definitions(${target} PRIVATE HAS_RICH_TUI)
endfunction()
