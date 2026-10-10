# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.21)
project(kfiv_vu_performance_verify LANGUAGES CXX)

set(VU_CANDIDATE_RUNTIME_DIR "" CACHE PATH "Candidate checkout's ps2xRuntime directory (required)")
set(VU_REFERENCE_RUNTIME_DIR "" CACHE PATH "Separate patch-39 checkout's ps2xRuntime directory (required)")
set(VU_PRIVATE_REPLAY "" CACHE FILEPATH "Optional privately owned contiguous VU recording")
option(VU_VERIFY_OB2 "Use /Ob2 on candidate VU units only (MSVC)" OFF)
option(VU_VERIFY_LTCG "Use /GL on candidate VU units only, and /LTCG at link (MSVC)" OFF)

if(NOT VU_CANDIDATE_RUNTIME_DIR OR NOT VU_REFERENCE_RUNTIME_DIR)
    message(FATAL_ERROR "Set both VU_CANDIDATE_RUNTIME_DIR and VU_REFERENCE_RUNTIME_DIR explicitly.")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
# The VIF/GS stubs isolate VU arithmetic; this allocator is a separate Q source input.
set(VU_MEMORY_RUNTIME_DIR "" CACHE PATH "Explicit pinned Q ps2xRuntime allocator source")
if(NOT EXISTS "${VU_MEMORY_RUNTIME_DIR}/src/lib/ps2_memory.cpp")
    message(FATAL_ERROR "Supply the separate VU_MEMORY_RUNTIME_DIR; no runtime source is bundled")
endif()
foreach(role IN ITEMS src/lib/ps2_memory.cpp include/runtime/ps2_memory.h)
    file(READ "${VU_MEMORY_RUNTIME_DIR}/${role}" allocator_source)
    string(REPLACE "\r\n" "\n" allocator_source "${allocator_source}")
    string(SHA256 allocator_hash "${allocator_source}")
    if(role STREQUAL "src/lib/ps2_memory.cpp")
        set(expected "daae9c938d62ed4075ea8513ddba89b5ea846322b7e1b0acc49e87002a7b0795")
    else()
        set(expected "6031945c7d94ecc0dcaed658326f161a18825389dd15aa68dbca39842c28958e")
    endif()
    if(NOT allocator_hash STREQUAL expected)
        message(FATAL_ERROR "Review the isolated Q allocator source binding: ${role}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VU_MEMORY_RUNTIME_DIR}/${role}")
endforeach()

set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(vu_files include/runtime/ps2_vu1.h src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp src/lib/vu/ps2_vu1_lower.cpp src/lib/vu/ps2_vu1_detail.h)
set(configure_inputs "${CMAKE_CURRENT_SOURCE_DIR}/prepare_sources.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/reference39-hashes.json")
foreach(file IN LISTS vu_files)
    list(APPEND configure_inputs "${VU_CANDIDATE_RUNTIME_DIR}/${file}" "${VU_REFERENCE_RUNTIME_DIR}/${file}")
endforeach()
# Track optional upstream lift support without requiring it in patch 39.
file(GLOB candidate_lift_inputs CONFIGURE_DEPENDS
    "${VU_CANDIDATE_RUNTIME_DIR}/include/runtime/ps2_vu1_lift.h"
    "${VU_CANDIDATE_RUNTIME_DIR}/src/lib/vu/ps2_vu1_lift.cpp")
list(APPEND configure_inputs ${candidate_lift_inputs})
file(GLOB authored_fixture_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${configure_inputs} ${authored_fixture_inputs})
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_sources.py"
    --candidate-runtime "${VU_CANDIDATE_RUNTIME_DIR}" --reference-runtime "${VU_REFERENCE_RUNTIME_DIR}"
    --output "${generated}" RESULT_VARIABLE preparation_status OUTPUT_VARIABLE preparation_output
    ERROR_VARIABLE preparation_error)
if(NOT preparation_status EQUAL 0)
    message(FATAL_ERROR "${preparation_error}")
endif()
message(STATUS "${preparation_output}")

set(candidate_sources "${generated}/candidate/src/lib/vu/ps2_vu1_core.cpp"
    "${generated}/candidate/src/lib/vu/ps2_vu1_upper.cpp"
    "${generated}/candidate/src/lib/vu/ps2_vu1_lower.cpp")
set(candidate_lift_sources)
include("${generated}/candidate-lift.cmake")
if(VU_VERIFY_HAS_STATIC_LIFT)
    list(APPEND candidate_lift_sources "${generated}/candidate/src/lib/vu/ps2_vu1_lift.cpp")
endif()
set(reference_sources "${generated}/reference/src/lib/vu/ps2_vu1_core.cpp"
    "${generated}/reference/src/lib/vu/ps2_vu1_upper.cpp"
    "${generated}/reference/src/lib/vu/ps2_vu1_lower.cpp")
add_executable(kfiv_vu_performance_verify verifier.cpp unrelated_stubs.cpp
    "${VU_MEMORY_RUNTIME_DIR}/src/lib/ps2_memory.cpp"
    ${candidate_sources} ${candidate_lift_sources} ${reference_sources})
target_compile_features(kfiv_vu_performance_verify PRIVATE cxx_std_20)
set_property(TARGET kfiv_vu_performance_verify PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
target_include_directories(kfiv_vu_performance_verify PRIVATE
    "${generated}/candidate/include" "${generated}/reference/include" "${VU_MEMORY_RUNTIME_DIR}/include")
if(MSVC)
    target_compile_options(kfiv_vu_performance_verify PRIVATE /W4 /permissive- /bigobj /fp:strict)
    target_compile_definitions(kfiv_vu_performance_verify PRIVATE _CRT_SECURE_NO_WARNINGS)
    # CMake Release normally supplies /Ob2 globally. Override it per unit so
    # optional inlining/LTCG changes never alter the scalar reference build.
    set_source_files_properties(${reference_sources}
        PROPERTIES COMPILE_OPTIONS "/Ob1;/GL-")
    set(candidate_options /Ob1)
    if(VU_VERIFY_OB2)
        set(candidate_options /Ob2)
    endif()
    if(VU_VERIFY_LTCG)
        list(APPEND candidate_options /GL)
        target_link_options(kfiv_vu_performance_verify PRIVATE /LTCG)
    else()
        list(APPEND candidate_options /GL-)
    endif()
    set_source_files_properties(${candidate_sources} ${candidate_lift_sources}
        PROPERTIES COMPILE_OPTIONS "${candidate_options}")
else()
    if(VU_VERIFY_OB2 OR VU_VERIFY_LTCG)
        message(FATAL_ERROR "VU_VERIFY_OB2 and VU_VERIFY_LTCG require MSVC.")
    endif()
    target_compile_options(kfiv_vu_performance_verify PRIVATE -frounding-math -ffp-contract=off -fno-fast-math)
    if(candidate_lift_sources)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" fixture_processor)
        if(NOT fixture_processor MATCHES "^(x86_64|amd64|i[3-6]86|x86)$" OR
                NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
            message(FATAL_ERROR "The candidate's upstream static-lift support requires an x86 SSE4.1 compiler.")
        endif()
        # Its header uses SSE4.1. The rest of the fixture retains its normal
        # architecture options and the lift remains disabled during tests.
        set_property(SOURCE ${candidate_lift_sources} APPEND PROPERTY COMPILE_OPTIONS -msse4.1)
    endif()
endif()

enable_testing()
add_test(NAME vu_decoded_metadata_exact COMMAND kfiv_vu_performance_verify --metadata)
add_test(NAME vu_verifier_decoded_metadata_negative_control
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_negative_control.py"
    --executable "$<TARGET_FILE:kfiv_vu_performance_verify>" --category "decoded-metadata")
add_test(NAME vu_current39_exact_synthetic COMMAND kfiv_vu_performance_verify)
foreach(kind IN ITEMS registers flags cycles memory packet)
    add_test(NAME "vu_verifier_${kind}_negative_control"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_negative_control.py"
        --executable "$<TARGET_FILE:kfiv_vu_performance_verify>" --category "${kind}")
endforeach()
if(VU_PRIVATE_REPLAY)
    if(NOT EXISTS "${VU_PRIVATE_REPLAY}")
        message(FATAL_ERROR "VU_PRIVATE_REPLAY does not exist; supply a private recording explicitly.")
    endif()
    add_test(NAME vu_current39_private_replay COMMAND kfiv_vu_performance_verify "--replay=${VU_PRIVATE_REPLAY}")
    set_tests_properties(vu_current39_private_replay PROPERTIES TIMEOUT 600 LABELS "private-replay")
endif()
set_tests_properties(vu_current39_exact_synthetic PROPERTIES TIMEOUT 180)
get_property(vu_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${vu_registered_tests} PROPERTIES ENVIRONMENT "PS2X_VU1_LIFT=0")

add_test(NAME vu_host_quantum_cases COMMAND kfiv_vu_performance_verify --stepping)
add_test(NAME vu_host_job_existing_synthetic COMMAND kfiv_vu_performance_verify --host-jobs)
add_test(NAME vu_host_job_lift_bypass COMMAND kfiv_vu_performance_verify --job-lift-bypass)
set_tests_properties(vu_host_quantum_cases vu_host_job_existing_synthetic PROPERTIES ENVIRONMENT "PS2X_VU1_LIFT=0")
set_tests_properties(vu_host_job_lift_bypass PROPERTIES ENVIRONMENT "PS2X_VU1_LIFT=1")
