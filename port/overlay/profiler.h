// The profiler's panel (Shift+P, port/include/port/profile.h): what holds the frame rate down, thread by thread,
// drawn over the game by the overlay (port/overlay/overlay.cpp)
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
// draws it, inside the overlay's ImGui frame; overlay_open: the settings are open, the mouse the overlay's (the
// panel's button can be pressed)
void profiler_draw(bool overlay_open);

#endif
