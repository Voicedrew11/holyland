/* What a thread is doing, for the crash report (port/include/port/crash_note.h) */
#include "port/crash_note.h"

#include <string.h>

static _Thread_local char note[192];

void port_crash_note_set(const char *text) {
    size_t length = text ? strlen(text) : 0;
    if (length >= sizeof note) {
        length = sizeof note - 1;
    }
    if (length) {
        memcpy(note, text, length);
    }
    note[length] = '\0';
}

const char *port_crash_note(void) {
    return note;
}
