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

/* KF4_DATA_DIR, else %APPDATA%\holyland, else $XDG_CONFIG_HOME/holyland, else ~/.config/holyland, else
   "holyland" (relative to the working folder) */
int data_dir_resolve(DataDirSystem system, DataDirEnv env, char *out, size_t out_size);

/* KF4_BUILDS_DIR, else %LOCALAPPDATA%\holyland\builds, else $XDG_DATA_HOME/holyland/builds, else
   ~/.local/share/holyland/builds, else <data folder>/builds */
int data_dir_resolve_builds(DataDirSystem system, DataDirEnv env, char *out, size_t out_size);

/* the system this program was built for */
DataDirSystem data_dir_this_system(void);

#ifdef __cplusplus
}
#endif

#endif
