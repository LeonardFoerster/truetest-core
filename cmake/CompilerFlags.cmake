# cmake/CompilerFlags.cmake
#
# Central compiler/linker flag policy. Included once from the root
# CMakeLists.txt. Exposes helper functions that consuming targets call.
#
# Public surface:
#   tt_apply_common_flags(<target>)      # build profile + warnings + sanitizers
#   tt_apply_live_flags(<target>)        # -march=native etc. when ENABLE_NATIVE_OPT
#                                        # (applied to engine_core + engine entry points)
#
# Global behaviour applied on include:
#   * Sanitizer mutual exclusivity
# ─────────────────────────────────────────────────────────────────────────────

if(DEFINED _TT_COMPILER_FLAGS_INCLUDED)
    return()
endif()
set(_TT_COMPILER_FLAGS_INCLUDED TRUE)

# ── Sanitizer mutual exclusivity ────────────────────────────────────────────
if(ENABLE_TSAN AND (ENABLE_ASAN OR ENABLE_UBSAN))
    message(FATAL_ERROR
        "TSAN cannot be combined with ASAN or UBSAN. Disable one.")
endif()

# ── tt_apply_common_flags(target) ───────────────────────────────────────────
# Apply to every first-party target (engine_core, tests, benchmarks, binaries).
# Never apply to FetchContent-built third-party libs.
function(tt_apply_common_flags target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>)
    else()
        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-DNDEBUG>
            $<$<CONFIG:RelWithDebInfo>:-O2>
            $<$<CONFIG:RelWithDebInfo>:-g>
            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Debug>:-g>
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion)
        if(ENABLE_LTO)
            target_compile_options(${target} PRIVATE
                $<$<CONFIG:Release>:-flto>)
            target_link_options(${target} PRIVATE
                $<$<CONFIG:Release>:-flto>)
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_definitions(${target} PRIVATE
                $<$<CONFIG:Debug>:_GLIBCXX_DEBUG>)
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
        target_compile_options(${target} PRIVATE
            -fsanitize=undefined
            -fno-sanitize-recover=undefined)
        target_link_options(${target} PRIVATE
            -fsanitize=undefined
            -fno-sanitize-recover=undefined)
    endif()
endfunction()

# ── tt_apply_live_flags(target) ─────────────────────────────────────────────
# Release-build microarchitecture tuning when ENABLE_NATIVE_OPT is ON.
# Root CMakeLists applies this to engine_core and all three engine entry points
# (operators who opt in want the complete engine code tuned).
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
