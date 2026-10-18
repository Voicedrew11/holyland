/* state_log.c: the game's state in one line, for the PC port's per-frame log (--log; port/platform/
   kernel.c writes it at each vblank). PC-only: what a test reads to see where the game is. */
#include <stdio.h>

#include "common.h"
#include "core/game.h"
#include "map/map.h"
#include "player/player.h"

extern GameStatus game_status;
extern PlayerState player_state;
extern MapSlot map_slots[2];
extern int current_map_slot;
extern u32 frame_counter;
extern s8 main_menu_state;
extern int save_loaded;

void kf4_game_state(char *line, size_t size) {
    const MapSlot *slot = &map_slots[current_map_slot & 1];

    snprintf(line, size,
             "frame=%u menu=%d save=%d status=%08X map=%d:%02X%s pos=%.2f,%.2f,%.2f heading=%.3f hp=%d death=%u",
             (unsigned)frame_counter, main_menu_state, save_loaded, (unsigned)GAME_STATUS_FLAG_WORD(&game_status),
             current_map_slot, slot->tag, slot->loaded ? "" : "(empty)", player_state.position.x,
             player_state.position.y, player_state.position.z, player_state.rotation.y, player_state.hp_now,
             player_state.death_timer);
}
