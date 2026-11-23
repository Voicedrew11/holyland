// The game's pictures the scene renderer is given finer than the disc has them: the font sheets (font_sheet.h) and
// the word pictures (text_pictures.h). Each kind knows its own pictures by their texels and makes them `scale` times
// as fine across and down; a new kind is a new entry in finer_pictures.cpp's table.
//
// One thread at a time: nothing kept between calls.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pcr_textures.h"

namespace pcr {

struct FinerPicture {
    std::vector<uint32_t> colours; // (rows top down, the sampler's RGBA8 as colour_texels makes them; empty: none)
    uint32_t scale = 1;            // (its texels across one of the texture's)
};

// Whether a decoded texture has the shape of a picture some kind makes finer (the cheap part a draw's lookup asks)
bool may_be_finer(const DecodedTexture &texture);

// A texture's finer picture: colours is the texture's own (width x height), clut the CLUT's colours it was made with.
// None (colours empty) when no kind knows it.
FinerPicture finer_picture(const DecodedTexture &texture, std::span<const uint32_t> colours,
                           std::span<const uint32_t, 256> clut);

// A picture enlarged `scale` times as the GS's bilinear filter reads it at each finer texel's middle, its edges
// clamped: what the GS would show of it, before a kind draws its finer parts over it
std::vector<uint32_t> enlarged_as_read(std::span<const uint32_t> colours, uint32_t width, uint32_t height,
                                       uint32_t scale);

} // namespace pcr
