// The sub-pixel offsets a temporal anti-aliasing method has the 3D drawn at, one a drawing (docs/port/PC_RENDERER.md,
// "Temporal inputs"). A method names its sequence (pcr_antialias.h's AntiAliasMethod::jitter); the canvas moves the 3D
// of the frame's buffers by the offset (pcr_canvas.h's set_jitter, shaders/pcr_common.h's gs_to_clip). One table:
// a new sequence is a new entry.
#pragma once

#include <cstdint>

namespace pcr {

enum class JitterSequence : uint8_t {
    none,     // not moved (the methods of one picture: FXAA, SMAA 1x)
    smaa_t2x, // SMAA T2x's two (SMAA.hlsl's @SUBSAMPLE_INDICES): (0.25, -0.25) and (-0.25, 0.25), y up
    halton8,  // Halton (2, 3), 8 long, less a half: what DLSS and FSR ask for at native resolution
    count
};

// An offset in the picture's pixels: x right, y down (Vulkan's), each within -0.5 to 0.5
struct Jitter {
    float x_px = 0.0f, y_px = 0.0f;
};

// The offset at index (any number: it wraps) of sequence; none for none or a sequence past the table
Jitter jitter_at(JitterSequence sequence, uint32_t index);

// How long sequence is before it repeats (1 for none)
uint32_t jitter_length(JitterSequence sequence);

// (shaders/pcr_common.h's PrimPush::jitter) The offset as the shaders take it: in the target's own pixels, x in bits
// 0-15 and y in 16-31, each a signed fraction of one pixel (GLSL's packSnorm2x16); 0 is no move, exactly
uint32_t jitter_packed(Jitter jitter);

} // namespace pcr
