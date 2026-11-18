# libchdr

[libchdr](https://github.com/rtissera/libchdr) by Romain Tisserand, commit
`47d794151b2b64f6aba60304e6480f4f5a2bc24b` (2026-10-11), copied as it is. BSD 3-clause licence: see
`LICENSE.txt`.

It reads CHD disc images (MAME's compressed hunks, made with `chdman createdvd` or `createcd`) for
`port/platform/disc_image.c`, in the game and in the launcher. Built by `cmake/libchdr.cmake` from `unity.c`
plus `src/libchdr_codec_avhuff.c`, which `unity.c` leaves out though `libchdr_chd.c`'s codec table names it.

| file | what it is | licence |
| --- | --- | --- |
| `src/`, `include/libchdr/`, `unity.c` | the library | BSD 3-clause (`LICENSE.txt`) |
| `include/dr_libs/dr_flac.h` | FLAC decoding (CD audio in a CHD) | public domain or MIT-0 (in the file) |
| `deps/lzma-26.02/` | LZMA decoding | public domain (`LICENSE`) |
| `deps/miniz-3.1.2/` | zlib decoding | MIT (`LICENSE`, from miniz 3.1.2) |
| `deps/zstd-1.5.7/` | Zstandard decoding (one-file `zstddeclib.c`) | BSD (`LICENSE`, from zstd v1.5.7) or GPLv2 |

Left out: its CMake files, tests, docs, `contrib/` and the micro-flac backend. To update: copy the same files
from a newer commit, change the commit above, and run `iso9660_test` and `launcher_test` (they read
`tests/data/*.chd`).
