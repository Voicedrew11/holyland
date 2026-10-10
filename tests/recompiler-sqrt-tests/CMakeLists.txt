cmake_minimum_required(VERSION 3.21)
project(kfiv_recompiler_sqrt_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_tools()
if(NOT MSVC)
    message(FATAL_ERROR "This fixture is verified with native Windows MSVC")
endif()

# Compile the actual production translator, plus an operand-bug control. The
# control restores only the two former case bodies, leaving decoder/codegen real.
file(READ "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/fpu_translator.cpp" FIXED_SOURCE)
set(CONTROL_SOURCE "${FIXED_SOURCE}")
foreach(OP SQRT RSQRT)
    if(OP STREQUAL "SQRT")
        set(NEXT ABS)
        set(OLD_BODY "                return fmt::format(\"ctx->f[{}] = FPU_SQRT_S(ctx->f[{}]);\", fd, fs);\n")
    else()
        set(NEXT ADDA)
        set(OLD_BODY "                return fmt::format(\"ctx->f[{}] = 1.0f / sqrtf(ctx->f[{}]);\", fd, fs);\n")
    endif()
    set(BEGIN "            case COP1_S_${OP}:\n")
    set(END "            case COP1_S_${NEXT}:\n")
    string(FIND "${CONTROL_SOURCE}" "${BEGIN}" BEGIN_POS)
    string(FIND "${CONTROL_SOURCE}" "${END}" END_POS)
    if(BEGIN_POS LESS 0 OR END_POS LESS BEGIN_POS)
        message(FATAL_ERROR "Cannot restore original ${OP} case")
    endif()
    string(SUBSTRING "${CONTROL_SOURCE}" 0 ${BEGIN_POS} PREFIX)
    string(SUBSTRING "${CONTROL_SOURCE}" ${END_POS} -1 SUFFIX)
    set(CONTROL_SOURCE "${PREFIX}${BEGIN}${OLD_BODY}${SUFFIX}")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fpu_translator-control.cpp" "${CONTROL_SOURCE}")

function(add_emitter target source)
    add_executable(${target} emit_cases.cpp "${source}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE LIBDWARF_STATIC _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj /utf-8)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRecomp/include"
        "${TOOLS_BUILD_DIR}/_deps/fmt-src/include"
        "${TOOLS_BUILD_DIR}/_deps/elfio-src"
        "${TOOLS_BUILD_DIR}/_deps/toml11-src/include"
        "${TOOLS_BUILD_DIR}/_deps/libdwarf-src/src/lib/libdwarf")
    target_link_libraries(${target} PRIVATE
        "${TOOLS_BUILD_DIR}/ps2xRecomp/${TOOLS_CONFIG}/ps2_recomp_lib.lib"
        "${TOOLS_BUILD_DIR}/_deps/fmt-build/${TOOLS_CONFIG}/fmt.lib"
        "${TOOLS_BUILD_DIR}/_deps/libdwarf-build/src/lib/libdwarf/${TOOLS_CONFIG}/dwarf.lib"
        "${TOOLS_BUILD_DIR}/ps2xRecomp/${TOOLS_CONFIG}/rabbitizer.lib")
endfunction()
add_emitter(emit_fixed "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/fpu_translator.cpp")
add_emitter(emit_control "${CMAKE_CURRENT_BINARY_DIR}/fpu_translator-control.cpp")

function(add_execution target emitter control)
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/${target}/generated_cases.h")
    add_custom_command(OUTPUT "${HEADER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/${target}"
        COMMAND $<TARGET_FILE:${emitter}> "${HEADER}"
        DEPENDS ${emitter} VERBATIM)
    add_executable(${target} execute_cases.cpp "${HEADER}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE /W4 /permissive- /fp:strict)
    target_compile_definitions(${target} PRIVATE EXPECT_ORIGINAL_OPERAND_BUG=${control})
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/${target}"
        "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${PS2RECOMP_DIR}/ps2xIOP/include")
endfunction()
add_execution(kfiv_sqrt_tests emit_fixed 0)
add_execution(kfiv_sqrt_control emit_control 1)
enable_testing()
add_test(NAME kfiv_sqrt_generated_execution COMMAND kfiv_sqrt_tests)
add_test(NAME kfiv_sqrt_original_operand_bug COMMAND kfiv_sqrt_control)
set_tests_properties(kfiv_sqrt_generated_execution kfiv_sqrt_original_operand_bug PROPERTIES TIMEOUT 30)
