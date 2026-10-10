cmake_minimum_required(VERSION 3.20)
project(kfiv_host_input_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_raylib()




add_executable(kfiv_host_input_tests host_input_tests.cpp
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_pad.cpp")
target_compile_features(kfiv_host_input_tests PRIVATE cxx_std_17)
target_include_directories(kfiv_host_input_tests PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include" "${RAYLIB_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(kfiv_host_input_tests PRIVATE /W4 /permissive-)
endif()
enable_testing()
add_test(NAME kfiv_host_input_regression COMMAND kfiv_host_input_tests)
set_tests_properties(kfiv_host_input_regression PROPERTIES
    ENVIRONMENT "PS2X_HOST_INPUT=;PS2X_INPUT_TRACE=")
