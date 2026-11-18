// The profiler (Shift+P; docs/port/PROFILING.md): one window, drawn over the game by the overlay
// (port/overlay/overlay.cpp); its contents are frames_view.cpp's
#ifndef PORT_OVERLAY_PROFILER_H
#define PORT_OVERLAY_PROFILER_H

#include <SDL.h>

// Shift+P: shows or hides the panel; true when the event was its
bool profiler_key(const SDL_Event &event);
bool profiler_visible();
// KF4_OVERLAY_SHOT's profiler@V (overlay.cpp): shown for the test picture, or not
void profiler_show_for_shot(bool show);
// once a vblank (port_overlay_vblank): takes a reading twice a second
void profiler_step();
// draws it, inside the overlay's ImGui frame. While it is shown the mouse is its (port_overlay_holds_mouse): the
// cursor free, and the keyboard too while one of its text fields is being typed in.
void profiler_draw();

#endif
