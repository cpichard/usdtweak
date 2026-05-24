# Addons live in src/addons/. Each addon owns a subdirectory and a small
# CMakeLists.txt that calls usdtweak_add_addon(...).
#
# Addons are compiled directly into the usdtweak executable. We rely on
# USD's TF_REGISTRY_FUNCTION mechanism (ARCH_CONSTRUCTOR + _ARCH_ENSURE_PER_LIB_INIT)
# so that an addon TU survives linker stripping even when sources are in a
# static lib.

# ----- frontend API (built unconditionally) ---------------------------------
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/AddonRegistry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/AddonRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Api.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Api.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Notices.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Notices.cpp
)
target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ----- helper used by each addon's CMakeLists.txt ---------------------------
# Usage:
#   usdtweak_add_addon(MyAddon
#       SOURCES MyAddon.cpp MyAddon.h
#   )
function(usdtweak_add_addon ADDON_NAME)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(ABS_SOURCES)
    foreach(SRC ${ARG_SOURCES})
        list(APPEND ABS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}")
    endforeach()

    target_sources(usdtweak PRIVATE ${ABS_SOURCES})
    target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

# ----- registered addons ----------------------------------------------------
# Each subdirectory containing a CMakeLists.txt is treated as an addon.
# Drop a new folder here and re-run cmake — no edits to this file needed.
# CONFIGURE_DEPENDS lets the build system pick up new addons automatically
# on the next invocation of `cmake --build`.
file(GLOB ADDON_CMAKELISTS
    LIST_DIRECTORIES false
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
list(SORT ADDON_CMAKELISTS)
foreach(ADDON_CMAKELIST ${ADDON_CMAKELISTS})
    get_filename_component(ADDON_DIR ${ADDON_CMAKELIST} DIRECTORY)
    message(STATUS "Registering addon: ${ADDON_DIR}")
    add_subdirectory(${ADDON_DIR})
endforeach()
