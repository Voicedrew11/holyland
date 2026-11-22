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
# Without a preset (a plain `cmake -B build`) the build is linux-clang-release's: clang where it is installed (CC and
# CXX, or -DCMAKE_C_COMPILER=..., still choose), LTO and no frame pointers (KF4_LTO, KF4_FRAME_POINTERS below)
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(KF4_DEFAULT_CC clang)
    if(KF4_DEFAULT_CC)
        set(CMAKE_C_COMPILER ${KF4_DEFAULT_CC})
    endif()
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(KF4_DEFAULT_CXX clang++)
    if(KF4_DEFAULT_CXX)
        set(CMAKE_CXX_COMPILER ${KF4_DEFAULT_CXX})
    endif()
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "The build type (the presets': RelWithDebInfo)" FORCE)
endif()
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
         -Wno-missing-braces -Wno-parentheses-equality
         # the decomp as it is (patches/ leaves these alone): printf of a message variable, s8 for char,
         # `*/` paths in comments, arrays indexed past their declared length, tests of an array's address
         -Wno-format-security -Wno-pointer-sign -Wno-comment -Wno-array-bounds -Wno-pointer-bool-conversion
         -Wno-tautological-pointer-compare -Wno-unused-value)
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

# ---- Frame pointers (docs/port/PROFILING.md) ------------------------------------------------------
# The function profiler (port/include/port/sampler.h) walks stacks by frame pointers: kept in every function,
# leaves too, for about 1% of the speed. Off by default (the release's); the Linux developer presets turn it on.
option(KF4_FRAME_POINTERS "Keep frame pointers everywhere, for the function profiler" OFF)
if(KF4_FRAME_POINTERS AND NOT MSVC)
    add_compile_options(-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
endif()

# ---- Player builds ----------------------------------------------------------------------------------
# KF4_PLAYER_BUILD: the build players get (the *-release presets and the launcher's build, launcher/build_local.cpp):
# choices still being tried are fixed for them (port/include/port/scene_depth.h: the GPU's depth, the player
# menu's other levels dimmed). Off in the developers' builds, which keep every choice.
option(KF4_PLAYER_BUILD "The build players get: choices still being tried fixed (the depth)" OFF)
if(KF4_PLAYER_BUILD)
    add_compile_definitions(KF4_PLAYER_BUILD)
endif()

# ---- Release builds: LTO and PGO (the *-release presets, tools/README.md "A release build") ----
# KF4_LTO: link-time optimisation (ThinLTO with clang) of everything but the save state's image. The
# game's objects, ico-audio's and the two markers (state_begin.c, state_end.c, below) are left as
# plain objects: an LTO linker puts the code and data it compiles after every plain object, which
# would take the game's globals out from between the markers; and the game's code stays compiled
# exactly as without LTO, with its own flags, never inlined into port code nor port code into it.
# The rest (port/, the renderers, paraLLEl-GS, the tests) is LTO'd; their per-file float flags
# (-frounding-math, -fdenormal-fp-math) are function attributes the cross-file inliner respects.
# On by default (the release's); the developer presets turn it off.
option(KF4_LTO "Link-time optimisation of everything but the save state's image (the game, ico-audio)" ON)
# KF4_PGO: profile-guided optimisation. "generate" builds a program that writes a profile when it
# stops after --frames (clang: where LLVM_PROFILE_FILE says; gcc: .gcda files beside the objects).
# Then, in the same build folder, the merged profile (.profdata) with clang, or "use" with gcc (the
# .gcda files beside the objects). tools/pgo_train.py does all the steps. PGO covers the game's code
# too: it moves code and changes what is inlined within a file, not how a float is computed; the
# frames and state are checked the same as without it (tools/build_compare.py).
set(KF4_PGO "" CACHE STRING "Profile-guided optimisation: generate, then the merged .profdata (clang) or use (gcc)")
if(KF4_LTO)
    enable_language(CXX) # (the renderers are C++, enabled further down)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT KF4_LTO_SUPPORTED OUTPUT KF4_LTO_ERROR LANGUAGES C CXX)
    if(NOT KF4_LTO_SUPPORTED)
        message(FATAL_ERROR "KF4_LTO: the compiler or linker cannot do LTO: ${KF4_LTO_ERROR}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    message(STATUS "LTO: on (not for the game's objects, ico-audio's or the state markers)")
endif()
if(KF4_PGO STREQUAL "generate")
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-fprofile-generate)
        add_link_options(-fprofile-generate)
    else()
        add_compile_options(-fprofile-generate -fprofile-update=prefer-atomic)
        add_link_options(-fprofile-generate)
    endif()
    add_compile_definitions(KF4_PGO_GENERATE)
    message(STATUS "PGO: building a program that writes a profile")
elseif(KF4_PGO)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT EXISTS ${KF4_PGO})
        message(FATAL_ERROR "KF4_PGO: no profile at ${KF4_PGO} (clang wants the merged .profdata)")
    elseif(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT KF4_PGO STREQUAL "use")
        message(FATAL_ERROR "KF4_PGO: generate or use with gcc, not ${KF4_PGO}")
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
        # a file the profile does not cover (a test, a tool) is built as without it, quietly
        add_compile_options(-fprofile-use=${KF4_PGO} -Wno-profile-instr-unprofiled
            -Wno-profile-instr-out-of-date -Wno-backend-plugin)
    else()
        add_compile_options(-fprofile-use -fprofile-partial-training -Wno-missing-profile)
    endif()
    message(STATUS "PGO: using ${KF4_PGO}")
endif()

# ---- The PS2 side ---------------------------------------------------------------------------------
# The release this build is (port/include/port/version.h): the launcher gives it from the release's tag;
# empty in a developer's build
set(KF4_VERSION "" CACHE STRING "The release's version (0.2.0), set by the launcher's build; empty for a developer's")
if(NOT KF4_VERSION MATCHES "^([0-9]+(\\.[0-9]+)*)?$")
    message(FATAL_ERROR "KF4_VERSION must be dotted numbers (0.2.0), not '${KF4_VERSION}'")
endif()
configure_file(port/platform/version.c.in ${CMAKE_BINARY_DIR}/version.c @ONLY)

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
    port/platform/interp_camera.c
    port/platform/adaptive_sync.c
    port/platform/field_frames.c
    port/platform/video.c
    port/platform/refresh_clock.c
    port/platform/mouse_lead.c
    port/platform/screen_vblank.c
    port/platform/disc.c
    port/platform/disc_image.c
    port/platform/iso9660.c
    port/platform/ipu.c
    port/platform/host_time.c
    port/platform/int_divide.c
    port/math/libvu0.c
    port/sdk/libcdvd.c
    port/sdk/libdma.c
    port/sdk/libgraph.c
    port/platform/save_files.c
    port/sdk/libmpeg.c
    port/sdk/mpeg2.c
    port/sdk/libpad.c
    port/sdk/controls.c
    port/sdk/twin_stick.c
    port/sdk/walk_keys.c
    port/sdk/libsdr.c
    port/platform/savestate.c
    port/platform/quicksave.c
    port/platform/teleport.c
    port/platform/cheats.c
    port/platform/cinematic.c
    port/platform/cinematic_math.c
    port/platform/automap.c
    port/platform/lz4_block.c
    port/debug/debug.c
    port/platform/profile_run.c
    port/debug/gs_capture.c
    port/overlay/settings.c
    port/platform/data_dir.c
    port/platform/update_notice.c
    ${CMAKE_BINARY_DIR}/version.c
    port/overlay/live_settings.c
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
include(cmake/libchdr.cmake) # CHD disc images (port/platform/disc_image.c)
kf4_add_libchdr(${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kf4_port PRIVATE kf4_chdr)
# the plain GS types the PC renderer and the port share (port/gs/shared/gsn.h, gs_api.h includes it)
target_include_directories(kf4_port PRIVATE port/gs/shared)
# native draw's renderer core (port/include/port/render.h): the game's side hands it draws, the PC renderer
# draws them (docs/port/NATIVE_DRAW.md)
enable_language(CXX)
add_library(kf4_render STATIC port/render/render.cpp port/render/vu1_record.cpp port/render/ripple.cpp)
# (vu1_record.cpp's fast arithmetic sets MXCSR's rounding and reads its flags: the compiler must neither assume the
# rounding nor move a float operation across those). gcc has no -ffp-exception-behavior: its -ftrapping-math (on by
# default, stated here) with -frounding-math is its equivalent; vu1_record_test compares the fast way with the exact one
set_source_files_properties(port/render/vu1_record.cpp PROPERTIES COMPILE_OPTIONS
                            "-frounding-math;$<IF:$<CXX_COMPILER_ID:GNU>,-ftrapping-math,-ffp-exception-behavior=strict>")
target_include_directories(kf4_render PUBLIC port/include)
set_target_properties(kf4_render PROPERTIES CXX_STANDARD 14)
target_link_libraries(kf4_port PUBLIC kf4_render)
# the native draws' moves in the pictures between, for the PC renderer (port/render/part_moves.hpp): a library of its own
# (C++20; the renderer core is C++14), with its test below
add_library(kf4_part_moves STATIC port/render/part_moves.cpp port/render/pass_draw.cpp)
target_include_directories(kf4_part_moves PUBLIC port/include port/render)
set_target_properties(kf4_part_moves PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(kf4_part_moves PRIVATE -Wall -Wextra -Wshadow)
endif()
target_link_libraries(kf4_port PUBLIC ico_audio)
# the profiler's counters (port/include/port/profile.h, Shift+P): a library of their own, so that the
# GS libraries (kf4_gs, built into tests without the port) add to them as well; with them each thread's note for the
# crash report (port/include/port/crash_note.h), which the GS libraries write too
add_library(kf4_profile STATIC port/platform/profile.c port/platform/zones.c port/platform/crash_note.c)
target_include_directories(kf4_profile PUBLIC port/include)
target_link_libraries(kf4_port PUBLIC kf4_profile)
# the frame profiler's frames, worked out from the zones' rings (port/include/port/zone_record.hpp) on a thread of
# their own (port/include/port/frame_profiler.h): Shift+P's "Frames", KF4_PROFILE and its CSV
add_library(kf4_zone_record STATIC port/platform/zone_record.cpp port/platform/frame_profiler.cpp)
target_include_directories(kf4_zone_record PUBLIC port/include)
set_target_properties(kf4_zone_record PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(kf4_zone_record PRIVATE -Wall -Wextra -Wshadow)
endif()
target_link_libraries(kf4_zone_record PUBLIC kf4_profile)
target_link_libraries(kf4_port PUBLIC kf4_zone_record)
# the function profiler (port/include/port/sampler.h): a library of its own, for its test; with liblzma it names
# the functions of the system's stripped libraries too (their MiniDebugInfo)
add_library(kf4_sampler STATIC port/platform/sampler.c port/platform/sample_table.c port/platform/elf_symbols.c)
target_include_directories(kf4_sampler PUBLIC port/include)
target_link_libraries(kf4_sampler PUBLIC Threads::Threads)
if(NOT WIN32)
    target_compile_options(kf4_sampler PRIVATE -Wall -Wextra -Wshadow)
    find_package(LibLZMA QUIET)
    if(LibLZMA_FOUND)
        target_compile_definitions(kf4_sampler PRIVATE KF4_HAVE_LZMA)
        target_link_libraries(kf4_sampler PRIVATE LibLZMA::LibLZMA)
    else()
        message(STATUS "liblzma not found: the profiler names only the exported functions of stripped libraries")
    endif()
endif()
target_link_libraries(kf4_port PUBLIC kf4_sampler)
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
# The screen's vblanks for the refresh clock (port/platform/screen_vblank.c): the desktop compositor's on
# Windows, the kernel's (libdrm, where it is) on Linux
if(WIN32)
    target_link_libraries(kf4_port PRIVATE dwmapi)
elseif(NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBDRM QUIET IMPORTED_TARGET libdrm)
    endif()
    # a release's build has no pkg-config: libdrm is found in its sysroot by its files (docs/port/LAUNCHER.md)
    if(NOT LIBDRM_FOUND)
        find_path(KF4_LIBDRM_INCLUDE xf86drm.h PATH_SUFFIXES libdrm)
        find_library(KF4_LIBDRM_LIBRARY drm)
        if(KF4_LIBDRM_INCLUDE AND KF4_LIBDRM_LIBRARY)
            add_library(kf4_libdrm INTERFACE)
            target_include_directories(kf4_libdrm INTERFACE ${KF4_LIBDRM_INCLUDE} ${KF4_LIBDRM_INCLUDE}/libdrm)
            target_link_libraries(kf4_libdrm INTERFACE ${KF4_LIBDRM_LIBRARY})
            set(LIBDRM_FOUND TRUE)
        endif()
    endif()
    if(LIBDRM_FOUND)
        target_compile_definitions(kf4_port PRIVATE KF4_HAVE_LIBDRM)
        if(TARGET PkgConfig::LIBDRM)
            target_link_libraries(kf4_port PRIVATE PkgConfig::LIBDRM)
        else()
            target_link_libraries(kf4_port PRIVATE kf4_libdrm)
        endif()
    else()
        message(STATUS "libdrm not found: the screen's vblanks are not known, and --frame-rate monitor times the pictures by the rate alone")
    endif()
endif()
# SDL3 for the window, the keyboard, the mouse, controllers and sound; without it the pad takes only a pad
# script and there is no window
option(KF4_USE_SDL "Use SDL3 for the window, input and sound" ON)
if(KF4_USE_SDL)
    find_package(SDL3 CONFIG)
endif()
if(KF4_USE_SDL AND SDL3_FOUND)
    target_compile_definitions(kf4_port PRIVATE KF4_HAVE_SDL)
    target_link_libraries(kf4_port PRIVATE SDL3::SDL3)
    # The window's icon (port/platform/window_icon.c), built in
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/window_icon_bmp.c
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/packaging/shared/holyland.bmp
                -DOUT=${CMAKE_BINARY_DIR}/window_icon_bmp.c -DNAME=port_window_icon_bmp
                -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/packaging/shared/holyland.bmp ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
        COMMENT "The window's icon" VERBATIM)
    target_sources(kf4_port PRIVATE port/platform/window_icon.c ${CMAKE_BINARY_DIR}/window_icon_bmp.c)
    # The settings overlay (F1): Dear ImGui (third_party/imgui) on the window's SDL_Renderer
    enable_language(CXX)
    add_library(imgui STATIC
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_sdl3.cpp
        third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(imgui PUBLIC third_party/imgui)
    target_link_libraries(imgui PUBLIC SDL3::SDL3)
    set_target_properties(imgui PROPERTIES CXX_STANDARD 11)
    target_sources(kf4_port PRIVATE port/overlay/overlay.cpp port/overlay/update_badge.cpp port/overlay/automap_view.cpp
                   port/overlay/profiler.cpp
                   port/overlay/frames_view.cpp
                   port/overlay/profile_run_view.cpp)
    target_link_libraries(kf4_port PRIVATE imgui)
else()
    message(STATUS "SDL3 not used: the pad takes only a pad script (--input), and there is no window")
    target_sources(kf4_port PRIVATE port/platform/update_offer_none.c)
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
    # Granite with Holyland's edits, made in its checkout (cmake/granite_edits.cmake lists them)
    include(cmake/granite_edits.cmake)
    kf4_granite_edits(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite)
    # Granite asks for CMake 3.6, before CMP0069: without this its targets would not take the release's LTO
    # (and CMake warns about it); and its policies as of 3.10 at least, the oldest CMake supports without a warning
    set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    add_subdirectory(third_party/parallel-gs EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    unset(CMAKE_POLICY_DEFAULT_CMP0069)
    # (clang) Granite's command_buffer.cpp keeps a static inline helper it no longer calls (is_legacy_layout)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND TARGET granite-vulkan)
        set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite/vulkan/command_buffer.cpp
            TARGET_DIRECTORY granite-vulkan
            PROPERTIES COMPILE_OPTIONS -Wno-unused-function)
    endif()
    # paraLLEl-GS as a library of its own: the developer comparison (--renderer gs), never a player's GS
    add_library(kf4_gs STATIC port/gs/gs_parallel.cpp)
    target_include_directories(kf4_gs PRIVATE port/include)
    target_link_libraries(kf4_gs PRIVATE parallel-gs)
    target_link_libraries(kf4_gs PUBLIC kf4_render)
    target_link_libraries(kf4_gs PUBLIC kf4_profile)
    set_target_properties(kf4_gs PROPERTIES CXX_STANDARD 14)
    target_compile_definitions(kf4_port PRIVATE KF4_HAVE_PARALLEL_GS)
    target_link_libraries(kf4_port PUBLIC kf4_gs)
    # the PC renderer (port/gs/pc/, docs/port/PC_RENDERER.md), on Granite as well, with what it shares with the scene
    # renderer in port/gs/shared (gs_drawing.h, the native draws: pcr_native.cpp) and gsn.h's plain types
    # (its parts without Vulkan, kf4_pc_gif and kf4_pc_textures, are libraries of their own with tests, below)
    file(GLOB KF4_PC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/port/gs/pc/*.cpp)
    list(APPEND KF4_PC_SOURCES ${CMAKE_SOURCE_DIR}/port/gs/shared/pcr_native.cpp)
    # (the pipeline seed, port/gs/pc/pipeline_seed.bin, as an array: cmake/pipeline_seed.cmake)
    set(KF4_PIPELINE_SEED ${CMAKE_SOURCE_DIR}/port/gs/pc/pipeline_seed.bin)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/pcr_pipeline_seed.cpp
        COMMAND ${CMAKE_COMMAND} -DSEED=${KF4_PIPELINE_SEED} -DOUT=${CMAKE_BINARY_DIR}/pcr_pipeline_seed.cpp
                -P ${CMAKE_SOURCE_DIR}/cmake/pipeline_seed.cmake
        DEPENDS ${KF4_PIPELINE_SEED} ${CMAKE_SOURCE_DIR}/cmake/pipeline_seed.cmake
        COMMENT "The PC renderer's pipeline seed" VERBATIM)
    list(APPEND KF4_PC_SOURCES ${CMAKE_BINARY_DIR}/pcr_pipeline_seed.cpp)
    add_library(kf4_pc STATIC ${KF4_PC_SOURCES})
    target_include_directories(kf4_pc PRIVATE port/include third_party/smaa PUBLIC port/gs/pc port/gs/shared)
    target_link_libraries(kf4_pc PRIVATE parallel-gs kf4_pc_gif kf4_pc_textures kf4_render kf4_part_moves
                          kf4_profile) # (port_crash_note_set: pcr_pipelines.cpp)
    target_compile_options(kf4_pc PRIVATE -Wall -Wextra -Wno-missing-field-initializers) # (the last: Granite's headers)
    set_target_properties(kf4_pc PROPERTIES CXX_STANDARD 20)
    target_link_libraries(kf4_port PUBLIC kf4_pc)
    # Khronos's synchronization2 layer in vk_layers/ beside kf4, for a driver without VK_KHR_synchronization2, which
    # Granite cannot do without (port/gs/pc/pcr_sync2.h); built whenever kf4 is
    include(cmake/vk_sync2_layer.cmake)
    kf4_vk_sync2_layer(${CMAKE_SOURCE_DIR}/third_party/parallel-gs/Granite/third_party/khronos/vulkan-headers/include)
    add_dependencies(kf4_pc kf4_vk_sync2_layer)
    # the scene renderer (port/gs/scene/, docs/plans/scene-renderer-design.md: --renderer scene, developers' only), under
    # the PC renderer's shell
    file(GLOB KF4_SCENE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/port/gs/scene/*.cpp)
    add_library(kf4_scene STATIC ${KF4_SCENE_SOURCES})
    target_include_directories(kf4_scene PRIVATE port/include PUBLIC port/gs/scene)
    target_link_libraries(kf4_scene PRIVATE parallel-gs kf4_pc kf4_pc_gif kf4_pc_textures kf4_profile)
    target_compile_options(kf4_scene PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    set_target_properties(kf4_scene PROPERTIES CXX_STANDARD 20)
    target_link_libraries(kf4_port PUBLIC kf4_scene)
    # --presenter vulkan (port/platform/present_vk.cpp): the window's swapchain on the PC renderer's
    # device, with the overlay drawn by Dear ImGui's Vulkan backend (functions loaded at run time)
    if(TARGET imgui)
        target_sources(imgui PRIVATE third_party/imgui/backends/imgui_impl_vulkan.cpp)
        target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
        target_link_libraries(imgui PUBLIC Vulkan::Headers)
        target_sources(kf4_port PRIVATE port/platform/present_vk.cpp)
        target_compile_definitions(kf4_port PRIVATE KF4_HAVE_VULKAN_PRESENTER)
        # The player menu (port/ui/, port/include/port/player_ui.h): RmlUi, drawn by the Vulkan presenter
        # (port/platform/present_vk_ui.cpp); its files (port/ui/res/) carried in the game as arrays
        include(cmake/rmlui.cmake)
        set(KF4_UI_RES ${CMAKE_SOURCE_DIR}/port/ui/res)
        file(GLOB_RECURSE KF4_UI_FILES CONFIGURE_DEPENDS RELATIVE ${KF4_UI_RES}
             ${KF4_UI_RES}/*.rml ${KF4_UI_RES}/*.rcss ${KF4_UI_RES}/*.ttf ${KF4_UI_RES}/*.alpha)
        list(SORT KF4_UI_FILES)
        list(TRANSFORM KF4_UI_FILES PREPEND ${KF4_UI_RES}/ OUTPUT_VARIABLE KF4_UI_FILE_PATHS)
        add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/ui_assets.cpp
            COMMAND ${CMAKE_COMMAND} -DROOT=${KF4_UI_RES} "-DFILES=${KF4_UI_FILES}"
                    -DOUT=${CMAKE_BINARY_DIR}/ui_assets.cpp -P ${CMAKE_SOURCE_DIR}/cmake/ui_assets.cmake
            DEPENDS ${KF4_UI_FILE_PATHS} ${CMAKE_SOURCE_DIR}/cmake/ui_assets.cmake
            COMMENT "The player menu's files" VERBATIM)
        add_library(kf4_player_ui STATIC port/ui/player_ui.cpp port/ui/menu_model.cpp port/ui/settings_rows.cpp
                    port/ui/setting_row.cpp port/ui/rows_rml.cpp port/ui/setup_pages.cpp port/ui/setup_model.cpp
                    port/ui/setup_view.cpp port/ui/ui_render.cpp port/ui/ui_ties.cpp port/ui/game_menu_art.cpp
                    port/ui/pad_glyphs.cpp ${CMAKE_BINARY_DIR}/ui_assets.cpp)
        target_include_directories(kf4_player_ui PRIVATE port/include port/ui)
        target_compile_definitions(kf4_player_ui PRIVATE KF4_HAVE_PLAYER_UI KF4_HAVE_SDL)
        target_link_libraries(kf4_player_ui PRIVATE kf4_rmlui SDL3::SDL3)
        target_compile_options(kf4_player_ui PRIVATE -Wall -Wextra -Wshadow)
        set_target_properties(kf4_player_ui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        target_sources(kf4_port PRIVATE port/platform/present_vk_ui.cpp)
        target_compile_definitions(kf4_port PRIVATE KF4_HAVE_PLAYER_UI)
        target_link_libraries(kf4_port PRIVATE kf4_player_ui)
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
# A release (packaging/payload.sh) carries decomp/src/game and src/nonmatching with the patches already
# applied and no git, so that the launcher can build on a player's machine with only the shipped tools
# (docs/port/LAUNCHER.md). There the copy is taken as it is.
option(KF4_DECOMP_PREPATCHED "decomp/ already has the patches applied (a release's source; no git needed)" OFF)
if(KF4_DECOMP_PREPATCHED)
    file(REMOVE_RECURSE ${KF4_STAGE})
    file(MAKE_DIRECTORY ${KF4_STAGE}/src)
    file(COPY ${KF4_DECOMP}/src/game ${KF4_DECOMP}/src/nonmatching DESTINATION ${KF4_STAGE}/src
        NO_SOURCE_PERMISSIONS)
else()
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
    file(COPY ${KF4_DECOMP}/src/game ${KF4_DECOMP}/src/nonmatching DESTINATION ${KF4_STAGE}/src
        NO_SOURCE_PERMISSIONS)
    # its own repo, so that git apply works on paths relative to the copy's root (not Holyland's)
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
endif()

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
        # no memory card on the PC: game/saves.c stands in for the card layer (save_card.c, and mc.c under it)
        list(FILTER sources EXCLUDE REGEX "/save/(save_card|mc)\\.c$")
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
add_library(game_data OBJECT ${game_data_sources} game/state_log.c game/widescreen.c game/fov.c game/draw_distance.c game/frame_interp.c game/native_draw/native_draw.c game/native_draw/nd_entities.c game/native_draw/nd_arm.c game/native_draw/nd_objects.c game/native_draw/nd_sky.c game/native_draw/nd_effects.c game/native_draw/nd_shadows.c game/native_draw/nd_water.c game/native_draw/nd_passes.c game/quicksave.c game/saves.c game/cheats.c game/attack_tracking.c game/cinematic.c game/screen_fx.c game/menu_pointer.c
    port/debug/game_dump.c) # F10's dump reads the game's structures (port/include/port/debug.h)
list(APPEND KF4_GAME_TARGETS game_data)
# PC mods built as game code whose state is the host's: their objects are linked after the save state's
# image (below), since their globals hold the host's memory (malloc'd buffers), which a state cannot carry
# from one run, or one moment, to another. A part that matters to a state writes it itself (the automap's
# fog of war: Automap_State, port/platform/savestate.c).
add_library(game_host OBJECT game/automap.c game/automap_guide.c game/automap_script.c)
# the extended stats mod (port/include/port/stats.h, docs/port/EXTENDED_STATS.md)
target_sources(game_data PRIVATE game/stats.c)
target_sources(kf4_port PRIVATE port/overlay/stats_mod.c)
if(TARGET imgui)
    target_sources(kf4_port PRIVATE port/overlay/stats_panel.cpp port/overlay/stats_menus.cpp
                   port/overlay/stats_timers.cpp)
endif()
# the hands mod (port/include/port/hands.h)
target_sources(game_data PRIVATE game/hands.c game/hands_cross.c game/hands_put.c)
target_sources(game_data PRIVATE game/menu_sounds.c)
target_sources(kf4_port PRIVATE port/platform/hands.c port/platform/hands_rules.c)
# the game's main is called by the host's (port/platform/main_host.c)
set_source_files_properties(${KF4_SRC}/src/game/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=kf4_game_main")

# Every game file calls the PS2's math library, not the host's (port/include/port/libm_ps2.h)
foreach(target ${KF4_GAME_TARGETS} game_host)
    target_include_directories(${target} PRIVATE ${KF4_SRC}/src/game port/include)
    target_compile_options(${target} PRIVATE ${KF4_GAME_FLAGS} ${KF4_EE_FLOAT_FLAGS} ${KF4_GAME_ERRORS}
        -include ${CMAKE_SOURCE_DIR}/port/include/port/libm_ps2.h)
endforeach()
add_custom_target(game DEPENDS ${KF4_GAME_TARGETS} game_host)

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
# never LTO'd (KF4_LTO, above): the linker would lay their data out after every plain object's
set_target_properties(kf4_state_begin kf4_state_end ico_audio ${KF4_GAME_TARGETS} game_host PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION OFF)
add_executable(kf4 port/platform/main_host.c $<TARGET_OBJECTS:kf4_state_begin> ${KF4_GAME_OBJECTS}
    $<TARGET_OBJECTS:ico_audio> $<TARGET_OBJECTS:kf4_state_end> $<TARGET_OBJECTS:game_host>)
target_compile_options(kf4 PRIVATE ${KF4_GAME_FLAGS})
target_link_libraries(kf4 PRIVATE kf4_port)
if(WIN32)
    # the icon (packaging/shared/holyland.ico) in the .exe, for Explorer, the taskbar and the window
    enable_language(RC)
    set(HOLYLAND_ICON ${CMAKE_SOURCE_DIR}/packaging/shared/holyland.ico)
    configure_file(packaging/shared/holyland.rc.in ${CMAKE_BINARY_DIR}/holyland.rc @ONLY)
    target_sources(kf4 PRIVATE ${CMAKE_BINARY_DIR}/holyland.rc)
    target_link_options(kf4 PRIVATE -Wl,--disable-dynamicbase)
else()
    target_link_options(kf4 PRIVATE -no-pie)
endif()

# The debugging tools (port/debug/, docs/port/README.md "Debugging"): the build a GS capture names,
# from git at each build (build_rev.h)
add_custom_target(kf4_build_rev
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/build_rev.h -DVERSION=${KF4_VERSION}
        -P ${CMAKE_SOURCE_DIR}/tools/build_rev.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/build_rev.h)
add_dependencies(kf4_port kf4_build_rev)
target_include_directories(kf4_port PRIVATE ${CMAKE_BINARY_DIR})
# kf4-gsreplay draws a GS frame capture again (tools/gsreplay): the GIF stream, or the VIF1 stream
# through VU1 (vif1.c and vu1.c, built in), on paraLLEl-GS
if(TARGET kf4_gs)
    add_executable(kf4-gsreplay tools/gsreplay/gsreplay.c port/platform/vif1.c port/platform/vu1.c
        port/platform/vu1_hle.c port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c port/platform/video.c port/platform/refresh_clock.c port/platform/adaptive_sync.c port/platform/lz4_block.c)
    target_include_directories(kf4-gsreplay PRIVATE port/include port/gs/shared port/gs/pc port/gs/scene)
    target_compile_options(kf4-gsreplay PRIVATE ${KF4_GAME_FLAGS})
    target_link_libraries(kf4-gsreplay PRIVATE kf4_gs kf4_scene kf4_pc Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(kf4-gsreplay PRIVATE m)
    endif()
    # kf4-layerbench (tools/layer_bench): what a full-screen layer costs on this GPU, with no game in it. Built where
    # glslangValidator is (its shaders are compiled here, not committed).
    find_program(KF4_GLSLANG glslangValidator)
    if(KF4_GLSLANG)
        set(KF4_LB ${CMAKE_SOURCE_DIR}/tools/layer_bench)
        set(KF4_LB_OUT ${CMAKE_BINARY_DIR}/layer_bench)
        file(MAKE_DIRECTORY ${KF4_LB_OUT})
        set(KF4_LB_PARTS)
        foreach(name layer_vert layer_frag_fill layer_frag_textured layer_frag_discards layer_frag_own_depth
                     layer_frag_mask_one layer_frag_mask_all)
            list(APPEND KF4_LB_PARTS ${KF4_LB_OUT}/${name}.inc)
        endforeach()
        # (all seven in one step, quietly: cmake/layer_bench_shaders.cmake lists each one's source and defines)
        add_custom_command(OUTPUT ${KF4_LB_PARTS}
            COMMAND ${CMAKE_COMMAND} -DGLSLANG=${KF4_GLSLANG} -DSRC=${KF4_LB} -DOUT=${KF4_LB_OUT}
                    -P ${CMAKE_SOURCE_DIR}/cmake/layer_bench_shaders.cmake
            DEPENDS ${KF4_LB}/layer.vert ${KF4_LB}/layer.frag ${CMAKE_SOURCE_DIR}/cmake/layer_bench_shaders.cmake
            COMMENT "The layer bench's shaders" VERBATIM)
        file(WRITE ${KF4_LB_OUT}/layer_bench_shaders.h
            "#include <stdint.h>\n#include \"layer_vert.inc\"\n#include \"layer_frag_fill.inc\"\n#include \"layer_frag_textured.inc\"\n#include \"layer_frag_discards.inc\"\n#include \"layer_frag_own_depth.inc\"\n#include \"layer_frag_mask_one.inc\"\n#include \"layer_frag_mask_all.inc\"\n")
        add_executable(kf4-layerbench tools/layer_bench/layer_bench.cpp ${KF4_LB_PARTS})
        target_include_directories(kf4-layerbench PRIVATE ${KF4_LB_OUT})
        target_link_libraries(kf4-layerbench PRIVATE parallel-gs Threads::Threads)
        target_compile_options(kf4-layerbench PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
        set_target_properties(kf4-layerbench PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    endif()
    # The CPU model of the GS's memory (port/gs/shared/gsn_vram.c) against paraLLEl-GS
    add_executable(gsn_vram_test tests/gsn_vram_test.c port/gs/shared/gsn_vram.c)
    target_include_directories(gsn_vram_test PRIVATE port/include port/gs/shared)
    target_link_libraries(gsn_vram_test PRIVATE kf4_gs)
    if(KF4_TESTS)
        add_test(NAME gsn_vram COMMAND gsn_vram_test)
        set_tests_properties(gsn_vram PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# kf4-sampler, the sampling profiler on Windows (tools/sampler, docs/port/PROFILING.md): every thread of a
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

# The frame profiler: the zones' rings read while written, and the frames, self and inclusive times worked out
# from them (port/platform/zones.c, zone_record.cpp)
add_executable(zones_test tests/zones_test.cpp)
target_link_libraries(zones_test PRIVATE kf4_zone_record)
set_target_properties(zones_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(zones_test PRIVATE -Wall -Wextra -Wshadow)
endif()
find_package(Threads REQUIRED)
target_link_libraries(zones_test PRIVATE Threads::Threads)
if(KF4_TESTS)
    add_test(NAME zones COMMAND zones_test)
endif()

# The function profiler: its counts, the ELF symbols that name them, and a live run on a busy thread
# (port/platform/sample_table.c, elf_symbols.c, sampler.c)
add_executable(sampler_test tests/sampler_test.c)
target_link_libraries(sampler_test PRIVATE kf4_sampler)
if(NOT WIN32)
    target_compile_options(sampler_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME sampler COMMAND sampler_test)
endif()

# The update notice's rules and the data folder's place, shared by the game and the launcher
# (port/platform/update_notice.c, data_dir.c; docs/port/LAUNCHER.md)
add_executable(update_notice_test tests/update_notice_test.c port/platform/update_notice.c port/platform/data_dir.c
    third_party/cjson/cJSON.c)
target_include_directories(update_notice_test PRIVATE port/include third_party/cjson)
if(NOT WIN32)
    target_compile_options(update_notice_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME update_notice COMMAND update_notice_test)
endif()

# An ISO 9660 image's files, as the game's disc reader and the launcher's disc check list them
# (port/platform/iso9660.c, through disc_image.c): a small image made by the test, broken ones, and the same
# image as CHDs (tests/data/: a DVD's and a CD's, made with chdman)
add_executable(iso9660_test tests/iso9660_test.c port/platform/iso9660.c port/platform/disc_image.c)
target_include_directories(iso9660_test PRIVATE port/include)
target_link_libraries(iso9660_test PRIVATE kf4_chdr)
target_compile_definitions(iso9660_test PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
if(NOT WIN32)
    target_compile_options(iso9660_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME iso9660 COMMAND iso9660_test)
endif()

# The picture's anti-aliasing methods by name (port/include/port/antialias.h, header only)
add_executable(antialias_test tests/antialias_test.c)
target_include_directories(antialias_test PRIVATE port/include)
if(NOT WIN32)
    target_compile_options(antialias_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME antialias COMMAND antialias_test)
endif()

# The scene renderer's depth modes by name, and the developers' variables over them (port/include/port/scene_depth.h)
add_executable(scene_depth_test tests/scene_depth_test.c)
target_include_directories(scene_depth_test PRIVATE port/include)
if(NOT WIN32)
    target_compile_options(scene_depth_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME scene_depth COMMAND scene_depth_test)
endif()

# The screen's refresh clock: its period and phase from the times pictures were seen (port/platform/refresh_clock.c)
add_executable(refresh_clock_test tests/refresh_clock_test.c port/platform/refresh_clock.c)
target_include_directories(refresh_clock_test PRIVATE port/include)
if(NOT WIN32)
    target_link_libraries(refresh_clock_test PRIVATE m)
endif()
if(KF4_TESTS)
    add_test(NAME refresh_clock COMMAND refresh_clock_test)
endif()

# Adaptive sync's cap under a VRR screen's rate (port/platform/adaptive_sync.c)
add_executable(adaptive_sync_test tests/adaptive_sync_test.c port/platform/adaptive_sync.c)
target_include_directories(adaptive_sync_test PRIVATE port/include)
if(KF4_TESTS)
    add_test(NAME adaptive_sync COMMAND adaptive_sync_test)
endif()

# Whole frames: which vblanks are drawn again for the other field (port/platform/field_frames.c)
add_executable(field_frames_test tests/field_frames_test.c port/platform/field_frames.c)
target_include_directories(field_frames_test PRIVATE port/include)
if(KF4_TESTS)
    add_test(NAME field_frames COMMAND field_frames_test)
endif()

# Instant mouse look's turn of the pictures between (port/platform/mouse_lead.c)
add_executable(mouse_lead_test tests/mouse_lead_test.c port/platform/mouse_lead.c port/math/libvu0.c)
target_include_directories(mouse_lead_test PRIVATE port/include)
if(NOT WIN32)
    target_link_libraries(mouse_lead_test PRIVATE m pthread)
endif()
if(KF4_TESTS)
    add_test(NAME mouse_lead COMMAND mouse_lead_test)
endif()

# The menus' pointer: hit-testing lines, stepping the cursor, clicks (game/menu_pointer.c)
add_executable(menu_pointer_test tests/menu_pointer_test.c game/menu_pointer.c)
target_include_directories(menu_pointer_test PRIVATE port/include)
if(KF4_TESTS)
    add_test(NAME menu_pointer COMMAND menu_pointer_test)
endif()

# The pictures between's cameras: cuts, and the camera between (port/platform/interp_camera.c)
add_executable(interp_camera_test tests/interp_camera_test.c port/platform/interp_camera.c port/platform/mouse_lead.c
    port/math/libvu0.c)
target_include_directories(interp_camera_test PRIVATE port/include)
if(NOT WIN32)
    target_link_libraries(interp_camera_test PRIVATE m pthread)
endif()
if(KF4_TESTS)
    add_test(NAME interp_camera COMMAND interp_camera_test)
endif()

# The cinematic camera's arithmetic: the lag, the turn the short way round, the look filter (port/platform/cinematic_math.c)
add_executable(cinematic_test tests/cinematic_test.c port/platform/cinematic_math.c)
target_include_directories(cinematic_test PRIVATE port/include)
if(NOT WIN32)
    target_compile_options(cinematic_test PRIVATE -Wall -Wextra -Wshadow)
    target_link_libraries(cinematic_test PRIVATE m)
endif()
if(KF4_TESTS)
    add_test(NAME cinematic COMMAND cinematic_test)
endif()

# The saves as files: written whole or not at all, read back, mended after a crash (port/platform/save_files.c)
add_executable(save_files_test tests/save_files_test.c port/platform/save_files.c)
target_include_directories(save_files_test PRIVATE port/include)
if(NOT WIN32)
    target_compile_options(save_files_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME save_files COMMAND save_files_test)
endif()

# Twin-stick controls: the sticks as the game's buttons and the speeds they set (port/sdk/twin_stick.c)
add_executable(hands_test tests/hands_test.c port/platform/hands_rules.c)
target_include_directories(hands_test PRIVATE port/include)
if(NOT WIN32)
    target_compile_options(hands_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME hands COMMAND hands_test)
endif()

add_executable(twin_stick_test tests/twin_stick_test.c port/sdk/twin_stick.c)
target_include_directories(twin_stick_test PRIVATE port/include)
if(NOT WIN32)
    target_link_libraries(twin_stick_test PRIVATE m)
    target_compile_options(twin_stick_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME twin_stick COMMAND twin_stick_test)
endif()

# The keyboard's walking: W A S D as the game's walking buttons, twin-stick on or off (port/sdk/walk_keys.c)
add_executable(walk_keys_test tests/walk_keys_test.c port/sdk/walk_keys.c port/sdk/twin_stick.c)
target_include_directories(walk_keys_test PRIVATE port/include)
if(NOT WIN32)
    target_link_libraries(walk_keys_test PRIVATE m)
    target_compile_options(walk_keys_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME walk_keys COMMAND walk_keys_test)
endif()

# The scene renderer's pictures by budget: when each is begun, which are left out (port/gs/pc/pcr_budget.h)
add_executable(pcr_budget_test tests/pcr_budget_test.cpp)
target_include_directories(pcr_budget_test PRIVATE port/gs/pc)
set_target_properties(pcr_budget_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_budget_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_budget COMMAND pcr_budget_test)
endif()

# The synchronization2 layer's choices: the loader's variable, its value, which GPUs need the layer (port/gs/pc/pcr_sync2.h)
add_executable(pcr_sync2_test tests/pcr_sync2_test.cpp)
target_include_directories(pcr_sync2_test PRIVATE port/gs/pc
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/parallel-gs/Granite/third_party/khronos/vulkan-headers/include)
set_target_properties(pcr_sync2_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_sync2_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_sync2 COMMAND pcr_sync2_test)
endif()

# --frame-rate unlocked: which pictures the PC renderer's pump leaves out as too late (port/gs/pc/pcr_late.h)
add_executable(pcr_late_test tests/pcr_late_test.cpp)
target_include_directories(pcr_late_test PRIVATE port/gs/pc)
set_target_properties(pcr_late_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_late_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_late COMMAND pcr_late_test)
endif()

# The temporal anti-aliasing's sub-pixel offsets (port/gs/pc/pcr_jitter.h)
add_executable(pcr_jitter_test tests/pcr_jitter_test.cpp port/gs/pc/pcr_jitter.cpp)
target_include_directories(pcr_jitter_test PRIVATE port/gs/pc)
set_target_properties(pcr_jitter_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_jitter_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_jitter COMMAND pcr_jitter_test)
endif()

# The player menu's moving about (port/ui/menu_model.hpp): focus, lists, tabs and the pointer, on stand-in rows
add_executable(ui_menu_test tests/ui_menu_test.cpp port/ui/menu_model.cpp)
target_include_directories(ui_menu_test PRIVATE port/ui)
set_target_properties(ui_menu_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(ui_menu_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME ui_menu COMMAND ui_menu_test)
endif()

# The first-start setup (port/ui/setup_model.hpp, setup_pages.hpp): its pages, cards, presets, Cancel and Skip, on
# stand-in rows named as the tables name the menu's
add_executable(ui_setup_test tests/ui_setup_test.cpp port/ui/setup_model.cpp port/ui/setup_pages.cpp
               port/ui/setting_row.cpp port/ui/menu_model.cpp)
target_include_directories(ui_setup_test PRIVATE port/ui port/include)
set_target_properties(ui_setup_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(ui_setup_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME ui_setup COMMAND ui_setup_test)
endif()

# The game's menu art for the player menu (port/ui/game_menu_art.hpp): menu.tmx's textures decoded, on a made-up file
add_executable(ui_game_art_test tests/ui_game_art_test.cpp port/ui/game_menu_art.cpp)
target_include_directories(ui_game_art_test PRIVATE port/ui port/include)
set_target_properties(ui_game_art_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(ui_game_art_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME ui_game_art COMMAND ui_game_art_test)
endif()

# A controller's buttons as the player menu's pictures (port/ui/pad_glyphs.hpp): each family's buttons against SDL's
# own labels, bindings to buttons, the RML written
if(TARGET SDL3::SDL3)
    add_executable(ui_pad_glyphs_test tests/ui_pad_glyphs_test.cpp port/ui/pad_glyphs.cpp)
    target_include_directories(ui_pad_glyphs_test PRIVATE port/ui port/include)
    target_link_libraries(ui_pad_glyphs_test PRIVATE SDL3::SDL3)
    set_target_properties(ui_pad_glyphs_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    if(NOT MSVC)
        target_compile_options(ui_pad_glyphs_test PRIVATE -Wall -Wextra -Wshadow)
    endif()
    if(KF4_TESTS)
        add_test(NAME ui_pad_glyphs COMMAND ui_pad_glyphs_test)
    endif()
endif()

# The PC renderer's spare surface images (port/gs/pc/pcr_spares.h): which image a copy is made into, which are let go
add_executable(pcr_spares_test tests/pcr_spares_test.cpp)
target_include_directories(pcr_spares_test PRIVATE port/gs/pc)
set_target_properties(pcr_spares_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_spares_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_spares COMMAND pcr_spares_test)
endif()

# The PC renderer's surface texture copies (port/gs/pc/pcr_copy_pool.h): which image a copy is made into, when cleared
add_executable(pcr_copy_pool_test tests/pcr_copy_pool_test.cpp)
target_include_directories(pcr_copy_pool_test PRIVATE port/gs/pc)
set_target_properties(pcr_copy_pool_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_copy_pool_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_copy_pool COMMAND pcr_copy_pool_test)
endif()

# The flash recorder's ring size (port/platform/flash_ring.h): held to its most bytes at any frame rate
add_executable(flash_ring_test tests/flash_ring_test.cpp)
target_include_directories(flash_ring_test PRIVATE port/platform)
set_target_properties(flash_ring_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(flash_ring_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME flash_ring COMMAND flash_ring_test)
endif()

# The GIF front end (port/gs/shared/pcr_gif.cpp; the PC and scene renderers'): GIF packets into events, no VRAM, no drawing
add_library(kf4_pc_gif STATIC port/gs/shared/pcr_gif.cpp)
target_include_directories(kf4_pc_gif PUBLIC port/gs/pc port/gs/shared PRIVATE port/include)
set_target_properties(kf4_pc_gif PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_executable(pcr_gif_test tests/pcr_gif_test.cpp)
target_link_libraries(pcr_gif_test PRIVATE kf4_pc_gif)
set_target_properties(pcr_gif_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(kf4_pc_gif PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)
    target_compile_options(pcr_gif_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_gif COMMAND pcr_gif_test)
endif()

# The PC renderer's frame walk rules (port/gs/pc/pcr_frame_walk.h, header only): the registers of a frame's own walk
# as its pictures between's walk, with pcr_gif.h's include path from kf4_pc_gif
add_executable(pcr_frame_walk_test tests/pcr_frame_walk_test.cpp)
target_link_libraries(pcr_frame_walk_test PRIVATE kf4_pc_gif)
set_target_properties(pcr_frame_walk_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_frame_walk_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_frame_walk COMMAND pcr_frame_walk_test)
endif()

# Dead writes in a walk drawn again (port/gs/pc/pcr_dead_writes.h, header only)
add_executable(pcr_dead_writes_test tests/pcr_dead_writes_test.cpp)
target_include_directories(pcr_dead_writes_test PRIVATE port/gs/pc)
set_target_properties(pcr_dead_writes_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_dead_writes_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_dead_writes COMMAND pcr_dead_writes_test)
endif()

# The GS's blend as Vulkan's (port/gs/shared/pcr_blend.h, header only)
add_executable(pcr_blend_test tests/pcr_blend_test.cpp)
target_include_directories(pcr_blend_test PRIVATE port/gs/shared
                           ${CMAKE_CURRENT_SOURCE_DIR}/third_party/parallel-gs/Granite/third_party/khronos/vulkan-headers/include)
set_target_properties(pcr_blend_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(pcr_blend_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_blend COMMAND pcr_blend_test)
endif()

# The watch for a GPU that has stopped answering (port/gs/pc/pcr_hang_notes.h, pcr_hang_watch.h): the notes kept, the
# report, and (pcr_hang_watch, about 4 s) a wait that never ends ending the process with the report written
add_executable(pcr_hang_watch_test tests/pcr_hang_watch_test.cpp port/gs/pc/pcr_hang_watch.cpp)
target_include_directories(pcr_hang_watch_test PRIVATE port/gs/pc)
set_target_properties(pcr_hang_watch_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
target_link_libraries(pcr_hang_watch_test PRIVATE Threads::Threads)
if(NOT MSVC)
    target_compile_options(pcr_hang_watch_test PRIVATE -Wall -Wextra -Wshadow)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_hang_notes COMMAND pcr_hang_watch_test)
    add_test(NAME pcr_hang_watch COMMAND pcr_hang_watch_test hang $<TARGET_FILE:pcr_hang_watch_test>
             ${CMAKE_CURRENT_BINARY_DIR}/pcr_hang_watch_out)
endif()

# The texture store, the GS's textures from their uploads (port/gs/shared/pcr_textures.h; the PC and scene renderers'), with the GS's memory model
# (port/gs/shared/gsn_vram.c) as its format library and nothing else of the old native GS
add_library(kf4_gsn_vram STATIC port/gs/shared/gsn_vram.c)
target_include_directories(kf4_gsn_vram PUBLIC port/gs/shared)
# and the game's fonts made sharper from them (port/gs/shared/font_sheet.h), their smaller levels made on the PC
# (port/gs/shared/mip_chain.h)
add_library(kf4_pc_textures STATIC port/gs/shared/pcr_textures.cpp port/gs/shared/font_sheet.cpp
            port/gs/shared/mip_chain.cpp)
target_include_directories(kf4_pc_textures PUBLIC port/gs/shared)
target_link_libraries(kf4_pc_textures PUBLIC kf4_gsn_vram)
set_target_properties(kf4_pc_textures PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# against the same uploads written into a 4 MB VRAM with gsn_vram.c in order
add_executable(pcr_textures_test tests/pcr_textures_test.cpp)
target_link_libraries(pcr_textures_test PRIVATE kf4_pc_textures)
set_target_properties(pcr_textures_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(kf4_pc_textures PRIVATE -Wall -Wextra)
    target_compile_options(pcr_textures_test PRIVATE -Wall -Wextra)
endif()
add_executable(font_sheet_test tests/font_sheet_test.cpp)
target_link_libraries(font_sheet_test PRIVATE kf4_pc_textures)
set_target_properties(font_sheet_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(font_sheet_test PRIVATE -Wall -Wextra)
endif()
add_executable(mip_chain_test tests/mip_chain_test.cpp)
target_link_libraries(mip_chain_test PRIVATE kf4_pc_textures)
set_target_properties(mip_chain_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(mip_chain_test PRIVATE -Wall -Wextra)
endif()
if(KF4_TESTS)
    add_test(NAME pcr_textures COMMAND pcr_textures_test)
    add_test(NAME font_sheet COMMAND font_sheet_test)
    add_test(NAME mip_chain COMMAND mip_chain_test)
endif()

# INT_MIN / -1 and a division by zero, as the EE does them (port/platform/int_divide.c)
add_executable(int_divide_test tests/int_divide_test.c port/platform/int_divide.c)
target_include_directories(int_divide_test PRIVATE port/include)
target_compile_options(int_divide_test PRIVATE ${KF4_GAME_FLAGS})
# the operand forms the game's code is compiled to, which is never LTO'd: with LTO the whole program
# shows g_divisor is never written, folds it to -1 and INT_MIN / -1 to undefined
set_target_properties(int_divide_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# The PS2's math library against the retail program's results (tools/ee_libm_ref.py)
add_executable(libm_test tests/libm_test.c port/sdk/libm_ps2.c $<TARGET_OBJECTS:ps2_libm>)
target_include_directories(libm_test PRIVATE port/include)
target_compile_options(libm_test PRIVATE ${KF4_GAME_FLAGS} ${KF4_EE_FLOAT_FLAGS})

# The VU1 interpreter's self-test: hand-written microcode run on port/platform/vu1.c alone
add_executable(vu1_test tests/vu1_test.c port/platform/vu1.c port/platform/vu1_hle.c port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c port/math/libvu0.c)
target_include_directories(vu1_test PRIVATE port/include)
target_link_libraries(vu1_test PRIVATE kf4_profile) # (vu1.c times its programs as the frame profiler's zones)
target_compile_options(vu1_test PRIVATE -frounding-math -ffp-contract=off)
if(NOT WIN32)
    target_link_libraries(vu1_test PRIVATE m)
endif()

# A native draw's record for program 0x000 (port/render/vu1_record.cpp) against VU1's own set-up (the HLE's)
add_executable(vu1_record_test tests/vu1_record_test.cpp port/render/vu1_record.cpp port/platform/vu1.c
               port/platform/vu1_hle.c port/platform/vu1_hle_280.c port/platform/vu1_hle_5a0.c port/platform/vu1_hle_700.c
               port/math/libvu0.c)
target_include_directories(vu1_record_test PRIVATE port/include)
target_link_libraries(vu1_record_test PRIVATE kf4_profile)
target_compile_options(vu1_record_test PRIVATE -frounding-math -ffp-contract=off)
set_target_properties(vu1_record_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
    target_link_libraries(vu1_record_test PRIVATE m)
endif()
add_test(NAME vu1_record COMMAND vu1_record_test)

# The reflection's rippled water's record and ADC (port/render/ripple.cpp), and the S and T arithmetic of its shader
# (port/render/shaders/ripple_maths.h), against Wobble_DrawRippledModel's maths as game/native_draw/nd_water.c writes it
add_executable(ripple_test tests/ripple_test.cpp port/render/ripple.cpp)
target_include_directories(ripple_test PRIVATE port/include)
target_compile_options(ripple_test PRIVATE -frounding-math -ffp-contract=off)
set_target_properties(ripple_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
    target_link_libraries(ripple_test PRIVATE m)
endif()
add_test(NAME ripple COMMAND ripple_test)

# Exact Z's plane (port/render/shaders/plane_z.h, the PC renderer's fragment stage's arithmetic, compiled as C++)
# against 128-bit integers
add_executable(plane_z_test tests/plane_z_test.cpp)
set_target_properties(plane_z_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(plane_z_test PRIVATE -Wall -Wextra)
endif()
add_test(NAME plane_z COMMAND plane_z_test)

# The native draws' moves in the pictures between (port/render/part_moves.cpp): the parts' moves against their matrices
# at t 0 and 1, the sections, the pairing by identity, the trail's pairs
add_executable(part_moves_test tests/part_moves_test.cpp)
target_link_libraries(part_moves_test PRIVATE kf4_part_moves)
set_target_properties(part_moves_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
    target_link_libraries(part_moves_test PRIVATE m)
endif()
add_test(NAME part_moves COMMAND part_moves_test)

# A screen pass as GIF packets (port/render/pass_draw.cpp) against the game's own passes' packets (decomp game.c,
# gs_draw.c) through the GIF front end, and the pictures between's moves against gif_moves.h's
add_executable(pass_draw_test tests/pass_draw_test.cpp)
target_link_libraries(pass_draw_test PRIVATE kf4_part_moves kf4_pc_gif)
target_include_directories(pass_draw_test PRIVATE port/gs/pc)
set_target_properties(pass_draw_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
    target_link_libraries(pass_draw_test PRIVATE m)
endif()
add_test(NAME pass_draw COMMAND pass_draw_test)

# Windows: put the DLLs the program needs next to it, so it runs from Explorer or a shortcut (SDL3, and
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
endif()
