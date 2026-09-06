#
# strres -- compile authored JSON string catalogues for ESP-IDF projects.
#
# ESP-IDF includes this file into *project* scope before any component's
# CMakeLists.txt is added to the build, which is what lets a component living in
# another repository call strres_generate() without the consuming project having
# to define anything. js2c's project_include.cmake explains the mechanism at
# length; this follows it deliberately.
#
# Note the variable trap: here CMAKE_CURRENT_SOURCE_DIR is the *project* root,
# not this component. Use COMPONENT_DIR, which IDF sets for each
# project_include.
#

set(STRRES_TOOL_DIR "${COMPONENT_DIR}/tool")
set(STRRES_COMPILER "${STRRES_TOOL_DIR}/strres_compile.py")

if(NOT EXISTS "${STRRES_COMPILER}")
    message(FATAL_ERROR "strres: compiler missing at ${STRRES_COMPILER}")
endif()

# BUILD_DIR is readable during early expansion; CMAKE_CURRENT_BINARY_DIR is not,
# and it also differs between a local component and a namespaced
# managed_components one. Deriving from BUILD_DIR is stable in both placements.
idf_build_get_property(_strres_build_dir BUILD_DIR)
set(STRRES_GEN_ROOT "${_strres_build_dir}/strres_gen")
file(MAKE_DIRECTORY "${STRRES_GEN_ROOT}")

#
# strres_generate(<source-dir> IMAGE_DIR <dir>
#                 [BASE_LOCALE <name>] [GEN_DIR <dir>]
#                 [OUT_C <var>] [OUT_H <var>] [OUT_INCLUDE_DIR <var>])
#
# <source-dir> holds one subdirectory per locale, each holding <section>.json.
#
# Runs at *configure* time, not build time, for the reasons js2c_generate()
# documents: the generated header is part of the owning component's public
# interface, and a build-time custom command gives a dependent's compile step no
# ordering edge to the generator, so a dependent that includes the header races
# it on a clean parallel build. idf_component_register() also rejects an
# INCLUDE_DIRS entry that is not an existing directory, and SRCS must name a
# file that exists.
#
# The cost is a re-configure when a string changes, which the
# CMAKE_CONFIGURE_DEPENDS below makes automatic.
#
# Two outputs go to two different places, deliberately:
#
#   - The header and catalogue go under BUILD_DIR, never a component source dir,
#     which a managed_components component is re-extracted into on dependency
#     changes.
#   - The .sr blobs go to IMAGE_DIR, which must be inside the directory the
#     LittleFS image is built from, because littlefs_create_partition_image()
#     packs a directory as it finds it.
#
# MUST be called inside `if(NOT CMAKE_BUILD_EARLY_EXPANSION)`. IDF re-runs every
# component CMakeLists.txt in `cmake -P` script mode to collect requirements,
# and in that pass this function does not exist.
#
function(strres_generate SOURCE_DIR)
    cmake_parse_arguments(ARG ""
        "IMAGE_DIR;BASE_LOCALE;GEN_DIR;OUT_C;OUT_H;OUT_INCLUDE_DIR" "" ${ARGN})

    get_filename_component(_src "${SOURCE_DIR}" ABSOLUTE
                           BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    if(NOT IS_DIRECTORY "${_src}")
        message(FATAL_ERROR "strres_generate: no such directory: ${_src}")
    endif()
    if(NOT ARG_IMAGE_DIR)
        message(FATAL_ERROR "strres_generate: IMAGE_DIR is required")
    endif()
    if(NOT ARG_BASE_LOCALE)
        set(ARG_BASE_LOCALE "en-US")
    endif()
    if(NOT ARG_GEN_DIR)
        set(ARG_GEN_DIR "${STRRES_GEN_ROOT}/${COMPONENT_NAME}")
    endif()
    file(MAKE_DIRECTORY "${ARG_GEN_DIR}")

    set(_h   "${ARG_GEN_DIR}/strres_ids.h")
    set(_c   "${ARG_GEN_DIR}/strres_catalog.c")
    set(_map "${ARG_GEN_DIR}/strres_ids.txt")

    # Use IDF's python, not a bare python3: the compiler must run under the same
    # interpreter the rest of the build uses.
    idf_build_get_property(_py PYTHON)
    if(NOT _py)
        set(_py "python3")
    endif()

    execute_process(
        COMMAND "${_py}" "${STRRES_COMPILER}" "${_src}"
                --base-locale "${ARG_BASE_LOCALE}"
                --out-fs      "${ARG_IMAGE_DIR}"
                --out-header  "${_h}"
                --out-catalog "${_c}"
                --out-map     "${_map}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE  _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "strres: compiling ${_src} failed\n${_err}${_out}")
    endif()
    string(STRIP "${_out}" _out)
    message(STATUS "${_out}")

    # Re-run CMake when any authored string or the compiler changes. GLOB_RECURSE
    # at configure time is exactly right here: the list is only consulted to
    # decide whether to re-configure, and adding a .json file should do that.
    file(GLOB_RECURSE _inputs CONFIGURE_DEPENDS "${_src}/*.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${_inputs} "${STRRES_COMPILER}")

    if(ARG_OUT_C)
        set(${ARG_OUT_C} "${_c}" PARENT_SCOPE)
    endif()
    if(ARG_OUT_H)
        set(${ARG_OUT_H} "${_h}" PARENT_SCOPE)
    endif()
    if(ARG_OUT_INCLUDE_DIR)
        set(${ARG_OUT_INCLUDE_DIR} "${ARG_GEN_DIR}" PARENT_SCOPE)
    endif()
endfunction()
