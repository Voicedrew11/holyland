#version 450
// (exact Z) A decal's pixels reset before its own depth is written (pcr_canvas.cpp's settle_decal_depth): drawn where
// the stencil's decal_won bit is set, which the tests made before the shader find, with the viewport's depth range
// 0 to 0, the farthest. Nothing to shade: its layout is the batch programs' (pcr_canvas.cpp's frag), so that a change
// to it binds nothing again.
layout(early_fragment_tests) in;

void main() {
}
