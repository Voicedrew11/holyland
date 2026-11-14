# The PC renderer's pipeline seed (port/gs/pc/pipeline_seed.bin, made by tools/pipeline_seed.py) as a C++ array:
# pcr_pipeline_seed and pcr_pipeline_seed_size (port/gs/pc/pcr_pipelines.cpp, "Made before play"). Run as a script
# (cmake -DSEED=... -DOUT=... -P cmake/pipeline_seed.cmake) by the build, again whenever the seed changes. No seed:
# an empty one (nothing made before play but the data folder's list).
if(EXISTS "${SEED}")
    file(READ "${SEED}" hex HEX)
else()
    set(hex "")
endif()
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
# (lines of 32 bytes)
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}.new"
    "// Made by cmake/pipeline_seed.cmake from port/gs/pc/pipeline_seed.bin: do not edit\n"
    "#include <cstddef>\n"
    "extern const unsigned char pcr_pipeline_seed[] = {\n${bytes}0};\n"
    "extern const size_t pcr_pipeline_seed_size = ${size};\n")
file(COPY_FILE "${OUT}.new" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.new")
