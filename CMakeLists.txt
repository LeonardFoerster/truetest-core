cmake_minimum_required(VERSION 3.22)

# Bootstrap vcpkg
include(FetchContent)
FetchContent_Declare(
  vcpkg
  GIT_REPOSITORY https://github.com/microsoft/vcpkg.git
  GIT_TAG vcpkg-2024.07.12 # A recent, stable tag
)
FetchContent_MakeAvailable(vcpkg)
set(CMAKE_TOOLCHAIN_FILE ${vcpkg_SOURCE_DIR}/scripts/buildsystems/vcpkg.cmake CACHE STRING "")

project(hft-engine2 LANGUAGES CXX)

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
    BacktestEngine/src/strategy/strategy.cpp
    BacktestEngine/src/utils/log/log.cpp
)
add_executable(hft-engine2 ${SOURCES})
target_include_directories(hft-engine2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/BacktestEngine/src")

find_package(libpqxx CONFIG REQUIRED)

target_link_libraries(hft-engine2 PRIVATE 
    libpqxx::pqxx
)

# Platform-specific compiler flags
if(MSVC)
    # Flags for Microsoft Visual C++
    target_compile_options(hft-engine2 PRIVATE /W4 /WX)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(hft-engine2 PRIVATE /D_ITERATOR_DEBUG_LEVEL=2)
    endif()
else()
    # Flags for GCC/Clang
    target_compile_options(hft-engine2 PRIVATE -Wall -Wextra -Werror)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(hft-engine2 PRIVATE _GLIBCXX_DEBUG)
    endif()
endif()