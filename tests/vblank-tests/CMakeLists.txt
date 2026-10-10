cmake_minimum_required(VERSION 3.20)
project(kfiv_vblank_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_runtime()



set(RUNTIME_CONFIG "RelWithDebInfo" CACHE STRING "Configuration of existing runtime libraries")
set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")

if(NOT MSVC)
    message(FATAL_ERROR "These focused tests validate the native Windows MSVC runtime")
endif()

function(add_vblank_test target)
    add_executable(${target} vblank_tests.cpp
        "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS
        PS2_RUNTIME_LOGS=1 PS2X_ENABLE_IOP_RPC_TRACE=0 PS2X_HAS_FFMPEG=1)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/${RUNTIME_CONFIG}/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/${RUNTIME_CONFIG}/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/${RUNTIME_CONFIG}/raylib.lib"
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib"
        "${FFMPEG_DIR}/bin/swscale.lib"
        bcrypt secur32 ws2_32
        opengl32 glu32 winmm kernel32 user32 gdi32 winspool shell32 ole32
        oleaut32 uuid comdlg32 advapi32)
    target_link_options(${target} PRIVATE /STACK:8388608)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()

add_vblank_test(kfiv_vblank_tests)

# Compiling the captured original scheduler supplies its symbols before the
# static runtime library, so that executable exercises the real old scheduler.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/control/EeScheduler.cpp")
    add_vblank_test(kfiv_vblank_burst_control)
    target_sources(kfiv_vblank_burst_control PRIVATE control/EeScheduler.cpp)
    target_compile_definitions(kfiv_vblank_burst_control PRIVATE EXPECT_ORIGINAL_BURST=1)
endif()

enable_testing()
add_test(NAME kfiv_vblank_pacing_regression COMMAND kfiv_vblank_tests)
if(TARGET kfiv_vblank_burst_control)
    add_test(NAME kfiv_vblank_original_burst_control COMMAND kfiv_vblank_burst_control)
    set_tests_properties(kfiv_vblank_original_burst_control PROPERTIES TIMEOUT 10)
endif()
set_tests_properties(kfiv_vblank_pacing_regression PROPERTIES TIMEOUT 10)
