/* A file written whole beside its place (`temp`), then put in place of the old one in one step, so a crash or
   a full disk never leaves half a file and never loses the old one: settings, quick saves, save states, the
   automap's and the hands' files, the update notice.

   On Windows the step is MoveFileEx with MOVEFILE_REPLACE_EXISTING (C's rename refuses an existing file), tried
   again for a little while when another program has either file open: a virus scanner or a backup or cloud
   folder program opening a file the moment it is written is common there, and the open lasts milliseconds. */
#ifndef PORT_FILE_REPLACE_H
#define PORT_FILE_REPLACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Puts `temp` in place of `path` (which need not exist). Returns 1, or 0 with `temp` and `path` as they were.
   Paths are UTF-8 (the Windows manifest's code page, packaging/shared/holyland.manifest). Waits at most
   FILE_REPLACE_WAIT_MS when the files are held open by another program. */
int port_replace_file(const char *temp, const char *path);

#define FILE_REPLACE_WAIT_MS 1000

#ifdef __cplusplus
}
#endif

#endif
