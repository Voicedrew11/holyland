# Writes OUT (build_rev.h) with the source tree's `git describe`, at every build, for the GS frame
# capture's metadata (port/debug/gs_capture.c); the file is rewritten only when the text changes, so
# nothing recompiles unless the commit does. A release's source has no git: there the release's version
# (VERSION, KF4_VERSION) names the build instead.
#   cmake -DSRC=<source dir> -DOUT=<header> [-DVERSION=<x.y.z>] -P tools/build_rev.cmake
find_package(Git QUIET)
set(rev "")
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${SRC} describe --always --dirty
        OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(rev STREQUAL "" AND NOT "${VERSION}" STREQUAL "")
    set(rev "v${VERSION}")
endif()
if(rev STREQUAL "")
    set(rev unknown)
endif()
set(text "#define KF4_BUILD_REV \"${rev}\"\n")
set(old "")
if(EXISTS ${OUT})
    file(READ ${OUT} old)
endif()
if(NOT old STREQUAL text)
    file(WRITE ${OUT} "${text}")
endif()
