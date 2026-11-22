// The automap mod's drawing (port/overlay/automap_view.cpp), for port/overlay/overlay.cpp: the full map and the
// minimap, with Dear ImGui. See port/include/port/automap.h.
#ifndef PORT_AUTOMAP_VIEW_H
#define PORT_AUTOMAP_VIEW_H

union SDL_Event;

// whether the map wants a frame drawn (the minimap in play, or the full map open)
bool automap_view_visible();
// an input event while the full map is open, or the map key: true when taken
bool automap_view_event(const SDL_Event *event);
// the picture's place on the window (the window's pixels), inside the overlay's ImGui frame, under its windows
void automap_view_draw(int x, int y, int width, int height);
// the minimap's square (ImGui's coordinates, x0 y0 x1 y1) for the picture at x, y, width, height (the
// window's pixels), its corner (0 top right, 1 bottom right, 2 top left, 3 bottom left) and opacity (0-1),
// from the settings whether it is drawn or not (the mod off too); true when it is drawn now
bool automap_view_minimap_place(int x, int y, int width, int height, float box[4], int *corner, float *opacity);
// KF4_OVERLAY_SHOT's "map": the full map opened for a test picture
void automap_view_open_for_shot(bool open);

#endif
