cmake_minimum_required(VERSION 3.21)
project(kfiv_ee_clock_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_runtime()



set(RUNTIME_CONFIG "RelWithDebInfo" CACHE STRING "Existing runtime library configuration")
set(FFMPEG_DIR "${RUNTIME_BUILD_DIR}/ThirdParty/ffmpeg-prefix/src/ffmpeg_external" CACHE PATH "FFmpeg package root")
if(NOT MSVC)
    message(FATAL_ERROR "This package verifies the native Windows MSVC runtime")
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

function(add_ee_clock_test target)
    set(fixture ee_clock_tests.cpp)
    if(ARGC GREATER 1)
        set(fixture "${ARGV1}")
    endif()
    add_executable(${target} ${fixture} "${PS2RECOMP_DIR}/ps2xTest/src/test_function_table.cpp")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj /fp:strict)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS PS2X_HAS_FFMPEG=1)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRuntime/include" "${PS2RECOMP_DIR}/ps2xIOP/include")
    target_link_libraries(${target} PRIVATE
        "${RUNTIME_BUILD_DIR}/ps2xRuntime/${RUNTIME_CONFIG}/ps2_runtime.lib"
        "${RUNTIME_BUILD_DIR}/ps2xIOP/${RUNTIME_CONFIG}/ps2_iop.lib"
        "${RUNTIME_BUILD_DIR}/_deps/raylib-build/raylib/${RUNTIME_CONFIG}/raylib.lib"
        ${vulkan_runtime_libraries}
        "${FFMPEG_DIR}/bin/avcodec.lib" "${FFMPEG_DIR}/bin/avformat.lib"
        "${FFMPEG_DIR}/bin/avutil.lib" "${FFMPEG_DIR}/bin/swresample.lib" "${FFMPEG_DIR}/bin/swscale.lib"
        opengl32 glu32 winmm bcrypt secur32 ws2_32 user32 advapi32 ole32 shell32 gdi32)
    target_link_options(${target} PRIVATE /STACK:16777216)
    add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${FFMPEG_DIR}/bin" "-Ddest_dir=$<TARGET_FILE_DIR:${target}>"
        -P "${PS2RECOMP_DIR}/ps2xRuntime/cmake/CopyFfmpegDlls.cmake")
endfunction()

add_ee_clock_test(kfiv_ee_clock_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(scheduler_source "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp")
set(control_generator "${CMAKE_CURRENT_SOURCE_DIR}/prepare_clock_control.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${scheduler_source}" "${control_generator}")
foreach(mode IN ITEMS no-wall-floor bulk-credit)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${control_generator}"
        --source "${scheduler_source}" --output "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-${mode}.cpp"
        --mode "${mode}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Review ${mode} control generation: ${output}${error}")
    endif()
endforeach()

add_ee_clock_test(kfiv_ee_clock_both_control)
target_sources(kfiv_ee_clock_both_control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-no-wall-floor.cpp")
target_compile_definitions(kfiv_ee_clock_both_control PRIVATE EXPECT_BOTH_CLOCKS_CONTROL=1)
add_ee_clock_test(kfiv_ee_wall_clock wall_clock_tests.cpp)
add_ee_clock_test(kfiv_ee_wall_clock_control wall_clock_tests.cpp)
target_sources(kfiv_ee_wall_clock_control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-no-wall-floor.cpp")
target_compile_definitions(kfiv_ee_wall_clock_control PRIVATE EXPECT_CLOCK_UNDERCREDIT=1)
add_ee_clock_test(kfiv_ee_chronological chronological_clock_tests.cpp)
add_ee_clock_test(kfiv_ee_bulk_control chronological_clock_tests.cpp)
target_sources(kfiv_ee_bulk_control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/EeScheduler-bulk-credit.cpp")
target_compile_definitions(kfiv_ee_bulk_control PRIVATE EXPECT_LOST_ROLLOVERS=1)

enable_testing()
add_test(NAME kfiv_ee_busy_poll_clock_regression COMMAND kfiv_ee_clock_tests)
add_test(NAME kfiv_ee_no_wall_floor_control COMMAND kfiv_ee_clock_both_control)
foreach(mode IN ITEMS normal fragmented long-isr masked no-handler reset-cycle idle instruction-ledger native-mmio)
    add_test(NAME kfiv_ee_wall_clock_${mode} COMMAND kfiv_ee_wall_clock ${mode})
endforeach()
add_test(NAME kfiv_ee_native_undercredit_control COMMAND kfiv_ee_wall_clock_control normal)
foreach(mode IN ITEMS simple fragmented masked no-handler simultaneous multiple blocking alarm callback-wait callback-poll)
    add_test(NAME kfiv_ee_chronological_${mode} COMMAND kfiv_ee_chronological ${mode})
endforeach()
add_test(NAME kfiv_ee_bulk_credit_control COMMAND kfiv_ee_bulk_control fragmented)
get_property(all_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${all_tests} PROPERTIES TIMEOUT 15)
