# FreeType

[FreeType](https://freetype.org) 2.14.3 (tag `VER-2-14-3`), copied as it is. FreeType Licence (BSD-style, with
credit): see `LICENSE.TXT` and `FTL.TXT` (used under the FTL, not the GPL).

It draws the player menu's fonts for RmlUi (`third_party/rmlui`, `port/ui/`). Built by `cmake/rmlui.cmake` from a
handful of its modules: TrueType and OpenType (CFF) fonts, the smooth (anti-aliased) renderer and the auto-hinter.
`kf4/ftmodule.h` lists those modules and `kf4/ftoption.h` turns off the compressed font formats (no zlib).

Kept: `include/` and `src/{base,truetype,sfnt,smooth,autofit,psnames,cff,psaux,pshinter}`. Left out: the other
modules, its build systems, docs, tests and tools. To update: copy the same folders from a newer release, change the
version above, and run the game with the player menu open (F1).
