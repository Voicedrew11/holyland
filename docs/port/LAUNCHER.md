# The launcher: Holyland

Players get **Holyland**, a launcher that builds the game on their own computer and then plays it. A
release never contains a built `kf4`: the game's code comes from the decomp, so, like the Verdite ports
(`verdite-core/launcher`), a release ships the **source and the tools** and the build happens on the player's
machine. The design follows Verdite's launcher; Holyland is C/C++, so the code is new (`launcher/`).

"Holyland" is the project's name (it was Darker Reality from 2026-11-18 to 2026-11-20): `launcher/product.h`,
`packaging/package.env`, and the folders in `port/platform/data_dir.c`. The game's program is still `kf4`. The
first start after the rename moves the `darker-reality` data and local folders back to `holyland`
(`data_dir_move_old_folders`), so players keep their settings and saves.

## What it does

1. Windows: prints into the console it was started from, if any.
2. Starts the update check on a thread of its own (below).
3. The first start's setup (`launcher/setup_flow.h`, `setup_pages.h`): the Holyland logo over one page at a time.
   The disc's page comes first, when there is no disc to use: the disc given (`--disc PATH` or a path alone), else
   the one chosen last time (`launcher.json`), else it asks. The player drops the disc on the window, or clicks the
   drop area for the system's own file chooser (SDL's: the desktop's portal on Linux, IFileOpenDialog on Windows;
   where SDL has none, kdialog or zenity as before), or types a path. The disc is checked as the game checks it
   (`SYSTEM.CNF`, `DATA\KF4.DAT`, read with the game's own readers: an `.iso` or `.chd` image through
   `port/platform/disc_image.c`, its files through `iso9660.c`) and must boot `SLUS_203.18`, the USA release the
   decomp is of; a wrong disc says why under the drop area.
   Options to pick before the first game go after the disc, a page each (`make_..._page` in `setup_pages.h` and a
   line in `Session::setup_pages`); none exist yet. They show until the setup is gone through once
   (`setup_done` in `launcher.json`).
4. The build key (`launcher/build_key.h`): SHA-256 of the version, `content/manifest.txt` and the tools' `ID`.
   No `builds/<key>/kf4` yet: the build starts as the setup opens (it needs no disc), so it runs while the player
   is on the setup's pages; then a progress window if it is still going. A failure keeps the window up with the
   end of `build.log`, Try again, Copy the log.
5. Play: the window closes, `kf4 --disc <disc> [what came after --]` runs in the player's working folder, its
   output goes to the console and to `game.log`. A crash shows "The game stopped" with the log's end.

Later starts go straight into the game. `holyland --launcher` (Start menu: "Holyland (disc, rebuild,
updates)"; the desktop entry's action) shows the launcher instead: Play, Change disc (the setup's disc page),
Rebuild, Open data folder, "Check for updates at launch".

The window (`launcher/window.h`) is Dear ImGui on SDL's renderer, in the player menu's colours, with its logo and
fonts (`port/ui/res/`, carried in the launcher by `cmake/ui_assets.cmake`). On a screen the desktop scales (115%)
ImGui's drawing is scaled to the window's pixels. `--build-only` builds with no window and no disc (CI, tests).

## The files

| Where | What | Written by |
|---|---|---|
| data folder (`port/platform/data_dir.c`: `%APPDATA%\holyland`, `~/.local/share/holyland`, `KF4_DATA_DIR`) | `settings.json`, saves | the game |
| | `launcher.json`: disc, `update_check`, `skipped`, `setup_done` | the launcher's window; the overlay sets `skipped` only, while the launcher's window is closed |
| | `update.json`: the cached answer (`checked`, `tag`, `url`) | the launcher's update thread only |
| | `build.log`, `game.log` | the launcher |
| builds folder (`%LOCALAPPDATA%\holyland\builds`, `$XDG_DATA_HOME/holyland/builds`, `KF4_BUILDS_DIR`; under `KF4_DATA_DIR` when that is set) | `<key>/kf4` and its libraries; older keys are removed after a build | the launcher |
| install folder (read-only) | the launcher, `content/` | the installer |

## Platforms

`launcher/platform.h` is everything the launcher needs from a system: where `content/` is, the shipped tools
(`Toolchain`), the game's file names, starting and stopping processes, the disc chooser, HTTPS. One file per
system in `launcher/platform/`; `launcher/CMakeLists.txt` compiles exactly one.

- **A new desktop system (macOS)**: `platform/macos.cpp` and a line in `launcher/CMakeLists.txt`; its tools in
  `packaging/macos/`. Nothing else changes.
- **iOS, Android**: these cannot run a compiler and CMake as an app. `BuildStrategy` (`launcher/build_strategy.h`)
  is how a playable game is made on a system; the desktop ones use `LocalCMakeBuild` (`build_local.cpp`). A mobile
  system gets a strategy of its own; the rest of the launcher is unchanged. None exists yet.

## The tools

Pinned by SHA-256 in `packaging/toolchain.lock`, downloaded once into `out/packaging-cache/` by
`packaging/fetch.sh`. Nothing is downloaded on the player's machine.

- **Linux** (`packaging/linux/toolchain.sh`): clang, lld and llvm-ar from LLVM 22.1.8 (stripped; the developers use
  clang 22); CMake 3.31.6; Ninja; a sysroot from Ubuntu 22.04's release pocket (glibc 2.35, libstdc++ 12 linked
  in statically, libdrm); SDL3 3.4.20 built from source on Ubuntu 22.04 (`packaging/linux/build-sdl3.sh`), every
  system library it uses loaded at run time, so it needs only the C library. It goes beside the game (run path
  `$ORIGIN`). The game runs on glibc 2.35 and newer: Ubuntu 22.04, Debian 12, SteamOS 3, current Fedora/Arch.
  The release workflow checks that.
  - Ubuntu 22.04 has no SDL3 package at all. SDL3's headers need none of X11's or Wayland's (SDL2's `SDL_syswm.h`
    did), so the sysroot holds SDL3's headers alone.
  - SDL3 opens the window through Wayland where there is one (SDL2 2.30 in the AppImage went through XWayland).
- **Windows** (`packaging/windows/toolchain.sh`): llvm-mingw 20260616 (clang 22, x86_64 only, no debugger or
  sanitizer runtimes), CMake, Ninja, SDL3's MinGW build. The same as `ci.yml`.

Change a tool: its line in `toolchain.lock` (`packaging/fetch.sh --print NAME` gives the new SHA-256), then the
`ID` in that platform's `toolchain.sh`, which makes every player's next start build again.

The release workflow keeps the finished Linux tools (`HOLYLAND_TOOLCHAIN_CACHE`, read by
`build-appimage.sh`) and the Windows downloads in GitHub caches saved only on `main` (a tag's own cache is seen by
that tag alone). Pushing a change to the tools' files to `main` runs the package jobs to refill them.

### Sizes (2026-11-18)

| | Download | Installed |
|---|---|---|
| Linux AppImage | 163 MB | 600 MB (content: 62 MB source, 538 MB tools, of which LLVM is 408 MB) |
| Windows zip | 126 MB | 520 MB (llvm-mingw 397 MB) |

Verdite's are ~40 MB because its compiler (Roslyn) is a library inside .NET. Most of this is the C/C++ compiler.
The way to shrink it later: one multi-call LLVM build (`LLVM_TOOL_LLVM_DRIVER_BUILD`, clang + lld + llvm-ar in one
binary), built once in CI and pinned like the rest.

## What a release holds

`packaging/payload.sh` writes `content/src`:
- this checkout's tracked files the build reads (not `docs/`, `patches/`, `launcher/`, `packaging/`, `.github/`);
- `decomp/src/game` and `src/nonmatching` with `patches/` applied, and the decomp's libm and libc. The build takes
  them as they are with `-DKF4_DECOMP_PREPATCHED=ON`: no git on the player's machine;
- paraLLEl-GS with Granite's own files and only volk and the Vulkan headers of its submodules (not the 19 MB
  registry), with Granite's edits made (`cmake/apply_granite_edits.cmake`).

The player's source is read-only (an AppImage is a read-only mount): configure must write nothing into it.
Granite's edits (`cmake/granite_edits.cmake`) write a file only when its text changes; `ci.yml`'s launcher job
checks that on a read-only copy.

`packaging/manifest.sh` writes `content/manifest.txt` (every file's SHA-256), which the build key hashes.

## Builds

`LocalCMakeBuild` runs the developers' release preset (`RelWithDebInfo`, `KF4_LTO=ON`, `KF4_FRAME_POINTERS=OFF`)
with `KF4_TESTS=OFF`, `KF4_DECOMP_PREPATCHED=ON` and `KF4_VERSION=<release>`, target `kf4` only, then keeps the game
and its libraries (`Platform::game_files`, `Toolchain::runtime_files`) and removes the objects. On a 16-core
machine it takes about 16 seconds.

The game is the same game: on 2026-11-18, `tools/build_compare.py` gave SAME at spots 0, 9, 9:2 and 63 between
a developer's `out/linux-clang/kf4` and one built with the shipped Linux tools (pictures byte for byte, state lines,
state dumps). Check again whenever a tool moves.

PGO with the player's disc (`tools/pgo_train.py`) is a possible later option; it is not done.

## Updates

The same as Verdite's launcher: the player is told; nothing installs itself.

- `launcher/update_check.cpp` asks `GET https://api.github.com/repos/Voicedrew11/darker-reality/releases/latest` (drafts
  and pre-releases are never "latest") on its own thread, at most once an hour, 10 s at most; the answer goes to
  `update.json`. No network: the cached answer stands. A URL that is not `https://github.com/` is refused.
- The rules are shared C (`port/platform/update_notice.c`, `tests/update_notice_test.c`): `v` dropped, dotted
  numbers compared, suffixes ignored (Verdite's `UpdateCheck.IsNewer`); a developer's build (no version) is never
  told.
- The launcher's window: a gold "Update available!" menu and, once a start, a window: Download page, Skip this
  version, Later.
- The game (`port/overlay/update_badge.cpp`): reads `update.json` for the first minute (the check may finish after
  the game starts); a note when the game starts ("Update available: v0.2.0 (F1 for details)"), the gold menu in
  the F1 bar, the same window. It logs `[update] ...` to `game.log`.
- `KF4_UPDATE_CHECK=0` turns it off, `=force` ignores the hour; `KF4_UPDATE_URL` (a test only; `file://` works on
  Linux) answers instead of GitHub.

## Versions

The tag is the version (`tools/version.sh`: `$HOLYLAND_VERSION`, else the newest `vX.Y.Z` tag, else 0.0.0). The
launcher is built with `-DHOLYLAND_VERSION`, and passes it to the game's build as `KF4_VERSION`
(`port/include/port/version.h`, shown in About and the bug report). A developer's build has none.

## Making a release

1. `tools/release.sh X.Y.Z` tags HEAD (it never pushes).
2. Check a launcher-built `kf4` against a developer's build: `tools/build_compare.py --exe out/linux-clang/kf4
   --exe <builds>/<key>/kf4 --keep out/compare`.
3. `git push origin vX.Y.Z`: `.github/workflows/release.yml` builds the AppImage (on Ubuntu 22.04), the Windows zip
   and installer, builds the game from each package with `--build-only` as a player would, and opens a **draft**
   release. Publishing the draft is what players' launchers see.

By hand: `packaging/linux/build-appimage.sh [SDL3_PREFIX]` (on Ubuntu 22.04, or with an SDL3 built there, e.g. in
`podman run ubuntu:22.04`), `packaging/windows/build-windows.sh` (Git Bash; or on Linux with `LLVM_MINGW` set to
llvm-mingw's Linux build, which cross-compiles the launcher and makes no installer).

## Previews

A build for testers, as Verdite3's: `tools/prerelease.sh` on a pushed branch publishes its HEAD as the rolling
`preview` prerelease, replacing the last one (one preview at a time, links that never change). Each package (the
AppImage, the Windows zip, the installer) is in a zip locked with the `PREVIEW_PASSWORD` repository secret
(AES-256); the release notes say to ask on the Verdite Project Discord for it.

- It asks by pushing the tag `preview-<commit>`, which starts `release.yml` from that commit on any branch, and the
  job deletes it afterwards. (Verdite's way, starting the workflow by hand, also works here, once `release.yml` is
  on the default branch: `gh workflow run release.yml --ref <branch> -f preview=true`.)
- A prerelease is never GitHub's "latest": no launcher is told about a preview. A preview's own version is the last
  release's tag (0.0.0 before the first), so testers are told when the next release is out.
- `preview-cleanup.yml` deletes the preview when a release is published.
- Change the password: `gh secret set PREVIEW_PASSWORD`, then a new preview.

## Not done

- macOS, iOS, Android (see Platforms).
- A real Windows play test of an installed release: the Windows launcher and its tests were built and run here
  only through cross-compiling and Wine; the release workflow's Windows job is the first native run.
- An icon: the AppImage carries a transparent square.
- One-click updating (the owner chose the notice only, as Verdite's).
