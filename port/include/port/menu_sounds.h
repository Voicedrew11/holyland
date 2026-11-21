/* The game's own menu sounds for Holyland's menus (game/menu_sounds.c): the first four sounds of the se_com bank,
   played as King's Field IV's menus play them (Sound_PlayMenu), at the game's sound volume option.

   A sound asked for before the game has loaded its sounds, or while it loads them again (Sound_Init after a game
   over, a movie, a reset), is not played. The sound is sent to the sound driver at once, so it is heard while the
   menu holds the game paused.

   On the game's thread, outside the game's own sound calls (from the SDL events the port polls). */
#ifndef PORT_MENU_SOUNDS_H
#define PORT_MENU_SOUNDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* the program numbers in se_com, by what the game's menus play them for */
typedef enum {
    PORT_MENU_SOUND_CONFIRM = 0, /* Cross: a choice made, a screen opened */
    PORT_MENU_SOUND_MOVE = 1,    /* the cursor moved, a value stepped */
    PORT_MENU_SOUND_BACK = 2,    /* Circle: backed out, closed */
    PORT_MENU_SOUND_REFUSED = 3, /* Cross on something that cannot be done now */
} PortMenuSound;

void port_menu_sound(PortMenuSound sound);

#ifdef __cplusplus
}
#endif

#endif
