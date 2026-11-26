# The mods_game_interface test (cmake/mod_hookable.cmake): the build's game interface ID against the one last
# accepted in modding/game_interface.txt. -DEXPECTED_FILE=<that file> -DACTUAL=<the build's ID>
file(STRINGS ${EXPECTED_FILE} expected REGEX "^[0-9a-f]+$")
if(NOT expected STREQUAL ACTUAL)
    message(FATAL_ERROR "The game's interface for mods changed: ${expected} -> ${ACTUAL}.\n"
        "A game header (a patch), modding/hookable.txt or RmlUi's version is not what it was, so mods built for "
        "${expected} may no longer work and players will be warned about them. If that is meant, write ${ACTUAL} "
        "in modding/game_interface.txt (and say so in the release notes for mod makers).")
endif()
