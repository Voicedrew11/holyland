// The reflection's rippled water on the GPU (RD_PROGRAM_RIPPLE; port/render/ripple.hpp), shared by the renderers that
// draw native draws: the PC renderer's pcr_ndripple.vert. This part: a draw's record as the shaders read it (ripple.hpp's
// RippleRecord: a qword a column) and its flags. Include it, declare the buffer of records, then ripple_maths.h.
struct Draw {
    mat4 vertex;   // the world to screen matrix by the part's (VU arithmetic)
    mat4 screen;   // view to screen (pictures between: sx, sy, cx, cy, cz)
    vec4 sines[8]; // wobble_sines
    ivec4 info;    // XYOFFSET's X and Y (12.4), RIPPLE_C_*, RGBAQ's RGBA
    vec4 fog;      // FOG's fog, in x
};
#define RIPPLE_C_MOVING 1
#define RIPPLE_C_NEAREST 0x100 // the game's floats round to nearest (--fpu-round nearest), not toward zero
