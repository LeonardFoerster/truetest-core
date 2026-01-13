cmake_minimum_required(VERSION 3.22)
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

find_package(PostgreSQL)
if (NOT PostgreSQL_FOUND)
    message(FATAL_ERROR "PostgreSQL not found. Please install the PostgreSQL development libraries for your system.

*   **Ubuntu/Debian:** `sudo apt-get install libpq-dev`
*   **macOS:** `brew install postgresql`
*   **Windows:** Install PostgreSQL from the official website and make sure the `bin` and `lib` directories are in your PATH.

If it's installed in a non-standard location, you can tell CMake where to find it by setting the `POSTGRESQL_ROOT_DIR` environment variable.
")
endif()

include(FetchContent)
FetchContent_Declare(
  libpqxx
  GIT_REPOSITORY https://github.com/jtv/libpqxx.git
  GIT_TAG 7.7.4 
)

FetchContent_MakeAvailable(libpqxx)

target_include_directories(hft-engine PRIVATE ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(hft-engine PRIVATE 
    pqxx
    ${PostgreSQL_LIBRARIES}
)

if(MSVC)
    target_compile_options(hft-engine PRIVATE /W4)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(hft-engine PRIVATE /D_ITERATOR_DEBUG_LEVEL=2)
    endif()
else()
    target_compile_options(hft-engine PRIVATE -Wall -Wextra)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_definitions(hft-engine PRIVATE _GLIBCXX_DEBUG)
    endif()
endif()
