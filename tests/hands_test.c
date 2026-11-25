/* port/platform/hands_rules.c: the hands mod's slots, the step to the next slot, and the hands' line of text
   (with the weapon sets mod's old one).

     hands_test */
#include <stdio.h>
#include <string.h>

#include "port/hands.h"

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int not_empty(HandKind kind, int value, const void *context) {
    (void)kind;
    (void)context;
    return value != 0;
}

/* the hands' rule in game/hands.c: an empty slot (the bare fist, no spell) is swapped to too */
static int any(HandKind kind, int value, const void *context) {
    (void)kind;
    (void)value;
    (void)context;
    return 1;
}

/* a hotkey of an item holds something only while one is owned */
static int owned(HandKind kind, int value, const void *context) {
    const unsigned char *counts = context;

    (void)kind;
    if (value == 0) {
        return 0;
    }
    return HOTKEY_TYPE(value) != HOTKEY_ITEM || counts[HOTKEY_NUMBER(value)] != 0;
}

static void slots(void) {
    check("two weapons", hand_slot_count(HAND_WEAPON) == 2);
    check("two spells", hand_slot_count(HAND_SPELL) == 2);
    check("five hotkeys", hand_slot_count(HAND_HOTKEY) == 5);
    check("no other hand", hand_slot_count(HAND_COUNT) == 0);
    check("the last weapon", hand_value_valid(HAND_WEAPON, 69) && !hand_value_valid(HAND_WEAPON, 70));
    check("no negative weapon", !hand_value_valid(HAND_WEAPON, -1));
    check("the last spell", hand_value_valid(HAND_SPELL, 90) && !hand_value_valid(HAND_SPELL, 91));
    check("an empty hotkey", hand_value_valid(HAND_HOTKEY, 0));
    check("an item hotkey", hand_value_valid(HAND_HOTKEY, HOTKEY_VALUE(HOTKEY_ITEM, 149)) &&
                                !hand_value_valid(HAND_HOTKEY, HOTKEY_VALUE(HOTKEY_ITEM, 150)));
    check("a spell hotkey", hand_value_valid(HAND_HOTKEY, HOTKEY_VALUE(HOTKEY_SPELL, 90)) &&
                                !hand_value_valid(HAND_HOTKEY, HOTKEY_VALUE(HOTKEY_SPELL, 91)));
    check("no third kind of hotkey", !hand_value_valid(HAND_HOTKEY, HOTKEY_VALUE(3, 1)));
}

static void stepping(void) {
    Hand weapons = {0, {5, 9}};
    Hand one_weapon = {0, {5, 0}};
    Hand hotkeys = {1, {HOTKEY_VALUE(HOTKEY_ITEM, 3), HOTKEY_VALUE(HOTKEY_ITEM, 7), 0, HOTKEY_VALUE(HOTKEY_SPELL, 2),
                        HOTKEY_VALUE(HOTKEY_ITEM, 4)}};
    Hand bad = {7, {1, 2}};
    unsigned char counts[150] = {0};

    check("the other weapon", hand_next_slot(&weapons, HAND_WEAPON, 1, not_empty, NULL) == 1);
    check("the other weapon, backwards", hand_next_slot(&weapons, HAND_WEAPON, -1, not_empty, NULL) == 1);
    check("no other weapon: none", hand_next_slot(&one_weapon, HAND_WEAPON, 1, not_empty, NULL) == -1);
    check("an empty slot taken: the bare fist", hand_next_slot(&one_weapon, HAND_WEAPON, 1, any, NULL) == 1);
    one_weapon.out = 1;
    check("and back from the fist", hand_next_slot(&one_weapon, HAND_WEAPON, -1, any, NULL) == 0);
    one_weapon.out = 0;
    check("the next hotkey skips an empty one", hand_next_slot(&hotkeys, HAND_HOTKEY, 1, not_empty, NULL) == 3);
    check("the previous hotkey", hand_next_slot(&hotkeys, HAND_HOTKEY, -1, not_empty, NULL) == 0);
    counts[3] = 1;
    check("the next owned: the spell", hand_next_slot(&hotkeys, HAND_HOTKEY, 1, owned, counts) == 3);
    hotkeys.out = 3;
    check("round past the end: item 4 not owned, item 3 is", hand_next_slot(&hotkeys, HAND_HOTKEY, 1, owned, counts) == 0);
    hotkeys.out = 0;
    counts[3] = 0;
    check("only the one in use is usable: none",
          hand_next_slot(&hotkeys, HAND_HOTKEY, -1, owned, counts) == 3 && hotkeys.out == 0);
    check("a slot out of range: none", hand_next_slot(&bad, HAND_WEAPON, 1, not_empty, NULL) == -1);
}

static void text(void) {
    HandsState state;
    HandsState back;
    HandsState untouched;
    char line[256];
    char small[16];

    memset(&state, 0, sizeof(state));
    state.hands[HAND_WEAPON] = (Hand){1, {12, 40}};
    state.hands[HAND_SPELL] = (Hand){0, {3, 17}};
    state.hands[HAND_HOTKEY] = (Hand){4, {HOTKEY_VALUE(HOTKEY_ITEM, 1), 0, HOTKEY_VALUE(HOTKEY_SPELL, 90), 0,
                                          HOTKEY_VALUE(HOTKEY_ITEM, 149)}};
    state.shield_away = 7;
    check("written", hands_format(&state, line, sizeof(line)) > 0);
    printf("     %s", line);
    memset(&back, 0xAA, sizeof(back));
    check("read back", hands_parse(line, &back) && memcmp(&state, &back, sizeof(state)) == 0);
    check("no room: nothing", hands_format(&state, small, sizeof(small)) == 0);

    check("the weapon sets mod's", hands_parse("set2 weapon 33 shield 4\n", &back));
    check("its weapon is weapon 2", back.hands[HAND_WEAPON].out == 0 && back.hands[HAND_WEAPON].slots[1] == 33);
    check("its shield is the one put away", back.shield_away == 4);
    check("nothing else", back.hands[HAND_SPELL].slots[0] == 0 && back.hands[HAND_HOTKEY].slots[4] == 0);

    memset(&untouched, 0x55, sizeof(untouched));
    back = untouched;
    check("an old weapon out of range", !hands_parse("set2 weapon 70 shield 0", &back));
    check("a slot out of range", !hands_parse("hands 2 weapon 2 1 1 spell 0 0 0 hotkey 0 0 0 0 0 0 shield 0", &back));
    check("a spell out of range", !hands_parse("hands 2 weapon 0 1 1 spell 0 91 0 hotkey 0 0 0 0 0 0 shield 0", &back));
    check("a hotkey cut short", !hands_parse("hands 2 weapon 0 1 1 spell 0 0 0 hotkey 0 0 0", &back));
    check("no shield", !hands_parse("hands 2 weapon 0 1 1 spell 0 0 0 hotkey 0 0 0 0 0 0", &back));
    check("a later version", !hands_parse("hands 3 weapon 0 1 1", &back));
    check("nothing", !hands_parse("", &back));
    check("left alone when not read", memcmp(&back, &untouched, sizeof(back)) == 0);
}

static void putting(void) {
    Hand weapons = {0, {5, 9}};
    Hand hotkeys = {1, {HOTKEY_VALUE(HOTKEY_ITEM, 3), HOTKEY_VALUE(HOTKEY_ITEM, 7), 0, 0, 0}};

    check("put in hand: the game equips", hand_put(&weapons, HAND_WEAPON, 0, 12) == HAND_PUT_EQUIP);
    check("put in hand what the other slot holds: the game equips (they change places)",
          hand_put(&weapons, HAND_WEAPON, 0, 9) == HAND_PUT_EQUIP);
    check("put in the other slot: kept", hand_put(&weapons, HAND_WEAPON, 1, 12) == HAND_PUT_KEEP);
    check("put in the other slot what is in hand: refused", hand_put(&weapons, HAND_WEAPON, 1, 5) == HAND_PUT_ELSEWHERE);
    check("put where it is already: nothing", hand_put(&weapons, HAND_WEAPON, 1, 9) == HAND_PUT_NOTHING &&
                                                   hand_put(&weapons, HAND_WEAPON, 0, 5) == HAND_PUT_NOTHING);
    check("the bare hand empties the other slot", hand_put(&weapons, HAND_WEAPON, 1, 0) == HAND_PUT_KEEP);
    check("no third weapon slot", hand_put(&weapons, HAND_WEAPON, 2, 12) == HAND_PUT_NOTHING &&
                                      hand_put(&weapons, HAND_WEAPON, -1, 12) == HAND_PUT_NOTHING);
    check("a hotkey kept in an empty slot", hand_put(&hotkeys, HAND_HOTKEY, 4, HOTKEY_VALUE(HOTKEY_SPELL, 2)) == HAND_PUT_KEEP);
    check("a hotkey held by another slot: refused",
          hand_put(&hotkeys, HAND_HOTKEY, 3, HOTKEY_VALUE(HOTKEY_ITEM, 3)) == HAND_PUT_ELSEWHERE);
    check("emptying a hotkey while others are empty", hand_put(&hotkeys, HAND_HOTKEY, 0, 0) == HAND_PUT_KEEP);
}

int main(void) {
    slots();
    stepping();
    text();
    putting();
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
