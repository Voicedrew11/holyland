# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.21)
project(kfiv_dma_chain_tests LANGUAGES CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
find_package(Threads REQUIRED)
set(GS_SOURCE_DIR "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/gs")

# Compile the actual frontend and CPU backend: FINISH is tested via GS CSR,
# including the actual VIF DIRECT and GIF arbiter paths, without a window/GPU.
add_library(dma_chain_graphics STATIC
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp"
    "${GS_SOURCE_DIR}/ps2_gif_arbiter.cpp"
    "${GS_SOURCE_DIR}/gs_frontend.cpp"
    "${GS_SOURCE_DIR}/gs_cpu_backend.cpp"
    "${GS_SOURCE_DIR}/ps2_gs_memory.cpp"
    "${GS_SOURCE_DIR}/gs_trace.cpp"
    "${GS_SOURCE_DIR}/gs_record.cpp")
target_compile_features(dma_chain_graphics PUBLIC cxx_std_20)
target_include_directories(dma_chain_graphics PUBLIC
    "${PS2RECOMP_DIR}/ps2xRuntime/include" "${GS_SOURCE_DIR}")
target_link_libraries(dma_chain_graphics PUBLIC Threads::Threads)

function(add_dma_chain_test target memory_source)
    add_executable(${target} dma_chain_tests.cpp "${memory_source}")
    target_link_libraries(${target} PRIVATE dma_chain_graphics)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()
if(MSVC)
    target_compile_options(dma_chain_graphics PRIVATE /W4 /permissive- /bigobj)
    target_compile_definitions(dma_chain_graphics PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(MEMORY_SOURCE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_memory.cpp")
add_dma_chain_test(kfiv_dma_chain_tests "${MEMORY_SOURCE}")

# Restore only the old 4096-tag guard in build scratch. Keep the actual
# memory/MMIO, VIF, arbiter, frontend and raster source otherwise identical.
file(READ "${MEMORY_SOURCE}" MEMORY_TEXT)
string(REPLACE "\r\n" "\n" MEMORY_TEXT "${MEMORY_TEXT}")
set(CYCLE_START "                    // A finite source chain can contain more than 4096 tags.")
set(CYCLE_END "                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;")
string(FIND "${MEMORY_TEXT}" "${CYCLE_START}" CYCLE_START_OFFSET)
string(FIND "${MEMORY_TEXT}" "${CYCLE_END}" CYCLE_END_OFFSET)
if(CYCLE_START_OFFSET LESS 0 OR CYCLE_END_OFFSET LESS CYCLE_START_OFFSET)
    message(FATAL_ERROR "Review the prior DMA guard control against the current chain walker")
endif()
math(EXPR CYCLE_LENGTH "${CYCLE_END_OFFSET} - ${CYCLE_START_OFFSET}")
string(SUBSTRING "${MEMORY_TEXT}" ${CYCLE_START_OFFSET} ${CYCLE_LENGTH} CYCLE_TEXT)
string(REPLACE "${CYCLE_TEXT}"
    "                    const int kMaxChainTags = 4096;\n                    int tagsProcessed = 0;\n"
    CONTROL_TEXT "${MEMORY_TEXT}")
set(LOOP_START "                    while (true)\n                    {\n                        if (tagsProcessed != 0u)")
string(FIND "${CONTROL_TEXT}" "${LOOP_START}" LOOP_START_OFFSET)
string(FIND "${CONTROL_TEXT}" "                        const uint32_t currentTagAddr = tagAddr;" LOOP_END_OFFSET)
if(LOOP_START_OFFSET LESS 0 OR LOOP_END_OFFSET LESS LOOP_START_OFFSET)
    message(FATAL_ERROR "Review the prior DMA loop control against the current chain walker")
endif()
math(EXPR LOOP_LENGTH "${LOOP_END_OFFSET} - ${LOOP_START_OFFSET}")
string(SUBSTRING "${CONTROL_TEXT}" ${LOOP_START_OFFSET} ${LOOP_LENGTH} LOOP_TEXT)
string(REPLACE "${LOOP_TEXT}" "                    while (tagsProcessed < kMaxChainTags)\n                    {\n"
    CONTROL_TEXT "${CONTROL_TEXT}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ps2_memory_prior.cpp" "${CONTROL_TEXT}")
add_dma_chain_test(kfiv_dma_chain_prior_control "${CMAKE_CURRENT_BINARY_DIR}/ps2_memory_prior.cpp")

enable_testing()
add_test(NAME kfiv_dma_finite_chain_regression COMMAND kfiv_dma_chain_tests)
add_test(NAME kfiv_dma_prior_limit_control COMMAND kfiv_dma_chain_prior_control)
set_tests_properties(kfiv_dma_finite_chain_regression kfiv_dma_prior_limit_control PROPERTIES
    TIMEOUT 30 ENVIRONMENT "PS2X_GS_THREADS=1;PS2X_GS_BACKEND=cpu;PS2X_GS_TRACE=;PS2X_GS_RECORD=")
set_tests_properties(kfiv_dma_prior_limit_control PROPERTIES WILL_FAIL TRUE)
