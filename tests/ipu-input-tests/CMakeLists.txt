cmake_minimum_required(VERSION 3.21)
project(kfiv_ipu_input_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")

add_executable(kfiv_ipu_input_tests ipu_input_tests.cpp unrelated_graphics_stubs.cpp
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_memory.cpp")
target_compile_features(kfiv_ipu_input_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_ipu_input_tests PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
if(MSVC)
    target_compile_options(kfiv_ipu_input_tests PRIVATE /W4 /permissive- /bigobj)
    target_compile_definitions(kfiv_ipu_input_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
enable_testing()
add_test(NAME kfiv_ipu_input_dma_regression COMMAND kfiv_ipu_input_tests)
