# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(spu_inlining_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
cmake_path(IS_PREFIX CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}" NORMALIZE BUILD_IN_PACKAGE)
if(BUILD_IN_PACKAGE)
    message(FATAL_ERROR "Keep scratch runtime copies outside this source package: choose an external -B directory")
endif()
set(SPU_CORE_DIR "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core")
if(NOT EXISTS "${SPU_CORE_DIR}/iop_spu2.cpp" OR NOT EXISTS "${SPU_CORE_DIR}/iop_spu2.h")
    message(FATAL_ERROR "PS2RECOMP_DIR must provide the actual SPU2 source/header")
endif()
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
set(SPU_REFERENCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/spu-reference")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_spu_reference.py"
    --runtime-dir "${PS2RECOMP_DIR}" --output-dir "${SPU_REFERENCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/make_spu_reference.py"
    "${SPU_CORE_DIR}/iop_spu2.cpp" "${SPU_CORE_DIR}/iop_spu2.h")
if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

function(configure_fixture target mode)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${SPU_CORE_DIR}" "${SPU_REFERENCE_DIR}"
        "${PS2RECOMP_DIR}/ps2xIOP/src")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-
            "$<$<NOT:$<CONFIG:Debug>>:/O2>" /fp:strict)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(mode STREQUAL "candidate")
            target_compile_options(${target} PRIVATE /Ob2)
        else()
            target_compile_options(${target} PRIVATE /Ob1)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE -O2 -fno-fast-math -ffp-contract=off)
        if(mode STREQUAL "candidate")
            target_compile_options(${target} PRIVATE -finline-functions)
        else()
            target_compile_options(${target} PRIVATE -fno-inline)
        endif()
    else()
        message(FATAL_ERROR "Compiler controls are defined for MSVC, GNU and Clang only")
    endif()
endfunction()

add_library(spu_inlining_reference OBJECT "${SPU_REFERENCE_DIR}/iop_spu2_ref.cpp")
configure_fixture(spu_inlining_reference baseline)
add_library(spu_inlining_candidate OBJECT "${SPU_CORE_DIR}/iop_spu2.cpp")
configure_fixture(spu_inlining_candidate candidate)
add_executable(spu_inlining_tests spu_inlining_tests.cpp
    $<TARGET_OBJECTS:spu_inlining_reference> $<TARGET_OBJECTS:spu_inlining_candidate>)
configure_fixture(spu_inlining_tests baseline)
foreach(mode baseline candidate)
    add_executable(spu_audio_regression_${mode} "${CMAKE_CURRENT_SOURCE_DIR}/../audio-tests/audio_tests.cpp"
        "${SPU_CORE_DIR}/iop_spu2.cpp" "${SPU_CORE_DIR}/iop_memory.cpp")
    configure_fixture(spu_audio_regression_${mode} ${mode})
endforeach()

enable_testing()
add_test(NAME spu_inlining_exact COMMAND spu_inlining_tests)
add_test(NAME spu_inlining_negative_pcm COMMAND spu_inlining_tests --negative-pcm)
add_test(NAME spu_audio_regression_baseline COMMAND spu_audio_regression_baseline)
add_test(NAME spu_audio_regression_candidate COMMAND spu_audio_regression_candidate)
set_tests_properties(spu_inlining_exact spu_inlining_negative_pcm
    spu_audio_regression_baseline spu_audio_regression_candidate
    PROPERTIES ENVIRONMENT "PS2X_SPU2_TRACE=0" TIMEOUT 120)
set_tests_properties(spu_inlining_negative_pcm PROPERTIES
    PASS_REGULAR_EXPRESSION "expected PCM-only corruption detected")
