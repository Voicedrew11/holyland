/* Where the player's files live: one rule for the game (port_data_dir, settings.c) and the launcher
   (launcher/), so the two can never look in different places.

   The data folder holds settings.json, saves, quick saves, captures and the launcher's small files
   (launcher.json, update.json). The builds folder holds the launcher's builds of the game: big, and made
   again from the release whenever they are missing, so it is the local (not roaming) one on Windows and
   XDG's data folder on Linux.

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

/* KF4_DATA_DIR, else %APPDATA%\darker-reality, else $XDG_CONFIG_HOME/darker-reality, else
   ~/.config/darker-reality, else "darker-reality" (relative to the working folder) */
int data_dir_resolve(DataDirSystem system, DataDirEnv env, char *out, size_t out_size);

/* KF4_BUILDS_DIR, else %LOCALAPPDATA%\darker-reality\builds, else $XDG_DATA_HOME/darker-reality/builds, else
   ~/.local/share/darker-reality/builds, else <data folder>/builds */
int data_dir_resolve_builds(DataDirSystem system, DataDirEnv env, char *out, size_t out_size);

/* Until 2026-11-18 the folders were named holyland (the project's old name): moves the old data folder and
   the old local folder (the one holding builds/, and on Linux states/) to the names above, each only when the
   old one exists and the new one does not. Nothing moves under KF4_DATA_DIR or KF4_BUILDS_DIR. Call it once,
   first thing, before either folder is used (the game's main and the launcher's). Returns how many moved. */
int data_dir_move_old_folders(DataDirSystem system, DataDirEnv env);

/* the system this program was built for */
DataDirSystem data_dir_this_system(void);

#ifdef __cplusplus
}
#endif

#endif
