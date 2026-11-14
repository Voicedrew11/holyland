/* The weapon sets mod (docs/port/WEAPON_SETS.md): a second weapon and shield kept beside the ones in hand,
   swapped with them on one key (R, a controller's right stick press; rebindable), through the game's own
   equipping, and chosen on the equipment screen as two more rows, "Weapon 2" and "Shield 2", above the
   weapon's. Off by default, like every mod (the Gameplay page, settings.json's "mods" "weapon_sets",
   --weapon-sets); with it off the game runs exactly as without it.

   Three parts:
   - game/weapon_sets.c, the game's side: the two rows of the equipment screen (patched into
     ui/menu_screens.c), and the swap, taken once a game frame from Quick_PollGame where the game itself
     would let a menu open, done as the equipment screen and the menu's closing do it;
   - port/platform/weapon_sets.c: the switch, the command line, the key's press, and set 2 kept per save
     (as the automap keeps its explored maps: a live file, a copy per quick save and per memory card save);
   - port/overlay/weapon_sets_view.cpp: the rows' "2", set 2's names on the equipment screen and the swap's
     message, drawn in the menus' style (Dear ImGui), and the Gameplay page's section.
   Everything runs on the game's thread (the overlay draws at the vblank). */
#ifndef PORT_WEAPON_SETS_H
#define PORT_WEAPON_SETS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The switch (port/platform/weapon_sets.c) -------------------------------------------------- */
typedef struct {
    int enabled; /* the mod: off by default */
    int forced;  /* --weapon-sets: on for this run whatever settings.json says */
} PortWeaponSetsOptions;
extern PortWeaponSetsOptions port_weapon_sets;

int port_weapon_sets_option(int argc, char **argv, int *i); /* --weapon-sets */
void port_weapon_sets_settings_read(const void *mods);       /* settings.json's "mods" (a cJSON *) */
void port_weapon_sets_settings_write(void *mods);
void port_weapon_sets_set_enabled(int on);                   /* the Gameplay page */

/* ---- Set 2 (port/platform/weapon_sets.c keeps it, game/weapon_sets.c uses it) ------------------- */
typedef struct {
    int weapon; /* PlayerState.weapon's number: the item less ITEM_WEAPON_FIRST, 0 the bare hand */
    int shield; /* PlayerState.equipment[EQUIP_SHIELD]'s: the item less ITEM_WORN_FIRST, 0 none */
} PortWeaponSet;
extern PortWeaponSet port_weapon_set2;
void port_weapon_sets_changed(void); /* set 2 changed: the live file is written */

/* ---- The swap key ------------------------------------------------------------------------------ */
void port_weapon_sets_key(void); /* port/sdk/controls.c: the key was pressed (ACTION_WEAPON_SWAP) */
/* the vblank of the press waiting to be taken (0: none); the game's side takes or drops it */
unsigned long long port_weapon_sets_pressed(void);
void port_weapon_sets_take_press(void);

/* ---- Per save (port/platform/weapon_sets.c) ----------------------------------------------------- */
void port_weapon_sets_card_file(const char *name, int writing); /* libmc: the game opened a card file */
void port_weapon_sets_quick_saved(const char *path);             /* quicksave.c: a quick save was written */
void port_weapon_sets_quick_loading(const char *path);           /* quicksave.c: one is being loaded */
void port_weapon_sets_title(void);  /* (game) the title screen: the next game may be a new one */
int port_weapon_sets_new_game(void); /* (game, the first frame in play after the title) 1: a new game */

/* ---- What the overlay draws (game/weapon_sets.c fills it, port/overlay/weapon_sets_view.cpp draws) - */
#define PORT_WEAPON_SETS_NAME 48
typedef struct {
    unsigned long long vblank; /* the last vblank the equipment screen ran (0: never) */
    int row;                   /* the row shown: 0 one of the game's, 1 Weapon 2, 2 Shield 2 */
    int group;                 /* the row's kind of item (MenuItem.group): 0 weapons, 2 shields, ... */
    float alpha;               /* the screen's text fade, 0 to 1 */
    float label_half_width;    /* half the width of the row's name ("Weapon"), in the menus' 640 x 448 */
    int covered;               /* the game's detail panel (Triangle) or list (Square) is up */
    int empty;                 /* set 2 is the bare hand and no shield */
    char weapon[PORT_WEAPON_SETS_NAME]; /* set 2's names, the game's own */
    char shield[PORT_WEAPON_SETS_NAME];
} PortWeaponSetsMenu;
extern PortWeaponSetsMenu port_weapon_sets_menu;

typedef struct {
    unsigned long long vblank; /* when the last swap was made (0: none) */
    char text[2 * PORT_WEAPON_SETS_NAME + 8]; /* what is in hand now: "Hunting Bow" or "Fiery Sword, Large Shield" */
} PortWeaponSetsSwap;
extern PortWeaponSetsSwap port_weapon_sets_swap;

/* the overlay's side (port/overlay/weapon_sets_view.cpp, called by port/overlay/overlay.cpp) */
int weapon_sets_view_visible(void);
void weapon_sets_view_draw(int x, int y, int width, int height);
void weapon_sets_view_mods_section(void);

#ifdef __cplusplus
}
#endif

#endif
