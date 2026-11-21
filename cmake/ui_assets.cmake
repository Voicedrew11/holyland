# The player menu's files (port/ui/res/: documents, style sheets, fonts) as C++ arrays, so the game carries them
# inside it: ui_assets and ui_asset_count (port/ui/ui_assets.hpp). Run as a script by the build
# (cmake -DROOT=port/ui/res -DOUT=... -DFILES="a;b;c" -P cmake/ui_assets.cmake), again whenever one changes. Each
# file is named by its path under ROOT, as the documents name it. A second folder's files may join them
# (-DMORE_ROOT=launcher/res -DMORE_FILES="x;y"), named by their paths under it: the launcher's pages beside the menu's.
set(body "")
set(table "")
set(index 0)
set(entries "")
foreach(name ${FILES})
    list(APPEND entries "${ROOT}|${name}")
endforeach()
foreach(name ${MORE_FILES})
    list(APPEND entries "${MORE_ROOT}|${name}")
endforeach()
foreach(entry ${entries})
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} root)
    math(EXPR after "${bar} + 1")
    string(SUBSTRING "${entry}" ${after} -1 name)
    file(READ "${root}/${name}" hex HEX)
    string(LENGTH "${hex}" digits)
    math(EXPR size "${digits} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
    string(APPEND body "const unsigned char asset_${index}[] = {\n${bytes}0};\n")
    string(APPEND table "    {\"${name}\", asset_${index}, ${size}},\n")
    math(EXPR index "${index} + 1")
endforeach()
file(WRITE "${OUT}.new"
    "// Made by cmake/ui_assets.cmake from port/ui/res/: do not edit\n"
    "#include \"ui_assets.hpp\"\n"
    "namespace {\n${body}} // namespace\n"
    "const UiAsset ui_assets[] = {\n${table}};\n"
    "const size_t ui_asset_count = ${index};\n")
file(COPY_FILE "${OUT}.new" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.new")
