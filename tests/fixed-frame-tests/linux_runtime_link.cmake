# SPDX-License-Identifier: GPL-3.0-or-later
# Link the actual already-built Linux runtime, never Windows objects or stubs
# for the scheduler/GS under test. Only generated game dispatch is substituted.
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswresample libswscale)
file(STRINGS "${RUNTIME_BUILD_DIR}/CMakeCache.txt" runtime_vulkan_enabled
    REGEX "^PS2X_ENABLE_VULKAN_GS:BOOL=ON$")
set(vulkan_runtime_libraries)
if(runtime_vulkan_enabled)
    foreach(library IN ITEMS
        parallel-gs/gs/libparallel-gs.a
        parallel-gs/Granite/vulkan/libgranite-vulkan.a
        parallel-gs/Granite/third_party/libgranite-volk.a
        parallel-gs/Granite/util/libgranite-util.a
        parallel-gs/Granite/math/libgranite-math.a)
        if(NOT EXISTS "${RUNTIME_BUILD_DIR}/${library}")
            message(FATAL_ERROR "Build the matching Linux dependency: ${library}")
        endif()
        list(APPEND vulkan_runtime_libraries "${RUNTIME_BUILD_DIR}/${library}")
    endforeach()
endif()
function(add_fixed_runtime_test target)
    set(fixture runtime_tests.cpp)
    if(ARGC GREATER 1)
        set(fixture "${ARGV1}")
    endif()
    add_executable(${target} ${fixture} "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -msse4.1 -fno-fast-math -frounding-math -ffp-contract=off)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include" "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE -Wl,--start-group
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/libps2_runtime.a"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/libps2_iop.a"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/libraylib.a"
        ${vulkan_runtime_libraries} -Wl,--end-group
        PkgConfig::FFMPEG Threads::Threads OpenGL::GL ${X11_LIBRARIES} ${CMAKE_DL_LIBS} m)
endfunction()
