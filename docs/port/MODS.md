# Mods

The port reads replacement game files and text from a **mods folder** at start, and the game reads them in place
of the disc's. Nothing on the disc changes, and taking a mod's folder away puts the game back as it was.

## Where

`<data folder>/mods/` (on Windows `%APPDATA%\holyland\mods\`, on Linux `~/.local/share/holyland/mods/`),
or the folder given with `--mods DIR`. `--mods none` turns mods off for a run.

Each folder inside is one mod. They are applied in name order, so where two mods change the same thing the later
one wins. A folder whose name starts with `.` or `_` is skipped: rename `kf4-retranslation` to
`_kf4-retranslation` to turn it off.

## What a mod can hold

```
mods/
  my-mod/
    files/              whole files of DATA\KF4.DAT, by their names in the archive
      font.txc
      param/item_nam.prm
      msg/m0010.tm2
    text.json           text in place of the game's own
```

**`files/`**: any member of `KF4.DAT`, at its path in the archive (the names are listed in the log, or by the
decomp's `tools/extract_gamedata.py` as `files.txt`). It replaces the member whole, at any size. A file that
is not in the archive is reported and left out.

**`text.json`**:

```json
{
  "messages": { "10": "Nothing happens", "3103": "Well now, someone made it all the way down here.\nThis is..." },
  "items":    { "7": "Healing Medicine" },
  "spells":   { "1": "Fireball" }
}
```

- `messages`: `font.txc`, by message id (the id the game passes to `Message_OpenBox` and `Message_ShowNotice`;
  docs/GAME_DATA.md in the decomp says which is which). Ids left out keep the game's own text.
- `items`: the names in `param/item_nam.prm`, by item number (at most 22 characters).
- `spells`: the names in `param/magic.prm`, by record number (at most 23 characters).

Text is UTF-8 and drawn with the US font: printable ASCII. `\n` starts a new line; the game breaks lines only
there (the menus word-wrap item and spell descriptions themselves), so a mod wraps its own text. `{num3}` is the
game's number argument (a 3-digit amount, as in "{num3} gold obtained"); `{numN}`, `{nN}` and `{cN}` are the
other codes of `Message_Format`. Typographic quotes, dashes and the ellipsis become their ASCII look-alikes;
anything else the font lacks is drawn as `?` (the log counts them).

The rebuilt `font.txc` must fit the game's room for it, 0x5FC00 bytes (from 0x1A0400 to the end of the 2 MB menu
heap); the US file takes 0x29994. The log says when it does not, and the messages are then left out.

## How it works

`port/platform/mods.c` reads the mods after `port_disc_open`. `files/` entries go straight to
`port_disc_replace_member`; text is applied to the member as it then stands (so a `files/font.txc` and a
`text.json` combine), rebuilt in memory and replaced the same way.

`port_disc_replace_member` (`port/platform/disc.c`) keeps the new contents in memory at sectors past the
disc's end. When anything reads KF4.DAT's table (the game's `File_LoadArchiveToc`, or the port's own readers),
the replaced rows' size and offset are changed in the copy read, so the game's `KF_File_Open` finds the new
contents there and `File_Read` reads them through `sceCdRead` like any other file. The game code is unchanged.

The log shows what was applied:

```
[mods] kf4-retranslation
[mods]   text.json: 1150 messages, 291 item names, 110 spell names
[mods] font.txc rebuilt: 167080 bytes (was 170388)
```

The game reads `font.txc` and the name tables into memory, so a save state keeps the text it was made with;
quick saves and the game's saves hold no text and don't depend on mods.

Tests: `mods_test` (tests/mods_test.c).
