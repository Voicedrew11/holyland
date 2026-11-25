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
Rebuild, Open data folder, "Check for updates".

The window (`launcher/window.h`) is RmlUi, the player menu's UI library, drawn on SDL's renderer (no Vulkan: it
opens even where the game cannot). Its pages are documents in `launcher/res/` (`setup.rml`, `building.rml`,
`failed.rml`, `home.rml`, `stopped.rml`, one style sheet `launcher.rcss` in the menu's look: its Mincho and colours), carried in
the launcher with the menu's font and logo by `cmake/ui_assets.cmake`. A screen shows its page, fills its parts by
id (`set_text`, `set_rml`, `set_classes`) and asks for the buttons clicked (`data-action`, `take_action`). It shares
the menu's RmlUi code: `port/ui/ui_render.cpp` records RmlUi's drawing as a `ui::Frame`, which `window.cpp` draws
with `SDL_RenderGeometryRaw`, and `port/ui/ui_ties.cpp` gives RmlUi the clock, log, cursor, clipboard, files and
fonts. Sizes are in dp, from SDL's display scale, so a 115% screen gets sharp, larger text.

`KF4_LAUNCHER_SHOT=FILE.bmp` saves the window's picture at its tenth frame (with `KF4_DATA_DIR` at an empty folder:
the first start), for checking the pages without a screenshot of the desktop. `--build-only` builds with no window and no disc (CI, tests).

## The files

| Where | What | Written by |
|---|---|---|
| data folder (`port/platform/data_dir.c`: `%APPDATA%\holyland`, `~/.local/share/holyland`, `KF4_DATA_DIR`) | `settings.json`, saves | the game |
| | `launcher.json`: disc, `update_check`, `skipped`, `setup_done` | the launcher's window; the overlay sets `skipped` only, while the launcher's window is closed |
| | `update.json`: the cached answer (`checked`, `tag`, `url`) | the launcher's update thread only |
| | `update-request.json`: the release the player chose to update to (`tag`) | the game; the launcher reads and removes it |
| | `build.log`, `game.log` | the launcher |
| builds folder (`%LOCALAPPDATA%\holyland\builds`, `$XDG_DATA_HOME/holyland/builds`, `KF4_BUILDS_DIR`; under `KF4_DATA_DIR` when that is set) | `<key>/kf4` and its libraries, and `vk_layers/` (the synchronization2 layer: `docs/port/PC_RENDERER.md`); older keys (and a `<key>/obj/` left over) are removed after a build and at each start, read-only files too; one still open (an antivirus scanning it) waits for the next start | the launcher |
| install folder (read-only but for updates) | the launcher, `content/`; during and after an update `*.partial`, `*.delta`, `*.old`, `update.partial/`, removed at the next start | the installer, the launcher's update |

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
  - LLVM's release `lld` asks for `libxml2.so.2` (for its Windows linker's manifests only), which Arch has only in
    `libxml2-legacy` since libxml2 2.14. A stand-in that does nothing goes in `llvm/lib`
    (`packaging/linux/libxml2-stub/`), and `toolchain.sh` fails if any tool needs a library that is neither shipped
    nor on every Linux.
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

The player is told, and can let Holyland update itself in one step, as OpenCE's updater (OpenCommunityEdition/OpenCE,
`port/linux/src/updater.c`): download, check, put in place, start again.

**Finding a release.** `launcher/update_check.cpp` asks `GET https://api.github.com/repos/Voicedrew11/holyland/releases/latest`
(drafts and pre-releases are never "latest") on its own thread, once each time the launcher starts, 10 s at most; the answer goes
to `update.json`. No network: the cached answer stands. A URL that is not `https://github.com/` is refused. The rules
are shared C (`port/platform/update_notice.c`, `tests/update_notice_test.c`): `v` dropped, dotted numbers compared,
suffixes ignored (Verdite's `UpdateCheck.IsNewer`); a developer's build (no version) is never told.

**Asking the player.**
- The game's title screen (`Quick_PollTitle` calls `port_update_title`, `port/include/port/update_offer.h`): once a
  run, when `update.json` names a newer release (the overlay reads it for the first minute, as the check may finish
  after the game starts), a native pop-up (`SDL_ShowMessageBox`: zenity on Linux, the system's own box on Windows),
  "Holyland v0.3.0 is available. You have 0.2.0.": **Update**, **Skip this version**, **Later**. It runs on a thread
  of its own, so the game keeps drawing while it is open; the answer is taken on the game's next frame. Skip writes `skipped` in `launcher.json`, so that release
  is never offered again; the next one is. Where no pop-up can open, the F1 menu opens on General instead.
- The F1 menu's General tab: **Update now**, Open download page, Skip this version, while an update is due.

**Updating.** Update in the game writes `update-request.json` (`{"tag": "v0.3.0"}`) and quits the game the way
closing its window does; the launcher, which started the game and waited for it, reads and removes the request and
opens its window on "Updating" (`launcher/self_update.h`, `res/updating.rml`):
1. GitHub's answer about that release (`.../releases/tags/<tag>`): this way of installing's file, `SHA256SUMS` and
   `SHA256SUMS.sig`. A release without the two is not installed from here (the Download page is).
2. `SHA256SUMS.sig` must be `SHA256SUMS`'s Ed25519 signature by the release key (`deploy/release-key.pub`, compiled
   in as `launcher/release_key.h`; checked with Monocypher, `third_party/monocypher/`).
3. The file is downloaded beside what it replaces and its SHA-256 must be the one `SHA256SUMS` gives.
4. It is put in place and the new launcher started with the same arguments; this one exits. The new one builds its
   release's game once (a new build key) and plays. Settings, saves and quick saves are in the data folder and are
   not touched; save states are tied to a build, as always.

**Deltas.** Step 3 downloads a few MB instead of the whole file when it can (`launcher/update_delta.h`). Each release
also carries, for each of the last 3 published releases, `<whole file>.from-<version>.delta`, listed in the signed
`SHA256SUMS` like every file. The launcher looks for the one from its own version; if there is one and its way of
installing takes deltas, it downloads it, checks its SHA-256, and makes the update from it:

| Installed as | The delta | Made into the update |
|---|---|---|
| AppImage | a file patch: `zstd --patch-from=<old AppImage>` | the running AppImage as the patch's reference (zstd's decoder is libchdr's copy), written to `<AppImage>.partial`, whose SHA-256 must be the whole AppImage's in `SHA256SUMS` |
| Windows zip | a tree delta: a zip of the new and changed files, and `delta-files.txt` (every file of the new zip: `<sha256>  <path>`) | unpacked into `update.partial\`, every other listed file hard-linked (or copied) from the install, each file checked against the list, nothing unlisted allowed; then put in place as an unpacked zip |
| Windows installer | none | the whole installer, as before |

Anything wrong with a delta (none from this version, a bad checksum, a patch that does not fit the installed file,
a file the list does not match) and the whole file is downloaded instead; the reason goes to the log
(`[update] the delta did not work (...)`). A launcher without a version (a developer's build) takes none. Launchers
before 0.2.0 do not know deltas: the first update that uses one is from 0.2.0.

Making them: `packaging/make_deltas.sh NEW_DIR OLD_DIR OLD_VERSION` (a delta under half its whole file is kept). The
release workflow's release job downloads the last 3 releases' AppImage and zip, makes the deltas, and checks every
one with the launcher's own code (`packaging/check_deltas.sh` running `launcher_delta_check`, built with
`-DLAUNCHER_WINDOW=OFF`, which needs no SDL3) before `SHA256SUMS` is written: a delta that does not make its whole
file exactly fails the release. `ci.yml`'s launcher job runs `packaging/delta_test.sh` (made-up releases, broken deltas
that must fail; ctest's `deltas`) and makes and checks deltas from the latest release's real files to a changed copy.

Sizes on 2026-11-22: v0.1.0 to a build of a later commit, AppImage 175 MB to a 5.8 MB delta, Windows zip 145 MB to
2.7 MB (most of it the new launcher). Making both takes ~20 s, applying the AppImage's under a second.

A whole update from a delta was run here on 2026-11-22 (the recipe in Testing below): 0.0.1 to 0.0.2 with the whole
file's URL pointing nowhere, so only the 1.5 MB delta could have made it; and again with a delta made from another
AppImage, which was refused and the whole file downloaded.

Any failure shows why (`res/update_failed.rml`): Try again, Download page, Play this version, Close. Nothing is
replaced before every check has passed.

How each way of installing is replaced (`Installation` in `launcher/platform.h`, one class each):

| Installed as | Detected by | The file | Put in place |
|---|---|---|---|
| AppImage (`platform/linux.cpp`) | `$APPIMAGE` (set by the AppImage's runtime), its folder writable | `*-x86_64.AppImage` | downloaded to `<AppImage>.partial`, renamed over the AppImage (the running one's mount keeps the old data), the new one started in a session of its own |
| Windows installer (`platform/windows.cpp`) | `unins000.exe` beside the launcher | `*-win-x64-setup.exe` | the launcher and `SDL3.dll` renamed to `.old` (a running program can be renamed, not overwritten), the installer run with `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /HOLYLANDRELAUNCH`; `installer.iss` starts the new launcher when given that switch |
| Windows zip | `content/manifest.txt` beside the launcher, the folder writable | `*-win-x64.zip` | unpacked into `update.partial\` with Windows' own `tar.exe` (Windows 10 1803 and later), each of its files and folders put in place after the old one is renamed to `.old`; rolled back if one fails |

The next start removes what an update left (`remove_leftovers`: `.old` files, a half download). A developer's build,
an AppImage in a folder it cannot write, or an extracted AppImage cannot update itself: Update then ends on the failed
page, whose Download page opens the release.

**Signing a release.** `tools/release_local.sh` writes `SHA256SUMS` over every file and signs it with
`tools/release_sign.sh` and the owner's key (a PEM Ed25519 private key, `~/.config/holyland-release/release-signing-key.pem`
or `RELEASE_SIGNING_KEY`); with no key it fails, so no unsigned release is made. The private key was made on 2026-11-22 and is kept by the owner, outside the
repository. A new key: `openssl genpkey -algorithm ed25519`, `tools/release_sign.sh public-key KEY.pem` into
`deploy/release-key.pub` and `launcher/release_key.h`, the key file replaced; launchers built before it never update to
a release signed with the new one.

**Testing.** `KF4_UPDATE_CHECK=0` turns the check off, `=force` ignores the hour. `KF4_UPDATE_URL` (a test only;
`file://` works on Linux) answers instead of GitHub, for the check and for the update; its files may then be `file://`
too, and they must be signed with `KF4_UPDATE_KEY` (64 hex digits) instead of the release key. A whole update was run
this way on 2026-11-22 with two AppImages built here (0.0.1 updating to 0.0.2), the pop-up's Update pressed by a
stand-in `zenity` on `PATH` (SDL runs zenity for the pop-up on Wayland and reads the button pressed from it).

## Versions

The tag is the version (`tools/version.sh`: `$HOLYLAND_VERSION`, else the newest `vX.Y.Z` tag, else 0.0.0). The
launcher is built with `-DHOLYLAND_VERSION`, and passes it to the game's build as `KF4_VERSION`
(`port/include/port/version.h`, shown in About and the bug report). A developer's build has none.

## Making a release

Releases are built on the owner's computer, not on GitHub's runners (since 0.1.5; the release workflow now makes
only previews, and a pushed `v` tag starts nothing).

1. Check a launcher-built `kf4` against a developer's build: `tools/build_compare.py --exe out/linux-clang/kf4
   --exe <builds>/<key>/kf4 --keep out/compare`.
2. `tools/release_local.sh X.Y.Z` on a pushed `main`: tags HEAD (`tools/release.sh`), builds the AppImage in an
   Ubuntu 22.04 container (`packaging/linux/Containerfile`, podman), the Windows zip (llvm-mingw cross-compiling the
   launcher) and installer (Inno Setup in Wine), builds the game from the AppImage with `--build-only` as a player
   would, makes and checks the deltas from the last three releases, signs `SHA256SUMS`, pushes the tag and opens a
   **draft** release. `--no-upload` stops before pushing; the files are in `out/release-X.Y.Z/dist`. Publishing the
   draft is what players' launchers see.
3. Not done locally: a player's build from the Windows packages and the installer's update over the last release
   (`packaging/windows/update_test.sh`), which need Windows. A preview (`tools/prerelease.sh`) of the same commit
   still runs both on GitHub's Windows runner.

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

## RecompLauncher

[RecompLauncher](https://github.com/elripalda/RecompLauncher) installs, sets up and starts Holyland from its
releases, by `recomplauncher.json` (its guide: that repository's `docs/DEVELOPERS.md`). The file is at the top of
the repository; `build-appimage.sh` puts it at the AppImage's root and `build-windows.sh` beside `holyland.exe`
(and `payload.sh` leaves it out of `content/src`, so editing it never changes the build key).

- The player's disc: `check.ids` (the disc's serial; a `.chd` is taken because the label names it) and the Redump
  dump of the USA disc for the verified badge (Redump disc 27994: 3464921088 bytes, CRC-32 `879215a7`).
- Its setup step runs `holyland --prepare`: `--build-only` with `progress 0.40 TEXT` lines on stdout.
- Play runs `holyland --disc <the disc>`. RecompLauncher gives its folder for the game (`{data}`, `<install>/recompdata`)
  as `RECOMPLAUNCHER_DATA`, and the data folder rule (`port/include/port/data_dir.h`) takes it: settings, saves, quick
  saves, save states and logs go there, and the builds in `{data}/cache/builds` (its backups leave `cache` out). A
  player from before keeps their usual folder (`~/.local/share/holyland`, `%APPDATA%\holyland`): it is used when it has
  a `saves` folder and RecompLauncher's has none, nothing moved or copied (as Super Odyssey's port does). The builds
  still go in RecompLauncher's folder. `recomplauncher.json` lists both places for saves, settings and logs, each
  `if` its `saves` folder is there, the same rule.
- RecompLauncher sets `RECOMPLAUNCHER=1` in every game it starts and installs the releases itself, so then the
  launcher asks GitHub nothing and the game offers no update (`update_left_to_recomplauncher`).
- Its game page shows Fullscreen, Frame Rate, Vertical Sync, Widescreen and Anti-Aliasing, written into
  `settings.json` (`display/...`). Renaming one of those keys means changing the file too.

## Not done

- macOS, iOS, Android (see Platforms).
- A real Windows play test of an installed release: the Windows launcher and its tests were built and run here
  only through cross-compiling and Wine; the release workflow's Windows job is the first native run.
- An icon: the AppImage carries a transparent square.
- A real Windows run of an update (installer or zip): built and type-checked here by cross-compiling only.
