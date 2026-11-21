# Holyland, the launcher (launcher/product.h, docs/port/LAUNCHER.md): builds King's Field IV's PC port on the player's computer
# from a release's source and tools, then plays it. Its own project: it needs neither the decomp nor the
# game's build, only SDL3 and a few files the game shares with it (port/platform/data_dir.c, disc_image.c (and third_party/libchdr), iso9660.c,
# update_notice.c, the player menu's RmlUi code (port/ui/) and its fonts and logo, third_party/rmlui, freetype and cjson).
#
#   cd launcher && cmake --preset linux && cmake --build --preset linux && ctest --test-dir ../out/launcher-linux
cmake_minimum_required(VERSION 3.21)
project(holyland_launcher C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# the release's version (tools/version.sh); empty in a developer's build, which is never told of updates
set(HOLYLAND_VERSION "" CACHE STRING "The release's version (0.2.0)")
if(NOT HOLYLAND_VERSION MATCHES "^([0-9]+(\\.[0-9]+)*)?$")
    message(FATAL_ERROR "HOLYLAND_VERSION must be dotted numbers (0.2.0), not '${HOLYLAND_VERSION}'")
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
    rml_text.cpp
    setup_flow.cpp
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

# The window: SDL3 and RmlUi (cmake/rmlui.cmake, as the player menu), drawn on SDL's renderer
find_package(SDL3 CONFIG REQUIRED)
include(${ROOT}/cmake/rmlui.cmake)

# The pages' files, carried inside the launcher as the game carries the menu's: the menu's fonts and logo
# (port/ui/res/) and the launcher's pages (launcher/res/)
set(LAUNCHER_UI_FILES logo.alpha fonts/Cinzel-Regular.ttf fonts/Cinzel-SemiBold.ttf fonts/Spectral-Regular.ttf
    fonts/Spectral-SemiBold.ttf fonts/Spectral-Italic.ttf)
file(GLOB LAUNCHER_PAGES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/res
     ${CMAKE_CURRENT_SOURCE_DIR}/res/*.rml ${CMAKE_CURRENT_SOURCE_DIR}/res/*.rcss)
list(SORT LAUNCHER_PAGES)
list(TRANSFORM LAUNCHER_UI_FILES PREPEND ${ROOT}/port/ui/res/ OUTPUT_VARIABLE LAUNCHER_UI_FILE_PATHS)
list(TRANSFORM LAUNCHER_PAGES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/res/ OUTPUT_VARIABLE LAUNCHER_PAGE_PATHS)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.cpp
    COMMAND ${CMAKE_COMMAND} -DROOT=${ROOT}/port/ui/res "-DFILES=${LAUNCHER_UI_FILES}"
            -DMORE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/res "-DMORE_FILES=${LAUNCHER_PAGES}"
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ui_assets.cpp -P ${ROOT}/cmake/ui_assets.cmake
    DEPENDS ${LAUNCHER_UI_FILE_PATHS} ${LAUNCHER_PAGE_PATHS} ${ROOT}/cmake/ui_assets.cmake
    COMMENT "The launcher's pages, fonts and logo" VERBATIM)

add_executable(launcher main.cpp session.cpp setup_pages.cpp window.cpp ${ROOT}/port/ui/ui_render.cpp
               ${ROOT}/port/ui/ui_ties.cpp ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.cpp)
target_include_directories(launcher PRIVATE ${ROOT}/port/ui) # ui_render.hpp, ui_ties.hpp, ui_assets.hpp
set_target_properties(launcher PROPERTIES OUTPUT_NAME holyland) # launcher/product.h: kProductId
target_compile_definitions(launcher PRIVATE HOLYLAND_VERSION="${HOLYLAND_VERSION}")
target_compile_options(launcher PRIVATE ${LAUNCHER_WARNINGS})
target_link_libraries(launcher PRIVATE launcher_platform kf4_rmlui SDL3::SDL3)
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
