cmake_minimum_required(VERSION 3.21)
project(kfiv_gs_alignment_tests LANGUAGES CXX)

set(PS2RECOMP_DIR "" CACHE PATH "Patched PS2Recomp checkout")
if(NOT PS2RECOMP_DIR OR NOT EXISTS "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/gs/gs_cpu_backend.cpp")
    message(FATAL_ERROR "Set PS2RECOMP_DIR to a patched PS2Recomp checkout")
endif()
set(GS_SOURCE_DIR "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/gs")
# Optional local controls use an external source file; no prior renderer or
# game data is included in this source-only package.
set(GS_ALIGNMENT_PRIOR_SOURCE "" CACHE FILEPATH "Optional pre-fix backend source for a failing control")
find_package(Threads REQUIRED)

function(add_alignment_target target source)
    add_executable(${target} gs_alignment_tests.cpp "${source}"
        "${GS_SOURCE_DIR}/ps2_gs_memory.cpp" "${GS_SOURCE_DIR}/gs_trace.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include" "${GS_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj /fp:strict)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

add_alignment_target(kfiv_gs_alignment_tests "${GS_SOURCE_DIR}/gs_cpu_backend.cpp")
enable_testing()
foreach(workers 1 8)
    add_test(NAME kfiv_gs_alignment_${workers}_workers COMMAND kfiv_gs_alignment_tests)
    set_tests_properties(kfiv_gs_alignment_${workers}_workers PROPERTIES
        ENVIRONMENT "PS2X_GS_THREADS=${workers}" TIMEOUT 60)
endforeach()
if(GS_ALIGNMENT_PRIOR_SOURCE)
    if(NOT EXISTS "${GS_ALIGNMENT_PRIOR_SOURCE}")
        message(FATAL_ERROR "GS_ALIGNMENT_PRIOR_SOURCE does not exist")
    endif()
    add_alignment_target(kfiv_gs_alignment_prior_control "${GS_ALIGNMENT_PRIOR_SOURCE}")
    add_test(NAME kfiv_gs_alignment_prior_control COMMAND kfiv_gs_alignment_prior_control)
    set_tests_properties(kfiv_gs_alignment_prior_control PROPERTIES
        ENVIRONMENT "PS2X_GS_THREADS=1" WILL_FAIL TRUE TIMEOUT 60)
endif()
