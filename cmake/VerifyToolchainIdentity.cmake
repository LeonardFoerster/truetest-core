cmake_minimum_required(VERSION 3.22)

foreach(environment_variable IN ITEMS
        CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH COMPILER_PATH
        LIBRARY_PATH GCC_EXEC_PREFIX LD_PRELOAD LD_AUDIT
        LD_LIBRARY_PATH LD_HWCAP_MASK GLIBC_TUNABLES)
    if(NOT "$ENV{${environment_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "deterministic build environment changed after configure: "
            "${environment_variable} must be empty")
    endif()
endforeach()

function(_tt_verify_binary label path expected_sha256)
    if(path STREQUAL "" OR expected_sha256 STREQUAL "unavailable")
        message(FATAL_ERROR
            "deterministic toolchain identity lacks ${label}")
    endif()
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "deterministic ${label} no longer exists: ${path}")
    endif()
    file(SHA256 "${path}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "deterministic ${label} changed after configure: expected "
            "${expected_sha256}, actual ${actual_sha256}")
    endif()
endfunction()

function(_tt_verify_driver_program compiler program expected_path)
    execute_process(
        COMMAND "${compiler}" "-print-prog-name=${program}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE actual_path
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0 OR actual_path STREQUAL "")
        message(FATAL_ERROR
            "deterministic compiler cannot resolve ${program}: ${error}")
    endif()
    if(NOT IS_ABSOLUTE "${actual_path}")
        find_program(actual_absolute
            NAMES "${actual_path}" REQUIRED NO_CACHE)
        set(actual_path "${actual_absolute}")
    endif()
    file(REAL_PATH "${actual_path}" actual_path)
    file(REAL_PATH "${expected_path}" expected_path)
    if(NOT actual_path STREQUAL expected_path)
        message(FATAL_ERROR
            "deterministic compiler resolves ${program} differently after "
            "configure: expected ${expected_path}, actual ${actual_path}")
    endif()
endfunction()

_tt_verify_binary(
    "C compiler" "${TT_C_COMPILER}" "${TT_C_COMPILER_SHA256}")
_tt_verify_binary(
    "C++ compiler" "${TT_COMPILER}" "${TT_COMPILER_SHA256}")
_tt_verify_binary(
    "linker" "${TT_LINKER}" "${TT_LINKER_SHA256}")
_tt_verify_binary(
    "archiver" "${TT_ARCHIVER}" "${TT_ARCHIVER_SHA256}")
_tt_verify_binary(
    "ranlib" "${TT_RANLIB}" "${TT_RANLIB_SHA256}")
_tt_verify_driver_program("${TT_C_COMPILER}" cc1 "${TT_CC1}")
_tt_verify_driver_program("${TT_COMPILER}" cc1plus "${TT_CC1PLUS}")
_tt_verify_driver_program("${TT_COMPILER}" collect2 "${TT_COLLECT2}")
_tt_verify_driver_program("${TT_COMPILER}" as "${TT_ASSEMBLER}")
_tt_verify_driver_program("${TT_COMPILER}" ld "${TT_DRIVER_LINKER}")
_tt_verify_binary(
    "GNU cc1" "${TT_CC1}" "${TT_CC1_SHA256}")
_tt_verify_binary(
    "GNU cc1plus" "${TT_CC1PLUS}" "${TT_CC1PLUS_SHA256}")
_tt_verify_binary(
    "GNU collect2" "${TT_COLLECT2}" "${TT_COLLECT2_SHA256}")
_tt_verify_binary(
    "assembler" "${TT_ASSEMBLER}" "${TT_ASSEMBLER_SHA256}")
_tt_verify_binary(
    "driver-resolved linker" "${TT_DRIVER_LINKER}"
    "${TT_DRIVER_LINKER_SHA256}")
_tt_verify_binary("CMake" "${TT_CMAKE}" "${TT_CMAKE_SHA256}")
_tt_verify_binary("Git" "${TT_GIT}" "${TT_GIT_SHA256}")
_tt_verify_binary(
    "build executor" "${TT_BUILD_EXECUTOR}" "${TT_BUILD_EXECUTOR_SHA256}")

if(NOT TT_TOOLCHAIN_FILE STREQUAL "")
    _tt_verify_binary(
        "toolchain file" "${TT_TOOLCHAIN_FILE}"
        "${TT_TOOLCHAIN_FILE_SHA256}")
elseif(NOT TT_TOOLCHAIN_FILE_SHA256 STREQUAL "not-configured")
    message(FATAL_ERROR
        "deterministic toolchain file identity is internally inconsistent")
endif()
