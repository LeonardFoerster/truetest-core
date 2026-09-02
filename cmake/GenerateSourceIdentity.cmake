cmake_minimum_required(VERSION 3.22)

foreach(_required IN ITEMS TT_SOURCE_DIR TT_OUTPUT_HEADER TT_TEMPLATE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "source identity generation requires ${_required}")
    endif()
endforeach()
if(NOT DEFINED TT_REQUIRE_EXACT)
    set(TT_REQUIRE_EXACT OFF)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/SourceIdentity.cmake")
tt_compute_source_identity(
    "${TT_SOURCE_DIR}" "${TT_REQUIRE_EXACT}"
    TRUETEST_GIT_SHA TRUETEST_GIT_DIRTY TRUETEST_GIT_DIFF_SHA256)
get_filename_component(_tt_output_directory "${TT_OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${_tt_output_directory}")
configure_file("${TT_TEMPLATE}" "${TT_OUTPUT_HEADER}" @ONLY)
