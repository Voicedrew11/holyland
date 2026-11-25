/* hands.c: PC-only. The hands mod's game side (port/include/port/hands.h): two weapons in the right hand, two
   spells in the left, five hotkeys, swapped and stepped on keys in play and chosen on the equipment screen.

   The slot in hand is always the game's own: PlayerState.weapon, equipped_magic, and shortcut/shortcut_type for
   the hotkey in use. Hands_Poll copies them into the slots in hand each frame (so whatever the game or its menu
   equips is what the slot holds), and a swap writes the other slot's value into the game's field and does what
   the game would do had its menu equipped it.

   On the equipment screen (Menu_UpdateEquip) Cross on an item of the Weapon, Magic or Shortcut row opens the
   "Put in" box (game/hands_put.c), which puts it in a slot: the slot in hand through the game's own equip, the
   others kept here. ui/menu_screens.c (in patches/) calls Hands_BuildRings and Hands_Choose. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "core/game.h"
#include "effect/effect.h"
#include "input/pad.h"
#include "item/item.h"
#include "map/map.h"
#include "player/player.h"
#include "sound/sound.h"
#include "ui/menu.h"
#include "port/hands.h"
#include "port/platform.h"
#include "hands_cross.h"
#include "hands_put.h"

extern GameStatus game_status;
extern PlayerState player_state;
extern u8 item_counts[ITEM_COUNT];
extern LevelDef item_name_defs[ITEM_COUNT];
extern ItemCommonDef item_common_defs[];
extern LevelRecord weapon_level_records[70];
extern LevelRecord worn_item_level_records[160];
extern PlayerItemDef weapon_defs[];
extern FxTableEntry fx_table[];
extern u8 spell_levels[6][5];
extern MenuItem weapon_entries[70];
extern MenuItem spell_entries[91];
extern MenuItem shortcut_entries[241];
extern MenuItemList equip_weapon_list;
extern MenuItemList equip_spell_list;
extern MenuItemList equip_shortcut_list;
extern SoundLoadQueue sound_load_queue;
extern MapLoadState map_load_state;
extern int context1_buffer_half;
s32 Sound_PlayMenu(s32);
void Player_RecomputeStats(void);
void Player_LoadArmModels(void);
int File_IsLocked(void);
int Sound_BgmLoadCanYield(void);
int Message_IsBoxOpen(void);
const char *Quick_WhyNotInPlay(void); /* game/quicksave.c */

#define WEAPONS 70
#define SHIELDS 30
#define SPELLS 91
#define PRESS_FRAMES 60     /* a press waits this many vblanks at most for the files and sounds */
#define SWING_FRAMES 120    /* and this many for a swing to end, before a weapon swap */
#define POLL_GAP 6          /* vblanks without a poll (a menu, a load): the presses made then are dropped */

#define SOUND_CURSOR 1  /* a move */
#define SOUND_REFUSED 3 /* Cross where nothing can be done */


HandsFeedback hands_feedback; /* game/hands_cross.c draws it */

/* ---- What may be in a slot ------------------------------------------------------------------------- */

/* As Menu_BuildItemRings rings the items of a row: owned, usable and not with the smith */
static int weapon_owned(int weapon) {
    if (weapon == 0) {
        return 1;
    }
    return weapon > 0 && weapon < WEAPONS && item_counts[ITEM_WEAPON_FIRST + weapon] != 0 &&
           (item_name_defs[ITEM_WEAPON_FIRST + weapon].state & 1) && weapon_level_records[weapon].in_smith_rings == 0;
}

static int shield_owned(int shield) {
    if (shield == 0) {
        return 1;
    }
    return shield > 0 && shield < SHIELDS && item_counts[ITEM_WORN_FIRST + shield] != 0 &&
           (item_name_defs[ITEM_WORN_FIRST + shield].state & 1) && worn_item_level_records[shield].in_smith_rings == 0;
}

/* ui/menu_screens.c's SPELL_IS_SET: three spells to a slot of spell_levels, numbered from 1 */
static int spell_learned(int spell) {
    return spell >= 1 && spell < SPELLS && spell_levels[(spell - 1) / 15][((spell - 1) / 3) % 5] == (spell - 1) % 3 + 1;
}

/* The Magic row's spells (fx_table's item_menu_magic 0: cast in play), the Shortcut row's the others */
static int battle_spell(int spell) {
    return spell_learned(spell) && fx_table[spell].item_menu_magic == 0;
}

static int menu_spell(int spell) {
    return spell_learned(spell) && fx_table[spell].item_menu_magic == 1;
}

/* the Shortcut row's items: owned, usable, not the ones it leaves out */
static int shortcut_item(int item) {
    return item > 0 && item < 150 && item_counts[item] != 0 && (item_name_defs[item].state & 1) &&
           !((u8)item_common_defs[item].flags & 1) && !(item >= 145 && item < 148);
}

/* A two-handed weapon (PlayerItemDef.action_handler_index 1) or a bow (2) leaves no room for a shield */
static int one_handed(int weapon) {
    return weapon_defs[weapon].action_handler_index == 0;
}

/* ---- The three hands, one entry each ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    MenuItemList *row;                         /* its row of the equipment screen */
    int (*held)(void);                         /* the value the game holds */
    int (*kept)(int value);                    /* may stay in a slot (owned, learned) */
    int (*usable)(int value);                  /* may be swapped to: for the hands, empty too (the bare hand, no
                                                  spell); for the hotkeys, not empty */
    MenuItem *(*entry)(int value);             /* its item of the row's ring of entries */
    int (*value_of)(const MenuItem *item);     /* the value of an item of the row */
    void (*put_in_hand)(int value);            /* the game holds it, as its menu would have equipped it */
} HandRules;

static int weapon_held(void) {
    return player_state.weapon;
}


static MenuItem *weapon_entry(int weapon) {
    return &weapon_entries[weapon];
}

static int weapon_value(const MenuItem *item) {
    return item->item_number - ITEM_WEAPON_FIRST;
}

static void cap_hp_mp(void) {
    if (player_state.hp_max < player_state.hp_now) {
        player_state.hp_now = player_state.hp_max;
    }
    if (player_state.mp_now > player_state.mp_max) {
        player_state.mp_now = player_state.mp_max;
    }
}

/* The menu's closing (GameLoop_OpenRequestedMenu): a weapon with magic of its own charges it (not a bow) */
static void charge_weapon_magic(int weapon) {
    PlayerItemDef *def = &weapon_defs[weapon];

    if (def->action_handler_index == 2) {
        return;
    }
    if (def->first_magic_id != 0xFFFF) {
        player_state.magic_gauge = 0;
        player_state.charging_magic = def->first_magic_id;
    } else if (def->second_magic_id != 0xFFFF && weapon_level_records[weapon].level_index + 1 == 3) {
        player_state.magic_gauge = 0;
        player_state.charging_magic = def->second_magic_id;
    }
}

/* The equipment screen's Cross on a weapon (Menu_UpdateEquip), the shield put away or back on, and the menu's
   closing (GameLoop_OpenRequestedMenu): the arm models, the strength gauge emptied, the weapon's magic
   charged */
static void weapon_put_in_hand(int weapon) {
    int old = player_state.weapon;
    int *away = &port_hands_state.shield_away;

    player_state.weapon = weapon;
    if (!one_handed(weapon) && player_state.equipment[EQUIP_SHIELD] != ITEM_WORN_INDEX(ITEM_SHIELD_NONE)) {
        *away = player_state.equipment[EQUIP_SHIELD];
        player_state.equipment[EQUIP_SHIELD] = ITEM_WORN_INDEX(ITEM_SHIELD_NONE);
    } else if (one_handed(weapon) && player_state.equipment[EQUIP_SHIELD] == ITEM_WORN_INDEX(ITEM_SHIELD_NONE) &&
               *away != 0 && shield_owned(*away)) {
        player_state.equipment[EQUIP_SHIELD] = *away;
        *away = 0;
    }
    Player_RecomputeStats();
    cap_hp_mp();
    Player_LoadArmModels();
    if (weapon == old) {
        return;
    }
    player_state.power_gauge = 0;
    charge_weapon_magic(weapon);
}

static int spell_held(void) {
    return player_state.equipped_magic;
}

static int spell_kept(int spell) {
    return spell == 0 || battle_spell(spell);
}


static MenuItem *spell_entry(int spell) {
    return &spell_entries[spell];
}

static int spell_value(const MenuItem *item) {
    return item->item_number;
}

/* the menu's closing (GameLoop_OpenRequestedMenu): the new spell charges from empty. The left hand emptied
   charges what charges with no spell: the hotkey's spell, else the weapon's own magic. */
static void spell_put_in_hand(int spell) {
    player_state.equipped_magic = spell;
    if (spell != 0) {
        player_state.charging_magic = spell;
        player_state.magic_gauge = 0;
        return;
    }
    if (player_state.shortcut_type == HOTKEY_SPELL) {
        player_state.magic_gauge = 0;
        player_state.charging_magic = player_state.shortcut;
        return;
    }
    charge_weapon_magic(player_state.weapon);
}

static int hotkey_held(void) {
    if (player_state.shortcut_type == HOTKEY_SPELL) {
        return HOTKEY_VALUE(HOTKEY_SPELL, player_state.shortcut);
    }
    if (player_state.shortcut_type == HOTKEY_ITEM && player_state.shortcut != 0) {
        return HOTKEY_VALUE(HOTKEY_ITEM, player_state.shortcut);
    }
    return 0;
}

/* an item used up stays, to come back when more are found; a spell stays once learned */
static int hotkey_kept(int value) {
    if (HOTKEY_TYPE(value) == HOTKEY_SPELL) {
        return menu_spell(HOTKEY_NUMBER(value));
    }
    return 1;
}

static int hotkey_usable(int value) {
    if (HOTKEY_TYPE(value) == HOTKEY_SPELL) {
        return menu_spell(HOTKEY_NUMBER(value));
    }
    return HOTKEY_TYPE(value) == HOTKEY_ITEM && shortcut_item(HOTKEY_NUMBER(value));
}

static MenuItem *hotkey_entry(int value) {
    if (HOTKEY_TYPE(value) == HOTKEY_SPELL) {
        return &shortcut_entries[150 + HOTKEY_NUMBER(value)];
    }
    return &shortcut_entries[HOTKEY_NUMBER(value)];
}

/* the Shortcut row's entries: items are group 11, spells group 1 (Menu_Init) */
static int hotkey_value(const MenuItem *item) {
    if (item->group == 1) {
        return HOTKEY_VALUE(HOTKEY_SPELL, item->item_number);
    }
    return item->item_number == 0 ? 0 : HOTKEY_VALUE(HOTKEY_ITEM, item->item_number);
}

/* the menu's closing: a spell of the shortcut charges when no spell is in the left hand */
static void hotkey_put_in_hand(int value) {
    player_state.shortcut_type = HOTKEY_TYPE(value);
    player_state.shortcut = HOTKEY_NUMBER(value);
    if (HOTKEY_TYPE(value) == HOTKEY_SPELL && player_state.equipped_magic == 0) {
        player_state.magic_gauge = 0;
        player_state.charging_magic = player_state.shortcut;
    }
}

static const HandRules rules[HAND_COUNT] = {
    [HAND_WEAPON] = {"weapon", &equip_weapon_list, weapon_held, weapon_owned, weapon_owned, weapon_entry, weapon_value,
                     weapon_put_in_hand},
    [HAND_SPELL] = {"spell", &equip_spell_list, spell_held, spell_kept, spell_kept, spell_entry, spell_value,
                    spell_put_in_hand},
    [HAND_HOTKEY] = {"hotkey", &equip_shortcut_list, hotkey_held, hotkey_kept, hotkey_usable, hotkey_entry, hotkey_value,
                     hotkey_put_in_hand},
};

static int usable_in(HandKind kind, int value, const void *context) {
    (void)context;
    return rules[kind].usable(value);
}

/* ---- The slots in hand follow the game ----------------------------------------------------------------- */

/* The slots in hand take what the game holds. Something held that was in another slot changes places with what
   the slot in hand held (equipping the other weapon on the equipment screen swaps the two); slots whose item is
   no longer owned are emptied. */
static void follow_game(void) {
    HandsState *state = &port_hands_state;
    int changed = 0;
    int kind;
    int slot;

    for (kind = 0; kind < HAND_COUNT; kind++) {
        Hand *hand = &state->hands[kind];
        int held = rules[kind].held();
        int old = hand->slots[hand->out];

        if (held != old) {
            for (slot = 0; slot < hand_slot_count(kind); slot++) {
                if (slot != hand->out && held != 0 && hand->slots[slot] == held) {
                    hand->slots[slot] = old;
                }
            }
            hand->slots[hand->out] = held;
            changed = 1;
        }
        for (slot = 0; slot < hand_slot_count(kind); slot++) {
            if (slot != hand->out && hand->slots[slot] != 0 && !rules[kind].kept(hand->slots[slot])) {
                port_event("hands: %s %d of slot %d is no longer owned, dropped", rules[kind].name, hand->slots[slot],
                           slot + 1);
                hand->slots[slot] = 0;
                changed = 1;
            }
        }
    }
    if (state->shield_away != 0 && !shield_owned(state->shield_away)) {
        state->shield_away = 0;
        changed = 1;
    }
    if (changed) {
        port_hands_changed();
    }
}

/* ---- The swaps --------------------------------------------------------------------------------------- */

/* Why the game would not take a press now (NULL: it would): the menu button's conditions (Player_Update,
   GameLoop_OpenRequestedMenu) */
static const char *refused(void) {
    if (game_status.bit9) {
        return "the game is leaving play";
    }
    if (game_status.player_control == 0 || (game_status.update_hold_flags & 1)) {
        return "the player is not in control";
    }
    if (player_state.death_timer != 0) {
        return "the player is dying";
    }
    if (game_status.paused || Message_IsBoxOpen()) {
        return "the game is paused or a message is open";
    }
    if (game_status.bit6 || game_status.item_use_cooldown > 0) {
        return "an item is being used";
    }
    if (game_status.bit2 || game_status.bit3 || game_status.bit4 || game_status.save_requested || game_status.bit8 ||
        game_status.bit14) {
        return "a menu is opening";
    }
    return NULL;
}

/* The weapon is out (mid-swing, a bow drawn): Player_LoadArmModels would cut it short, so a weapon swap waits */
static int weapon_out(void) {
    return player_state.item_action_step != 0 || player_state.frames_since_item_action < 2;
}

/* GameLoop_OpenRequestedMenu's waits before a menu: 1 while the arm models could not be loaded now */
static int files_busy(void) {
    if (context1_buffer_half == 0) {
        return 1;
    }
    if (sound_load_queue.map_bank_load_pending != 0 || sound_load_queue.entity_bank_load_pending != 0) {
        return 1;
    }
    if (sound_load_queue.bgm_load_pending != 0 && Sound_BgmLoadCanYield() == 0) {
        return 1;
    }
    /* the menu holds a map load at steps 2 to 4 (MapLoad_RequestHold); the swap waits for it instead */
    if (map_load_state.step >= 2 && map_load_state.step <= 4) {
        return 1;
    }
    return File_IsLocked() != 0;
}

static HandKind kind_of_key(HandsKey key) {
    switch (key) {
    case HANDS_KEY_WEAPON:
        return HAND_WEAPON;
    case HANDS_KEY_SPELL:
        return HAND_SPELL;
    default:
        return HAND_HOTKEY;
    }
}

static void refuse(HandKind kind, const char *why) {
    fprintf(stderr, "[hands] vblank %llu: %s refused: %s\n", (unsigned long long)port_vblank_count(), rules[kind].name, why);
    hands_feedback.refused_vblank[kind] = port_vblank_count();
    hands_feedback.pressed_vblank = port_vblank_count();
    Sound_PlayMenu(SOUND_REFUSED);
}

static void change(HandKind kind, int slot) {
    Hand *hand = &port_hands_state.hands[kind];
    int value = hand->slots[slot];

    hand->out = slot;
    rules[kind].put_in_hand(value);
    port_hands_changed();
    hands_feedback.changed_vblank[kind] = port_vblank_count();
    hands_feedback.pressed_vblank = port_vblank_count();
    Sound_PlayMenu(SOUND_CURSOR);
    fprintf(stderr,
            "[hands] vblank %llu: %s slot %d: %d (weapon %d, %s; shield %d, away %d; magic %d; shortcut %d:%d; charging %d, "
            "magic gauge %d, power gauge %d)\n",
            (unsigned long long)port_vblank_count(), rules[kind].name, slot + 1, value, player_state.weapon,
            one_handed(player_state.weapon) ? "one-handed" : "two-handed or a bow", player_state.equipment[EQUIP_SHIELD],
            port_hands_state.shield_away, player_state.equipped_magic, player_state.shortcut_type, player_state.shortcut,
            player_state.charging_magic, player_state.magic_gauge, player_state.power_gauge);
}

/* The key's press, waiting since `pressed`: 1 when it is done with (taken or refused), 0 to wait */
static int take(HandsKey key, unsigned long long pressed) {
    HandKind kind = kind_of_key(key);
    unsigned long long waited = port_vblank_count() + 1 - pressed;
    const char *why = refused();
    int slot;

    if (why != NULL) {
        refuse(kind, why);
        return 1;
    }
    if (kind == HAND_WEAPON && weapon_out()) {
        if (waited <= SWING_FRAMES) {
            return 0;
        }
        refuse(kind, "the weapon stayed out");
        return 1;
    }
    if (kind == HAND_WEAPON && files_busy()) {
        if (waited <= PRESS_FRAMES) {
            return 0;
        }
        refuse(kind, "the files or sounds were busy too long");
        return 1;
    }
    slot = hand_next_slot(&port_hands_state.hands[kind], kind, key == HANDS_KEY_HOTKEY_PREVIOUS ? -1 : 1, usable_in, NULL);
    if (slot < 0 || port_hands_state.hands[kind].slots[slot] == rules[kind].held()) {
        refuse(kind, "no other slot holds anything else");
        return 1;
    }
    change(kind, slot);
    return 1;
}

/* KF4_HANDS_PRESS_AT=V:K,V:K,...: key K (w weapon, s spell, n next hotkey, p previous) pressed at vblank V, for
   tests, which play a pad script; the weapon sets mod's KF4_WEAPON_SETS_SWAP_AT=V,V,... presses the weapon key */
static void test_presses(void) {
    static const char *next;
    static int read, old;
    unsigned long long at;
    HandsKey key = HANDS_KEY_WEAPON;
    char *end;

    if (!read) {
        read = 1;
        next = getenv("KF4_HANDS_PRESS_AT");
        if (next == NULL) {
            next = getenv("KF4_WEAPON_SETS_SWAP_AT");
            old = 1;
        }
    }
    if (next == NULL || *next == 0) {
        return;
    }
    at = strtoull(next, &end, 0);
    if (port_vblank_count() < at) {
        return;
    }
    if (!old && *end == ':') {
        key = end[1] == 's' ? HANDS_KEY_SPELL : end[1] == 'n' ? HANDS_KEY_HOTKEY_NEXT
                              : end[1] == 'p' ? HANDS_KEY_HOTKEY_PREVIOUS : HANDS_KEY_WEAPON;
        end += end[1] != 0 ? 2 : 1;
    }
    next = *end == ',' ? end + 1 : NULL;
    fprintf(stderr, "[hands] vblank %llu: key %d (test)\n", (unsigned long long)port_vblank_count(), (int)key);
    port_hands_key(key);
}

/* Once a game frame, from Quick_PollGame */
void Hands_Poll(void) {
    static unsigned long long last_poll;
    unsigned long long now = port_vblank_count();
    int gap = last_poll != 0 && now - last_poll > POLL_GAP;
    int key;

    last_poll = now;
    if (Quick_WhyNotInPlay() == NULL && port_hands_new_game() && port_hands.enabled) {
        memset(&port_hands_state, 0, sizeof(port_hands_state));
        port_hands_changed();
        port_event("hands: a new game, the hands empty");
    }
    if (!port_hands.enabled) {
        for (key = 0; key < HANDS_KEY_COUNT; key++) {
            port_hands_take_press(key);
        }
        return;
    }
    follow_game();
    test_presses();
    for (key = 0; key < HANDS_KEY_COUNT; key++) {
        unsigned long long pressed = port_hands_pressed(key);

        if (pressed == 0) {
            continue;
        }
        /* pressed while play was not polled (a menu, a load, the title): not meant for play */
        if (gap && pressed <= now - POLL_GAP) {
            port_hands_take_press(key);
            continue;
        }
        if (take(key, pressed)) {
            port_hands_take_press(key);
        }
    }
}

/* ---- The equipment screen (ui/menu_screens.c, and the "Put in" box, game/hands_put.c) ------------------------ */
static int kind_of_row(const MenuItemList *row) {
    int kind;

    for (kind = 0; kind < HAND_COUNT; kind++) {
        if (rules[kind].row == row) {
            return kind;
        }
    }
    return -1;
}

/* Menu_BuildItemRings, as it ends (the menu opening, and the equipment screen closing) */
void Hands_BuildRings(void) {
    HandsPut_Close();
    if (port_hands.enabled) {
        follow_game();
    }
}

void Hands_FollowGame(void) {
    follow_game();
}

/* As Menu_UpdateEquip names the item shown: a spell by fx_table, the rest by item_name_defs */
const u16 *Hands_ValueName(HandKind kind, int value) {
    const MenuItem *item;

    if (kind < 0 || kind >= HAND_COUNT || value == 0 || !hand_value_valid(kind, value)) {
        return NULL;
    }
    item = rules[kind].entry(value);
    if (item->group == 1) {
        return (const u16 *)&fx_table[item->item_number];
    }
    return (const u16 *)&item_name_defs[item->item_number];
}

/* The screen's Cross on an item: on the Weapon, Magic and Shortcut rows the "Put in" box opens (1), unless the
   box itself let this Cross through to equip (0, as on the rows without slots: the game equips) */
int Hands_Choose(MenuItemList *list, MenuItem *item) {
    int kind = port_hands.enabled ? kind_of_row(list) : -1;

    if (HandsPut_TakeEquip() || kind < 0) {
        return 0;
    }
    HandsPut_Open(kind, rules[kind].value_of(item));
    return 1;
}
