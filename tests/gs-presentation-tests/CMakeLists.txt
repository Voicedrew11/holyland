# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.21)
project(gs_presentation_tests LANGUAGES C CXX)
if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(PS2RECOMP_DIR "" CACHE PATH "Full patched W source checkout")
set(GS_PRIOR_RUNTIME_DIR "" CACHE PATH "Explicit pre-W ps2xRuntime source directory")
set(PARALLEL_GS_DIR "" CACHE PATH "Pinned paraLLEl-GS source with minimal submodules")
if(NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/gs/gs_frontend.cpp" OR
        NOT EXISTS "${GS_PRIOR_RUNTIME_DIR}/src/lib/gs/gs_frontend.cpp" OR
        NOT EXISTS "${PARALLEL_GS_DIR}/gs/gs_interface.cpp")
    message(FATAL_ERROR "Supply all three actual source directories explicitly")
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CooperativeRuntimeFixtures.cmake")
set(runtime "${PS2RECOMP_DIR}/ps2xRuntime")
set(gs "${runtime}/src/lib/gs")
set(derived "${CMAKE_CURRENT_BINARY_DIR}/derived")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_sources.py"
    --candidate-runtime "${runtime}" --prior-runtime "${GS_PRIOR_RUNTIME_DIR}" --output "${derived}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/prepare_sources.py" "${CMAKE_CURRENT_LIST_DIR}/prior-frontend-hashes.json"
    "${runtime}/src/lib/ps2_runtime.cpp" "${runtime}/include/runtime/gs/gs_frontend.h"
    "${GS_PRIOR_RUNTIME_DIR}/include/runtime/gs/gs_frontend.h"
    "${GS_PRIOR_RUNTIME_DIR}/src/lib/gs/gs_frontend.cpp")
set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
set(GRANITE_TOOLS OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERDOC_CAPTURE OFF CACHE BOOL "" FORCE)
set(GRANITE_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_DXGI_INTEROP OFF CACHE BOOL "" FORCE)
add_subdirectory("${PARALLEL_GS_DIR}" parallel-gs EXCLUDE_FROM_ALL)
add_executable(gs_presentation_admission presentation_tests.cpp "${gs}/gs_frontend.cpp"
    "${derived}/gs_frontend_reference.cpp" "${gs}/gs_cpu_backend.cpp" "${gs}/ps2_gs_memory.cpp"
    "${gs}/gs_trace.cpp" "${gs}/ps2_gif_arbiter.cpp" "${gs}/gs_vulkan_backend.cpp" "${gs}/gs_vulkan_scanout.cpp")
cooperative_fixture_flags(gs_presentation_admission)
target_include_directories(gs_presentation_admission PRIVATE "${derived}" "${runtime}/include" "${gs}")
target_compile_definitions(gs_presentation_admission PRIVATE GRANITE_SHIPPING)
target_link_libraries(gs_presentation_admission PRIVATE Threads::Threads parallel-gs)
add_executable(gs_upload_frame_state upload_frame_state.cpp)
cooperative_fixture_flags(gs_upload_frame_state)
target_include_directories(gs_upload_frame_state PRIVATE "${derived}")
enable_testing()
add_test(NAME gs_host_admission_upload_state COMMAND gs_upload_frame_state)
foreach(case unchanged mutation movie mode pending reset concurrent replacement)
    add_test(NAME gs_host_admission_${case} COMMAND gs_presentation_admission ${case})
    set_tests_properties(gs_host_admission_${case} PROPERTIES TIMEOUT 60
        ENVIRONMENT "PS2X_GS_THREADS=1;PS2X_GS_TRACE=;PS2X_GS_RECORD=;PS2X_NO_WEAVE=0"
        FAIL_REGULAR_EXPRESSION "Thread does not exist in thread manager")
endforeach()
