cmake_minimum_required(VERSION 3.21)
project(kfiv_ipu_input_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")

set(IPU_MEMORY_SOURCE "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_memory.cpp")
function(configure_ipu_fixture target memory_source)
    add_executable(${target} ipu_input_tests.cpp unrelated_graphics_stubs.cpp "${memory_source}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${PS2RECOMP_DIR}/ps2xRuntime/include")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()
configure_ipu_fixture(kfiv_ipu_input_tests "${IPU_MEMORY_SOURCE}")
enable_testing()
add_test(NAME kfiv_ipu_input_dma_regression COMMAND kfiv_ipu_input_tests)

option(IPU_RESUME_OLD_CONTROL "Compare a scratch source copy with the old CHCR resume cache" OFF)
if(IPU_RESUME_OLD_CONTROL)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(IPU_CONTROL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/resume-control/ps2_memory.cpp")
    add_custom_command(OUTPUT "${IPU_CONTROL_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_resume_control.py"
            "${IPU_MEMORY_SOURCE}" "${IPU_CONTROL_SOURCE}"
        DEPENDS "${IPU_MEMORY_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_resume_control.py"
        VERBATIM)
    configure_ipu_fixture(kfiv_ipu_input_old_control "${IPU_CONTROL_SOURCE}")
    target_compile_definitions(kfiv_ipu_input_old_control PRIVATE IPU_RESUME_OLD_CONTROL=1)
    add_test(NAME kfiv_ipu_input_dma_resume_old_control COMMAND kfiv_ipu_input_old_control)
endif()

option(IPU_ACTIVE_CHCR_OLD_CONTROL "Compare a scratch source copy with active CHCR overwrites" OFF)
if(IPU_ACTIVE_CHCR_OLD_CONTROL)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(IPU_ACTIVE_CONTROL_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/active-chcr-control/ps2_memory.cpp")
    add_custom_command(OUTPUT "${IPU_ACTIVE_CONTROL_SOURCE}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_active_chcr_control.py"
            "${IPU_MEMORY_SOURCE}" "${IPU_ACTIVE_CONTROL_SOURCE}"
        DEPENDS "${IPU_MEMORY_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_active_chcr_control.py"
        VERBATIM)
    configure_ipu_fixture(kfiv_ipu_active_chcr_old_control "${IPU_ACTIVE_CONTROL_SOURCE}")
    target_compile_definitions(kfiv_ipu_active_chcr_old_control PRIVATE IPU_ACTIVE_CHCR_OLD_CONTROL=1)
    add_test(NAME kfiv_ipu_input_active_chcr_old_control COMMAND kfiv_ipu_active_chcr_old_control)
endif()
