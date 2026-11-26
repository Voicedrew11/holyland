# Mods' side of the game's code (docs/port/MODDING.md, "Hooks" and "Stability"), run at configure on the patched
# copy of the decomp before it is compiled.
#
# kf4_mods_mark_hookable: each function in modding/hookable.txt is made hookable everywhere it is called. A hook
# writes over the function's start (port/platform/mod_hooks.c), so a call the compiler inlined, or sent to a copy it
# specialised, never reaches it: the function is marked noinline (and noipa on gcc, which also keeps gcc from making
# such copies). Its signature is checked against the list's at compile time, so a patch that changes it fails the
# build. Holyland's hook refuses any function not in the list (port_mod_hookable_names, written to OUT_C).
#
# kf4_mods_game_interface: the game's interface for mods, as one ID: a hash of the game's headers (patched), the
# hookable list and the RmlUi version a plugin's C++ is built against. A plugin carries the ID it was built with
# (modding/holyland_mod.cmake); Holyland warns when it differs. modding/game_interface.txt holds the ID as last
# accepted: the mods_game_interface test fails when the build's differs, so a change that can break mods is seen.

# Reads modding/hookable.txt: one C prototype per line ("void Input_Update(void)"), # comments
function(kf4_mods_read_hookable list_file out_names out_protos)
    file(STRINGS ${list_file} lines)
    set(names)
    set(protos)
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(NOT line MATCHES "([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\(")
            message(FATAL_ERROR "${list_file}: not a prototype: ${line}")
        endif()
        string(REGEX MATCH "[ *]([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\(" m "${line}")
        if(NOT m)
            message(FATAL_ERROR "${list_file}: no function name in: ${line}")
        endif()
        list(APPEND names ${CMAKE_MATCH_1})
        # (a list item cannot hold a ';': none in a prototype)
        list(APPEND protos "${line}")
    endforeach()
    set(${out_names} ${names} PARENT_SCOPE)
    set(${out_protos} ${protos} PARENT_SCOPE)
endfunction()

function(kf4_mods_mark_hookable game_dir list_file out_c)
    kf4_mods_read_hookable(${list_file} names protos)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set(attr "__attribute__((noinline, noipa))")
    else()
        set(attr "__attribute__((noinline))")
    endif()
    file(GLOB_RECURSE sources ${game_dir}/*.c)
    list(LENGTH names count)
    set(table "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(i RANGE ${last})
            list(GET names ${i} name)
            list(GET protos ${i} proto)
            set(found "")
            foreach(source IN LISTS sources)
                file(READ ${source} text)
                string(FIND "${text}" "${name}(" at)
                if(at EQUAL -1)
                    continue()
                endif()
                # a definition: at a line's start, the name, its parameters, then its body
                string(REGEX MATCH "\n([A-Za-z_][^\n;{}#/(=]*[ *])${name}\\([^;{]*\\)[ \t\n]*{" def "${text}")
                if(NOT def)
                    continue()
                endif()
                set(before "${CMAKE_MATCH_1}")
                if(before MATCHES "(^|[ \t])static[ \t]")
                    message(FATAL_ERROR "${list_file}: ${name} is static in ${source}: only the game's exported "
                        "functions can be hooked")
                endif()
                if(found)
                    message(FATAL_ERROR "${list_file}: ${name} is defined in both ${found} and ${source}")
                endif()
                set(found ${source})
                string(SUBSTRING "${def}" 1 -1 rest)
                string(REPLACE "${def}" "\n${attr} ${rest}" text "${text}")
                string(REGEX REPLACE "([ *])${name}[ \t]*\\(" "\\1hl_hookable_${name}_t(" type "${proto}")
                string(APPEND text "\n/* modding/hookable.txt (cmake/mod_hookable.cmake) */\n"
                    "typedef ${type};\n"
                    "_Static_assert(__builtin_types_compatible_p(__typeof__(${name}), hl_hookable_${name}_t),\n"
                    "               \"modding/hookable.txt gives ${name} another signature than the game's\");\n")
                file(WRITE ${source} "${text}")
            endforeach()
            if(NOT found)
                message(FATAL_ERROR "${list_file}: ${name} is not defined in the game's code")
            endif()
            string(APPEND table "    \"${name}\",\n")
        endforeach()
    endif()
    file(CONFIGURE OUTPUT ${out_c} CONTENT
"/* The game's functions mods may hook (modding/hookable.txt; written by cmake/mod_hookable.cmake) */
const char *const port_mod_hookable_names[] = {
@table@    0};
" @ONLY)
endfunction()

function(kf4_mods_game_interface game_dir list_file rmlui_file out_var)
    file(GLOB_RECURSE headers RELATIVE ${game_dir} ${game_dir}/*.h)
    list(SORT headers)
    set(all "")
    foreach(header IN LISTS headers)
        file(READ ${game_dir}/${header} text)
        string(APPEND all "== ${header}\n${text}")
    endforeach()
    file(READ ${list_file} text)
    string(APPEND all "== hookable\n${text}")
    file(READ ${rmlui_file} text)
    string(REGEX MATCH "RMLUI_VERSION=\\\\\"([^\\\\]*)\\\\\"" m "${text}")
    string(APPEND all "== rmlui ${CMAKE_MATCH_1}\n")
    string(REPLACE "\r" "" all "${all}") # (the same on a Windows checkout)
    string(SHA256 hash "${all}")
    string(SUBSTRING ${hash} 0 12 hash)
    set(${out_var} ${hash} PARENT_SCOPE)
endfunction()
