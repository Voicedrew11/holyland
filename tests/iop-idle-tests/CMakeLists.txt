# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(iop_idle_selection_tests LANGUAGES CXX)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(PS2RECOMP_DIR "" CACHE PATH "Candidate PS2Recomp source checkout")
set(IOP_REFERENCE_DIR "" CACHE PATH "Pinned pre-cache ps2xIOP source directory")
if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_kernel.cpp" OR
   NOT EXISTS "${IOP_REFERENCE_DIR}/src/emulator/core/iop_kernel.cpp")
  message(FATAL_ERROR "Set PS2RECOMP_DIR and IOP_REFERENCE_DIR to actual source checkouts")
endif()
set(scratch "${CMAKE_CURRENT_BINARY_DIR}/prepared")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_sources.py"
  --candidate "${PS2RECOMP_DIR}/ps2xIOP" --reference "${IOP_REFERENCE_DIR}" --output "${scratch}"
  COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/prepare_sources.py"
  "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_kernel.h"
  "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_kernel.cpp"
  "${IOP_REFERENCE_DIR}/src/emulator/core/iop_kernel.h"
  "${IOP_REFERENCE_DIR}/src/emulator/core/iop_kernel.cpp")
add_library(iop_kernel_proof_support STATIC
  "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_memory.cpp"
  "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core/iop_spu2.cpp")
target_compile_features(iop_kernel_proof_support PUBLIC cxx_std_20)
target_compile_definitions(iop_kernel_proof_support PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(iop_kernel_proof_support PRIVATE "${PS2RECOMP_DIR}/ps2xIOP/src")
enable_testing()
foreach(variant IN ITEMS candidate IopKernelNoInvalidate IopKernelWrongDeadline)
  if(variant STREQUAL "candidate")
    set(core "${scratch}/emulator/core/iop_kernel.cpp")
  else()
    set(core "${scratch}/${variant}.cpp")
  endif()
  add_executable(iop_idle_${variant} kernel_proof.cpp "${scratch}/iop_kernel_reference.cpp" "${core}")
  target_include_directories(iop_idle_${variant} PRIVATE "${scratch}" "${scratch}/emulator/core"
    "${PS2RECOMP_DIR}/ps2xIOP/src" "${PS2RECOMP_DIR}/ps2xIOP/src/emulator/core")
  target_link_libraries(iop_idle_${variant} PRIVATE iop_kernel_proof_support)
  if(MSVC)
    target_compile_options(iop_idle_${variant} PRIVATE /O2 /Ob1 /fp:strict /W4)
  endif()
  if(variant STREQUAL "candidate")
    add_test(NAME iop_idle_exact_state COMMAND iop_idle_${variant})
  else()
    target_compile_definitions(iop_idle_${variant} PRIVATE CANDIDATE_KERNEL_TYPE=${variant} CANDIDATE_KERNEL_HEADER="${variant}.h")
    add_test(NAME iop_idle_${variant}_control COMMAND iop_idle_${variant} --expect-selection-failure)
  endif()
endforeach()
set(PS2X_IOP_BUILD_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" actual-iop)
target_compile_definitions(ps2_iop PRIVATE _CRT_SECURE_NO_WARNINGS)
add_executable(iop_idle_audio_clock "${CMAKE_CURRENT_LIST_DIR}/../audio-clock-tests/audio_clock_tests.cpp")
target_link_libraries(iop_idle_audio_clock PRIVATE ps2_iop)
add_test(NAME iop_idle_audio_clock COMMAND iop_idle_audio_clock)
set_tests_properties(iop_idle_exact_state iop_idle_audio_clock PROPERTIES TIMEOUT 30)
