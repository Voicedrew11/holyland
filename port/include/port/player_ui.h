/* The player menu (port/ui/): Holyland's own settings, drawn with RmlUi in King's Field IV's look, over the
   game's picture. F1 (or a controller's L3 + R3) opens and closes it. While it is closed it shows the notices over
   the game (the keys' save states and quick saves, the debugging tools' messages, a problem report's progress) and
   "Preparing shaders" while the game waits for the pipelines made before play. It exists only with a window and
   settings.json (port_settings_active): never in a headless run or with a pad script, so tests stay the same.
   KF4_OVERLAY_SHOT's pages ui-<tab> open it for test pictures. The profiler (Shift+P, port/ui/profiler_overlay.hpp)
   is drawn in the same context, under the menu, and leaves the game its input.

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
/* 1 when the menu started (it has F1) */
int port_player_ui_ready(void);
/* 1 while the menu or a mod's page is open: the keyboard, mouse and controller are theirs */
int port_player_ui_open(void);
/* 1 while the menu is open and the game is to be paused (a mod's page takes the keys but keeps the game running) */
int port_player_ui_pauses(void);
/* 1 when the menu draws something this picture: open, or its hint of the keys that open it (F1, or L3 + R3) */
int port_player_ui_visible(void);
/* (port/overlay/overlay.cpp's test pictures) opens the menu on the tab of that id ("display", ...), then presses
   the keys after a colon, by name ("display:down.confirm": up down left right confirm back next previous; xbox, playstation,
   nintendo or keyboard: the prompts as if that was used last); "setup-<page>[:keys]" the setup on that page
   (port/ui/setup_pages.cpp's ids); or closes either (NULL). 0 when there is no such tab or page. */
int port_player_ui_show(const char *tab_id);
/* (the menu's Run setup again row) the first-start setup (port/ui/setup_model.hpp), in place of the menu */
void port_player_ui_run_setup(void);
/* (a row that hands the screen back: Report an Issue) the menu closes once the press that chose the row is done */
void port_player_ui_close(void);
/* (any thread: a problem report's progress) a line shown over the game while the menu is closed, for ms; a newer one
   takes its place */
void port_player_ui_notice(const char *text, unsigned ms);
/* (the game's thread) the same, its text read again from text() each picture it shows: a message that changes as
   its request is taken (port_state_message, port_quick_message) */
void port_player_ui_notice_follow(const char *(*text)(void), unsigned ms);
/* 1 while the setup shows: the game is held still under it, whatever "Pause while open" says */
int port_player_ui_setup_open(void);
/* (the presenter, when its drawing of the menu did not start) the menu stopped and never started again: an open
   menu or setup nobody can see would take the input and hold the game still on a picture with nothing over it */
void port_player_ui_cannot_draw(void);
/* (port/overlay/overlay.cpp's test pictures: profiler@V) the profiler (Shift+P, port/ui/profiler_overlay.hpp) shown
   or hidden */
void port_player_ui_profiler_show(int shown);
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
static inline int port_player_ui_pauses(void) {
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
static inline void port_player_ui_close(void) {
}
static inline void port_player_ui_notice(const char *text, unsigned ms) {
    (void)text, (void)ms;
}
static inline void port_player_ui_notice_follow(const char *(*text)(void), unsigned ms) {
    (void)text, (void)ms;
}
static inline int port_player_ui_setup_open(void) {
    return 0;
}
static inline void port_player_ui_cannot_draw(void) {
}
static inline void port_player_ui_profiler_show(int shown) {
    (void)shown;
}
static inline void port_player_ui_draw(int width, int height) {
    (void)width, (void)height;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
