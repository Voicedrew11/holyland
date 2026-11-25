/* Where the player's files live: one rule for the game (port_data_dir, settings.c) and the launcher
   (launcher/), so the two can never look in different places.

   The data folder holds settings.json, saves, quick saves, captures and the launcher's small files
   (launcher.json, update.json). The builds folder holds the launcher's builds of the game: big, and made
   again from the release whenever they are missing, so it is the local (not roaming) one on Windows. On Linux
   all of it is in one folder, XDG's data folder (~/.local/share/holyland), builds and save states included.

   Both read the environment through `env` (getenv, or a table in a test) and write the path into `out`.
   They return 1, or 0 if the path did not fit. */
#ifndef PORT_DATA_DIR_H
#define PORT_DATA_DIR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DATA_DIR_WINDOWS, DATA_DIR_POSIX } DataDirSystem;

typedef const char *(*DataDirEnv)(const char *name);

/* KF4_DATA_DIR, else (started by RecompLauncher) RECOMPLAUNCHER_DATA, else %APPDATA%\holyland, else
   $XDG_DATA_HOME/holyland, else ~/.local/share/holyland, else "holyland" (relative to the working folder).
   Under RecompLauncher a player's usual folder is kept instead when it has a saves folder and RecompLauncher's has
   none (docs/port/LAUNCHER.md, "RecompLauncher"). */
int data_dir_resolve(DataDirSystem system, DataDirEnv env, char *out, size_t out_size);

/* KF4_BUILDS_DIR, else (KF4_DATA_DIR unset) RECOMPLAUNCHER_DATA/cache/builds, else %LOCALAPPDATA%\holyland\builds,
   else $XDG_DATA_HOME/holyland/builds, else ~/.local/share/holyland/builds, else <data folder>/builds */
int data_dir_resolve_builds(DataDirSystem system, DataDirEnv env, char *out, size_t out_size);

/* Moves the folders of earlier versions to where they are now, before either folder is used: call it once,
   first thing (the game's main and the launcher's). Nothing moves under KF4_DATA_DIR or KF4_BUILDS_DIR.
   - From 2026-11-18 to 2026-11-20 the folders were named darker-reality (the project's name then): the old data
     folder and the old local folder (the one holding builds/, and on Linux states/) take the names above, each
     only when the old one exists and the new one does not.
   - Until 2026-11-21 Linux's data folder was XDG's config folder (~/.config/holyland, or darker-reality): its
     files move into the data folder one by one, a file of the same name already there never replaced (that one
     stays behind, and the old folder with it).
   Returns how many folders moved, wholly or in part. */
int data_dir_move_old_folders(DataDirSystem system, DataDirEnv env);

/* the system this program was built for */
DataDirSystem data_dir_this_system(void);

#ifdef __cplusplus
}
#endif

#endif
