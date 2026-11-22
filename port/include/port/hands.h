/* The hands mod: King's Field-style hands and hotkeys, from Verdite2's "Shortcuts" (the KF2 port,
   patches/shortcuts/). It replaces the weapon sets mod and reads its files.

   - The right hand holds one of two weapons, the left hand one of two spells (the equipped magic, cast with
     the magic button), and five hotkeys hold an item or a spell each; the hotkey in use is the game's own
     shortcut (PlayerState.shortcut, used with Select).
   - One press swaps the weapon (1, R; the d-pad's Right in play), one the spell (2; d-pad Left), one steps to
     the next hotkey that holds something (3; d-pad Down, Up the previous). Rebindable.
   - The shield is worn, not used, so it has no slot: a two-handed weapon or a bow puts it away, and the
     swap back to a one-handed weapon puts it on again.
   - On the equipment screen Cross on an item of the Weapon, Magic or Shortcut row opens a "Put in" box of
     the row's slots (Weapon 1 and 2, Magic 1 and 2, Shortcut 1 to 5), drawn with the menus' own text: Cross
     on the slot in hand equips as the game does, on another slot it keeps the item there.
   - In play a cross of three cards at the bottom left shows the spell, the weapon and the hotkey, each with
     the item's own menu model.

   Off by default, like every mod (the player menu's Gameplay tab, settings.json's "mods" "hands", --hands); with
   it off the game runs exactly as without it.

   Three parts (its switches are rows of the player menu's Gameplay tab):
   - port/platform/hands_rules.c: the plain rules (slots, stepping, the file's text), tested by
     tests/hands_test.c;
   - port/platform/hands.c: the switch, the keys, and the hands kept per save (a live file, a copy per quick
     save and per game save, as the automap keeps its explored maps);
   - game/hands.c, the game's side: the swaps (once a game frame from Quick_PollGame, where the game itself
     would let a menu open), with game/hands_put.c the equipment screen's "Put in" box (patched into
     ui/menu_screens.c), and game/hands_cross.c the cross and the hotkey's count (drawn after Hud_Draw).
   Everything runs on the game's thread. */
#ifndef PORT_HANDS_H
#define PORT_HANDS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The hands (port/platform/hands_rules.c) ------------------------------------------------------- */
typedef enum {
    HAND_WEAPON, /* a value: PlayerState.weapon's number (the item less ITEM_WEAPON_FIRST); 0, the bare hand, is empty */
    HAND_SPELL,  /* PlayerState.equipped_magic's number (fx_table's row); 0 is none */
    HAND_HOTKEY, /* HOTKEY_VALUE(PlayerState.shortcut_type, PlayerState.shortcut); 0 is none */
    HAND_COUNT
} HandKind;

#define HAND_SLOTS_MAX 5
#define HOTKEY_ITEM 1 /* PlayerState.shortcut_type */
#define HOTKEY_SPELL 2
#define HOTKEY_VALUE(type, number) (((type) << 12) | (number))
#define HOTKEY_TYPE(value) ((value) >> 12)
#define HOTKEY_NUMBER(value) ((value) & 0xFFF)

typedef struct {
    int out;                    /* the slot in hand (in use, for the hotkeys) */
    int slots[HAND_SLOTS_MAX];  /* each slot's value; slots[out] is what the game holds */
} Hand;

typedef struct {
    Hand hands[HAND_COUNT];
    int shield_away; /* the shield a two-handed weapon put away (equipment[EQUIP_SHIELD]'s number, 0 none) */
} HandsState;

int hand_slot_count(HandKind kind); /* 2 weapons, 2 spells, 5 hotkeys */
int hand_value_valid(HandKind kind, int value); /* in range for the game's tables (files are read from outside) */

/* The next slot from `hand->out`, `step` (+1 or -1) at a time, round, whose value `usable` takes; -1 when no
   other slot is. */
int hand_next_slot(const Hand *hand, HandKind kind, int step, int (*usable)(HandKind kind, int value, const void *context),
                   const void *context);

/* What putting `value` in `slot` (the equipment screen's "Put in" box, game/hands_put.c) does */
typedef enum {
    HAND_PUT_EQUIP,     /* the slot in hand: the game equips it (an item from another slot changes places) */
    HAND_PUT_KEEP,      /* another slot: kept there */
    HAND_PUT_ELSEWHERE, /* another slot, but another slot holds it already: refused */
    HAND_PUT_NOTHING    /* the slot holds it already, or there is no such slot */
} HandPut;
HandPut hand_put(const Hand *hand, HandKind kind, int slot, int value);

/* The hands as one line of text, and back. hands_parse also reads the weapon sets mod's "set2 weapon W shield S"
   (its weapon becomes weapon 2, its shield the one put away). 0 when the text is neither; `out` is then left
   alone. */
size_t hands_format(const HandsState *state, char *out, size_t size);
int hands_parse(const char *text, HandsState *out);

/* ---- The switch (port/platform/hands.c) --------------------------------------------------------------- */
/* the corner of the screen the cross is in */
typedef enum {
    PORT_HANDS_BOTTOM_LEFT, /* the default */
    PORT_HANDS_BOTTOM_RIGHT,
    PORT_HANDS_TOP_RIGHT,
    PORT_HANDS_TOP_LEFT, /* under the HUD */
    PORT_HANDS_CORNER_COUNT
} PortHandsCorner;

/* how the three cards are set out */
typedef enum {
    PORT_HANDS_LAYOUT_CROSS, /* the hotkey's card, the middle one, lower (the default) */
    PORT_HANDS_LAYOUT_ROW,   /* in line */
    PORT_HANDS_LAYOUT_COUNT
} PortHandsLayout;

/* the cards' shape */
typedef enum {
    PORT_HANDS_CARDS_TALL, /* the default */
    PORT_HANDS_CARDS_SQUARE,
    PORT_HANDS_CARDS_COUNT
} PortHandsCards;

#define PORT_HANDS_SCALE_MIN 0.4f
#define PORT_HANDS_SCALE_MAX 1.0f

typedef struct {
    int enabled;            /* off by default */
    int forced;             /* --hands: on for this run whatever settings.json says */
    int fade;               /* the cross fades after a while with nothing pressed (on by default) */
    float scale;            /* the cross's size, PORT_HANDS_SCALE_MIN to _MAX (1, the default: full) */
    PortHandsCorner corner; /* where it is */
    PortHandsLayout layout;
    PortHandsCards cards;
} PortHandsOptions;
extern PortHandsOptions port_hands;

int port_hands_option(int argc, char **argv, int *i); /* --hands (and the old --weapon-sets) */
void port_hands_settings_read(const void *mods);       /* settings.json's "mods" (a cJSON *) */
void port_hands_settings_write(void *mods);
void port_hands_set_enabled(int on); /* the player menu's Gameplay tab */
void port_hands_set_fade(int on);
void port_hands_set_scale(float scale); /* clamped to PORT_HANDS_SCALE_MIN to _MAX */
void port_hands_set_corner(PortHandsCorner corner);
void port_hands_set_layout(PortHandsLayout layout);
void port_hands_set_cards(PortHandsCards cards);
/* settings.json's names, also shown */
extern const char *const port_hands_corner_names[PORT_HANDS_CORNER_COUNT];
extern const char *const port_hands_layout_names[PORT_HANDS_LAYOUT_COUNT];
extern const char *const port_hands_cards_names[PORT_HANDS_CARDS_COUNT];

/* ---- The hands kept (port/platform/hands.c keeps them, game/hands.c changes them) --------------------- */
extern HandsState port_hands_state;
void port_hands_changed(void); /* written to the live file */

/* ---- The keys (port/sdk/controls.c presses, game/hands.c takes) ------------------------------------------- */
typedef enum {
    HANDS_KEY_WEAPON,
    HANDS_KEY_SPELL,
    HANDS_KEY_HOTKEY_NEXT,
    HANDS_KEY_HOTKEY_PREVIOUS,
    HANDS_KEY_COUNT
} HandsKey;

void port_hands_key(HandsKey key);
unsigned long long port_hands_pressed(HandsKey key); /* the vblank of the press waiting to be taken (0: none) */
void port_hands_take_press(HandsKey key);
/* the keys' names in pad scripts' event lines (port/sdk/libpad.c): "weapon_swap", "spell_swap", "hotkey_next",
   "hotkey_previous"; port_hands_event_key gives -1 for any other name */
const char *port_hands_event_name(HandsKey key);
int port_hands_event_key(const char *name);

/* ---- Per save (port/platform/hands.c) ----------------------------------------------------------------------- */
void port_hands_save_file(int number, int writing); /* game/saves.c: save `number` (1 to 3) written or loaded */
void port_hands_quick_saved(const char *path);       /* quicksave.c: a quick save was written */
void port_hands_quick_loading(const char *path);     /* quicksave.c: one is being loaded */
void port_hands_title(void);                         /* (game) the title screen: the next game may be a new one */
int port_hands_new_game(void);                       /* (game, the first frame in play after the title) 1: a new game */

#ifdef __cplusplus
}
#endif

#endif
