# Problem reports

**Report an Issue** (its own tab in the F1 menu's sidebar, last: its Make a Report row) saves a report file and opens the repository's issue form, filled in.
Modelled on Soulbrandt's (the Demon's Souls port): a link can fill in an issue's text but cannot attach a file, so
the player drags the zip into the form, whose "Report file (.zip)" box is required.

Code: `port/report/` (`problem_report.cpp` the game's flow, `system_facts.cpp` the game's own facts: GPU, displays,
the run), and the parts the launcher builds too (C++17, no SDL or game): `pc_facts.cpp` the PC, `report_bundle.cpp`
the files on their way into the zip, `report_text.cpp` redaction and the link, `zip_archive.cpp` the zip. Also
`port/include/port/problem_report.h`, `launcher/crash_report.h`, and the form `.github/ISSUE_TEMPLATE/report.yml`
(GitHub reads it from the default branch only).

## When the game crashes

The launcher's "The game stopped unexpectedly" page has **Report the problem**. The game cannot report its own crash,
so the launcher makes `<data folder>/reports/holyland-crash-<time>.zip` from what it kept (`launcher/crash_report.h`):
`crash.txt` (how the game ended, the launcher's version, the game's command line), `system.txt` (the PC; the GPU is in
the log's first lines), `environment.txt`, `settings.json`, the log of the run that stopped and the one before, the
build log and the saves. The form opens titled "The game stopped: <how>", with the log's last 25 lines in Details.
There is no screenshot, game state or quick save: the game is gone. Tested in `launcher_test` (`test_crash_report`).

## What happens

1. On the game's thread, when the row is chosen: the flash recorder's newest picture is asked for
   (`port_vk_flash_write_newest`), a quick save to a file (`port_quick_request_save_file`), F10's game state is
   written, and the system's, run's and environment's facts are taken. The menu closes, so the game is in play for
   the quick save and the browser gets the screen.
2. On a thread of its own ("problem report"): it waits up to 3 s for the picture and 8 s for the quick save, zips
   everything into `<data folder>/reports/holyland-report-<time>.zip`, opens the form and the folder (`SDL_OpenURL`),
   and the row's note says where the zip is.

## What the zip holds

| File | What |
|---|---|
| `system.txt` | the build (version, commit, optimized or not, compiler), SDL, the system and session (Wayland or X11, desktop, AppImage, gamescope, Wine), CPU and its instruction sets, memory (the PC's and the game's), the GPU, its driver and memory, each display (size, refresh, scale), the window (size, fullscreen), sound, controllers, free disk space |
| `game.txt` | running time, vblanks, the disc's file name, renderer, presenter, frame rate, the refresh seen; and what the report could not take |
| `game_state.json` | F10's dump: the last events and the game's state |
| `settings.json` | the settings file as it is |
| `environment.txt` | the command line and the variables that change the game, SDL, the Vulkan loader or a GPU driver (`KF4_`, `SDL_`, `VK_`, `RADV_`, `MESA_`, `DXVK_`, `LD_PRELOAD`, ...) |
| `logs/game.log` | this run's log: its first 300 lines and last 3 MB |
| `logs/game.prev.log` | the run before (a crash the player started again after), if from the last day: its end (1 MB) |
| `logs/build.log` | the end of the launcher's build of the game |
| `screenshot.png` | the newest picture shown, at the flash recorder's size (540 rows): the game's, not the menu |
| `saves/quick-now.kf4quick` | a quick save where the player stood (`--quick-load FILE` loads it) |
| `saves/`, `saves/quicksaves/` | the save points' saves and the quick saves (8 MB a file, 32 MB in all at most) |

**The log** is the launcher's: it already copies what the game prints into `game.log`, and now flushes each line,
keeps the run before as `game.prev.log`, and tells the game where it is (`KF4_GAME_LOG`). A game started without the
launcher (a developer's run) has no log in its report, and `game.txt` says so.

## Privacy

Reports are posted on a public issue. Every text in the zip and the link goes through `report::redact`: the home
folder becomes `~`, the user's name `<user>` and the computer's name `<computer>` (whole words, case ignored; a
default name such as Fedora's `fedora` is left, or it would take the system's name out), e-mail addresses
`<e-mail>`. The saves hold the player's character; the row's help says so. The game sends nothing.

## Testing

- `KF4_DEBUG_AT=report@VBLANK` chooses the row at that vblank; `KF4_REPORT_NO_OPEN=1` opens nothing and writes the
  link to `<zip>.url.txt`.
- A run as the launcher's: `KF4_DATA_DIR=<scratch> KF4_GAME_LOG=<scratch>/game.log KF4_REPORT_NO_OPEN=1
  KF4_DEBUG_AT=report@2400 kf4 --disc ... --input tests/newgame.pad --teleport 1900:009 --invincible --no-audio
  --saves <scratch>/saves --frames 2900 > <scratch>/game.log 2>&1` (a window: the picture needs the Vulkan presenter).
- `report_test`: redaction, the link and its cutting, a log's start and end, a zip read back, a bundle's redaction
  and saves budget. `KF4_REPORT_NO_OPEN=1` works for the launcher's crash report too.
