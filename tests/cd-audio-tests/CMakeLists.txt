cmake_minimum_required(VERSION 3.21)
project(kfiv_cd_audio_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")

add_executable(kfiv_cd_audio_tests cd_audio_tests.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/imports/iop_cdvd.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_kernel.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_memory.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_spu2.cpp
    ${PS2RECOMP_DIR}/ps2xIOP/src/ps2_path.cpp)
target_compile_features(kfiv_cd_audio_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_cd_audio_tests PRIVATE
    ${PS2RECOMP_DIR}/ps2xIOP/src ${PS2RECOMP_DIR}/ps2xIOP/include)
if(MSVC)
    target_compile_options(kfiv_cd_audio_tests PRIVATE /W4 /permissive-)
    target_compile_definitions(kfiv_cd_audio_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
enable_testing()
add_test(NAME kfiv_cd_audio_mapping_regression COMMAND kfiv_cd_audio_tests)
