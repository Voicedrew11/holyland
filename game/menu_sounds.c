/* menu_sounds.c: PC-only. The game's menu sounds for Holyland's menus (port/include/port/menu_sounds.h).

   Sound_PlayMenu queues the note in the game's list of driver commands, which Sound_Update sends once a game frame;
   while the menu holds the game paused there is no game frame, so the list is sent here at once. That is safe from
   any point the port polls events at: the game's sound calls make no kernel call that could pause the game in the
   middle of one (port/sdk/sif.c answers every RPC and DMA at once), and Sound_Loaded is 0 while Sound_Init loads
   the banks between kernel calls. Game thread only. */
#include "common.h"
#include "sound/sound.h"
#include "sound/sound_game.h"
#include "port/menu_sounds.h"

void port_menu_sound(PortMenuSound sound) {
    if (!Sound_Loaded()) {
        return;
    }
    Sound_PlayMenu((s32)sound);
    SoundDriver_FlushCommands();
}
