# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(kfiv_vu_native_verify LANGUAGES CXX)

set(VU_CANDIDATE_RUNTIME_DIR "" CACHE PATH "Candidate checkout's ps2xRuntime directory (required)")
set(VU_REFERENCE_RUNTIME_DIR "" CACHE PATH "Separate patch-39 checkout's ps2xRuntime directory (required)")
if(NOT VU_CANDIDATE_RUNTIME_DIR OR NOT VU_REFERENCE_RUNTIME_DIR)
    message(FATAL_ERROR "Set both VU_CANDIDATE_RUNTIME_DIR and VU_REFERENCE_RUNTIME_DIR explicitly.")
endif()
set(VU_NATIVE_GENERATOR "${VU_CANDIDATE_RUNTIME_DIR}/tools/generate_vu_native.py" CACHE FILEPATH
    "Actual candidate runtime's native generator")
if(NOT EXISTS "${VU_NATIVE_GENERATOR}")
    message(FATAL_ERROR "Candidate runtime must supply tools/generate_vu_native.py")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
option(VU_VERIFY_OB2 "Use /Ob2 on candidate VU units only (MSVC)" ${MSVC})
option(VU_VERIFY_LTCG "Use /GL on candidate VU units only and /LTCG at link (MSVC)" ${MSVC})

set(base_fixture "${CMAKE_CURRENT_SOURCE_DIR}/../vu-stepping-tests")
set(derived_fixture "${CMAKE_CURRENT_BINARY_DIR}/derived-fixture")
set(VU_AUTHORED_CATALOG_DIR "${CMAKE_CURRENT_BINARY_DIR}/authored-input")
set(VU_NATIVE_CATALOG "${CMAKE_CURRENT_BINARY_DIR}/native-catalog/catalog.inc")
set(native_manifest "${CMAKE_CURRENT_BINARY_DIR}/native-catalog/catalog.manifest.json")
set(configure_inputs "${CMAKE_CURRENT_SOURCE_DIR}/prepare_fixture.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/authored_program.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/authored_native_cases.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/check_native_control.py"
    "${VU_NATIVE_GENERATOR}")
get_filename_component(generator_directory "${VU_NATIVE_GENERATOR}" DIRECTORY)
list(APPEND configure_inputs "${generator_directory}/vu_native_input.py")
foreach(file IN ITEMS CMakeLists.txt prepare_sources.py reference39-hashes.json verifier.cpp
        metadata_cases.inc pipeline_cases.inc stepping_cases.inc unrelated_stubs.cpp)
    list(APPEND configure_inputs "${base_fixture}/${file}")
endforeach()
foreach(file IN ITEMS include/runtime/ps2_vu1.h src/lib/vu/ps2_vu1_core.cpp
        src/lib/vu/ps2_vu1_upper.cpp src/lib/vu/ps2_vu1_lower.cpp src/lib/vu/ps2_vu1_detail.h)
    list(APPEND configure_inputs "${VU_CANDIDATE_RUNTIME_DIR}/${file}"
        "${VU_REFERENCE_RUNTIME_DIR}/${file}")
endforeach()
file(GLOB candidate_lift_inputs CONFIGURE_DEPENDS
    "${VU_CANDIDATE_RUNTIME_DIR}/include/runtime/ps2_vu1_lift.h"
    "${VU_CANDIDATE_RUNTIME_DIR}/src/lib/vu/ps2_vu1_lift.cpp")
list(APPEND configure_inputs ${candidate_lift_inputs})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${configure_inputs})

execute_process(COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_fixture.py"
    --source-fixture "${base_fixture}" --destination "${derived_fixture}"
    --scratch "${VU_AUTHORED_CATALOG_DIR}" --build-root "${CMAKE_CURRENT_BINARY_DIR}"
    --candidate-runtime "${VU_CANDIDATE_RUNTIME_DIR}" --reference-runtime "${VU_REFERENCE_RUNTIME_DIR}"
    RESULT_VARIABLE preparation_status OUTPUT_VARIABLE preparation_output ERROR_VARIABLE preparation_error)
if(NOT preparation_status EQUAL 0)
    message(FATAL_ERROR "${preparation_error}")
endif()
message(STATUS "${preparation_output}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${Python3_EXECUTABLE}" "${VU_NATIVE_GENERATOR}"
    --runtime "${VU_CANDIDATE_RUNTIME_DIR}" --elf "${VU_AUTHORED_CATALOG_DIR}/authored.elf"
    --metadata "${VU_AUTHORED_CATALOG_DIR}/authored.mpg.json"
    --output "${VU_NATIVE_CATALOG}" --manifest "${native_manifest}"
    RESULT_VARIABLE generation_status OUTPUT_VARIABLE generation_output ERROR_VARIABLE generation_error)
if(NOT generation_status EQUAL 0)
    message(FATAL_ERROR "${generation_error}")
endif()
message(STATUS "${generation_output}")

enable_testing()
add_test(NAME vu_native_input_parser
    COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
        "PYTHONPATH=${generator_directory}"
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_vu_native_input.py")
add_subdirectory("${derived_fixture}" "${CMAKE_CURRENT_BINARY_DIR}/verifier-build")
