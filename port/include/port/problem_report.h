/* The problem report (port/report/problem_report.cpp; docs/port/PROBLEM_REPORTS.md): the player menu's "Report an
   Issue". It saves <data folder>/reports/holyland-report-<time>.zip and opens the repository's issue form in the
   browser, filled in, with the folder that holds the zip. GitHub cannot attach a file from a link, so the player drags
   the zip into the form. Nothing is sent by the game.

   The zip:
     README.txt        what is in it
     system.txt        the build, the system, the CPU, memory, the GPU, displays, the window, sound, controllers,
                       free disk space
     game.txt          the game's own facts now: running time, vblanks, the map, the options
     game_state.json   F10's dump (port_debug_write_state)
     settings.json     the player's settings file as it is
     environment.txt   the command line and the variables that change how the game, SDL or the GPU's driver run
     logs/game.log     this run's log (the launcher's copy of what the game printed: its first lines and its end)
     logs/game.prev.log  the run before it (a crash the player started again after), when from the last day
     logs/build.log    the end of the launcher's build of this game
     screenshot.png    the newest picture shown (the flash recorder's ring: the game's picture, not the menu)
     saves/            the save points' saves, the quick saves, and a quick save taken for the report where the
                       player stands (when the game can save there)
   Texts are redacted: the home folder, the user's and the computer's names and e-mail addresses are taken out.
   The saves hold the player's character, and are posted with the issue: the menu's help says so.

   KF4_REPORT_NO_OPEN=1: no browser or folder opened; the link goes to <zip>.url.txt (tests). KF4_DEBUG_AT=report@VBLANK
   starts one at that vblank (port/debug/debug.c). */
#ifndef PORT_PROBLEM_REPORT_H
#define PORT_PROBLEM_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef KF4_HAVE_PLAYER_UI
/* (the game's thread) starts a report: what needs the game is taken now, the zip is made and the form opened on a
   thread of its own a few seconds later. Nothing while one is being made. */
void port_report_start(void);
/* (the game's thread) what the last report did, for the menu: "" before the first */
const char *port_report_message(void);
#else
/* (a build without the player menu: no SDL, no Vulkan presenter) */
static inline void port_report_start(void) {
}
static inline const char *port_report_message(void) {
    return "";
}
#endif

#ifdef __cplusplus
}
#endif

#endif
