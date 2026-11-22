# One file as a C array: const unsigned char NAME[] and const size_t NAME_size, in OUT. Run as a script by the
# build (cmake -DIN=file -DOUT=file.c -DNAME=symbol -P cmake/embed_file.cmake), again whenever IN changes.
# Used for the window's icon (packaging/shared/holyland.bmp, port/platform/window_icon.c).
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUT}.new"
    "/* Made by cmake/embed_file.cmake from ${IN}: do not edit */\n"
    "#include <stddef.h>\n"
    "extern const unsigned char ${NAME}[];\n"
    "extern const size_t ${NAME}_size;\n"
    "const unsigned char ${NAME}[] = {\n${bytes}0};\n"
    "const size_t ${NAME}_size = ${size};\n")
file(COPY_FILE "${OUT}.new" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.new")
