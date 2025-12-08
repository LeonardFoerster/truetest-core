cmake_minimum_required(VERSION 3.22)
project(hft-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/BacktestEngine/src/*.cpp")
add_executable(hft-engine ${SOURCES})
target_include_directories(hft-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/BacktestEngine/src")

find_package(PostgreSQL REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

target_link_libraries(hft-engine PRIVATE 
    PostgreSQL::PostgreSQL
    libpqxx::pqxx
)

if(MSVC)
    target_compile_options(hft-engine PRIVATE /std:c++latest /W4 /WX /D_ITERATOR_DEBUG_LEVEL=2)
endif()