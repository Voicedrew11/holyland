# RmlUi (third_party/rmlui, MIT) with its FreeType font engine (third_party/freetype, FTL): the player menu
# (port/ui/). Both copied into the repository, as libchdr is, so a release builds without the network. Third-party
# code: no warnings, optimised in every build type.
set(KF4_FREETYPE ${CMAKE_CURRENT_LIST_DIR}/../third_party/freetype)
add_library(kf4_freetype STATIC
    ${KF4_FREETYPE}/src/base/ftbase.c
    ${KF4_FREETYPE}/src/base/ftinit.c
    ${KF4_FREETYPE}/src/base/ftsystem.c
    ${KF4_FREETYPE}/src/base/ftdebug.c
    ${KF4_FREETYPE}/src/base/ftbitmap.c
    ${KF4_FREETYPE}/src/base/ftglyph.c
    ${KF4_FREETYPE}/src/base/ftmm.c
    ${KF4_FREETYPE}/src/base/ftstroke.c
    ${KF4_FREETYPE}/src/base/ftsynth.c
    ${KF4_FREETYPE}/src/truetype/truetype.c
    ${KF4_FREETYPE}/src/sfnt/sfnt.c
    ${KF4_FREETYPE}/src/smooth/smooth.c
    ${KF4_FREETYPE}/src/autofit/autofit.c
    ${KF4_FREETYPE}/src/psnames/psnames.c
    ${KF4_FREETYPE}/src/cff/cff.c
    ${KF4_FREETYPE}/src/psaux/psaux.c
    ${KF4_FREETYPE}/src/pshinter/pshinter.c)
target_include_directories(kf4_freetype SYSTEM PUBLIC ${KF4_FREETYPE}/include PRIVATE ${KF4_FREETYPE})
target_compile_definitions(kf4_freetype PRIVATE FT2_BUILD_LIBRARY
    "FT_CONFIG_MODULES_H=<kf4/ftmodule.h>" "FT_CONFIG_OPTIONS_H=<kf4/ftoption.h>")
target_compile_options(kf4_freetype PRIVATE -w -O2)
set_target_properties(kf4_freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(KF4_RMLUI ${CMAKE_CURRENT_LIST_DIR}/../third_party/rmlui)
file(GLOB_RECURSE KF4_RMLUI_SOURCES CONFIGURE_DEPENDS ${KF4_RMLUI}/Source/Core/*.cpp)
add_library(kf4_rmlui STATIC ${KF4_RMLUI_SOURCES})
target_include_directories(kf4_rmlui SYSTEM PUBLIC ${KF4_RMLUI}/Include)
target_compile_definitions(kf4_rmlui PUBLIC RMLUI_STATIC_LIB PRIVATE RMLUI_FONT_ENGINE_FREETYPE "RMLUI_VERSION=\"6.3\"")
target_link_libraries(kf4_rmlui PRIVATE kf4_freetype)
target_compile_options(kf4_rmlui PRIVATE -w -O2)
set_target_properties(kf4_rmlui PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
# RmlUi's headers alone, for the mods' plugins (modding/holyland_mod.cmake): they use the program's RmlUi
add_library(kf4_rmlui_headers INTERFACE)
target_include_directories(kf4_rmlui_headers SYSTEM INTERFACE ${KF4_RMLUI}/Include)
target_compile_definitions(kf4_rmlui_headers INTERFACE RMLUI_STATIC_LIB)
