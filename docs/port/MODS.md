# Mods

The port reads replacement game files and text from a **mods folder** at start, and the game reads them in place
of the disc's. Nothing on the disc changes, and taking a mod's folder away puts the game back as it was.

## Where

`<data folder>/mods/` (on Windows `%APPDATA%\holyland\mods\`, on Linux `~/.local/share/holyland/mods/`),
or the folder given with `--mods DIR`. `--mods none` turns mods off for a run. A scripted run
(`--headless` or `--input`) has no mods unless `--mods DIR` names a folder, the way it reads no `settings.json`, so
tests and picture comparisons play the disc as it is; `KF4_READ_SETTINGS=1` (or `KF4_OVERLAY_SHOT`) reads the data
folder's mods too.

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

## Finer sign pictures

The signs, gravestones and inscriptions the player examines are pictures, `msg/mNNNN.tm2` (512 x 256, 4 or
8 bits a texel; `ui/splash.c`). A mod replaces one as any member (`files/msg/m0034.tm2`), and can give a finer
one beside it: `signs/m0034.png`, the same picture 2 to 8 times as wide and as tall (4: 2048 x 1024), 8 bits a
channel, not interlaced. The scene renderer draws the finer one in its place; the GS and the PC renderer draw the
game's, so a mod with a finer picture should give the 512 x 256 one too (made from it).

A finer picture goes with the picture it was made from: a mod's `signs/m0034.png` is used with that mod's own
`files/msg/m0034.tm2`, or with the disc's when the mod has none (`port_mods_find_with`). So a mod of finer pictures
for the disc's signs (kf4-signs) and a translation with its own signs and finer ones (kf4-retranslation) can both be
on: each sign shows the translation's when the translation replaces it, and the disc's sharpened otherwise, whatever
the mods' order.

How (`port/include/port/signs.h`, `port/platform/signs.c`): when the game reads a picture (`Splash_StateLoadImage`,
patched) the port keeps its number and a fingerprint of its texels; the scene renderer, making a 512 x 256 palette
texture for the GPU (`sampled_texture`), looks the fingerprint up and, when the mod whose picture is in use has a
`signs/mNNNN.png`, uses it, `finer` times as fine, as it does the sharper font's sheets. The PNG is read once (inflated
by the miniz libchdr brings). The log says so: `[signs] .../signs/m0034.png: 2048x1024, 4 times as fine`.
