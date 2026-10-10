cmake_minimum_required(VERSION 3.21)
project(kfiv_ee_clock_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_runtime()



set(RUNTIME_CONFIG "RelWithDebInfo" CACHE STRING "Existing runtime library configuration")
set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")
if(NOT MSVC)
    message(FATAL_ERROR "This package verifies the native Windows MSVC runtime")
endif()

function(add_ee_clock_test target)
    add_executable(${target} ee_clock_tests.cpp "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS PS2X_HAS_FFMPEG=1)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include" "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/${RUNTIME_CONFIG}/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/${RUNTIME_CONFIG}/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/${RUNTIME_CONFIG}/raylib.lib"
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib" "${FFMPEG_DIR}/bin/swscale.lib"
        opengl32 glu32 winmm bcrypt secur32 ws2_32 user32 advapi32 ole32 shell32 gdi32)
    target_link_options(${target} PRIVATE /STACK:16777216)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()

add_ee_clock_test(kfiv_ee_clock_tests)

# The control removes precisely the new host-due credit block from a scratch
# copy of the real scheduler. It preserves the prior no-burst fix and all
# unrelated current scheduler behavior. Production source is never modified.
file(READ "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp" SCHEDULER_TEXT)
set(CREDIT_START "            uint64_t pacedHardwareCycle =")
set(CREDIT_END "            for (const ScheduledEvent &item : m_deadlines)\n            {\n                if (item.deadlineCycle <= m_eeCycle")
string(REPLACE "\r\n" "\n" SCHEDULER_TEXT "${SCHEDULER_TEXT}")
string(FIND "${SCHEDULER_TEXT}" "${CREDIT_START}" START_OFFSET)
string(FIND "${SCHEDULER_TEXT}" "${CREDIT_END}" END_OFFSET)
if(START_OFFSET LESS 0 OR END_OFFSET LESS START_OFFSET)
    message(FATAL_ERROR "Review the controlled host-due credit removal against the current scheduler")
endif()
string(SUBSTRING "${SCHEDULER_TEXT}" 0 ${START_OFFSET} CONTROL_PREFIX)
string(SUBSTRING "${SCHEDULER_TEXT}" ${END_OFFSET} -1 CONTROL_SUFFIX)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-both-clocks.cpp" "${CONTROL_PREFIX}${CONTROL_SUFFIX}")
add_ee_clock_test(kfiv_ee_clock_both_control)
target_sources(kfiv_ee_clock_both_control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-both-clocks.cpp")
target_compile_definitions(kfiv_ee_clock_both_control PRIVATE EXPECT_BOTH_CLOCKS_CONTROL=1)

enable_testing()
add_test(NAME kfiv_ee_busy_poll_clock_regression COMMAND kfiv_ee_clock_tests)
add_test(NAME kfiv_ee_prior_both_clocks_control COMMAND kfiv_ee_clock_both_control)
set_tests_properties(kfiv_ee_busy_poll_clock_regression kfiv_ee_prior_both_clocks_control PROPERTIES TIMEOUT 10)
