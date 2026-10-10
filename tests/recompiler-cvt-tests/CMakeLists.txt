cmake_minimum_required(VERSION 3.21)
project(kfiv_recompiler_cvt_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_tools()
if(NOT MSVC)
    message(FATAL_ERROR "This fixture is verified with native Windows MSVC")
endif()

add_executable(emit_cvt emit_cases.cpp
    "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/fpu_translator.cpp")
target_compile_features(emit_cvt PRIVATE cxx_std_20)
target_compile_definitions(emit_cvt PRIVATE LIBDWARF_STATIC _CRT_SECURE_NO_WARNINGS)
target_compile_options(emit_cvt PRIVATE /W4 /permissive- /bigobj /utf-8)
target_include_directories(emit_cvt PRIVATE
    "${PS2RECOMP_DIR}/ps2xRecomp/include"
    "${TOOLS_BUILD_DIR}/_deps/fmt-src/include"
    "${TOOLS_BUILD_DIR}/_deps/elfio-src"
    "${TOOLS_BUILD_DIR}/_deps/toml11-src/include"
    "${TOOLS_BUILD_DIR}/_deps/libdwarf-src/src/lib/libdwarf")
target_link_libraries(emit_cvt PRIVATE
    "${TOOLS_BUILD_DIR}/ps2xRecomp/${TOOLS_CONFIG}/ps2_recomp_lib.lib"
    "${TOOLS_BUILD_DIR}/_deps/fmt-build/${TOOLS_CONFIG}/fmt.lib"
    "${TOOLS_BUILD_DIR}/_deps/libdwarf-build/src/lib/libdwarf/${TOOLS_CONFIG}/dwarf.lib"
    "${TOOLS_BUILD_DIR}/ps2xRecomp/${TOOLS_CONFIG}/rabbitizer.lib")

set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated_cases.h")
add_custom_command(OUTPUT "${GENERATED}"
    COMMAND $<TARGET_FILE:emit_cvt> "${GENERATED}"
    DEPENDS emit_cvt VERBATIM)
add_custom_target(generate_cvt_cases DEPENDS "${GENERATED}")

# The negative control changes only the CVT macro in a build-local header;
# the decoder, translator, emitted expressions and context remain production.
file(READ "${PS2RECOMP_DIR}/ps2xRuntime/include/ps2_runtime_macros.h" FIXED_MACROS)
set(FIXED_LINE "#define FPU_CVT_W_S(a) Ps2FpuCvtWS((float)(a))")
string(FIND "${FIXED_MACROS}" "${FIXED_LINE}" MACRO_POSITION)
if(MACRO_POSITION LESS 0)
    message(FATAL_ERROR "Expected corrected CVT.W.S helper macro")
endif()
string(REPLACE "${FIXED_LINE}"
    "#define FPU_CVT_W_S(a) ((int32_t)nearbyintf((float)(a)))"
    CONTROL_MACROS "${FIXED_MACROS}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/control")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/control/ps2_runtime_macros.h" "${CONTROL_MACROS}")

function(add_execution target control)
    add_executable(${target} execute_cases.cpp "${GENERATED}")
    add_dependencies(${target} generate_cvt_cases)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /fp:strict)
    target_compile_definitions(${target} PRIVATE ORIGINAL_ROUNDING_CONTROL=${control})
    if(control)
        target_include_directories(${target} BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/control")
    endif()
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${PS2RECOMP_DIR}/ps2xIOP/include")
endfunction()
add_execution(kfiv_cvt_tests 0)
add_execution(kfiv_cvt_control 1)
enable_testing()
add_test(NAME kfiv_cvt_generated_execution COMMAND kfiv_cvt_tests)
add_test(NAME kfiv_cvt_original_rounding_control COMMAND kfiv_cvt_control)
set_tests_properties(kfiv_cvt_generated_execution kfiv_cvt_original_rounding_control
    PROPERTIES TIMEOUT 30)
set_tests_properties(kfiv_cvt_original_rounding_control PROPERTIES WILL_FAIL TRUE)
