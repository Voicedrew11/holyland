set(RUNTIME_CONFIG "RelWithDebInfo" CACHE STRING "Existing runtime library configuration")
set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")
if(NOT MSVC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Integration linking supports native MSVC and Linux builds")
    endif()
    include(linux_runtime_link.cmake)
    return()
endif()

# A static runtime built with Vulkan retains its renderer's link dependencies
# even though this fixture does not render or open a GPU window. Use only the
# matching libraries already produced by the explicit runtime build.
file(STRINGS "${RUNTIME_BUILD_DIR}/CMakeCache.txt" runtime_vulkan_enabled
    REGEX "^PS2X_ENABLE_VULKAN_GS:BOOL=ON$")
set(vulkan_runtime_libraries)
if(runtime_vulkan_enabled)
    foreach(library IN ITEMS
        "parallel-gs/gs/${RUNTIME_CONFIG}/parallel-gs.lib"
        "parallel-gs/Granite/vulkan/${RUNTIME_CONFIG}/granite-vulkan.lib"
        "parallel-gs/Granite/third_party/${RUNTIME_CONFIG}/granite-volk.lib"
        "parallel-gs/Granite/util/${RUNTIME_CONFIG}/granite-util.lib"
        "parallel-gs/Granite/math/${RUNTIME_CONFIG}/granite-math.lib")
        if(NOT EXISTS "${RUNTIME_BUILD_DIR}/${library}")
            message(FATAL_ERROR "Rebuild the matching Vulkan runtime dependency: ${library}")
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
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj /fp:strict)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS PS2X_HAS_FFMPEG=1)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include" "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/${RUNTIME_CONFIG}/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/${RUNTIME_CONFIG}/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/${RUNTIME_CONFIG}/raylib.lib"
        ${vulkan_runtime_libraries}
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib" "${FFMPEG_DIR}/bin/swscale.lib"
        opengl32 glu32 winmm bcrypt secur32 ws2_32 user32 advapi32 ole32 shell32 gdi32)
    target_link_options(${target} PRIVATE /STACK:16777216)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()
