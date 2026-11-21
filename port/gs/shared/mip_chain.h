// A texture's smaller levels made on the PC, for the GPU's own level choice with anisotropic filtering (the scene
// renderer's sampled textures, scn_build.cpp's sampled_texture). The game gives most of its textures one level and reads
// them bilinear at every distance, as the GS picks its level from the distance alone; a floor or wall seen at a grazing
// angle then shimmers. With these levels the GPU reads each pixel at the level its footprint wants.
//
// Each level is half the one before across and down (at least 1), each texel the four under it averaged, the colour
// weighted by alpha so the transparent texels' colour (often black) does not bleed into a cut-out's edge.
//
// One thread at a time: nothing kept between calls.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace pcr {

// The levels a width x height texture has down to 1 x 1, itself counted
uint32_t mip_chain_levels(uint32_t width, uint32_t height);

// The levels after a width x height level (RGBA8, R | G << 8 | B << 16 | A << 24, rows top down), down to 1 x 1, back to
// back, each level's rows top down. Empty for a 1 x 1 level or texels fewer than width x height.
std::vector<uint32_t> mip_chain_below(std::span<const uint32_t> texels, uint32_t width, uint32_t height);

} // namespace pcr
