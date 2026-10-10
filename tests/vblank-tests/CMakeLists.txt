cmake_minimum_required(VERSION 3.20)
project(kfiv_vblank_tests LANGUAGES CXX)
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

function(add_vblank_test target)
    set(fixture vblank_tests.cpp)
    if(ARGC GREATER 1)
        set(fixture "${ARGV1}")
    endif()
    add_executable(${target} ${fixture}
        "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /fp:strict)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS
        PS2_RUNTIME_LOGS=1 PS2X_ENABLE_IOP_RPC_TRACE=0 PS2X_HAS_FFMPEG=1)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/${RUNTIME_CONFIG}/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/${RUNTIME_CONFIG}/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/${RUNTIME_CONFIG}/raylib.lib"
        ${vulkan_runtime_libraries}
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib"
        "${FFMPEG_DIR}/bin/swscale.lib"
        bcrypt secur32 ws2_32
        opengl32 glu32 winmm kernel32 user32 gdi32 winspool shell32 ole32
        oleaut32 uuid comdlg32 advapi32)
    target_link_options(${target} PRIVATE /STACK:8388608)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()

add_vblank_test(kfiv_vblank_tests)

# Compiling the captured original scheduler supplies its symbols before the
# static runtime library, so that executable exercises the real old scheduler.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/control/EeScheduler.cpp")
    add_vblank_test(kfiv_vblank_burst_control)
    target_sources(kfiv_vblank_burst_control PRIVATE control/EeScheduler.cpp)
    # The pinned old scheduler predates these interfaces. Supply no-op
    # compatibility definitions only to this negative control, preventing the
    # linker from pulling a second, current scheduler from the runtime archive.
    target_sources(kfiv_vblank_burst_control PRIVATE control/current_api_adapter.cpp)
    target_compile_definitions(kfiv_vblank_burst_control PRIVATE EXPECT_ORIGINAL_BURST=1)
endif()

enable_testing()
add_test(NAME kfiv_vblank_pacing_regression COMMAND kfiv_vblank_tests)
add_test(NAME kfiv_vblank_bounded_recovery COMMAND kfiv_vblank_tests --late-opposite)
add_test(NAME kfiv_vblank_overperiod_recovery COMMAND kfiv_vblank_tests --late-opposite-35)
if(TARGET kfiv_vblank_burst_control)
    add_test(NAME kfiv_vblank_original_burst_control COMMAND kfiv_vblank_burst_control)
    set_tests_properties(kfiv_vblank_original_burst_control PROPERTIES TIMEOUT 10)
endif()
set_tests_properties(kfiv_vblank_pacing_regression kfiv_vblank_bounded_recovery
    kfiv_vblank_overperiod_recovery PROPERTIES TIMEOUT 10)

# Both executables use the full rebuilt runtime's class layout and allocator.
# The control recompiles only the actual scheduler with the two independently
# testable dispatch protections disabled; it does not modify the checkout.
add_vblank_test(kfiv_vblank_handoff_tests handoff_tests.cpp)
add_vblank_test(kfiv_vblank_handoff_control handoff_tests.cpp)
target_sources(kfiv_vblank_handoff_control PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp")
target_compile_definitions(kfiv_vblank_handoff_control PRIVATE
    PS2X_VBLANK_HANDOFF=0 PS2X_VBLANK_HOST_ELIGIBILITY=0 EXPECT_MISSED_FIELDS=1)
foreach(mode IN ITEMS debt alarm multiple suspended suspend-ready delete-ready
    self-delete nested-wait nested-completion nested-missing nested-cancel
    polling priority stop no-waiters work35)
    add_test(NAME kfiv_vblank_handoff_${mode} COMMAND kfiv_vblank_handoff_tests ${mode})
    set_tests_properties(kfiv_vblank_handoff_${mode} PROPERTIES TIMEOUT 10)
endforeach()
add_test(NAME kfiv_vblank_continuous_only_control COMMAND kfiv_vblank_handoff_control debt)
set_tests_properties(kfiv_vblank_continuous_only_control PROPERTIES TIMEOUT 10)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(mode_header_generator "${CMAKE_CURRENT_SOURCE_DIR}/prepare_mode_fixture.py")
set(mode_proof_include "${CMAKE_CURRENT_BINARY_DIR}/mode-proof-include")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PS2RECOMP_DIR}/ps2xRuntime/include/runtime/ee_scheduler.h" "${mode_header_generator}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${mode_header_generator}"
    --runtime "${PS2RECOMP_DIR}/ps2xRuntime" --output "${mode_proof_include}"
    RESULT_VARIABLE mode_prepare_result OUTPUT_VARIABLE mode_prepare_output ERROR_VARIABLE mode_prepare_error)
if(NOT mode_prepare_result EQUAL 0)
    message(FATAL_ERROR "Review mode fixture header preparation: ${mode_prepare_output}${mode_prepare_error}")
endif()
add_vblank_test(kfiv_gs_field_period_modes mode_tests.cpp)
# Friend access exists only in the mechanically prepared private header. It
# adds no data members and uses the fully rebuilt canonical runtime allocator.
target_include_directories(kfiv_gs_field_period_modes BEFORE PRIVATE "${mode_proof_include}")
target_include_directories(kfiv_gs_field_period_modes PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel")
add_test(NAME kfiv_gs_field_period_modes_transitions COMMAND kfiv_gs_field_period_modes)
set_tests_properties(kfiv_gs_field_period_modes_transitions PROPERTIES TIMEOUT 30)
