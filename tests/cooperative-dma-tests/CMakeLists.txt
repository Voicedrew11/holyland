# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(cooperative_dma_tests LANGUAGES CXX)
set(PS2RECOMP_DIR "" CACHE PATH "Full patched W source checkout")
set(RUNTIME "${PS2RECOMP_DIR}/ps2xRuntime")
if(NOT EXISTS "${RUNTIME}/src/lib/ps2_vif1_interpreter.cpp")
    message(FATAL_ERROR "Set PS2RECOMP_DIR to the actual candidate source")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CooperativeRuntimeFixtures.cmake")
set(control "${CMAKE_CURRENT_BINARY_DIR}/controls")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_controls.py"
    --runtime "${RUNTIME}" --output "${control}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/prepare_controls.py"
    "${RUNTIME}/src/lib/ps2_memory.cpp" "${RUNTIME}/src/lib/ps2_vif1_interpreter.cpp")
function(memory_fixture target fixture memory_source parser_source)
    add_executable(${target} "${fixture}" memory_stubs.cpp "${memory_source}" "${parser_source}"
        "${RUNTIME}/src/lib/gs/ps2_gif_arbiter.cpp")
    cooperative_fixture_flags(${target})
    target_include_directories(${target} PRIVATE "${RUNTIME}/include")
endfunction()
memory_fixture(cooperative_memory memory_tests.cpp "${RUNTIME}/src/lib/ps2_memory.cpp" "${RUNTIME}/src/lib/ps2_vif1_interpreter.cpp")
memory_fixture(cooperative_ownership ownership_tests.cpp "${RUNTIME}/src/lib/ps2_memory.cpp" "${RUNTIME}/src/lib/ps2_vif1_interpreter.cpp")
memory_fixture(cooperative_chain_copy ownership_tests.cpp "${control}/chain_copy.cpp" "${RUNTIME}/src/lib/ps2_vif1_interpreter.cpp")
memory_fixture(cooperative_parser_copy ownership_tests.cpp "${RUNTIME}/src/lib/ps2_memory.cpp" "${control}/parser_copy.cpp")
memory_fixture(cooperative_old_offset memory_tests.cpp "${RUNTIME}/src/lib/ps2_memory.cpp" "${control}/old_offset.cpp")
enable_testing()
add_test(NAME cooperative_memory COMMAND cooperative_memory)
add_test(NAME cooperative_memory_exact_boundary COMMAND cooperative_memory --exact-boundary)
add_test(NAME cooperative_owned_allocations COMMAND cooperative_ownership)
add_test(NAME cooperative_chain_copy_control COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:cooperative_chain_copy>"
    "-DEXPECTED=owned chain allocation moved to incoming" -P "${CMAKE_CURRENT_SOURCE_DIR}/ownership_control.cmake")
add_test(NAME cooperative_parser_copy_control COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:cooperative_parser_copy>"
    "-DEXPECTED=empty active parser adopts incoming allocation" -P "${CMAKE_CURRENT_SOURCE_DIR}/ownership_control.cmake")
add_test(NAME cooperative_old_offset_control COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:cooperative_old_offset>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/old_offset_control.cmake")
get_property(all_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${all_tests} PROPERTIES TIMEOUT 30)
