cmake_minimum_required(VERSION 3.21)
project(kfiv_mpeg_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_runtime()



set(MPEG_SOURCE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/Stubs/MPEG.cpp" CACHE FILEPATH "Actual MPEG implementation under test")
set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")
add_executable(kfiv_mpeg_tests mpeg_tests.cpp
    "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
target_compile_features(kfiv_mpeg_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_mpeg_tests PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/Stubs"
    "${PS2RECOMP_DIR}/ps2xIOP/include"
    "${RUNTIME_BUILD_DIR}/_deps/raylib-src/src"
    "${FFMPEG_DIR}/include")
target_compile_definitions(kfiv_mpeg_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(kfiv_mpeg_tests PRIVATE
    "${RUNTIME_BUILD_DIR}/ps2xRuntime/$<CONFIG>/ps2_runtime.lib"
    "${RUNTIME_BUILD_DIR}/ps2xIOP/$<CONFIG>/ps2_iop.lib"
    "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/$<CONFIG>/raylib.lib"
    "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
    "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib"
    "${FFMPEG_DIR}/bin/swscale.lib"
    opengl32 glu32 winmm bcrypt secur32 ws2_32 user32 advapi32 ole32 shell32 gdi32)
if(EXISTS "${RUNTIME_BUILD_DIR}/parallel-gs/gs")
    target_link_libraries(kfiv_mpeg_tests PRIVATE
        "${RUNTIME_BUILD_DIR}/parallel-gs/gs/$<CONFIG>/parallel-gs.lib"
        "${RUNTIME_BUILD_DIR}/parallel-gs/Granite/vulkan/$<CONFIG>/granite-vulkan.lib"
        "${RUNTIME_BUILD_DIR}/parallel-gs/Granite/third_party/$<CONFIG>/granite-volk.lib"
        "${RUNTIME_BUILD_DIR}/parallel-gs/Granite/util/$<CONFIG>/granite-util.lib"
        "${RUNTIME_BUILD_DIR}/parallel-gs/Granite/math/$<CONFIG>/granite-math.lib")
endif()

if(MSVC)
    target_compile_options(kfiv_mpeg_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
enable_testing()
add_test(NAME kfiv_mpeg_sdk_lifecycle_regression COMMAND kfiv_mpeg_tests)

add_executable(kfiv_mpeg_demux_tests mpeg_demux_tests.cpp "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
get_target_property(MPEG_TEST_INCLUDES kfiv_mpeg_tests INCLUDE_DIRECTORIES)
get_target_property(MPEG_TEST_LIBRARIES kfiv_mpeg_tests LINK_LIBRARIES)
target_include_directories(kfiv_mpeg_demux_tests PRIVATE ${MPEG_TEST_INCLUDES})
target_link_libraries(kfiv_mpeg_demux_tests PRIVATE ${MPEG_TEST_LIBRARIES})
target_compile_features(kfiv_mpeg_demux_tests PRIVATE cxx_std_20)
target_compile_definitions(kfiv_mpeg_demux_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(kfiv_mpeg_demux_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_demux_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_demux_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_demux_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
add_test(NAME kfiv_mpeg_stream_acceptance_regression COMMAND kfiv_mpeg_demux_tests)
set_tests_properties(kfiv_mpeg_stream_acceptance_regression PROPERTIES TIMEOUT 10)

add_executable(kfiv_mpeg_video_tests mpeg_video_tests.cpp "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
target_include_directories(kfiv_mpeg_video_tests PRIVATE ${MPEG_TEST_INCLUDES})
target_link_libraries(kfiv_mpeg_video_tests PRIVATE ${MPEG_TEST_LIBRARIES})
target_compile_features(kfiv_mpeg_video_tests PRIVATE cxx_std_20)
target_compile_definitions(kfiv_mpeg_video_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(kfiv_mpeg_video_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_video_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_video_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_video_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
add_test(NAME kfiv_mpeg_video_dma_regression COMMAND kfiv_mpeg_video_tests)
set_tests_properties(kfiv_mpeg_video_dma_regression PROPERTIES TIMEOUT 10)

add_executable(kfiv_mpeg_cancellation_tests mpeg_cancellation_tests.cpp "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
target_include_directories(kfiv_mpeg_cancellation_tests PRIVATE ${MPEG_TEST_INCLUDES})
target_link_libraries(kfiv_mpeg_cancellation_tests PRIVATE ${MPEG_TEST_LIBRARIES})
target_compile_features(kfiv_mpeg_cancellation_tests PRIVATE cxx_std_20)
target_compile_definitions(kfiv_mpeg_cancellation_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(kfiv_mpeg_cancellation_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_cancellation_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_cancellation_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_cancellation_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
add_test(NAME kfiv_mpeg_queued_cancellation_regression COMMAND kfiv_mpeg_cancellation_tests)
set_tests_properties(kfiv_mpeg_queued_cancellation_regression PROPERTIES TIMEOUT 10)

add_executable(kfiv_mpeg_snapshot_tests mpeg_snapshot_tests.cpp "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
target_include_directories(kfiv_mpeg_snapshot_tests PRIVATE ${MPEG_TEST_INCLUDES})
target_link_libraries(kfiv_mpeg_snapshot_tests PRIVATE ${MPEG_TEST_LIBRARIES})
target_compile_features(kfiv_mpeg_snapshot_tests PRIVATE cxx_std_20)
target_compile_definitions(kfiv_mpeg_snapshot_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(kfiv_mpeg_snapshot_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_snapshot_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_snapshot_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_snapshot_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
add_test(NAME kfiv_mpeg_retry_snapshot_regression COMMAND kfiv_mpeg_snapshot_tests)
set_tests_properties(kfiv_mpeg_retry_snapshot_regression PROPERTIES TIMEOUT 10)

add_executable(kfiv_mpeg_audio_service_tests mpeg_audio_service_tests.cpp "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
target_include_directories(kfiv_mpeg_audio_service_tests PRIVATE ${MPEG_TEST_INCLUDES})
target_link_libraries(kfiv_mpeg_audio_service_tests PRIVATE ${MPEG_TEST_LIBRARIES})
target_compile_features(kfiv_mpeg_audio_service_tests PRIVATE cxx_std_20)
target_compile_definitions(kfiv_mpeg_audio_service_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(kfiv_mpeg_audio_service_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_audio_service_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_audio_service_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_audio_service_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
add_test(NAME kfiv_mpeg_audio_output_service_regression COMMAND kfiv_mpeg_audio_service_tests)
set_tests_properties(kfiv_mpeg_audio_output_service_regression PROPERTIES TIMEOUT 10)

add_executable(kfiv_mpeg_picture_input_tests mpeg_picture_input_tests.cpp "${MPEG_SOURCE}"
    "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
target_include_directories(kfiv_mpeg_picture_input_tests PRIVATE ${MPEG_TEST_INCLUDES})
target_link_libraries(kfiv_mpeg_picture_input_tests PRIVATE ${MPEG_TEST_LIBRARIES})
target_compile_features(kfiv_mpeg_picture_input_tests PRIVATE cxx_std_20)
target_compile_definitions(kfiv_mpeg_picture_input_tests PRIVATE PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(kfiv_mpeg_picture_input_tests PRIVATE /W4 /permissive- /bigobj)
    target_link_options(kfiv_mpeg_picture_input_tests PRIVATE /STACK:16777216)
endif()
add_custom_command(TARGET kfiv_mpeg_picture_input_tests POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_picture_input_tests>"
    -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
add_test(NAME kfiv_mpeg_picture_input_service_regression COMMAND kfiv_mpeg_picture_input_tests)
set_tests_properties(kfiv_mpeg_picture_input_service_regression PROPERTIES TIMEOUT 10)

option(MPEG_AUDIO_SERVICE_OLD_CONTROL "Build a scratch source comparison without audio output service" OFF)
if(MPEG_AUDIO_SERVICE_OLD_CONTROL)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(MPEG_AUDIO_CONTROL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/audio-service-control/MPEG.cpp")
    add_custom_command(OUTPUT "${MPEG_AUDIO_CONTROL_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_audio_service_control.py"
            "${MPEG_SOURCE}" "${MPEG_AUDIO_CONTROL_SOURCE}"
        DEPENDS "${MPEG_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_audio_service_control.py"
        VERBATIM)
    add_executable(kfiv_mpeg_audio_service_old_control mpeg_audio_service_tests.cpp
        "${MPEG_AUDIO_CONTROL_SOURCE}" "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(kfiv_mpeg_audio_service_old_control PRIVATE ${MPEG_TEST_INCLUDES})
    target_link_libraries(kfiv_mpeg_audio_service_old_control PRIVATE ${MPEG_TEST_LIBRARIES})
    target_compile_features(kfiv_mpeg_audio_service_old_control PRIVATE cxx_std_20)
    target_compile_definitions(kfiv_mpeg_audio_service_old_control PRIVATE
        PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS MPEG_AUDIO_SERVICE_OLD_CONTROL=1)
    if(MSVC)
        target_compile_options(kfiv_mpeg_audio_service_old_control PRIVATE /W4 /permissive- /bigobj)
        target_link_options(kfiv_mpeg_audio_service_old_control PRIVATE /STACK:16777216)
    endif()
    add_custom_command(TARGET kfiv_mpeg_audio_service_old_control POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_audio_service_old_control>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
    add_test(NAME kfiv_mpeg_audio_output_service_old_control COMMAND kfiv_mpeg_audio_service_old_control)
    set_tests_properties(kfiv_mpeg_audio_output_service_old_control PROPERTIES TIMEOUT 10)
endif()

option(MPEG_PICTURE_INPUT_OLD_CONTROL "Build a scratch source comparison without picture input service" OFF)
if(MPEG_PICTURE_INPUT_OLD_CONTROL)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(MPEG_PICTURE_CONTROL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/picture-input-control/MPEG.cpp")
    add_custom_command(OUTPUT "${MPEG_PICTURE_CONTROL_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_picture_input_control.py"
            "${MPEG_SOURCE}" "${MPEG_PICTURE_CONTROL_SOURCE}"
        DEPENDS "${MPEG_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_picture_input_control.py"
        VERBATIM)
    add_executable(kfiv_mpeg_picture_input_old_control mpeg_picture_input_tests.cpp
        "${MPEG_PICTURE_CONTROL_SOURCE}" "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(kfiv_mpeg_picture_input_old_control PRIVATE ${MPEG_TEST_INCLUDES})
    target_link_libraries(kfiv_mpeg_picture_input_old_control PRIVATE ${MPEG_TEST_LIBRARIES})
    target_compile_features(kfiv_mpeg_picture_input_old_control PRIVATE cxx_std_20)
    target_compile_definitions(kfiv_mpeg_picture_input_old_control PRIVATE
        PS2X_HAS_FFMPEG=1 _CRT_SECURE_NO_WARNINGS MPEG_PICTURE_INPUT_OLD_CONTROL=1)
    if(MSVC)
        target_compile_options(kfiv_mpeg_picture_input_old_control PRIVATE /W4 /permissive- /bigobj)
        target_link_options(kfiv_mpeg_picture_input_old_control PRIVATE /STACK:16777216)
    endif()
    add_custom_command(TARGET kfiv_mpeg_picture_input_old_control POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:kfiv_mpeg_picture_input_old_control>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
    add_test(NAME kfiv_mpeg_picture_input_service_old_control COMMAND kfiv_mpeg_picture_input_old_control)
    set_tests_properties(kfiv_mpeg_picture_input_service_old_control PROPERTIES TIMEOUT 10)
endif()
