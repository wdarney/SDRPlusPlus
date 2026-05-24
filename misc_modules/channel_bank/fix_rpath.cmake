# fix_rpath.cmake — run as a post-build cmake -P script.
# Usage: cmake -P fix_rpath.cmake -DDYLIB=<path>
#
# Rewrites any non-system, non-@rpath library references in DYLIB to use
# @rpath/<basename>, making the dylib portable inside an .app bundle whose
# main executable exposes @loader_path/../Frameworks via LC_RPATH.

cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED DYLIB)
    message(FATAL_ERROR "DYLIB not set")
endif()

find_program(OTOOL otool REQUIRED)
find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)

# Read the library dependency list
execute_process(
    COMMAND ${OTOOL} -L ${DYLIB}
    OUTPUT_VARIABLE OTOOL_OUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Ensure @loader_path/../Frameworks is in the rpath (idempotent)
execute_process(
    COMMAND ${INSTALL_NAME_TOOL} -add_rpath "@loader_path/../Frameworks" ${DYLIB}
    ERROR_QUIET  # already present → non-fatal error
)

# Parse each dependency line (skip first line which is the dylib itself)
string(REPLACE "\n" ";" LINES "${OTOOL_OUT}")
set(FIRST TRUE)
foreach(LINE IN LISTS LINES)
    if(FIRST)
        set(FIRST FALSE)
        continue()
    endif()
    # Extract the path (first whitespace-delimited token)
    string(REGEX MATCH "^[ \t]+([^ \t]+)" M "${LINE}")
    if(NOT M)
        continue()
    endif()
    set(DEP "${CMAKE_MATCH_1}")
    # Skip system paths and already-@rpath entries
    if(DEP MATCHES "^/System/" OR DEP MATCHES "^/usr/lib/" OR DEP MATCHES "^@")
        continue()
    endif()
    get_filename_component(LIB_NAME "${DEP}" NAME)
    # Known version substitutions: build machine may have an older minor version
    # than what is bundled in the .app (e.g. libvolk.3.1 built here, 3.2 bundled).
    if(LIB_NAME STREQUAL "libvolk.3.1.dylib")
        set(LIB_NAME "libvolk.3.2.dylib")
    endif()
    message(STATUS "  Rewriting ${DEP} -> @rpath/${LIB_NAME}")
    execute_process(
        COMMAND ${INSTALL_NAME_TOOL} -change "${DEP}" "@rpath/${LIB_NAME}" "${DYLIB}"
    )
endforeach()

message(STATUS "fix_rpath: done → ${DYLIB}")
