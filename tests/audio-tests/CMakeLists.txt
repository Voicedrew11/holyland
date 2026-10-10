cmake_minimum_required(VERSION 3.20)
project(kfiv_native_audio_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")

add_executable(kfiv_native_audio_tests audio_tests.cpp
    "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_spu2.cpp"
    "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_memory.cpp")
target_compile_features(kfiv_native_audio_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_native_audio_tests PRIVATE "${PS2RECOMP_DIR}/ps2xIOP/src")
if(MSVC)
    target_compile_options(kfiv_native_audio_tests PRIVATE /W4 /permissive-)
    target_compile_definitions(kfiv_native_audio_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
enable_testing()
add_test(NAME kfiv_native_spu2_regression COMMAND kfiv_native_audio_tests)
