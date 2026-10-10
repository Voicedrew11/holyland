cmake_minimum_required(VERSION 3.21)
project(kfiv_recompiler_resume_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_tools()


if(NOT MSVC)
    message(FATAL_ERROR "These tests validate native Windows MSVC")
endif()
file(READ "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/ps2_recompiler.cpp" RECOMPILER_SOURCE)
set(NEW_GUARD "// Match FunctionEmitter's resume switch for every decoded wrapper,\n            // including synthesized entries; this does not discover new calls.")
set(OLD_GUARD "if (isEntryFunctionName(function.name))\n            {\n                continue;\n            }")
string(REPLACE "${NEW_GUARD}" "${OLD_GUARD}" CONTROL_RECOMPILER "${RECOMPILER_SOURCE}")
if(CONTROL_RECOMPILER STREQUAL RECOMPILER_SOURCE)
    message(FATAL_ERROR "Resume discovery marker missing; cannot reproduce original exclusion")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ps2_recompiler-control.cpp" "${CONTROL_RECOMPILER}")
file(READ "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/function_table_emitter.cpp" TABLE_SOURCE)
set(EXPLICIT_HANDLERS [=[        for (const auto &[address, name] : stubFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : systemCallFunctions)
        {
            addEntry(address, name);
        }
        for (const auto &[address, name] : libraryFunctions)
        {
            addEntry(address, name);
        }

]=])
string(REPLACE "${EXPLICIT_HANDLERS}" "" CONTROL_TABLE "${TABLE_SOURCE}")
if(CONTROL_TABLE STREQUAL TABLE_SOURCE)
    message(FATAL_ERROR "Explicit handler precedence block missing")
endif()
string(REPLACE "        std::sort(entries.begin()," "${EXPLICIT_HANDLERS}        std::sort(entries.begin()," CONTROL_TABLE "${CONTROL_TABLE}")
set(OWNER_BEGIN "        std::unordered_set<uint32_t> entryWrapperStarts;")
set(OWNER_END "            const auto &targets = cg.m_resumeEntryTargetsByOwner.at(ownerStart);")
string(FIND "${CONTROL_TABLE}" "${OWNER_BEGIN}" OWNER_BEGIN_POSITION)
string(FIND "${CONTROL_TABLE}" "${OWNER_END}" OWNER_END_POSITION)
if(OWNER_BEGIN_POSITION LESS 0 OR OWNER_END_POSITION LESS OWNER_BEGIN_POSITION)
    message(FATAL_ERROR "Deterministic alias owner block missing")
endif()
string(LENGTH "${OWNER_END}" OWNER_END_LENGTH)
math(EXPR OWNER_SUFFIX_POSITION "${OWNER_END_POSITION} + ${OWNER_END_LENGTH}")
string(SUBSTRING "${CONTROL_TABLE}" 0 ${OWNER_BEGIN_POSITION} OWNER_PREFIX)
string(SUBSTRING "${CONTROL_TABLE}" ${OWNER_SUFFIX_POSITION} -1 OWNER_SUFFIX)
set(CONTROL_TABLE "${OWNER_PREFIX}        for (const auto &[ownerStart, targets] : cg.m_resumeEntryTargetsByOwner)\n        {${OWNER_SUFFIX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/function_table_emitter-control.cpp" "${CONTROL_TABLE}")
function(add_resume_test target)
    add_executable(${target} resume_tests.cpp)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE LIBDWARF_STATIC _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /bigobj)
    target_include_directories(${target} PRIVATE
        "${PS2RECOMP_DIR}/ps2xRecomp/include" "${PS2RECOMP_DIR}/ps2xRuntime/include"
        "${TOOLS_BUILD_DIR}/_deps/elfio-src" "${TOOLS_BUILD_DIR}/_deps/fmt-src/include"
        "${TOOLS_BUILD_DIR}/_deps/toml11-src/include"
        "${TOOLS_BUILD_DIR}/_deps/libdwarf-src/src/lib/libdwarf")
    target_link_libraries(${target} PRIVATE
        "${TOOLS_BUILD_DIR}/ps2xRecomp/${TOOLS_CONFIG}/ps2_recomp_lib.lib"
        "${TOOLS_BUILD_DIR}/_deps/fmt-build/${TOOLS_CONFIG}/fmt.lib"
        "${TOOLS_BUILD_DIR}/_deps/libdwarf-build/src/lib/libdwarf/${TOOLS_CONFIG}/dwarf.lib"
        "${TOOLS_BUILD_DIR}/ps2xRecomp/${TOOLS_CONFIG}/rabbitizer.lib")
endfunction()
add_resume_test(kfiv_resume_tests)
target_sources(kfiv_resume_tests PRIVATE
    "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/ps2_recompiler.cpp"
    "${PS2RECOMP_DIR}/ps2xRecomp/src/lib/function_table_emitter.cpp")
add_resume_test(kfiv_resume_control)
target_sources(kfiv_resume_control PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/ps2_recompiler-control.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/function_table_emitter-control.cpp")
target_compile_definitions(kfiv_resume_control PRIVATE EXPECT_ORIGINAL_MISSING_ALIASES=1)
enable_testing()
add_test(NAME kfiv_recompiler_resume_aliases COMMAND kfiv_resume_tests)
add_test(NAME kfiv_recompiler_original_missing_aliases COMMAND kfiv_resume_control)
set_tests_properties(kfiv_recompiler_resume_aliases kfiv_recompiler_original_missing_aliases PROPERTIES TIMEOUT 30)
