# Building mods' plugins with Holyland (docs/port/MODDING.md). Holyland's configure with -DKF4_MODS_SRC=<folder of
# mods> builds the plugin of each mod there that has src/CMakeLists.txt, which says what it is:
#
#     holyland_add_mod(OUTPUT journal SOURCES plugin.cpp journal.c ... GAME RMLUI SDL)
#
#   OUTPUT   the plugin's file name without its ending (journal: journal.dll, journal.so)
#   GAME     its C sources use the game's own headers (the decompilation's, as Holyland builds them) and flags
#   RMLUI    it uses RmlUi (the player menu's, in Holyland): its headers
#   SDL      it uses SDL3 (input events)
#
# The plugin is written to <mod>/bin/<system>/ (windows-x64, linux-x64), where mod.json names it. It links against
# the program's exports (Holyland, the game, RmlUi, cJSON) by name. It carries the game interface ID it was built for
# (hl_mod_game_interface: cmake/mod_hookable.cmake), and needs building again only when Holyland's differs.

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(HOLYLAND_MOD_SYSTEM windows-x64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(HOLYLAND_MOD_SYSTEM linux-arm64)
else()
    set(HOLYLAND_MOD_SYSTEM linux-x64)
endif()

function(holyland_add_mod)
    cmake_parse_arguments(MOD "GAME;RMLUI;SDL" "OUTPUT" "SOURCES" ${ARGN})
    get_filename_component(mod_folder ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
    get_filename_component(mod_id ${mod_folder} NAME)
    string(MAKE_C_IDENTIFIER "hlmod_${mod_id}" target)
    if(NOT MOD_OUTPUT)
        set(MOD_OUTPUT ${mod_id})
    endif()
    # (a Holyland built without a window has no SDL3 and no player menu: a mod that needs them is left out, not an error)
    if(MOD_SDL AND NOT TARGET SDL3::SDL3)
        message(STATUS "Mod plugin: ${mod_id} left out: it needs SDL3, and this Holyland is built without it")
        return()
    endif()
    if(MOD_RMLUI AND NOT TARGET kf4_rmlui_headers)
        message(STATUS "Mod plugin: ${mod_id} left out: it needs RmlUi, and this Holyland is built without the player menu")
        return()
    endif()
    set(interface_c ${CMAKE_CURRENT_BINARY_DIR}/hl_mod_game_interface.c)
    file(CONFIGURE OUTPUT ${interface_c} CONTENT
"#include \"holyland_mod.h\"
/* the game interface this plugin is built for (modding/holyland_mod.cmake) */
HL_EXPORT const char hl_mod_game_interface[] = \"@KF4_GAME_INTERFACE@\";
" @ONLY)
    add_library(${target} MODULE ${MOD_SOURCES} ${interface_c})
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME ${MOD_OUTPUT}
        LIBRARY_OUTPUT_DIRECTORY ${mod_folder}/bin/${HOLYLAND_MOD_SYSTEM}
        RUNTIME_OUTPUT_DIRECTORY ${mod_folder}/bin/${HOLYLAND_MOD_SYSTEM}
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        INTERPROCEDURAL_OPTIMIZATION OFF)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${KF4_ROOT}/modding/include
        ${KF4_ROOT}/port/include ${KF4_ROOT}/third_party/cjson)
    target_link_libraries(${target} PRIVATE kf4) # (the program's exports; on Windows its import library)
    if(MOD_GAME)
        # (C only: the game's folders have names C++'s library uses, debug/ among them)
        target_include_directories(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${KF4_SRC}/src/game>)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:${KF4_GAME_FLAGS}>)
        if(WIN32)
            target_compile_options(${target} PRIVATE -mno-ms-bitfields) # (the game's structures' layout)
        endif()
    endif()
    if(MOD_RMLUI)
        target_link_libraries(${target} PRIVATE kf4_rmlui_headers)
    endif()
    if(MOD_SDL)
        target_link_libraries(${target} PRIVATE SDL3::SDL3)
    endif()
    add_dependencies(holyland_mods ${target})
    message(STATUS "Mod plugin: ${mod_id} (${MOD_OUTPUT})")
endfunction()

add_custom_target(holyland_mods ALL)
file(GLOB holyland_mod_folders LIST_DIRECTORIES true ${KF4_MODS_SRC}/*)
foreach(folder ${holyland_mod_folders})
    get_filename_component(name ${folder} NAME)
    if(IS_DIRECTORY ${folder} AND EXISTS ${folder}/src/CMakeLists.txt AND NOT name MATCHES "^[._]")
        add_subdirectory(${folder}/src ${CMAKE_BINARY_DIR}/mods/${name})
    endif()
endforeach()
