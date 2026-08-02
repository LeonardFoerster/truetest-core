# cmake/CompilerFlags.cmake
#
# Central compiler/linker flag policy. Included once from the root
# CMakeLists.txt. Exposes helper functions that consuming targets call.
#
# Public surface:
#   tt_apply_common_flags(<target>)      # warnings + debug defines + sanitizers
#   tt_apply_live_flags(<target>)        # -march=native etc. when ENABLE_NATIVE_OPT
#                                        # (applied to all three engines from root)
#
# Global behaviour applied on include:
#   * CMAKE_CXX_STANDARD = 23 (REQUIRED)
#   * Per-config optimisation (-O3 + LTO for Release, -O2 -g for RelWithDebInfo,
#     -O0 -g for Debug)
#   * Sanitizer mutual exclusivity
# ─────────────────────────────────────────────────────────────────────────────

if(DEFINED _TT_COMPILER_FLAGS_INCLUDED)
    return()
endif()
set(_TT_COMPILER_FLAGS_INCLUDED TRUE)

# ── Language standard ───────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard (minimum 23)")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_STANDARD LESS 23)
    message(FATAL_ERROR
        "TrueTest requires C++23. Set CMAKE_CXX_STANDARD to 23 or higher.")
endif()

# ── Sanitizer mutual exclusivity ────────────────────────────────────────────
if(ENABLE_TSAN AND (ENABLE_ASAN OR ENABLE_UBSAN))
    message(FATAL_ERROR
        "TSAN cannot be combined with ASAN or UBSAN. Disable one.")
endif()

# ── Per-config optimisation profile ─────────────────────────────────────────
if(NOT MSVC)
    add_compile_options(
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Release>:-flto>
    )
    add_link_options($<$<CONFIG:Release>:-flto>)

    add_compile_options(
        $<$<CONFIG:RelWithDebInfo>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-g>
    )

    add_compile_options(
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g>
    )
endif()

# ── tt_apply_common_flags(target) ───────────────────────────────────────────
# Apply to every first-party target (engine_core, tests, benchmarks, binaries).
# Never apply to FetchContent-built third-party libs.
function(tt_apply_common_flags target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_compile_options(${target} PRIVATE /D_ITERATOR_DEBUG_LEVEL=2)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug"
           AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(${target} PRIVATE _GLIBCXX_DEBUG)
        endif()
    endif()

    # Sanitizer flags travel with the target so they land on both compile
    # and link steps.
    if(ENABLE_TSAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options   (${target} PRIVATE -fsanitize=thread)
    endif()
    if(ENABLE_ASAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options   (${target} PRIVATE -fsanitize=address)
    endif()
    if(ENABLE_UBSAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options   (${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

# ── tt_apply_live_flags(target) ─────────────────────────────────────────────
# Release-build microarchitecture tuning when ENABLE_NATIVE_OPT is ON.
# Root CMakeLists applies this to engine_backtest, engine_shadow, and
# engine_live alike (operators who opt in want it across the board).
# CI and portable builds keep ENABLE_NATIVE_OPT=OFF.
function(tt_apply_live_flags target)
    if(NOT ENABLE_NATIVE_OPT OR MSVC)
        return()
    endif()
    target_compile_options(${target} PRIVATE
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-mtune=native>
        $<$<CONFIG:Release>:-funroll-loops>
        $<$<CONFIG:Release>:-fomit-frame-pointer>
    )
endfunction()
