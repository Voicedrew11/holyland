// The game's words drawn as pictures in a Mincho, made sharper: the menu's buttons ("Item", "Magic", "Arrange",
// "System": a word on a blue glow, 256 x 64) and its sheet of button hints (":Back", ":Confirm" beside the
// controller's buttons, 512 x 512). (The title's and game over's choices are in typewriter letters, not a Mincho:
// left as the disc has them.) Each is known by its indices (text_pictures_data.h, made by tools/make_text_pictures.py) and given to the GPU text_picture_scale times
// as fine: everything enlarged as the GS's bilinear filter enlarges it (finer_pictures.h), each word drawn again over
// it from IPA P Mincho, its capitals on the game's rows, in the colour of the game's word.
//
// Where the game's word was is found in the picture each time (its whitish ink inside the word's region) and painted
// over first: on a glow, with the glow as the texels beside the word have it (each row's share of a curve fitted
// across the gap); on anything else, with the colour most of the word's box has where it is not ink.
//
// One thread at a time: nothing kept between calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pcr_textures.h"

namespace pcr {

// How many times as fine across and down a sharper word picture is than the game's
constexpr uint32_t text_picture_scale = 4;

// Whether a decoded texture has a word picture's shape (one level of indices, a size one of them has)
bool may_be_text_picture(const DecodedTexture &texture);

// A word picture's colours made text_picture_scale times as fine, rows top down: colours is the picture's own (width x
// height, the sampler's RGBA8). Empty when the texture is not one of the game's word pictures.
std::vector<uint32_t> sharper_text_picture(const DecodedTexture &texture, std::span<const uint32_t> colours);

// sharper_text_picture without the check against the disc's pictures (the tests'): colours drawn as the table's
// picture number `picture` (text_pictures_data.h's pictures[]), whatever they hold; empty when its size is not theirs
std::vector<uint32_t> redrawn_text_picture(size_t picture, std::span<const uint32_t> colours, uint32_t width,
                                           uint32_t height);

} // namespace pcr
