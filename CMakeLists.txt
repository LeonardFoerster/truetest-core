cmake_minimum_required(VERSION 3.22)

# ── Automatic dependency management ──────────────────────────────────────────
# Check for PostgreSQL before project() using pg_config (searches PATH and
# works without a compiler). If absent, bootstrap vcpkg to auto-install libpq.
find_program(_pg_config NAMES pg_config)

if(NOT _pg_config AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    message(STATUS "PostgreSQL not found on system — fetching via vcpkg...")
    include(FetchContent)
    FetchContent_Declare(
        vcpkg
        GIT_REPOSITORY https://github.com/microsoft/vcpkg.git
        GIT_TAG        2024.11.16
        SOURCE_DIR     "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg"
    )
    FetchContent_GetProperties(vcpkg)
    if(NOT vcpkg_POPULATED)
        FetchContent_Populate(vcpkg)
    endif()

    set(_vcpkg_exe "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg/vcpkg")
    if(WIN32)
        string(APPEND _vcpkg_exe ".exe")
    endif()

    if(NOT EXISTS "${_vcpkg_exe}")
        message(STATUS "Bootstrapping vcpkg (one-time)...")
        if(WIN32)
            execute_process(
                COMMAND           "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg/bootstrap-vcpkg.bat" -disableMetrics
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg"
                RESULT_VARIABLE   _vcpkg_result
            )
        else()
            execute_process(
                COMMAND           sh "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg/bootstrap-vcpkg.sh" -disableMetrics
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg"
                RESULT_VARIABLE   _vcpkg_result
            )
        endif()
        if(NOT _vcpkg_result EQUAL 0)
            message(FATAL_ERROR
                "vcpkg bootstrap failed (exit ${_vcpkg_result}).\n"
                "Please install PostgreSQL development libraries manually:\n"
                "  Ubuntu/Debian : sudo apt-get install libpq-dev\n"
                "  Arch Linux    : sudo pacman -S postgresql-libs\n"
                "  macOS         : brew install postgresql\n"
                "  Windows       : https://www.postgresql.org/download/")
        endif()
    endif()

    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/_vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain" FORCE)
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
endif()
# ─────────────────────────────────────────────────────────────────────────────

project(hft-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    BacktestEngine/src/main.cpp
    BacktestEngine/src/core/backtest_core.cpp
    BacktestEngine/src/data/csv_data_loader.cpp
    BacktestEngine/src/data/database_connection.cpp
    BacktestEngine/src/data/data_loader.cpp
    BacktestEngine/src/execution/portfolio.cpp
    BacktestEngine/src/orderbook/orderbook.cpp
    BacktestEngine/src/pricing/black_scholes_formula.cpp
    BacktestEngine/src/pricing/perform_calculations.cpp
    BacktestEngine/src/strategy/mean_reversion_strategy.cpp
    BacktestEngine/src/strategy/sma_strategy.cpp
    BacktestEngine/src/strategy/sma.cpp
    BacktestEngine/src/market_maker/market_maker.cpp
    BacktestEngine/src/utils/log/log.cpp
)
add_executable(hft-engine ${SOURCES})
target_include_directories(hft-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/BacktestEngine/src")

find_package(PostgreSQL REQUIRED)

include(FetchContent)
FetchContent_Declare(
    libpqxx
    GIT_REPOSITORY https://github.com/jtv/libpqxx.git
    GIT_TAG        7.9.2
)
set(SKIP_BUILD_AUDIT ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libpqxx)

target_include_directories(hft-engine PRIVATE ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(hft-engine PRIVATE
    pqxx
    ${PostgreSQL_LIBRARIES}
)

if(WIN32)
    target_link_libraries(hft-engine PRIVATE ws2_32 secur32)
endif()

if(MSVC)
    target_compile_options(hft-engine PRIVATE /W4)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(hft-engine PRIVATE /D_ITERATOR_DEBUG_LEVEL=2)
    endif()
else()
    target_compile_options(hft-engine PRIVATE -Wall -Wextra)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(hft-engine PRIVATE _GLIBCXX_DEBUG)
        endif()
    endif()
endif()
