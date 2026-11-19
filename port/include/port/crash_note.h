/* What a thread is doing, for the crash report (main_host.c's crash_report, Windows). A crash inside a driver
   shows only the driver's own addresses; a thread whose work calls into one where it may crash (a pipeline made
   ahead: the driver compiles it) notes that work here, and the report prints the note of the thread that crashed. */
#ifndef PORT_CRASH_NOTE_H
#define PORT_CRASH_NOTE_H

#ifdef __cplusplus
extern "C" {
#endif

/* (any thread) This thread's note, copied (cut at 191 characters); "" clears it */
void port_crash_note_set(const char *text);
/* (any thread, the crash report's) This thread's note: "" when none */
const char *port_crash_note(void);

#ifdef __cplusplus
}
#endif

#endif
