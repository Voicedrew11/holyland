/* The hands mod's plain rules (port/include/port/hands.h): how many slots each hand has, what a slot may
   hold, the step to the next slot, and the hands as a line of text. Nothing here reads the game. */
#include <stdio.h>
#include <string.h>

#include "port/hands.h"

#define WEAPONS 70 /* weapon_defs, weapon_entries */
#define SPELLS 91  /* the spells of shortcut_entries (150 to 240) and of the Magic row */
#define ITEMS 150  /* the items of shortcut_entries (0 to 149) */
#define SHIELDS 30 /* shield_entries */

static const int slot_counts[HAND_COUNT] = {
    [HAND_WEAPON] = 2,
    [HAND_SPELL] = 2,
    [HAND_HOTKEY] = 5,
};

int hand_slot_count(HandKind kind) {
    return kind >= 0 && kind < HAND_COUNT ? slot_counts[kind] : 0;
}

int hand_value_valid(HandKind kind, int value) {
    switch (kind) {
    case HAND_WEAPON:
        return value >= 0 && value < WEAPONS;
    case HAND_SPELL:
        return value >= 0 && value < SPELLS;
    case HAND_HOTKEY:
        if (value == 0) {
            return 1;
        }
        if (HOTKEY_TYPE(value) == HOTKEY_ITEM) {
            return HOTKEY_NUMBER(value) < ITEMS;
        }
        return HOTKEY_TYPE(value) == HOTKEY_SPELL && HOTKEY_NUMBER(value) < SPELLS;
    default:
        return 0;
    }
}

int hand_next_slot(const Hand *hand, HandKind kind, int step, int (*usable)(HandKind kind, int value, const void *context),
                   const void *context) {
    int count = hand_slot_count(kind);
    int i;
    int slot;

    if (count == 0 || hand->out < 0 || hand->out >= count) {
        return -1;
    }
    step = step < 0 ? count - 1 : 1;
    for (i = 1, slot = hand->out; i < count; i++) {
        slot = (slot + step) % count;
        if (usable(kind, hand->slots[slot], context)) {
            return slot;
        }
    }
    return -1;
}

/* "hands 2 weapon O A B spell O A B hotkey O A B C D E shield S" */
size_t hands_format(const HandsState *state, char *out, size_t size) {
    static const char *const names[HAND_COUNT] = {"weapon", "spell", "hotkey"};
    size_t at = 0;
    int kind;
    int slot;
    int n;

    n = snprintf(out, size, "hands 2");
    at = n > 0 ? (size_t)n : 0;
    for (kind = 0; kind < HAND_COUNT && at < size; kind++) {
        const Hand *hand = &state->hands[kind];

        n = snprintf(out + at, size - at, " %s %d", names[kind], hand->out);
        at += n > 0 ? (size_t)n : 0;
        for (slot = 0; slot < slot_counts[kind] && at < size; slot++) {
            n = snprintf(out + at, size - at, " %d", hand->slots[slot]);
            at += n > 0 ? (size_t)n : 0;
        }
    }
    if (at < size) {
        n = snprintf(out + at, size - at, " shield %d\n", state->shield_away);
        at += n > 0 ? (size_t)n : 0;
    }
    return at < size ? at : 0;
}

static int read_hand(const char **text, const char *name, HandKind kind, Hand *hand) {
    char word[16];
    int used;
    int slot;

    if (sscanf(*text, " %15s %d%n", word, &hand->out, &used) != 2 || strcmp(word, name) != 0) {
        return 0;
    }
    *text += used;
    if (hand->out < 0 || hand->out >= slot_counts[kind]) {
        return 0;
    }
    for (slot = 0; slot < slot_counts[kind]; slot++) {
        if (sscanf(*text, " %d%n", &hand->slots[slot], &used) != 1 || !hand_value_valid(kind, hand->slots[slot])) {
            return 0;
        }
        *text += used;
    }
    for (; slot < HAND_SLOTS_MAX; slot++) {
        hand->slots[slot] = 0;
    }
    return 1;
}

int hands_parse(const char *text, HandsState *out) {
    HandsState read;
    int weapon;
    int shield;
    int used;

    memset(&read, 0, sizeof(read));
    if (sscanf(text, "set2 weapon %d shield %d", &weapon, &shield) == 2) {
        /* the weapon sets mod's: what is in hand is the game's, so slot 0; its set 2 is slot 1 */
        if (weapon < 0 || weapon >= WEAPONS || shield < 0 || shield >= SHIELDS) {
            return 0;
        }
        read.hands[HAND_WEAPON].slots[1] = weapon;
        read.shield_away = shield;
        *out = read;
        return 1;
    }
    used = 0;
    if (sscanf(text, "hands 2%n", &used) == EOF || used == 0) {
        return 0;
    }
    text += used;
    if (!read_hand(&text, "weapon", HAND_WEAPON, &read.hands[HAND_WEAPON]) ||
        !read_hand(&text, "spell", HAND_SPELL, &read.hands[HAND_SPELL]) ||
        !read_hand(&text, "hotkey", HAND_HOTKEY, &read.hands[HAND_HOTKEY])) {
        return 0;
    }
    if (sscanf(text, " shield %d", &read.shield_away) != 1 || read.shield_away < 0 || read.shield_away >= SHIELDS) {
        return 0;
    }
    *out = read;
    return 1;
}

HandPut hand_put(const Hand *hand, HandKind kind, int slot, int value) {
    int other;

    if (slot < 0 || slot >= hand_slot_count(kind) || hand->slots[slot] == value) {
        return HAND_PUT_NOTHING;
    }
    if (slot == hand->out) {
        return HAND_PUT_EQUIP;
    }
    for (other = 0; other < hand_slot_count(kind); other++) {
        if (other != slot && value != 0 && hand->slots[other] == value) {
            return HAND_PUT_ELSEWHERE;
        }
    }
    return HAND_PUT_KEEP;
}
