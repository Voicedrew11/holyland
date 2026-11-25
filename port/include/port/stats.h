/* The extended stats mod (docs/port/EXTENDED_STATS.md): what the game keeps about the player and never
   shows, read from the game's own state, and the real numbers of the hits dealt and taken.

   It only reads: it changes nothing in the game, on or off. Off by default; settings.json's "mods"
   ("extended_stats" and its parts), --extended-stats, or the player menu's Interface tab turn it on.

   - game/stats.c (built with the game code) reads the game's state into a PortStatsSnapshot and
     watches the hits (port_stats_track, once a vblank, on the game's thread);
   - port/overlay/stats_mod.c keeps the switches and their settings.json keys;
   - port/overlay/stats_panel.cpp draws the lines it adds to the game's own menus, the panel and the hit
     log with Dear ImGui (its switches are rows of the player menu's Interface tab);
     port/overlay/stats_timers.cpp the ailments and timed effects running, by the minimap;
   - game/stats_bar.c draws the health bar of the creature last hit with the game's own HUD pictures, after
     the HUD (Hud_Draw), where the player chose (PortStatsBarPlace). */
#ifndef PORT_STATS_H
#define PORT_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The switches (port/overlay/stats_mod.c) ------------------------------------------------- */
/* where the health bar of the creature last hit is ("extended_stats_bar_place") */
typedef enum {
    PORT_STATS_BAR_TOP,          /* the top middle */
    PORT_STATS_BAR_TOP_LEFT,     /* under the HUD */
    PORT_STATS_BAR_TOP_RIGHT,
    PORT_STATS_BAR_BOTTOM,       /* the bottom middle */
    PORT_STATS_BAR_PLACE_COUNT
} PortStatsBarPlace;
/* their names in settings.json, by PortStatsBarPlace */
extern const char *const port_stats_bar_place_names[PORT_STATS_BAR_PLACE_COUNT];

typedef struct {
    int on;            /* the mod as a whole ("extended_stats") */
    int forced;        /* --extended-stats: on for this run whatever settings.json says */
    int menus;         /* its lines in the game's own menus ("extended_stats_menus") */
    int panel;         /* the stats panel, on its key ("extended_stats_panel") */
    int damage;        /* the hit log ("extended_stats_damage") */
    int durability;    /* the equipment's wear in the panel ("extended_stats_durability") */
    int timers;        /* the ailments and timed effects running, on screen by the minimap
                          ("extended_stats_timers") */
    int bar;           /* the health bar of the creature last hit ("extended_stats_bar") */
    PortStatsBarPlace bar_place; /* where it is */
} PortStatsOptions;
extern PortStatsOptions port_stats_options;

/* The Interface tab's switch: on for this run and in settings.json */
void port_stats_set_on(int on);
/* The panel's key (ACTION_STATS_PANEL, F6) shows or hides it */
void port_stats_toggle_panel(void);
int port_stats_panel_open(void);
void port_stats_set_panel_open(int open);
/* settings.json's "mods" object (a cJSON *): read into the switches, and written back */
void port_stats_settings_read(const void *mods);
void port_stats_settings_write(void *mods);

/* ---- The overlay's side (port/overlay/stats_panel.cpp, called by port/overlay/overlay.cpp) ---- */
void port_stats_overlay_update(void);            /* once a vblank, the game's thread: tracks and reads */
int port_stats_overlay_wants(void);              /* something of it is drawn this vblank (any thread) */
/* draws it, inside the overlay's ImGui frame; x, y, width, height: the game's picture in the window */
void port_stats_overlay_draw(int x, int y, int width, int height);
void port_stats_overlay_fonts(void);             /* adds its font to the overlay's, once at start */

/* ---- The game's side (game/stats.c) ---------------------------------------------------------- */
#define PORT_STATS_NAME 48
#define PORT_STATS_SLOTS 9     /* the weapon, then the eight worn slots (shield ... amulet) */
#define PORT_STATS_SPELLS 30   /* six schools of five */
#define PORT_STATS_EFFECTS 24
#define PORT_STATS_TIMERS 16
#define PORT_STATS_GAME_FPS 30 /* game frames a second: every timer below counts them */

typedef struct {
    int item;                  /* item number, 0: nothing there */
    char name[PORT_STATS_NAME];
    int none;                  /* the slot's "None" */
    int wears;                 /* it wears (Attack_WearWeapon, Attack_WearArmorFromHit) */
    int durability;            /* LevelRecord.level: the percent its values count at, 50 to 100 */
    int wear;                  /* LevelRecord.exp: wear toward the next point lost */
    int wear_per_point;        /* LevelDef.exp_per_level */
    int weapon_level;          /* weapons: LevelRecord.level_index + 1 (1 to 3) */
    int weapon_uses;           /* weapons: LevelRecord.use_count (counts to 480) */
} PortStatsItem;

typedef struct {
    char name[PORT_STATS_NAME];
    int level;                 /* spell_levels: 1 to 3 */
    int experience;            /* spell_experience: 50 makes level 2, 150 level 3 */
} PortStatsSpell;

typedef struct {
    char text[96];             /* what it does, with the numbers */
    char item[PORT_STATS_NAME]; /* the item (from the game's own names), or "" */
} PortStatsEffect;

typedef struct {
    char text[64];
    char brief[32];            /* the same, short (the timers on screen) */
    int frames;                /* game frames left */
} PortStatsTimer;

/* The screens of the game's menu (Menu_Run(0)) that the mod adds lines to */
#define PORT_STATS_MENU_NONE 0
#define PORT_STATS_MENU_MAIN 1     /* the main menu: Exp ... Condition on the right */
#define PORT_STATS_MENU_STATUS 2   /* Status */
#define PORT_STATS_MENU_EQUIP 3    /* Equipment */
#define PORT_STATS_MENU_ITEMS 4    /* Item (use one) */
#define PORT_STATS_MENU_ARRANGE 5  /* Arrange (everything owned) */
#define PORT_STATS_MENU_LINES 6
#define PORT_STATS_MENU_LINE 96

typedef struct {
    int screen;                /* PORT_STATS_MENU_ */
    float shown;               /* 0 to 1: how far the screen has faded in (its parts' alpha) */
    int covered;               /* a panel of the game's is over the place (the detail panel, the equipped list) */
    float icons[6][4];         /* main and Status: the ailment icons shown beside Condition, x0 y0 x1 y1 in
                                  the 640 x 448 picture of the menus */
    int icon_count;
    int item;                  /* Item and Arrange: the item the cursor is on, -1: none */
    char item_lines[PORT_STATS_MENU_LINES][PORT_STATS_MENU_LINE]; /* what it does, from the game's tables */
    int item_line_count;
} PortStatsMenu;

typedef struct {
    int valid;                 /* a game is being played */
    int in_menu;               /* the game's own menu is up */
    int in_play;               /* the play's own loop ran a moment ago, not paused, no menu (the minimap's
                                  test): the timers on screen show */
    PortStatsMenu menu;        /* which screen of it, and the item under the cursor */
    /* level */
    int level, experience, level_from, level_to; /* level_to 0: the last level */
    int next_hp, next_mp, next_power, next_magic; /* what the next level adds */
    int fortune;               /* experience counts double */
    /* physical and magic power */
    int power_base, power, power_uses;    /* power_uses: weapon_use_count, 0 to 99 */
    int magic_base, magic, magic_uses;    /* magic_uses: magic_use_count, 0 to 99 */
    /* gauges, in game frames from empty to full (0: does not fill) */
    int power_gauge, magic_gauge;         /* now, out of 5000 */
    int power_fill_frames, magic_fill_frames;
    char charging_magic[PORT_STATS_NAME];
    /* weight */
    int weight, max_weight;               /* tenths of a pound */
    /* attack and defence: slash, strike, stab, fire, earth, wind, water, light, dark */
    int attack[9], defence[9];
    int ailment_defence[6];               /* percent off the chance of each ailment */
    int ailment_frames[6];                /* frames each ailment has left (timed_flags) */
    int ailment_wear_thirds;              /* how fast ailments wear off, in thirds of a frame a frame (3: normal) */
    /* recovery and drain (Player_SumWornBonuses): one point every that many frames, 0: none */
    int hp_regen_period, hp_drain_period, mp_regen_period, mp_drain_period;
    int damage_to_hp, damage_to_mp;       /* percent of the damage dealt */
    int drain_curse;                      /* worn_drain_active: power and magic halved, -1 HP and MP a second */
    int fire_taken_percent;               /* the fire part of the damage taken, percent */
    /* equipment */
    PortStatsItem items[PORT_STATS_SLOTS];
    PortStatsSpell spells[PORT_STATS_SPELLS];
    int spell_count;
    PortStatsEffect effects[PORT_STATS_EFFECTS];
    int effect_count;
    PortStatsTimer timers[PORT_STATS_TIMERS];
    int timer_count;
} PortStatsSnapshot;

/* Reads the game's state (0 when no game is being played) */
int port_stats_sample(PortStatsSnapshot *out);

/* The hits, watched once a vblank (port_stats_track) by the entities' HP and the player's */
#define PORT_STATS_HIT_DEALT 0  /* the player's side hit a creature (or something hit it) */
#define PORT_STATS_HIT_TAKEN 1  /* the player was hit (Player_TakeDamage) */
#define PORT_STATS_HIT_OTHER 2  /* the player lost HP otherwise: poison, a curse, the ground, a fall */
typedef struct {
    int kind;
    int amount;                /* the damage: Entity.last_hit_damage, or the HP the player lost */
    int left, max;             /* the HP left and the most */
    int killed;
    unsigned long long vblank;
    int target;                /* a number for the creature, the same while it lives */
} PortStatsHit;

void port_stats_track(void);
/* The hits since `since` (port_vblank_count), newest last; returns how many were copied */
int port_stats_hits(PortStatsHit *out, int most, unsigned long long since);
/* The creature last hit by the player: 0 when none within `vblanks` */
int port_stats_target(PortStatsHit *out, unsigned long long vblanks);

#ifdef __cplusplus
}
#endif

#endif
