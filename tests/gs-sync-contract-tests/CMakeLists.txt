# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(gs_sync_contract_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_runtime()



set(RUNTIME_CONFIG "RelWithDebInfo" CACHE STRING "Configuration of existing runtime libraries")
set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")

if(NOT MSVC)
    message(FATAL_ERROR "These focused tests validate the native Windows MSVC runtime")
endif()

# A static runtime built with Vulkan retains its renderer's link dependencies
# even though this fixture does not render or open a GPU window. Use only the
# matching libraries already produced by the explicit runtime build.
file(STRINGS "${RUNTIME_BUILD_DIR}/CMakeCache.txt" runtime_vulkan_enabled
    REGEX "^PS2X_ENABLE_VULKAN_GS:BOOL=ON$")
set(vulkan_runtime_libraries)
if(runtime_vulkan_enabled)
    foreach(library IN ITEMS
        "parallel-gs/gs/${RUNTIME_CONFIG}/parallel-gs.lib"
        "parallel-gs/Granite/vulkan/${RUNTIME_CONFIG}/granite-vulkan.lib"
        "parallel-gs/Granite/third_party/${RUNTIME_CONFIG}/granite-volk.lib"
        "parallel-gs/Granite/util/${RUNTIME_CONFIG}/granite-util.lib"
        "parallel-gs/Granite/math/${RUNTIME_CONFIG}/granite-math.lib")
        if(NOT EXISTS "${RUNTIME_BUILD_DIR}/${library}")
            message(FATAL_ERROR "Rebuild the matching Vulkan runtime dependency: ${library}")
        endif()
        list(APPEND vulkan_runtime_libraries "${RUNTIME_BUILD_DIR}/${library}")
    endforeach()
endif()



find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(control_dir "${CMAKE_CURRENT_BINARY_DIR}/generated-controls")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_sources.py"
    --runtime "${PS2RECOMP_DIR}/ps2xRuntime" --output "${control_dir}"
    RESULT_VARIABLE source_result OUTPUT_VARIABLE source_output ERROR_VARIABLE source_error)
if(NOT source_result EQUAL 0)
    message(FATAL_ERROR "SDK control preparation failed: ${source_output}${source_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare_sources.py"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls/Interrupt.cpp")

function(add_contract target fixture scheduler interrupt)
    add_executable(${target} ${fixture} ${scheduler} ${interrupt}
        "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj /fp:strict)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS
        PS2_RUNTIME_LOGS=1 PS2X_ENABLE_IOP_RPC_TRACE=0 PS2X_HAS_FFMPEG=1 PS2X_HAS_VULKAN_GS=1)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel"
        "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls"
        "${PS2RECOMP_DIR}/ps2xRuntime/src/lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-src/src"
        "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/${RUNTIME_CONFIG}/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/${RUNTIME_CONFIG}/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/${RUNTIME_CONFIG}/raylib.lib"
        ${vulkan_runtime_libraries}
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib"
        "${FFMPEG_DIR}/bin/swscale.lib"
        bcrypt secur32 ws2_32 opengl32 glu32 winmm kernel32 user32 gdi32
        winspool shell32 ole32 oleaut32 uuid comdlg32 advapi32)
    target_link_options(${target} PRIVATE /STACK:8388608 /LTCG)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()
set(actual_scheduler "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp")
set(actual_interrupt "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/Syscalls/Interrupt.cpp")
add_contract(gs_sync_contract_actual contract_tests.cpp "${actual_scheduler}" "${actual_interrupt}")
add_contract(gs_sync_contract_csr_control contract_tests.cpp "${control_dir}/csr_control_scheduler.cpp" "${actual_interrupt}")
add_contract(gs_sync_contract_field_control contract_tests.cpp "${control_dir}/field_control_scheduler.cpp" "${control_dir}/wake_control_interrupt.cpp")
add_contract(gs_sync_delayed_actual delayed_tests.cpp "${actual_scheduler}" "${actual_interrupt}")
add_contract(gs_sync_delayed_wake_control delayed_tests.cpp "${actual_scheduler}" "${control_dir}/wake_control_interrupt.cpp")
enable_testing()
foreach(mode IN ITEMS interlace progressive fixed)
    add_test(NAME actual_sdk_${mode}_contract COMMAND gs_sync_contract_actual ${mode})
    add_test(NAME tick_counter_csr_${mode}_negative_control COMMAND gs_sync_contract_csr_control ${mode} csr)
    set_tests_properties(actual_sdk_${mode}_contract PROPERTIES TIMEOUT 10)
    set_tests_properties(tick_counter_csr_${mode}_negative_control PROPERTIES TIMEOUT 10)
endforeach()
add_test(NAME inverse_field_negative_control COMMAND gs_sync_contract_field_control interlace field)
set_tests_properties(inverse_field_negative_control PROPERTIES TIMEOUT 10)
add_test(NAME actual_sdk_delayed_return_contract COMMAND gs_sync_delayed_actual)
set_tests_properties(actual_sdk_delayed_return_contract PROPERTIES TIMEOUT 10)
add_test(NAME wake_sample_delayed_negative_control COMMAND gs_sync_delayed_wake_control --expect-wake-only)
set_tests_properties(wake_sample_delayed_negative_control PROPERTIES TIMEOUT 10)
