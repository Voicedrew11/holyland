// A picture between's vertex moved past the GS's guard band (2047 about its 2048 middle), for the moves of the PC
// renderer's shaders (vu1_*_maths.h's moved(), gif_moves.h's).
//
// The PS2's GS cannot draw a vertex past the guard band, and VU1's programs leave out its triangles (ADC). The game's
// frame keeps to that for its own camera, but a picture between is turned on from it, by the camera between and the
// mouse's turn it does not show yet (port/include/port/mouse_lead.h): in a fast turn of the mouse, the triangles at
// the edge the view turns toward are past the band where the picture's camera puts them, and left out, they came in
// only with the game's next frame (the world popping in at the screen's edge). The PC renderer's GPU clips a triangle
// to the screen itself, so a vertex in front of the eye is kept wherever it lands: drawn with w 1, its triangle is cut
// at the screen's edge as the GS's guard band would have it, and X, Y, Z and S T Q stay linear on the screen as the
// GS's are. Only a vertex behind the eye (or so near it that its place is past a float) still drops its triangles.
#ifndef PAST_BAND_H
#define PAST_BAND_H

#define PAST_BAND_MOST_PX 4194304.0 // (a place kept within this of the middle: 12.4 in an int; past it the edge's turn
                                    // on the screen is under a thousandth of a pixel a pixel)

// A moved place (GS pixels): brought within PAST_BAND_MOST_PX of the middle along its line from it; false when it
// is past what a float holds (its triangles are dropped)
bool past_band_place(inout vec2 n) {
    vec2 c = n - vec2(2048.0);
    float most = max(abs(c.x), abs(c.y));
    if (!(most < 1.0e30)) {
        return false;
    }
    if (most > PAST_BAND_MOST_PX) {
        n = vec2(2048.0) + c * (PAST_BAND_MOST_PX / most);
    }
    return true;
}

#endif
