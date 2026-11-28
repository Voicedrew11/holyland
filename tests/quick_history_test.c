/* The quick save history's rule (port/platform/quick_history_rules.c): which saves it keeps */
#include <stdio.h>
#include <string.h>

#include "port/quick_history.h"

static int failures;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                               \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

#define SPAN PORT_QUICK_HISTORY_SPAN_S
#define MOST 512

static void none(void) {
    uint8_t keep[1] = {7};

    CHECK(port_quick_history_choose(NULL, 0, keep) == 0);
}

/* F2 pressed ten times in a row: the newest three stay, and the older spans' saves are not pushed out */
static void mashing(void) {
    PortQuickHistoryEntry entries[12];
    uint8_t keep[12];
    int i;

    for (i = 0; i < 10; i++) {
        entries[i] = (PortQuickHistoryEntry){10 * SPAN + 99 - (uint32_t)i, 1};
    }
    entries[10] = (PortQuickHistoryEntry){9 * SPAN + 300, 1};
    entries[11] = (PortQuickHistoryEntry){8 * SPAN + 300, 1};
    CHECK(port_quick_history_choose(entries, 12, keep) == 5);
    for (i = 0; i < 3; i++) {
        CHECK(keep[i]);
    }
    for (i = 3; i < 10; i++) {
        CHECK(!keep[i]);
    }
    CHECK(keep[10] && keep[11]);
}

/* One save in each of 20 spans, one map: the newest PORT_QUICK_HISTORY_SPANS spans */
static void spans_capped(void) {
    PortQuickHistoryEntry entries[20];
    uint8_t keep[20];
    int i;

    for (i = 0; i < 20; i++) {
        entries[i] = (PortQuickHistoryEntry){(uint32_t)(20 - i) * SPAN, 1};
    }
    CHECK(port_quick_history_choose(entries, 20, keep) == PORT_QUICK_HISTORY_SPANS);
    for (i = 0; i < 20; i++) {
        CHECK(keep[i] == (i < PORT_QUICK_HISTORY_SPANS));
    }
}

/* The last save on a map before one on another is kept, in one span */
static void map_leaves(void) {
    static const int32_t maps[] = {2, 2, 2, 2, 1, 1, 3, 3};
    PortQuickHistoryEntry entries[8];
    uint8_t keep[8];
    static const uint8_t want[] = {1, 1, 1, 0, 1, 0, 1, 0};
    int i;

    for (i = 0; i < 8; i++) {
        entries[i] = (PortQuickHistoryEntry){5 * SPAN + 500 - (uint32_t)i, maps[i]};
    }
    CHECK(port_quick_history_choose(entries, 8, keep) == 5);
    for (i = 0; i < 8; i++) {
        CHECK(keep[i] == want[i]);
    }
}

/* Many map changes in one span: the newest PORT_QUICK_HISTORY_MAP_LEAVES of them */
static void leaves_capped(void) {
    PortQuickHistoryEntry entries[20];
    uint8_t keep[20];
    int i, leaves = 0;

    for (i = 0; i < 20; i++) {
        entries[i] = (PortQuickHistoryEntry){SPAN + 500 - (uint32_t)i, i % 2};
    }
    CHECK(port_quick_history_choose(entries, 20, keep) == PORT_QUICK_HISTORY_NEWEST + PORT_QUICK_HISTORY_MAP_LEAVES);
    for (i = PORT_QUICK_HISTORY_NEWEST; i < 20; i++) {
        leaves += keep[i];
    }
    CHECK(leaves == PORT_QUICK_HISTORY_MAP_LEAVES);
}

/* Adds one save and thins, as port/platform/quick_history.c does: the list stays newest first */
static int add_and_thin(PortQuickHistoryEntry *list, int count, PortQuickHistoryEntry save) {
    uint8_t keep[MOST];
    int kept = 0, i;

    memmove(list + 1, list, sizeof(list[0]) * (size_t)count);
    list[0] = save;
    count++;
    port_quick_history_choose(list, count, keep);
    for (i = 0; i < count; i++) {
        if (keep[i]) {
            list[kept++] = list[i];
        }
    }
    return kept;
}

/* Three hours of play, a save each 30 s and a new map each 5 min, then F2 pressed 30 times at the end: never more than
   PORT_QUICK_HISTORY_MAX, the newest always there, no gap over two spans in the last hour, and a save from ten
   minutes or more before the mashing still there after it */
static void long_session(void) {
    PortQuickHistoryEntry list[MOST];
    int count = 0, i;
    uint32_t t;

    for (t = 30; t <= 3 * 3600; t += 30) {
        PortQuickHistoryEntry save = {t, (int32_t)(t / 300)};

        count = add_and_thin(list, count, save);
        CHECK(count <= PORT_QUICK_HISTORY_MAX);
        CHECK(list[0].play_seconds == t);
        for (i = 1; i < count && t - list[i].play_seconds <= 3600; i++) {
            CHECK(list[i - 1].play_seconds - list[i].play_seconds <= 2 * SPAN);
        }
    }
    t -= 30;
    for (i = 0; i < 30; i++) {
        count = add_and_thin(list, count, (PortQuickHistoryEntry){t, (int32_t)(t / 300)});
    }
    CHECK(count <= PORT_QUICK_HISTORY_MAX);
    {
        int older = 0;

        for (i = 0; i < count; i++) {
            older += t - list[i].play_seconds >= SPAN;
        }
        CHECK(older >= PORT_QUICK_HISTORY_SPANS - 1);
    }
}

int main(void) {
    none();
    mashing();
    spans_capped();
    map_leaves();
    leaves_capped();
    long_session();
    if (failures) {
        fprintf(stderr, "quick_history: %d failures\n", failures);
        return 1;
    }
    printf("quick_history: ok\n");
    return 0;
}
