# disc/

Place your own dump of **King's Field: The Ancient City (NTSC-U, SLUS-20318)**
here:

```
disc/King's Field The Ancient City.iso
```

Any dump of the same release works; the filename is only an example.

Nothing in this directory is committed (see `.gitignore`) and no game data is
distributed with this project. The recompiler reads the boot ELF from your disc
and the runtime reads the disc again at play time, so a working port still
requires it.

Pass the path to the scripts, for example:

```sh
./scripts/01-extract.sh "disc/King's Field The Ancient City.iso" ~/.local/share/kfiv-pc
```
