/* The developers' overlay (port/overlay/overlay.cpp): Dear ImGui over the game's picture, ` (the backtick), or F1
   (Shift+F1 where the player menu, port/include/port/player_ui.h, has F1; port_overlay_open, _visible and _paused
   count the menu too). It exists
   only with a window (SDL3), and never in a headless run or with a pad script (--input), so tests
   stay the same; KF4_OVERLAY_SHOT is the exception, for pictures of it (docs/port/README.md). */
#ifndef PORT_OVERLAY_H
#define PORT_OVERLAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's state for the Developer page (game/state_log.c) */
typedef struct {
    unsigned frame;     /* the game's frame counter */
    int menu;           /* main_menu_state */
    unsigned status;    /* the game-status flag word */
    int map_slot;       /* the map slot in use, its map's tag, and whether it is loaded */
    int map_tag;
    int map_loaded;
    float x, y, z;      /* the player's position and heading */
    float heading;
    int hp, hp_max, mp, mp_max;
    int level, gold;
} PortGameReadout;
void kf4_game_readout(PortGameReadout *out);

#ifdef KF4_HAVE_SDL
/* (port/platform/video.c) after the window and its renderer are made: an SDL_Renderer, or NULL for
   the Vulkan presenter (port/platform/present_vk.cpp), which records ImGui's draw lists itself */
void port_overlay_init(void *sdl_window, void *sdl_renderer);
/* (port/sdk/libpad.c) each SDL event: 1 when the overlay took it, and the game must not see it */
int port_overlay_event(const void *sdl_event);
/* 1 when the overlay draws something this vblank (open, a message, a test picture; not the frame-rate
   counter, which the presenters draw themselves: port_video_counter_draw) */
int port_overlay_visible(void);
/* 1 when what is drawn over the picture must be made for each picture on the game's thread: the player menu (RmlUi)
   or a test picture due. Otherwise ImGui's draw lists made once a vblank serve every picture of it
   (port_vk_overlay_keep). */
int port_overlay_each_picture(void);
/* (port/platform/video.c) draws it over the picture, before the window is presented; the picture is
   at x, y, width x height in the window's pixels. On the Vulkan presenter it only makes the draw
   lists, and port_overlay_draw_data hands them out:
   an ImDrawData *, or NULL when nothing is drawn this time. */
void port_overlay_draw(int x, int y, int width, int height);
void *port_overlay_draw_data(void);
/* (port/platform/kernel.c) once a vblank on the game's thread, before the window is presented: what
   follows the game's state (the extended stats mod) and so decides whether the overlay draws. Not in
   port_overlay_draw, which runs only while the overlay draws (and, above 60 pictures a second, as many
   times a vblank as it shows pictures). */
void port_overlay_vblank(void);
/* (the Vulkan presenter) a test picture (KF4_OVERLAY_SHOT) is due: the window, read back after this
   vblank's drawing, goes to port_overlay_shot as RGBA (R in the low byte) */
int port_overlay_shot_due(void);
void port_overlay_shot(const unsigned int *rgba, int width, int height);
/* 1 while it is open: the keyboard, mouse and controller go to it, not to the game */
int port_overlay_open(void);
/* 1 while a layer over the game has the keyboard, mouse and controller: the player menu, this overlay, the automap's
   full map. The one place that decides (port/sdk/controls.c blocks the
   game and lets the mouse go from it at each vblank; port/sdk/libpad.c leaves out the controller's sticks): the
   layers only open and close. */
int port_overlay_holds_input(void);
/* The key that opens it, and so gives its windows (the profiler, the extended stats) the mouse: "Shift+F1 or `"
   where the player menu has F1, else "F1 or `" (the backtick opens it everywhere) */
const char *port_overlay_key(void);
/* The game holds still while the overlay or the player menu is open (the player menu's "Pause while open") */
int port_overlay_paused(void);
/* (port/sdk/controls.c) a save state was asked for by its key: shows port_state_message() a while */
void port_overlay_notify_state(void);
/* (port/sdk/controls.c) a quick save or load was asked for by its key: shows port_quick_message() a while */
void port_overlay_notify_quick(void);
/* (port/debug/debug.c) the flash recorder's key: shows port_debug_message() a while (any thread) */
void port_overlay_notify_debug(void);
#else
static inline void port_overlay_vblank(void) {
}
static inline int port_overlay_open(void) {
    return 0;
}
static inline int port_overlay_holds_input(void) {
    return 0;
}
static inline int port_overlay_visible(void) {
    return 0;
}
static inline int port_overlay_each_picture(void) {
    return 0;
}
static inline int port_overlay_paused(void) {
    return 0;
}
static inline void port_overlay_notify_state(void) {
}
static inline void port_overlay_notify_quick(void) {
}
static inline void port_overlay_notify_debug(void) {
}
#endif

#ifdef __cplusplus
}
#endif

#endif
