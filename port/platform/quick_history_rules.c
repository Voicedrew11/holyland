/* The quick save history's rule (port/include/port/quick_history.h): which saves it keeps. Nothing here reads files
   or the game.

   The spans are fixed stretches of play time (0 to 10 minutes, 10 to 20, ...), not gaps between saves: a gap
   measured from the newest save would move with each new save and drop the save that marked the last ten
   minutes, while a fixed span keeps its newest save until the span is no longer among the last few. After a load of
   an older save the play time goes back, and the saves of the two runs share the spans: the newer save of a span is
   the one kept, whichever run it came from. */
#include "port/quick_history.h"

#include <string.h>

static uint32_t span_of(const PortQuickHistoryEntry *entry) {
    return entry->play_seconds / PORT_QUICK_HISTORY_SPAN_S;
}

/* Whether an entry newer than `index` (one of 0 .. index-1) is in the same span */
static int span_seen(const PortQuickHistoryEntry *entries, int index) {
    for (int i = 0; i < index; i++) {
        if (span_of(&entries[i]) == span_of(&entries[index])) {
            return 1;
        }
    }
    return 0;
}

int port_quick_history_choose(const PortQuickHistoryEntry *entries, int count, uint8_t *keep) {
    int spans = 0, leaves = 0, kept = 0;

    if (count <= 0) {
        return 0;
    }
    memset(keep, 0, (size_t)count);
    for (int i = 0; i < count; i++) {
        int take = i < PORT_QUICK_HISTORY_NEWEST;

        /* the newest of its span */
        if (!span_seen(entries, i) && spans < PORT_QUICK_HISTORY_SPANS) {
            spans++;
            take = 1;
        }
        /* the last on its map before a save on another (the entry just newer is on another map), older than the
           newest (those are kept anyway) */
        if (i >= PORT_QUICK_HISTORY_NEWEST && entries[i - 1].map_tag != entries[i].map_tag &&
            leaves < PORT_QUICK_HISTORY_MAP_LEAVES) {
            leaves++;
            take = 1;
        }
        keep[i] = (uint8_t)take;
        kept += take;
    }
    return kept;
}
