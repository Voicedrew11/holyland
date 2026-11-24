/* The game's side of the mods' events (port/include/port/mod_api.h): each frame, the end of each frame, a new game.
   Built as game code, so its functions also have the hook pad (port_mod_pad_probe: port/platform/mod_hooks.c). */
#include "common.h"
#include "core/game.h"
#include "port/mod_api.h"

extern GameStatus game_status;

void port_mod_in_play(void);

/* (core/game_loop.c, after Input_Update) */
void ModEvents_Frame(void) {
    if (game_status.player_control != 0 && !game_status.bit9) {
        port_mod_in_play(); /* (a new game, the first time in play since the title without a load) */
    }
    port_mod_event(HL_EVENT_FRAME, NULL);
}

/* (core/game_loop.c, the end of the frame, over the HUD) */
void ModEvents_Draw(void) {
    port_mod_event(HL_EVENT_DRAW, NULL);
}

/* A function compiled as the game's are: the pad every game function starts with (port/platform/mod_hooks.c) */
__attribute__((noinline)) void port_mod_pad_probe(void) {
    __asm__ volatile("");
}
