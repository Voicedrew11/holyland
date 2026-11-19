# Darker Reality, the launcher (launcher/product.h, docs/port/LAUNCHER.md): builds King's Field IV's PC port on the player's computer
# from a release's source and tools, then plays it. Its own project: it needs neither the decomp nor the
# game's build, only SDL3 and a few files the game shares with it (port/platform/data_dir.c, disc_image.c (and third_party/libchdr), iso9660.c,
# update_notice.c, third_party/imgui and cjson).
#
#   cd launcher && cmake --preset linux && cmake --build --preset linux && ctest --test-dir ../out/launcher-linux
cmake_minimum_required(VERSION 3.21)
project(darker_reality_launcher C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# the release's version (tools/version.sh); empty in a developer's build, which is never told of updates
set(DARKER_REALITY_VERSION "" CACHE STRING "The release's version (0.2.0)")
if(NOT DARKER_REALITY_VERSION MATCHES "^([0-9]+(\\.[0-9]+)*)?$")
    message(FATAL_ERROR "DARKER_REALITY_VERSION must be dotted numbers (0.2.0), not '${DARKER_REALITY_VERSION}'")
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set(LAUNCHER_WARNINGS -Wall -Wextra -Wshadow)
endif()

# What the launcher shares with the game, and its own parts that need no window: tested on their own
add_library(launcher_core STATIC
    ${ROOT}/port/platform/data_dir.c
    ${ROOT}/port/platform/disc_image.c
    ${ROOT}/port/platform/iso9660.c
    ${ROOT}/port/platform/update_notice.c
    ${ROOT}/third_party/cjson/cJSON.c
    build_key.cpp
    build_log.cpp
    build_local.cpp
    disc_check.cpp
    launcher_files.cpp
    update_check.cpp)
target_include_directories(launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ROOT}/port/include ${ROOT}/third_party/cjson)
target_compile_options(launcher_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${LAUNCHER_WARNINGS}>)
find_package(Threads REQUIRED)
include(${ROOT}/cmake/libchdr.cmake) # CHD disc images (port/platform/disc_image.c)
kf4_add_libchdr(${ROOT})
target_link_libraries(launcher_core PUBLIC Threads::Threads kf4_chdr)

# One file per system (platform.h); a new system is a new file and a line here
if(WIN32)
    set(LAUNCHER_PLATFORM platform/windows.cpp)
    set(LAUNCHER_PLATFORM_LIBS winhttp ole32 shell32 uuid)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LAUNCHER_PLATFORM platform/linux.cpp)
    set(LAUNCHER_PLATFORM_LIBS ${CMAKE_DL_LIBS})
else()
    message(FATAL_ERROR "the launcher has no platform file for ${CMAKE_SYSTEM_NAME} yet (launcher/platform/)")
endif()
add_library(launcher_platform STATIC ${LAUNCHER_PLATFORM})
target_link_libraries(launcher_platform PUBLIC launcher_core ${LAUNCHER_PLATFORM_LIBS})
target_compile_options(launcher_platform PRIVATE ${LAUNCHER_WARNINGS})

# The window: SDL3 and Dear ImGui on SDL's renderer
find_package(SDL3 CONFIG REQUIRED)
add_library(launcher_imgui STATIC
    ${ROOT}/third_party/imgui/imgui.cpp
    ${ROOT}/third_party/imgui/imgui_draw.cpp
    ${ROOT}/third_party/imgui/imgui_tables.cpp
    ${ROOT}/third_party/imgui/imgui_widgets.cpp
    ${ROOT}/third_party/imgui/backends/imgui_impl_sdl3.cpp
    ${ROOT}/third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(launcher_imgui PUBLIC ${ROOT}/third_party/imgui)
target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3)

add_executable(launcher main.cpp session.cpp window.cpp)
set_target_properties(launcher PROPERTIES OUTPUT_NAME darker-reality) # launcher/product.h: kProductId
target_compile_definitions(launcher PRIVATE DARKER_REALITY_VERSION="${DARKER_REALITY_VERSION}")
target_compile_options(launcher PRIVATE ${LAUNCHER_WARNINGS})
target_link_libraries(launcher PRIVATE launcher_platform launcher_imgui)
if(WIN32)
    # a window program: no console window of its own (it prints into one it was started from)
    target_link_options(launcher PRIVATE -mwindows)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # no llvm-mingw DLLs beside it: the launcher runs before anything is built
        target_link_options(launcher PRIVATE -static)
    endif()
endif()
# (Linux: a release's toolchain file gives it the run path $ORIGIN, and the AppImage puts SDL3 beside it)

# Tests: the launcher's parts with a pretend system (tests/launcher_test.cpp)
option(LAUNCHER_TESTS "Build the launcher's unit tests" ON)
if(LAUNCHER_TESTS)
    enable_testing()
    add_executable(launcher_test tests/launcher_test.cpp)
    target_link_libraries(launcher_test PRIVATE launcher_core)
    target_compile_definitions(launcher_test PRIVATE TEST_DATA_DIR="${ROOT}/tests/data")
    target_compile_options(launcher_test PRIVATE ${LAUNCHER_WARNINGS})
    add_test(NAME launcher COMMAND launcher_test)
endif()
