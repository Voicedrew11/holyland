# The PC port of King's Field IV: the decomp's game code compiled for the PC, with the PS2 side
# (port/) under it. See docs/port/README.md.
#
# Layout:
#   decomp/          git submodule of kfiv-decomp, pinned to one commit. Never edited here.
#   patches/         the PC edits to the decomp, as git diffs with paths relative to the decomp root
#                    (patches/game/<folder>.patch, patches/game/_top.patch, patches/nonmatching.patch).
#   port/            the PS2 side: libraries, SDK stubs and the host's main.
#   game/            PC-only game code (the data sections in C), compiled with the patched decomp.
#   third_party/     git submodules: parallel-gs, the GS renderer (docs/port/README.md says how to
#                    check out only the parts of it the port builds).
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
# out and as clang does by itself, so gcc and clang builds have the same neighbours. Nothing known
# relies on it any more: File_Read's whole sectors no longer run past a global (port_cd_read_bytes,
# libcdvd.h), and the sanitizer build (KF4_SANITIZE) finds a read or write that crosses into the next.
# No stack protector (on by default in many Linux gccs): a save state puts back the EE threads' stacks
# in another process, whose canary is not the one their frames hold (port/platform/savestate.c).
set(KF4_GAME_FLAGS
    -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-fast-math -fsigned-char -fno-common
    -fgnu89-inline -fno-delete-null-pointer-checks -fno-stack-protector)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND KF4_GAME_FLAGS -fno-aggressive-loop-optimizations -fno-toplevel-reorder)
endif()
if(WIN32)
    list(APPEND KF4_GAME_FLAGS -mno-ms-bitfields)
endif()
# The game's threads run with denormals flushed (MXCSR's FTZ and DAZ, as the EE's FPU: port/platform/
# fiber.c), which the compiler is told, or it may turn a test of a float's bits into a comparison with
# 0.0 that a denormal then passes (clang does in newlib's sqrtf). Only the code that runs there.
set(KF4_EE_FLOAT_FLAGS)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(KF4_EE_FLOAT_FLAGS -fdenormal-fp-math=preserve-sign,preserve-sign)
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

# ---- Sanitizers (docs/port/README.md, "Sanitizers") -----------------------------------------------
# -DKF4_SANITIZE=address,undefined builds everything (the game, port/, ico-audio, the tests) with
# those sanitizers. Empty (the default) leaves the build as it is.
set(KF4_SANITIZE "" CACHE STRING "Build with these sanitizers (-fsanitize=...), e.g. address,undefined")
if(KF4_SANITIZE)
    # recoverable, so ASAN_OPTIONS=halt_on_error=0 can list every report of a run
    add_compile_options(-fsanitize=${KF4_SANITIZE} -fsanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${KF4_SANITIZE})
    message(STATUS "Sanitizers: ${KF4_SANITIZE}")
endif()

# ---- The PS2 side ---------------------------------------------------------------------------------
add_library(kf4_port STATIC
    port/platform/memory.c
    port/platform/ee_address.c
    port/platform/fiber.c
    port/platform/kernel.c
    port/platform/dmac.c
    port/platform/vif1.c
    port/platform/vu1.c
    port/platform/vu1_hle.c
    port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c
    port/platform/gs.c
    port/platform/frame_interp.c
    port/platform/field_frames.c
    port/platform/video.c
    port/platform/disc.c
    port/platform/ipu.c
    port/platform/host_time.c
    port/platform/int_divide.c
    port/math/libvu0.c
    port/sdk/libcdvd.c
    port/sdk/libdma.c
    port/sdk/libgraph.c
    port/sdk/libmc.c
    port/sdk/libmpeg.c
    port/sdk/mpeg2.c
    port/sdk/libpad.c
    port/sdk/controls.c
    port/sdk/libsdr.c
    port/platform/savestate.c
    port/platform/quicksave.c
    port/platform/teleport.c
    port/platform/cheats.c
    port/platform/automap.c
    port/platform/lz4_block.c
    port/debug/debug.c
    port/debug/gs_capture.c
    port/overlay/settings.c
    third_party/cjson/cJSON.c
    port/sdk/sif.c
    port/sdk/stubs.c
    port/sdk/game_stubs.c
    port/sdk/libc_ps2.c
    port/sdk/libm_ps2.c
    port/audio/iop.c
    port/audio/sd.c
    port/audio/libsd_irx.c
    port/audio/output.c
    port/audio/fsiopsnd.c
    port/audio/modmidi.c
    port/audio/modhsyn.c
    port/audio/sdrdrv.c)
target_include_directories(kf4_port PUBLIC port/include)
target_include_directories(kf4_port PRIVATE third_party/cjson)
# native draw's renderer core (port/include/port/render.h): the game's side hands it draws, the native
# GS draws them (docs/port/NATIVE_DRAW.md)
enable_language(CXX)
add_library(kf4_render STATIC port/render/render.cpp)
target_include_directories(kf4_render PUBLIC port/include)
set_target_properties(kf4_render PROPERTIES CXX_STANDARD 14)
target_link_libraries(kf4_port PUBLIC kf4_render)
target_link_libraries(kf4_port PUBLIC ico_audio)
# the profiler's counters (port/include/port/profile.h, Shift+P): a library of their own, so that the
# native GS (kf4_gs, built into tests without the port) adds to them as well
add_library(kf4_profile STATIC port/platform/profile.c)
target_include_directories(kf4_profile PUBLIC port/include)
target_link_libraries(kf4_port PUBLIC kf4_profile)
target_compile_options(kf4_port PRIVATE $<$<COMPILE_LANGUAGE:C>:${KF4_GAME_FLAGS}>)
# VU1 rounds toward zero, which it sets with fesetround: the compiler must not fold float maths
# across that
set_source_files_properties(port/platform/vu1.c port/platform/vu1_hle.c port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c PROPERTIES COMPILE_OPTIONS "-frounding-math;-falign-functions=64;-falign-loops=64")
set_source_files_properties(port/sdk/libm_ps2.c port/math/libvu0.c PROPERTIES COMPILE_OPTIONS "${KF4_EE_FLOAT_FLAGS}")
# The fibers switch stacks by hand (port/platform/fiber.c), which a shadow stack would refuse
if(NOT WIN32)
    set_source_files_properties(port/platform/fiber.c PROPERTIES COMPILE_OPTIONS "-fcf-protection=none")
endif()
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
        set(KF4_SDL2_LINK SDL2::SDL2)
    else()
        target_include_directories(kf4_port PRIVATE ${SDL2_INCLUDE_DIRS})
        set(KF4_SDL2_LINK ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(kf4_port PRIVATE ${KF4_SDL2_LINK})
    # The settings overlay (F1): Dear ImGui (third_party/imgui) on the window's SDL_Renderer
    enable_language(CXX)
    add_library(imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl2.cpp
        third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp)
    target_include_directories(imgui PUBLIC third_party/imgui)
    if(NOT TARGET SDL2::SDL2)
        target_include_directories(imgui PRIVATE ${SDL2_INCLUDE_DIRS})
    endif()
    target_link_libraries(imgui PUBLIC ${KF4_SDL2_LINK})
    set_target_properties(imgui PROPERTIES CXX_STANDARD 11)
    target_sources(kf4_port PRIVATE port/overlay/overlay.cpp port/overlay/automap_view.cpp
                   port/overlay/profiler.cpp)
    target_link_libraries(kf4_port PRIVATE imgui)
else()
    message(STATUS "SDL2 not used: the pad takes only a pad script (--input), and there is no window")
endif()
# paraLLEl-GS, the GS in Vulkan compute; without it the game runs and draws nothing
# The software SPU2 (third_party/ico-audio, from ico-pc) and its tests
option(KF4_TESTS "Build the unit tests (ctest)" ON)
if(KF4_TESTS)
    enable_testing()
endif()
add_subdirectory(third_party/ico-audio)

option(KF4_USE_PARALLEL_GS "Draw with paraLLEl-GS (third_party/parallel-gs)" ON)
if(KF4_USE_PARALLEL_GS AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite/third_party/volk/volk.h)
    enable_language(CXX)
    set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
    # Granite with the depth bounds test (cmake/granite_depth_bounds.cmake: made in its checkout)
    include(cmake/granite_depth_bounds.cmake)
    kf4_granite_depth_bounds(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite)
    # and with dynamic state for the native GS's draws (cmake/granite_dynamic_state.cmake, after the above)
    include(cmake/granite_dynamic_state.cmake)
    kf4_granite_dynamic_state(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite)
    # and with the hooks of the native GS's pipeline list (cmake/granite_pipeline_warm.cmake, after the above)
    include(cmake/granite_pipeline_warm.cmake)
    kf4_granite_pipeline_warm(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite)
    kf4_granite_pipeline_cache_locks(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite)
    add_subdirectory(third_party/parallel-gs EXCLUDE_FROM_ALL)
    # and the native GS (port/gs/native/), on the same Vulkan layer
    add_library(kf4_gs STATIC port/gs/gs_parallel.cpp port/gs/native/gsn.cpp port/gs/native/gsn_vram.c
                port/gs/native/upscale_ffx.cpp port/gs/native/upscale_dlss.cpp)
    target_include_directories(kf4_gs PRIVATE port/include third_party/ffx-api/include PUBLIC port/gs/native)
    # --upscaler fsr3 (docs/port/UPSCALING.md): AMD's signed amd_fidelityfx_vk.dll, loaded at run time from beside
    # kf4.exe. Not kept here: KF4_FFX_DLL names one (the FidelityFX SDK's PrebuiltSignedDLL) to copy there
    set(KF4_FFX_DLL "" CACHE FILEPATH "amd_fidelityfx_vk.dll to copy beside kf4.exe (--upscaler fsr3)")
    # --upscaler dlss: NGX's headers from NVIDIA's DLSS SDK, whose licence keeps it out of this repository.
    # KF4_DLSS_SDK names the SDK (github.com/NVIDIA/DLSS); without it the build has no DLSS
    set(KF4_DLSS_SDK "" CACHE PATH "NVIDIA's DLSS SDK, for --upscaler dlss (Windows)")
    if(KF4_DLSS_SDK AND WIN32)
        if(NOT EXISTS ${KF4_DLSS_SDK}/include/nvsdk_ngx_defs.h)
            message(FATAL_ERROR "KF4_DLSS_SDK: ${KF4_DLSS_SDK}/include/nvsdk_ngx_defs.h is not there")
        endif()
        set_source_files_properties(port/gs/native/upscale_dlss.cpp PROPERTIES
            COMPILE_DEFINITIONS KF4_HAVE_DLSS INCLUDE_DIRECTORIES ${KF4_DLSS_SDK}/include)
    endif()
    target_link_libraries(kf4_gs PRIVATE parallel-gs)
    target_link_libraries(kf4_gs PUBLIC kf4_render)
    target_link_libraries(kf4_gs PUBLIC kf4_profile)
    set_target_properties(kf4_gs PROPERTIES CXX_STANDARD 14)
    target_compile_definitions(kf4_port PRIVATE KF4_HAVE_PARALLEL_GS)
    target_link_libraries(kf4_port PUBLIC kf4_gs)
    # --presenter vulkan (port/platform/present_vk.cpp): the window's swapchain on the native GS's
    # device, with the overlay drawn by Dear ImGui's Vulkan backend (functions loaded at run time)
    if(TARGET imgui)
        target_sources(imgui PRIVATE third_party/imgui/backends/imgui_impl_vulkan.cpp)
        target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
        target_link_libraries(imgui PUBLIC Vulkan::Headers)
        target_sources(kf4_port PRIVATE port/platform/present_vk.cpp)
        target_include_directories(kf4_port PRIVATE port/gs/native)
        target_compile_definitions(kf4_port PRIVATE KF4_HAVE_VULKAN_PRESENTER)
    endif()
elseif(KF4_USE_PARALLEL_GS)
    message(STATUS "third_party/parallel-gs is not checked out: the game will draw nothing")
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
add_library(game_data OBJECT ${game_data_sources} game/state_log.c game/widescreen.c game/draw_distance.c game/frame_interp.c game/native_draw/native_draw.c game/native_draw/nd_entities.c game/native_draw/nd_arm.c game/native_draw/nd_objects.c game/native_draw/nd_sky.c game/native_draw/nd_effects.c game/native_draw/nd_shadows.c game/native_draw/nd_water.c game/native_draw/nd_passes.c game/quicksave.c game/cheats.c game/automap.c game/automap_guide.c game/automap_script.c game/screen_fx.c
    port/debug/game_dump.c) # F10's dump reads the game's structures (port/include/port/debug.h)
list(APPEND KF4_GAME_TARGETS game_data)
# the extended stats mod (port/include/port/stats.h, docs/port/EXTENDED_STATS.md)
target_sources(game_data PRIVATE game/stats.c)
target_sources(kf4_port PRIVATE port/overlay/stats_mod.c)
if(TARGET imgui)
    target_sources(kf4_port PRIVATE port/overlay/stats_panel.cpp port/overlay/stats_menus.cpp
                   port/overlay/stats_timers.cpp)
endif()
# the weapon sets mod (port/include/port/weapon_sets.h, docs/port/WEAPON_SETS.md)
target_sources(game_data PRIVATE game/weapon_sets.c)
target_sources(kf4_port PRIVATE port/platform/weapon_sets.c)
if(TARGET imgui)
    target_sources(kf4_port PRIVATE port/overlay/weapon_sets_view.cpp)
endif()
# the game's main is called by the host's (port/platform/main_host.c)
set_source_files_properties(${KF4_SRC}/src/game/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=kf4_game_main")

# Every game file calls the PS2's math library, not the host's (port/include/port/libm_ps2.h)
foreach(target ${KF4_GAME_TARGETS})
    target_include_directories(${target} PRIVATE ${KF4_SRC}/src/game port/include)
    target_compile_options(${target} PRIVATE ${KF4_GAME_FLAGS} ${KF4_EE_FLOAT_FLAGS} ${KF4_GAME_ERRORS}
        -include ${CMAKE_SOURCE_DIR}/port/include/port/libm_ps2.h)
endforeach()
add_custom_target(game DEPENDS ${KF4_GAME_TARGETS})

# ---- The PS2's math library -----------------------------------------------------------------------
# newlib's libm as the SDK built it, from the decomp's own sources (read, never edited), its names made
# ps2_* (port/sdk/libm_ps2_names.h); port/sdk/libm_ps2.c has the rest. ef_sqrt.c, wf_sqrt.c and
# w_sqrt.c are port/sdk's (the rounding ee-gcc folded, and dp-bit's NaN, written out), and atanf is
# wrapped there.
file(GLOB KF4_PS2_LIBM_SOURCES ${CMAKE_SOURCE_DIR}/decomp/src/sce/libm/math/*.c
     ${CMAKE_SOURCE_DIR}/decomp/src/sce/libm/common/*.c)
list(FILTER KF4_PS2_LIBM_SOURCES EXCLUDE REGEX "/(ef|wf|w)_sqrt\\.c$")
add_library(ps2_libm OBJECT ${KF4_PS2_LIBM_SOURCES})
target_include_directories(ps2_libm PRIVATE decomp/src/sce/libm decomp/src/sce/libc)
target_compile_options(ps2_libm PRIVATE ${KF4_GAME_FLAGS} ${KF4_EE_FLOAT_FLAGS} -w
    -include ${CMAKE_SOURCE_DIR}/port/sdk/libm_ps2_names.h)
set_source_files_properties(${CMAKE_SOURCE_DIR}/decomp/src/sce/libm/math/sf_atan.c PROPERTIES
    COMPILE_DEFINITIONS "atanf=ps2_atanf_newlib")
target_link_libraries(kf4_port PUBLIC ps2_libm)

# ---- The program ----------------------------------------------------------------------------------
set(KF4_GAME_OBJECTS)
foreach(target ${KF4_GAME_TARGETS})
    list(APPEND KF4_GAME_OBJECTS $<TARGET_OBJECTS:${target}>)
endforeach()
# Save states (port/platform/savestate.c) copy the globals of the game and of ico-audio's SPU2 as one
# block of the program's image: their objects are linked between two markers, which the linker keeps
# in that order. The program is linked at a fixed address (no ASLR), so that a state's pointers to the
# program's code and data hold in the next run of the same build.
add_library(kf4_state_begin OBJECT port/platform/state_begin.c)
add_library(kf4_state_end OBJECT port/platform/state_end.c)
add_executable(kf4 port/platform/main_host.c $<TARGET_OBJECTS:kf4_state_begin> ${KF4_GAME_OBJECTS}
    $<TARGET_OBJECTS:ico_audio> $<TARGET_OBJECTS:kf4_state_end>)
target_compile_options(kf4 PRIVATE ${KF4_GAME_FLAGS})
target_link_libraries(kf4 PRIVATE kf4_port)
if(WIN32)
    target_link_options(kf4 PRIVATE -Wl,--disable-dynamicbase)
else()
    target_link_options(kf4 PRIVATE -no-pie)
endif()

# The debugging tools (port/debug/, docs/port/README.md "Debugging"): the build a GS capture names,
# from git at each build (build_rev.h)
add_custom_target(kf4_build_rev
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/build_rev.h
        -P ${CMAKE_SOURCE_DIR}/tools/build_rev.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/build_rev.h)
add_dependencies(kf4_port kf4_build_rev)
target_include_directories(kf4_port PRIVATE ${CMAKE_BINARY_DIR})
# kf4-gsreplay draws a GS frame capture again (tools/gsreplay): the GIF stream, or the VIF1 stream
# through VU1 (vif1.c and vu1.c, built in), on paraLLEl-GS
if(TARGET kf4_gs)
    add_executable(kf4-gsreplay tools/gsreplay/gsreplay.c tools/gsreplay/nd_oracle.c tools/gsreplay/nd_oracle_280.c tools/gsreplay/nd_oracle_700.c port/platform/vif1.c port/platform/vu1.c
        port/platform/vu1_hle.c port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c port/platform/video.c port/platform/lz4_block.c)
    target_include_directories(kf4-gsreplay PRIVATE port/include)
    target_compile_options(kf4-gsreplay PRIVATE ${KF4_GAME_FLAGS})
    target_link_libraries(kf4-gsreplay PRIVATE kf4_gs Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(kf4-gsreplay PRIVATE m)
    endif()
    # The CPU model of the GS's memory (port/gs/native/gsn_vram.c) against paraLLEl-GS
    add_executable(gsn_vram_test tests/gsn_vram_test.c port/gs/native/gsn_vram.c)
    target_include_directories(gsn_vram_test PRIVATE port/include port/gs/native)
    target_link_libraries(gsn_vram_test PRIVATE kf4_gs)
    if(KF4_TESTS)
        add_test(NAME gsn_vram COMMAND gsn_vram_test)
        set_tests_properties(gsn_vram PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    # The native GS's drawing against paraLLEl-GS: random draws, the same packets to both
    add_executable(gsn_draw_test tests/gsn_draw_test.c)
    target_include_directories(gsn_draw_test PRIVATE port/include port/gs/native)
    target_link_libraries(gsn_draw_test PRIVATE kf4_gs)
    if(NOT WIN32)
        target_link_libraries(gsn_draw_test PRIVATE m)
    endif()
    if(KF4_TESTS)
        add_test(NAME gsn_draw COMMAND gsn_draw_test)
        set_tests_properties(gsn_draw PROPERTIES SKIP_RETURN_CODE 77)
        if(NOT WIN32)
            # likely lavapipe: it keeps what a process's device has had until the process ends (5 cases
            # a process), and its sampler filters paraLLEl-GS's textures otherwise than a GPU's
            set_tests_properties(gsn_draw PROPERTIES ENVIRONMENT "GSN_CHUNK=5;GSN_SOFTWARE=1")
        endif()
    endif()
endif()

# kf4-sampler, the sampling profiler (tools/sampler, docs/port/RENDERER.md "Profiling"): every thread of a
# running kf4.exe, its stacks unwound from the program's .pdata
if(WIN32)
    add_executable(kf4-sampler tools/sampler/sampler.c)
    target_link_libraries(kf4-sampler PRIVATE psapi winmm)
endif()

# The profiler's counters and what its report names as bounding the frame rate (port/platform/profile.c)
add_executable(profile_test tests/profile_test.c)
target_link_libraries(profile_test PRIVATE kf4_profile)
if(KF4_TESTS)
    add_test(NAME profile COMMAND profile_test)
endif()

# INT_MIN / -1 and a division by zero, as the EE does them (port/platform/int_divide.c)
add_executable(int_divide_test tests/int_divide_test.c port/platform/int_divide.c)
target_include_directories(int_divide_test PRIVATE port/include)
target_compile_options(int_divide_test PRIVATE ${KF4_GAME_FLAGS})

# The PS2's math library against the retail program's results (tools/ee_libm_ref.py)
add_executable(libm_test tests/libm_test.c port/sdk/libm_ps2.c $<TARGET_OBJECTS:ps2_libm>)
target_include_directories(libm_test PRIVATE port/include)
target_compile_options(libm_test PRIVATE ${KF4_GAME_FLAGS} ${KF4_EE_FLOAT_FLAGS})

# The VU1 interpreter's self-test: hand-written microcode run on port/platform/vu1.c alone
add_executable(vu1_test tests/vu1_test.c port/platform/vu1.c port/platform/vu1_hle.c port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c port/math/libvu0.c)
target_include_directories(vu1_test PRIVATE port/include)
target_compile_options(vu1_test PRIVATE -frounding-math -ffp-contract=off)
if(NOT WIN32)
    target_link_libraries(vu1_test PRIVATE m)
endif()

# Windows: put the DLLs the program needs next to it, so it runs from Explorer or a shortcut (SDL2, and
# llvm-mingw's C++ runtime, which paraLLEl-GS needs)
if(WIN32)
    get_filename_component(KF4_COMPILER_BIN ${CMAKE_C_COMPILER} DIRECTORY)
    set(KF4_RUNTIME_DLLS)
    foreach(dll libc++.dll libunwind.dll)
        find_file(KF4_DLL_${dll} ${dll} PATHS ${KF4_COMPILER_BIN} ${KF4_COMPILER_BIN}/../x86_64-w64-mingw32/bin
            NO_DEFAULT_PATH)
        if(KF4_DLL_${dll})
            list(APPEND KF4_RUNTIME_DLLS ${KF4_DLL_${dll}})
        endif()
    endforeach()
    add_custom_command(TARGET kf4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:kf4> ${KF4_RUNTIME_DLLS}
            $<TARGET_FILE_DIR:kf4>
        COMMAND_EXPAND_LISTS)
    if(KF4_FFX_DLL) # (--upscaler fsr3: the FidelityFX DLL beside kf4.exe)
        add_custom_command(TARGET kf4 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KF4_FFX_DLL} $<TARGET_FILE_DIR:kf4>/amd_fidelityfx_vk.dll)
    endif()
    if(KF4_DLSS_SDK) # (--upscaler dlss: the SDK's release build of DLSS beside kf4.exe)
        add_custom_command(TARGET kf4 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KF4_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll
                $<TARGET_FILE_DIR:kf4>/nvngx_dlss.dll)
    endif()
endif()
