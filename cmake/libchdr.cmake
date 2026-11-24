# libchdr (third_party/libchdr; where it came from: its UPSTREAM.md): reads CHD disc images for
# port/platform/disc_image.c. Built from its unity.c the same way for the game and the launcher.
function(kf4_add_libchdr root)
    if(TARGET kf4_chdr)
        return()
    endif()
    # (unity.c leaves out the A/V codec, which chd.c's codec table still names)
    add_library(kf4_chdr STATIC ${root}/third_party/libchdr/unity.c
        ${root}/third_party/libchdr/src/libchdr_codec_avhuff.c)
    target_include_directories(kf4_chdr PUBLIC ${root}/third_party/libchdr/include)
    # its CMakeLists.txt's defaults: whole sectors and subcode in a CD's hunks (disc_image.c picks the 2048
    # bytes out), and each hunk's checksum checked
    target_compile_definitions(kf4_chdr PRIVATE WANT_RAW_DATA_SECTOR=1 WANT_SUBCODE=1 VERIFY_BLOCK_CRC=1
        LOWRAM_TARGET=0)
    # miniz's deflate too (unity.c leaves it out unless asked): the problem report's zip (port/report/zip_archive.cpp)
    target_compile_definitions(kf4_chdr PRIVATE MINIZ_DEFLATE_APIS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(kf4_chdr PRIVATE -w) # third-party code: its warnings are not ours to fix
    endif()
endfunction()
