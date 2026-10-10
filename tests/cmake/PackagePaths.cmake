# Shared path validation for standalone source-only regression packages.
set(PS2RECOMP_DIR "" CACHE PATH "Pinned PS2Recomp checkout with the Holyland patches")
set(RUNTIME_BUILD_DIR "" CACHE PATH "Existing native Windows runtime build")
set(TOOLS_BUILD_DIR "" CACHE PATH "Existing native Windows tools build")
set(RUNTIME_CONFIG "RelWithDebInfo" CACHE STRING "Configuration of existing runtime libraries")
set(TOOLS_CONFIG "Release" CACHE STRING "Configuration of existing tools libraries")
set(RAYLIB_INCLUDE_DIR "${RUNTIME_BUILD_DIR}/_deps/raylib-src/src"
    CACHE PATH "Directory containing raylib.h")

if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime.h")
    message(FATAL_ERROR "Set -DPS2RECOMP_DIR to the patched PS2Recomp checkout")
endif()

function(kfiv_require_runtime)
    if(NOT EXISTS "${RUNTIME_BUILD_DIR}/CMakeCache.txt")
        message(FATAL_ERROR "Set -DRUNTIME_BUILD_DIR to the configured native runtime build")
    endif()
endfunction()

function(kfiv_require_tools)
    if(NOT EXISTS "${TOOLS_BUILD_DIR}/CMakeCache.txt")
        message(FATAL_ERROR "Set -DTOOLS_BUILD_DIR to the configured native tools build")
    endif()
endfunction()

function(kfiv_require_raylib)
    if(NOT EXISTS "${RAYLIB_INCLUDE_DIR}/raylib.h")
        message(FATAL_ERROR "Set -DRAYLIB_INCLUDE_DIR to the directory containing raylib.h")
    endif()
endfunction()

if(MSVC)
    add_compile_options(/fp:strict)
endif()
