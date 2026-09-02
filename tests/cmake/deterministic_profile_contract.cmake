cmake_minimum_required(VERSION 3.22)

foreach(_required IN ITEMS GUARD_SOURCE_DIR GUARD_BUILD_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "deterministic profile contract requires ${_required}")
    endif()
endforeach()

file(READ "${GUARD_SOURCE_DIR}/CMakePresets.json" _presets)
string(JSON _preset_count LENGTH "${_presets}" configurePresets)
set(_found FALSE)
math(EXPR _last_preset "${_preset_count} - 1")
foreach(_index RANGE 0 ${_last_preset})
    string(JSON _name GET "${_presets}" configurePresets ${_index} name)
    if(NOT _name STREQUAL "linux-deterministic")
        continue()
    endif()
    set(_found TRUE)
    string(JSON _generator GET "${_presets}"
        configurePresets ${_index} generator)
    if(NOT _generator STREQUAL "Ninja")
        message(FATAL_ERROR
            "linux-deterministic preset must use Ninja, got ${_generator}")
    endif()
    foreach(_pair IN ITEMS
            "CMAKE_BUILD_TYPE=Release"
            "CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG"
            "CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG"
            "BUILD_TESTS=ON"
            "BUILD_SHARED_LIB=OFF"
            "ENABLE_ASAN=OFF"
            "ENABLE_BENCHMARKS=OFF"
            "ENABLE_BINANCE=OFF"
            "ENABLE_BITGET=OFF"
            "ENABLE_BITUNIX=OFF"
            "ENABLE_DEBUG=OFF"
            "ENABLE_IMGUI=OFF"
            "ENABLE_LIVE_DATA=OFF"
            "ENABLE_LTO=OFF"
            "ENABLE_NATIVE_OPT=OFF"
            "ENABLE_QUESTDB=OFF"
            "ENABLE_TSAN=OFF"
            "ENABLE_UBSAN=OFF"
            "ENABLE_WEB=OFF"
            "CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF"
            "CMAKE_UNITY_BUILD=OFF"
            "FETCHCONTENT_FULLY_DISCONNECTED=OFF"
            "FETCHCONTENT_UPDATES_DISCONNECTED=ON"
            "TRUETEST_RESEARCH_ONLY=ON"
            "TRUETEST_DETERMINISTIC_BUILD=ON"
            "TRUETEST_BUILD_PROFILE=linux-deterministic-v1")
        string(REPLACE "=" ";" _parts "${_pair}")
        list(GET _parts 0 _key)
        list(GET _parts 1 _expected)
        string(JSON _actual GET "${_presets}"
            configurePresets ${_index} cacheVariables ${_key})
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR
                "linux-deterministic preset has ${_key}=${_actual}, expected ${_expected}")
        endif()
    endforeach()
    foreach(_empty_key IN ITEMS
            CMAKE_C_FLAGS CMAKE_CXX_FLAGS
            CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_RELEASE
            CMAKE_SHARED_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS_RELEASE
            CMAKE_MODULE_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS_RELEASE
            CMAKE_STATIC_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS_RELEASE)
        string(JSON _actual GET "${_presets}"
            configurePresets ${_index} cacheVariables ${_empty_key})
        if(NOT _actual STREQUAL "")
            message(FATAL_ERROR
                "linux-deterministic preset has ${_empty_key}=${_actual}, expected empty")
        endif()
    endforeach()
endforeach()
if(NOT _found)
    message(FATAL_ERROR "linux-deterministic configure preset is missing")
endif()

file(READ "${GUARD_BUILD_DIR}/CMakeCache.txt" _cache)
set(_current_tree_is_deterministic FALSE)
if(_cache MATCHES "TRUETEST_BUILD_PROFILE:STRING=linux-deterministic-v1")
    set(_current_tree_is_deterministic TRUE)
endif()

if(_current_tree_is_deterministic)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${GUARD_BUILD_DIR}" --
            CXX_FLAGS=-ffast-math
        RESULT_VARIABLE _build_override_result
        OUTPUT_VARIABLE _build_override_stdout
        ERROR_VARIABLE _build_override_stderr)
    if(_build_override_result EQUAL 0)
        message(FATAL_ERROR
            "deterministic generator accepted Make-style CXX_FLAGS override")
    endif()

    set(_fake_tool_directory "${GUARD_BUILD_DIR}/deterministic-fake-tools")
    set(_fake_git_marker "${_fake_tool_directory}/git-was-invoked")
    file(REMOVE_RECURSE "${_fake_tool_directory}")

    set(_fake_linker_directory
        "${GUARD_BUILD_DIR}/deterministic-fake-linker")
    set(_fake_linker_marker
        "${_fake_linker_directory}/ld-was-invoked")
    file(REMOVE_RECURSE "${_fake_linker_directory}")
    file(MAKE_DIRECTORY "${_fake_linker_directory}")
    file(WRITE "${_fake_linker_directory}/ld"
        "#!/bin/sh\n"
        "touch \"${_fake_linker_marker}\"\n"
        "exec /usr/bin/ld \"$@\"\n")
    file(CHMOD "${_fake_linker_directory}/ld"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${_fake_linker_directory}:$ENV{PATH}"
            "${CMAKE_COMMAND}" --build "${GUARD_BUILD_DIR}" --target
            truetest_toolchain_identity
        RESULT_VARIABLE _fake_linker_build_result
        OUTPUT_VARIABLE _fake_linker_build_stdout
        ERROR_VARIABLE _fake_linker_build_stderr)
    if(_fake_linker_build_result EQUAL 0 OR EXISTS "${_fake_linker_marker}"
       OR NOT "${_fake_linker_build_stdout}${_fake_linker_build_stderr}"
              MATCHES "resolves ld differently")
        message(FATAL_ERROR
            "deterministic build did not reject PATH-resolved linker "
            "substitution: "
            "${_fake_linker_build_stdout}${_fake_linker_build_stderr}")
    endif()
    file(REMOVE_RECURSE "${_fake_linker_directory}")
    file(MAKE_DIRECTORY "${_fake_tool_directory}")
    file(WRITE "${_fake_tool_directory}/git"
        "#!/bin/sh\n"
        "touch \"${_fake_git_marker}\"\n"
        "exec /usr/bin/git \"$@\"\n")
    file(CHMOD "${_fake_tool_directory}/git"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "PATH=${_fake_tool_directory}:$ENV{PATH}"
            "${CMAKE_COMMAND}" --build "${GUARD_BUILD_DIR}" --target
            truetest_source_identity truetest_dependency_identity
        RESULT_VARIABLE _fake_git_build_result
        OUTPUT_VARIABLE _fake_git_build_stdout
        ERROR_VARIABLE _fake_git_build_stderr)
    if(NOT _fake_git_build_result EQUAL 0 OR EXISTS "${_fake_git_marker}")
        message(FATAL_ERROR
            "deterministic build consulted unaudited Git from PATH: "
            "${_fake_git_build_stdout}${_fake_git_build_stderr}")
    endif()
    file(REMOVE_RECURSE "${_fake_tool_directory}")

    foreach(_late_environment IN ITEMS LD_LIBRARY_PATH LD_HWCAP_MASK)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "${_late_environment}=${GUARD_SOURCE_DIR}"
                "${CMAKE_COMMAND}" --build "${GUARD_BUILD_DIR}" --target
                truetest_toolchain_identity
            RESULT_VARIABLE _late_environment_result
            OUTPUT_VARIABLE _late_environment_stdout
            ERROR_VARIABLE _late_environment_stderr)
        string(FIND
            "${_late_environment_stdout}${_late_environment_stderr}"
            "${_late_environment}" _late_environment_diagnostic)
        if(_late_environment_result EQUAL 0
           OR _late_environment_diagnostic EQUAL -1)
            message(FATAL_ERROR
                "deterministic build did not fail closed for late "
                "${_late_environment}: "
                "${_late_environment_stdout}${_late_environment_stderr}")
        endif()
    endforeach()
endif()

function(_assert_deterministic_profile_rejects _label _cache_argument)
    set(_negative_build
        "${GUARD_BUILD_DIR}/deterministic-negative-${_label}")
    file(REMOVE_RECURSE "${_negative_build}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --preset linux-deterministic
            -B "${_negative_build}" "${_cache_argument}"
        WORKING_DIRECTORY "${GUARD_SOURCE_DIR}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    file(REMOVE_RECURSE "${_negative_build}")
    if(_result EQUAL 0)
        message(FATAL_ERROR
            "deterministic profile accepted forbidden ${_label} override")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " _diagnostic
        "${_stdout}${_stderr}")
    string(FIND "${_diagnostic}"
        "forbids custom global/config-specific"
        _diagnostic_position)
    if(_diagnostic_position EQUAL -1)
        message(FATAL_ERROR
            "deterministic profile rejected ${_label} without the expected "
            "fail-closed diagnostic: ${_stdout}${_stderr}")
    endif()
endfunction()

function(_assert_deterministic_profile_rejects_with _label _cache_argument
         _expected_diagnostic)
    set(_negative_build
        "${GUARD_BUILD_DIR}/deterministic-negative-${_label}")
    file(REMOVE_RECURSE "${_negative_build}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --preset linux-deterministic
            -B "${_negative_build}" "${_cache_argument}"
        WORKING_DIRECTORY "${GUARD_SOURCE_DIR}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    file(REMOVE_RECURSE "${_negative_build}")
    if(_result EQUAL 0)
        message(FATAL_ERROR
            "deterministic profile accepted forbidden ${_label} override")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " _diagnostic
        "${_stdout}${_stderr}")
    string(FIND "${_diagnostic}"
        "${_expected_diagnostic}" _diagnostic_position)
    if(_diagnostic_position EQUAL -1)
        message(FATAL_ERROR
            "deterministic profile rejected ${_label} without the expected "
            "fail-closed diagnostic: ${_stdout}${_stderr}")
    endif()
endfunction()

_assert_deterministic_profile_rejects(
    "fast-math" "-DCMAKE_CXX_FLAGS=-ffast-math")
_assert_deterministic_profile_rejects(
    "native-arch" "-DCMAKE_CXX_FLAGS_RELEASE=-march=native")
_assert_deterministic_profile_rejects(
    "link-lto" "-DCMAKE_EXE_LINKER_FLAGS=-flto")
_assert_deterministic_profile_rejects(
    "c-link-lto" "-DCMAKE_C_FLAGS_RELEASE=-flto")
_assert_deterministic_profile_rejects_with(
    "external-force-include" "-DCMAKE_CXX_FLAGS=-include /tmp/injected.hpp"
    "forbids custom global/config-specific")
_assert_deterministic_profile_rejects_with(
    "external-link-object" "-DCMAKE_EXE_LINKER_FLAGS=/tmp/injected.o"
    "forbids custom global/config-specific")
_assert_deterministic_profile_rejects_with(
    "external-response-file" "-DCMAKE_CXX_FLAGS=@/tmp/injected.rsp"
    "forbids custom global/config-specific")
_assert_deterministic_profile_rejects_with(
    "cmake-ipo" "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON"
    "profile invariants were overridden")
_assert_deterministic_profile_rejects_with(
    "incomplete-profile" "-DTRUETEST_BUILD_PROFILE=default"
    "requires the complete linux-deterministic-v1 profile")
_assert_deterministic_profile_rejects_with(
    "local-dependency-source"
    "-DFETCHCONTENT_SOURCE_DIR_CLI11=${GUARD_SOURCE_DIR}"
    "forbids local FetchContent source override")
_assert_deterministic_profile_rejects_with(
    "dependency-find-mode" "-DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS"
    "forbids FetchContent resolution override")
_assert_deterministic_profile_rejects_with(
    "web-feature" "-DENABLE_WEB=ON"
    "profile invariants were overridden")
_assert_deterministic_profile_rejects_with(
    "unity-build" "-DCMAKE_UNITY_BUILD=ON"
    "profile invariants were overridden")
_assert_deterministic_profile_rejects_with(
    "compiler-launcher" "-DCMAKE_CXX_COMPILER_LAUNCHER=/bin/false"
    "forbids build launcher")
_assert_deterministic_profile_rejects_with(
    "alternate-linker" "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=gold"
    "forbids custom global/config-specific")
_assert_deterministic_profile_rejects_with(
    "linker-type" "-DCMAKE_LINKER_TYPE=LLD"
    "forbids alternate CMAKE_LINKER_TYPE")
_assert_deterministic_profile_rejects_with(
    "compiler-arg1" "-DCMAKE_CXX_COMPILER=/usr/bin/c++;-ffast-math"
    "forbids implicit compiler")
_assert_deterministic_profile_rejects_with(
    "compile-rule"
    "-DCMAKE_CXX_COMPILE_OBJECT=<CMAKE_CXX_COMPILER> -include /tmp/injected.hpp -c <SOURCE> -o <OBJECT>"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "link-rule"
    "-DCMAKE_CXX_LINK_EXECUTABLE=<CMAKE_CXX_COMPILER> <OBJECTS> /tmp/injected.o -o <TARGET>"
    "forbids pre-project CMake")

set(_cmake_injection
    "${GUARD_BUILD_DIR}/deterministic-cmake-injection.cmake")
file(WRITE "${_cmake_injection}"
    "add_compile_options(-ffast-math)\n")
_assert_deterministic_profile_rejects_with(
    "project-include" "-DCMAKE_PROJECT_INCLUDE=${_cmake_injection}"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "user-rules" "-DCMAKE_USER_MAKE_RULES_OVERRIDE=${_cmake_injection}"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "module-path" "-DCMAKE_MODULE_PATH=${GUARD_SOURCE_DIR}"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "toolchain-file" "-DCMAKE_TOOLCHAIN_FILE=${_cmake_injection}"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "standard-include"
    "-DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES=${GUARD_SOURCE_DIR}"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "standard-library"
    "-DCMAKE_CXX_STANDARD_LIBRARIES=-Wl,--build-id=none"
    "forbids pre-project CMake")
_assert_deterministic_profile_rejects_with(
    "cross-system" "-DCMAKE_SYSTEM_NAME=Linux"
    "forbids pre-project CMake")
file(REMOVE "${_cmake_injection}")

foreach(_environment_variable IN ITEMS CPATH LD_LIBRARY_PATH LD_HWCAP_MASK)
    string(TOLOWER "${_environment_variable}" _environment_label)
    set(_environment_negative_build
        "${GUARD_BUILD_DIR}/deterministic-negative-${_environment_label}")
    file(REMOVE_RECURSE "${_environment_negative_build}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "${_environment_variable}=${GUARD_SOURCE_DIR}"
            "${CMAKE_COMMAND}" --preset linux-deterministic
            -B "${_environment_negative_build}"
        WORKING_DIRECTORY "${GUARD_SOURCE_DIR}"
        RESULT_VARIABLE _environment_result
        OUTPUT_VARIABLE _environment_stdout
        ERROR_VARIABLE _environment_stderr)
    file(REMOVE_RECURSE "${_environment_negative_build}")
    string(FIND "${_environment_stdout}${_environment_stderr}"
        "${_environment_variable}" _environment_diagnostic)
    if(_environment_result EQUAL 0
       OR _environment_diagnostic EQUAL -1)
        message(FATAL_ERROR
            "deterministic profile did not fail closed for inherited "
            "${_environment_variable}: "
            "${_environment_stdout}${_environment_stderr}")
    endif()
endforeach()

set(_compiler_environment_build
    "${GUARD_BUILD_DIR}/deterministic-negative-compiler-environment")
file(REMOVE_RECURSE "${_compiler_environment_build}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CXX=/usr/bin/c++ -ffast-math"
        "${CMAKE_COMMAND}" --preset linux-deterministic
        -B "${_compiler_environment_build}"
    WORKING_DIRECTORY "${GUARD_SOURCE_DIR}"
    RESULT_VARIABLE _compiler_environment_result
    OUTPUT_VARIABLE _compiler_environment_stdout
    ERROR_VARIABLE _compiler_environment_stderr)
file(REMOVE_RECURSE "${_compiler_environment_build}")
if(_compiler_environment_result EQUAL 0
   OR NOT "${_compiler_environment_stdout}${_compiler_environment_stderr}"
          MATCHES "forbids implicit compiler")
    message(FATAL_ERROR
        "deterministic profile accepted compiler ARG1 from CXX: "
        "${_compiler_environment_stdout}${_compiler_environment_stderr}")
endif()

set(_direct_cxx "/usr/bin/c++")
if(NOT EXISTS "${_direct_cxx}")
    message(FATAL_ERROR "deterministic profile requires /usr/bin/c++")
endif()
set(_compiler_wrapper
    "${GUARD_BUILD_DIR}/deterministic-compiler-wrapper.sh")
file(WRITE "${_compiler_wrapper}"
    "#!/bin/sh\nexec \"${_direct_cxx}\" \"$@\"\n")
file(CHMOD "${_compiler_wrapper}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
_assert_deterministic_profile_rejects_with(
    "compiler-wrapper" "-DCMAKE_CXX_COMPILER=${_compiler_wrapper}"
    "requires the canonical /usr/bin/cc")
file(REMOVE "${_compiler_wrapper}")

set(_elf_proxy_source
    "${GUARD_BUILD_DIR}/deterministic-compiler-proxy.cpp")
set(_elf_proxy "${GUARD_BUILD_DIR}/deterministic-compiler-proxy")
file(WRITE "${_elf_proxy_source}"
    "#include <unistd.h>\n"
    "int main(int, char** argv) {\n"
    "  argv[0] = const_cast<char*>(\"/usr/bin/c++\");\n"
    "  ::execv(\"/usr/bin/c++\", argv);\n"
    "  return 127;\n"
    "}\n")
execute_process(
    COMMAND "${_direct_cxx}" "${_elf_proxy_source}" -o "${_elf_proxy}"
    RESULT_VARIABLE _elf_proxy_compile_result
    ERROR_VARIABLE _elf_proxy_compile_error)
if(NOT _elf_proxy_compile_result EQUAL 0)
    message(FATAL_ERROR
        "failed to compile deterministic compiler proxy fixture: "
        "${_elf_proxy_compile_error}")
endif()
_assert_deterministic_profile_rejects_with(
    "elf-compiler-proxy" "-DCMAKE_CXX_COMPILER=${_elf_proxy}"
    "requires the canonical /usr/bin/cc")
file(REMOVE "${_elf_proxy_source}" "${_elf_proxy}")

file(READ "${GUARD_SOURCE_DIR}/cmake/CompilerFlags.cmake" _compiler_policy)
foreach(_required_flag IN ITEMS "-fno-fast-math" "-ffp-contract=off")
    string(FIND "${_compiler_policy}" "${_required_flag}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR
            "deterministic compiler policy lacks ${_required_flag}")
    endif()
endforeach()

if(_cache MATCHES "TRUETEST_BUILD_PROFILE:STRING=linux-deterministic-v1")
    foreach(_cache_entry IN ITEMS
            "TRUETEST_DETERMINISTIC_BUILD:BOOL=ON"
            "TRUETEST_RESEARCH_ONLY:BOOL=ON"
            "ENABLE_LTO:BOOL=OFF"
            "ENABLE_NATIVE_OPT:BOOL=OFF"
            "FETCHCONTENT_FULLY_DISCONNECTED:BOOL=OFF"
            "FETCHCONTENT_UPDATES_DISCONNECTED:BOOL=ON")
        string(FIND "${_cache}" "${_cache_entry}" _position)
        if(_position EQUAL -1)
            message(FATAL_ERROR
                "configured deterministic tree lacks ${_cache_entry}")
        endif()
    endforeach()
    foreach(_off_cache_variable IN ITEMS
            CMAKE_INTERPROCEDURAL_OPTIMIZATION CMAKE_UNITY_BUILD)
        if(NOT _cache MATCHES
           "${_off_cache_variable}:[A-Z]+=(OFF|FALSE|0)")
            message(FATAL_ERROR
                "configured deterministic tree lacks "
                "${_off_cache_variable}=OFF")
        endif()
    endforeach()
    set(_commands "${GUARD_BUILD_DIR}/compile_commands.json")
    if(NOT EXISTS "${_commands}")
        message(FATAL_ERROR
            "deterministic profile must export compile_commands.json")
    endif()
    file(READ "${_commands}" _compile_commands)

    set(_version_header
        "${GUARD_BUILD_DIR}/generated/tt/truetest_version.h")
    if(NOT EXISTS "${_version_header}")
        message(FATAL_ERROR
            "deterministic profile lacks generated build identity header")
    endif()
    file(READ "${_version_header}" _version_header_contents)
    if(NOT _version_header_contents MATCHES
       "TRUETEST_BUILD_TIMESTAMP[ ]+\"deterministic-build-no-wall-clock\"")
        message(FATAL_ERROR
            "deterministic build identity embeds configure wall time")
    endif()
    foreach(_required_flag IN ITEMS
            "-std=c++23" "-fno-fast-math" "-ffp-contract=off")
        string(FIND "${_compile_commands}" "${_required_flag}" _position)
        if(_position EQUAL -1)
            message(FATAL_ERROR
                "deterministic compile commands lack ${_required_flag}")
        endif()
    endforeach()
    foreach(_forbidden IN ITEMS
            "-Ofast" "-ffast-math" "-ffp-contract=fast" "-flto"
            "-march=" "-mtune=" "-mcpu=")
        string(FIND "${_compile_commands}" "${_forbidden}" _position)
        if(NOT _position EQUAL -1)
            message(FATAL_ERROR
                "deterministic compile commands contain ${_forbidden}")
        endif()
    endforeach()
endif()

message(STATUS "deterministic build profile contract satisfied")
