/* Debug cheats for testing (the player menu's Cheats tab; game/cheats.c): things a new
   character does not have yet, for testing the menus, the magic and later areas. Each is a one-off
   change to the game's state: it lasts as any progress does, and is kept by a save made after it.

   The overlay asks (port_cheat_request); the game takes the request in play, at the end of a frame
   (Quick_PollGame), so nothing changes under a menu being built or a frame being drawn. */
#ifndef PORT_CHEATS_H
#define PORT_CHEATS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_CHEAT_SPELLS 1    /* every spell of the six schools, at port_cheat_spell_level() */
#define PORT_CHEAT_EQUIPMENT 2 /* one of every weapon and worn item */
#define PORT_CHEAT_ITEMS 4     /* the carried items, 99 of each (one of a one-of-a-kind item); no keys */
#define PORT_CHEAT_MAGIC 8     /* MP and Magic Power at 999 (the game's own cap), MP full */

/* Asks for the cheats (PORT_CHEAT_ bits), taken once the player is in play */
void port_cheat_request(int cheats);
/* The level PORT_CHEAT_SPELLS gives (1 to 3; 3 at first). Each level is its own spell, with its own
   strength, effect and length: Earth Heal 1 heals for under 3 s, Earth Heal 3 also gives a 30 s boost
   and shows it as long. A spell already known goes to this level too. */
void port_cheat_set_spell_level(int level);
int port_cheat_spell_level(void);

/* The last thing the cheats did or why they waited, for the overlay */
const char *port_cheat_message(void);

/* ---- The game's side (game/cheats.c) --------------------------------------------------------- */
/* The cheats asked for, cleared (0: none) */
int port_cheat_take(void);
/* The cheats asked for, left as they are */
int port_cheat_pending(void);
void port_cheat_set_message(const char *text);

#ifdef __cplusplus
}
#endif

#endif
