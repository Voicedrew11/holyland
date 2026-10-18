# The PC port of King's Field IV: the decomp's game code compiled for the PC, with the PS2 side
# (port/) under it. See docs/port/README.md.
#
# Layout:
#   decomp/          git submodule of kfiv-decomp, pinned to one commit. Never edited here.
#   patches/         the PC edits to the decomp, as git diffs with paths relative to the decomp root
#                    (patches/game/<folder>.patch, patches/game/_top.patch, patches/nonmatching.patch).
#   port/            the PS2 side: libraries, SDK stubs and the host's main.
#   game/            PC-only game code (the data sections in C), compiled with the patched decomp.
# At configure time the decomp's src/game and src/nonmatching are copied, the patches are applied to
# the copy, and the copy (out/<preset>/decomp_src/src/) is compiled.
cmake_minimum_required(VERSION 3.21)
project(kf4 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The PC port is 64-bit only")
endif()
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The game code is GNU C: build it with clang or gcc (see docs/port/BUILDING.md)")
endif()

# ---- How the game code is compiled ---------------------------------------------------------------
# ee-gcc 2.9's behaviour where the game depends on it: no type-based alias analysis, wrapping signed
# overflow, no fused multiply-add, signed char, GCC's bit-field layout (also on Windows). Retail reads
# past the ends of some arrays on purpose and tests some pointers after using them: keep the compiler
# from optimising on either. A file's globals keep the order they are defined in, as ee-gcc laid them
# out: the game reads files in whole sectors, so a read into a table runs on into the next one
# (item_nam.prm into item_common_defs), which retail then loads. clang keeps that order by itself.
set(KF4_GAME_FLAGS
    -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-fast-math -fsigned-char -fno-common
    -fgnu89-inline -fno-delete-null-pointer-checks)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND KF4_GAME_FLAGS -fno-aggressive-loop-optimizations -fno-toplevel-reorder)
endif()
if(WIN32)
    list(APPEND KF4_GAME_FLAGS -mno-ms-bitfields)
endif()
# The step-0 rules (docs/port/RULES.md) as errors: nothing may squeeze a 64-bit pointer into 32 bits
# or call a function it has not declared.
set(KF4_GAME_ERRORS
    -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast -Werror=implicit-function-declaration
    -Werror=implicit-int -Werror=int-conversion -Werror=return-type)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # (clang only) an address in a 64-bit integer must not be narrowed back to 32 bits without a cast
    list(APPEND KF4_GAME_ERRORS -Werror=incompatible-function-pointer-types -Werror=shorten-64-to-32
         -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function
         -Wno-missing-braces -Wno-parentheses-equality)
endif()

# ---- The PS2 side ---------------------------------------------------------------------------------
add_library(kf4_port STATIC
    port/platform/memory.c
    port/platform/ee_address.c
    port/platform/fiber.c
    port/platform/kernel.c
    port/platform/dmac.c
    port/platform/disc.c
    port/platform/host_time.c
    port/math/libvu0.c
    port/sdk/libcdvd.c
    port/sdk/libdma.c
    port/sdk/libmc.c
    port/sdk/libpad.c
    port/sdk/sif.c
    port/sdk/fssnd.c
    port/sdk/stubs.c
    port/sdk/game_stubs.c
    port/sdk/libc_ps2.c)
target_include_directories(kf4_port PUBLIC port/include)
target_compile_options(kf4_port PRIVATE ${KF4_GAME_FLAGS})
find_package(Threads REQUIRED)
target_link_libraries(kf4_port PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(kf4_port PUBLIC winmm)
else()
    target_link_libraries(kf4_port PUBLIC m)
endif()
# SDL2 for the pad (and, from step 2, the window); without it the pad takes only a pad script
option(KF4_USE_SDL "Use SDL2 for the pad" ON)
if(KF4_USE_SDL)
    find_package(SDL2 CONFIG)
endif()
if(KF4_USE_SDL AND SDL2_FOUND)
    target_compile_definitions(kf4_port PRIVATE KF4_HAVE_SDL)
    if(TARGET SDL2::SDL2)
        target_link_libraries(kf4_port PRIVATE SDL2::SDL2)
    else()
        target_include_directories(kf4_port PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(kf4_port PRIVATE ${SDL2_LIBRARIES})
    endif()
else()
    message(STATUS "SDL2 not used: the pad takes only a pad script (--input)")
endif()

# ---- The decomp's game code, patched --------------------------------------------------------------
# Everything below compiles the copy in decomp_src/, never decomp/ itself. Each configure builds the
# patched decomp in a staging directory, then copies into decomp_src/ only the files whose content
# changed, so a patch edit recompiles the files it touches and nothing else.
set(KF4_DECOMP ${CMAKE_SOURCE_DIR}/decomp)
set(KF4_STAGE ${CMAKE_BINARY_DIR}/decomp_stage)
set(KF4_SRC ${CMAKE_BINARY_DIR}/decomp_src)
if(NOT EXISTS ${KF4_DECOMP}/src/game)
    message(FATAL_ERROR "decomp/src/game is missing: run git submodule update --init")
endif()
find_package(Git REQUIRED)

file(GLOB_RECURSE KF4_PATCHES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/*.patch)
list(SORT KF4_PATCHES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KF4_PATCHES})
# moving the decomp pin re-runs configure: git's HEAD file of the submodule changes with it
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${KF4_DECOMP} rev-parse --git-path HEAD
    OUTPUT_VARIABLE KF4_DECOMP_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(rc EQUAL 0)
    get_filename_component(KF4_DECOMP_HEAD ${KF4_DECOMP_HEAD} ABSOLUTE BASE_DIR ${KF4_DECOMP})
    if(EXISTS ${KF4_DECOMP_HEAD})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KF4_DECOMP_HEAD})
    endif()
endif()

# decomp/ must be at its pin, unedited: an edit there would be compiled without being in a patch
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${KF4_DECOMP} status --porcelain -- src/game src/nonmatching
    OUTPUT_VARIABLE KF4_DECOMP_EDITS OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git status failed in ${KF4_DECOMP}:\n${out}")
endif()
if(NOT "${KF4_DECOMP_EDITS}" STREQUAL "")
    message(FATAL_ERROR "decomp/ has local changes:\n${KF4_DECOMP_EDITS}\n"
        "decomp/ must stay at its pin, unedited. PC edits go in patches/ (tools/patches.py).")
endif()

# the staging copy: the pinned decomp with the patches applied, rebuilt from scratch every configure
file(REMOVE_RECURSE ${KF4_STAGE})
file(MAKE_DIRECTORY ${KF4_STAGE}/src)
file(COPY ${KF4_DECOMP}/src/game ${KF4_DECOMP}/src/nonmatching DESTINATION ${KF4_STAGE}/src)
# its own repo, so that git apply works on paths relative to the copy's root (not holyland's)
execute_process(COMMAND ${GIT_EXECUTABLE} init -q WORKING_DIRECTORY ${KF4_STAGE} RESULT_VARIABLE rc
    OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "git init failed in ${KF4_STAGE}:\n${out}")
endif()
foreach(patch ${KF4_PATCHES})
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${patch}
        WORKING_DIRECTORY ${KF4_STAGE} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "patch ${patch} does not apply to decomp/ at its pin:\n${out}")
    endif()
endforeach()

# sync the staging copy into decomp_src/. COPY_FILE leaves a file alone when it is already the same,
# so its timestamp, and the object built from it, survive. (The .git is at the staging root, not under src.)
# decomp_src/ is kept read-only between configures (see below), so make it writable for the sync.
if(EXISTS ${KF4_SRC}/src)
    file(CHMOD_RECURSE ${KF4_SRC}/src
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
        DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
file(GLOB_RECURSE KF4_STAGED LIST_DIRECTORIES false RELATIVE ${KF4_STAGE} ${KF4_STAGE}/src/*)
foreach(rel ${KF4_STAGED})
    get_filename_component(dir ${KF4_SRC}/${rel} DIRECTORY)
    file(MAKE_DIRECTORY ${dir})
    file(COPY_FILE ${KF4_STAGE}/${rel} ${KF4_SRC}/${rel} ONLY_IF_DIFFERENT)
endforeach()
# what decomp_src/ has and the staging copy does not (a patch's removed file, an old folder, the old
# layout's .git) goes
file(REMOVE_RECURSE ${KF4_SRC}/.git)
file(GLOB_RECURSE KF4_OLD LIST_DIRECTORIES true RELATIVE ${KF4_SRC} ${KF4_SRC}/src/*)
foreach(rel ${KF4_OLD})
    if(NOT EXISTS ${KF4_STAGE}/${rel})
        file(REMOVE_RECURSE ${KF4_SRC}/${rel})
    endif()
endforeach()
file(REMOVE_RECURSE ${KF4_STAGE})
# decomp_src/ is generated: compiler errors point into it, but an edit there is lost at the next
# configure. Its files are read-only so that the edit fails instead; edit work/ (tools/patches.py).
file(CHMOD_RECURSE ${KF4_SRC}/src
    FILE_PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# ---- The game, one object library per folder of src/game ------------------------------------------
# The libdev members the decomp keeps as src/game/func_0022*.c are PS2 debugging code: not built.
file(GLOB KF4_GAME_FOLDERS LIST_DIRECTORIES true RELATIVE ${KF4_SRC}/src/game
     ${KF4_SRC}/src/game/*)
set(KF4_GAME_TARGETS)
foreach(folder ${KF4_GAME_FOLDERS})
    if(IS_DIRECTORY ${KF4_SRC}/src/game/${folder})
        file(GLOB sources CONFIGURE_DEPENDS ${KF4_SRC}/src/game/${folder}/*.c)
        set(target game_${folder})
        add_library(${target} OBJECT ${sources})
        list(APPEND KF4_GAME_TARGETS ${target})
    endif()
endforeach()
target_sources(game_map PRIVATE ${KF4_SRC}/src/nonmatching/func_00112BD0.c) # the map draw at 0x112BD0, asm on the PS2
file(GLOB top_sources CONFIGURE_DEPENDS ${KF4_SRC}/src/game/*.c)
list(FILTER top_sources EXCLUDE REGEX "/func_0022[0-9A-F]+\\.c$")
add_library(game_top OBJECT ${top_sources})
list(APPEND KF4_GAME_TARGETS game_top)
# the PC-only data sections (game/data/), built with the decomp's game code
file(GLOB game_data_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/game/data/*.c)
add_library(game_data OBJECT ${game_data_sources} game/state_log.c)
list(APPEND KF4_GAME_TARGETS game_data)
# the game's main is called by the host's (port/platform/main_host.c)
set_source_files_properties(${KF4_SRC}/src/game/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=kf4_game_main")

foreach(target ${KF4_GAME_TARGETS})
    target_include_directories(${target} PRIVATE ${KF4_SRC}/src/game port/include)
    target_compile_options(${target} PRIVATE ${KF4_GAME_FLAGS} ${KF4_GAME_ERRORS})
endforeach()
add_custom_target(game DEPENDS ${KF4_GAME_TARGETS})

# ---- The program ----------------------------------------------------------------------------------
set(KF4_GAME_OBJECTS)
foreach(target ${KF4_GAME_TARGETS})
    list(APPEND KF4_GAME_OBJECTS $<TARGET_OBJECTS:${target}>)
endforeach()
add_executable(kf4 port/platform/main_host.c ${KF4_GAME_OBJECTS})
target_compile_options(kf4 PRIVATE ${KF4_GAME_FLAGS})
target_link_libraries(kf4 PRIVATE kf4_port)
