cmake_minimum_required(VERSION 3.20)
project(kfiv_host_audio_tests LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PackagePaths.cmake")
kfiv_require_raylib()




add_executable(kfiv_host_audio_tests host_audio_tests.cpp
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_audio.cpp"
    "${PS2RECOMP_DIR}/ps2xRuntime/src/lib/ps2_audio_vag.cpp")
target_compile_features(kfiv_host_audio_tests PRIVATE cxx_std_20)
target_include_directories(kfiv_host_audio_tests PRIVATE
    "${PS2RECOMP_DIR}/ps2xRuntime/include" "${RAYLIB_INCLUDE_DIR}")
if(MSVC)
    target_compile_options(kfiv_host_audio_tests PRIVATE /W4 /permissive-)
endif()
enable_testing()
add_test(NAME kfiv_host_audio_regression COMMAND kfiv_host_audio_tests)
set_tests_properties(kfiv_host_audio_regression PROPERTIES
    ENVIRONMENT "PS2X_AUDIO_TRACE=;PS2X_AUDIO_DUMP=;PS2X_AUDIO_SOURCE_DUMP="
    TIMEOUT 30)
