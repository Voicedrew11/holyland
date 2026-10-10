cmake_minimum_required(VERSION 3.21)
project(kfiv_ee_callback_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_runtime()


set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")
if(NOT MSVC)
    message(FATAL_ERROR "These focused tests validate native Windows MSVC")
endif()
file(READ "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp" SCHEDULER_SOURCE)
string(REPLACE "if (!running->invocations.empty() && running->invocations.back().shouldExecute)"
               "if (false && !running->invocations.empty() && running->invocations.back().shouldExecute)"
               CONTROL_SOURCE "${SCHEDULER_SOURCE}")
if(CONTROL_SOURCE STREQUAL SCHEDULER_SOURCE)
    message(FATAL_ERROR "Actual scheduler predicate was not found; control cannot reproduce its absence")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-callback-control.cpp" "${CONTROL_SOURCE}")
function(add_callback_test target)
    add_executable(${target} ee_callback_tests.cpp
        "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj)
    target_link_options(${target} PRIVATE /STACK:16777216)
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/$<CONFIG>/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/$<CONFIG>/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/$<CONFIG>/raylib.lib"
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib"
        "${FFMPEG_DIR}/bin/swscale.lib"
        opengl32 glu32 winmm bcrypt secur32 ws2_32 user32 advapi32 ole32 shell32 gdi32)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()
add_callback_test(kfiv_ee_callback_tests)
add_callback_test(kfiv_ee_callback_control)
target_sources(kfiv_ee_callback_control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-callback-control.cpp")
target_compile_definitions(kfiv_ee_callback_control PRIVATE EXPECT_STALE_CALLBACKS=1)
enable_testing()
add_test(NAME kfiv_ee_callback_lifetime_regression COMMAND kfiv_ee_callback_tests)
add_test(NAME kfiv_ee_callback_original_stale_control COMMAND kfiv_ee_callback_control)
set_tests_properties(kfiv_ee_callback_lifetime_regression kfiv_ee_callback_original_stale_control PROPERTIES TIMEOUT 30)
