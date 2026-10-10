cmake_minimum_required(VERSION 3.21)
project(kfiv_ioman_completion_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")


add_executable(kfiv_ioman_completion_tests
    ioman_completion_tests.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/imports/iop_ioman.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_memory.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_spu2.cpp)
target_compile_features(kfiv_ioman_completion_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_ioman_completion_tests PRIVATE
    ${PS2RECOMP_DIR}/ps2xIOP/src
    ${PS2RECOMP_DIR}/ps2xIOP/include)
if(MSVC)
    target_compile_options(kfiv_ioman_completion_tests PRIVATE /W4 /permissive-)
endif()
enable_testing()
add_test(NAME kfiv_ioman_completion_regression COMMAND kfiv_ioman_completion_tests)
