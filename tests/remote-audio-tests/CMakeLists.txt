cmake_minimum_required(VERSION 3.20)
project(kfiv_remote_audio_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")

add_executable(kfiv_remote_audio_tests remote_audio_tests.cpp
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/kfiv_runtime.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/game_overrides.cpp"
    "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_memory.cpp"
    "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_spu2.cpp")
target_compile_features(kfiv_remote_audio_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_remote_audio_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../menu-tests/fakes"
    "${PS2RECOMP_DIR}/ps2xRuntime/include"
    "${PS2RECOMP_DIR}/ps2xIOP/include"
    "${PS2RECOMP_DIR}/ps2xIOP/src")
if(MSVC)
    target_compile_options(kfiv_remote_audio_tests PRIVATE /W4 /permissive-)
    target_compile_definitions(kfiv_remote_audio_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
enable_testing()
add_test(NAME kfiv_remote_audio_sdk_regression COMMAND kfiv_remote_audio_tests)
