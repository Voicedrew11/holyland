#version 450
// (developers', KF4_SCN_FLAT=1: slice 3f's bound, docs/plans/scene-slice4-results.md) every draw's fragment stage this
// one: a constant colour, no texture, fog or test of its own. The depth test, the blend and the rasterising are the
// draw's: the world's time with it is what the GS's pixel in the shader costs at most to take away. The picture is
// wrong.
layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = vec4(0.5, 0.5, 0.5, 1.0);
}
