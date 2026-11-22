/* The player menu (port/ui/): Holyland's own settings, drawn with RmlUi in King's Field IV's look, over the
   game's picture. F1 (or a controller's L3 + R3) opens and closes it; the developers' overlay (Dear ImGui,
   port/include/port/overlay.h) moved to Shift+F1 (or `). Like the overlay it exists only with a window and
   settings.json (port_settings_active): never in a headless run or with a pad script, so tests stay the same.
   KF4_OVERLAY_SHOT's pages ui-<tab> open it for test pictures.

   Drawn by the Vulkan presenter (port/platform/present_vk.cpp), which takes this picture's triangles from
   player_ui_frame (port/ui/ui_frame.hpp). All on the game's thread. */
#ifndef PORT_PLAYER_UI_H
#define PORT_PLAYER_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef KF4_HAVE_PLAYER_UI
/* (port/overlay/overlay.cpp's port_overlay_init) once the window is made, for the Vulkan presenter; RmlUi is shut
   down at exit */
void port_player_ui_init(void *sdl_window);
/* (port/overlay/overlay.cpp, each SDL event) 1 when the menu took it, and nothing else may see it */
int port_player_ui_event(const void *sdl_event);
/* 1 when the menu started (it has F1, and the developers' overlay Shift+F1 or `) */
int port_player_ui_ready(void);
/* 1 while the menu is open: the keyboard, mouse and controller are its */
int port_player_ui_open(void);
/* 1 when the menu draws something this picture: open, or its hint of the keys that open it (F1, or L3 + R3) */
int port_player_ui_visible(void);
/* (port/overlay/overlay.cpp's test pictures) opens the menu on the tab of that id ("display", ...), then presses
   the keys after a colon, by name ("display:down.confirm": up down left right confirm back next previous; xbox, playstation,
   nintendo or keyboard: the prompts as if that was used last); "setup-<page>[:keys]" the setup on that page
   (port/ui/setup_pages.cpp's ids); or closes either (NULL). 0 when there is no such tab or page. */
int port_player_ui_show(const char *tab_id);
/* (the menu's Run setup again row) the first-start setup (port/ui/setup_model.hpp), in place of the menu */
void port_player_ui_run_setup(void);
/* 1 while the setup shows: the game is held still under it, whatever "Pause while open" says */
int port_player_ui_setup_open(void);
/* (the presenter, before it draws) lays the menu out for a window of width x height pixels and records its
   triangles: player_ui_frame */
void port_player_ui_draw(int width, int height);
#else
static inline int port_player_ui_event(const void *sdl_event) {
    (void)sdl_event;
    return 0;
}
static inline int port_player_ui_ready(void) {
    return 0;
}
static inline int port_player_ui_open(void) {
    return 0;
}
static inline int port_player_ui_visible(void) {
    return 0;
}
static inline int port_player_ui_show(const char *tab_id) {
    (void)tab_id;
    return 0;
}
static inline void port_player_ui_run_setup(void) {
}
static inline int port_player_ui_setup_open(void) {
    return 0;
}
static inline void port_player_ui_draw(int width, int height) {
    (void)width, (void)height;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
