// The game's fonts made sharper: the two 512 x 512 font sheets (font.tmr for messages, menu/font.tmr for the menus)
// given to the GPU four times as fine, their English letters drawn again from IPA Mincho (font_letters_data.h, made by
// tools/make_font_letters.py) and everything else on them enlarged as the GS's bilinear filter enlarges it.
//
// A sheet is 4-bit and holds two pictures, one in the indices' low two bits and one in their high two; the CLUT picks
// which shows (each colour the same for the four indices that share the bits it reads). Each picture's texels are one
// of four levels, its bits 3 none (the cell's ground) to 0 a stroke's core. The English letters are in three sets:
// - the messages' (both sheets, the high bits): 10 x 20 texel cells from texel row 460 (message.c's glyph_sheets), the
//   ASCII character 0x20 + n at column n % 50, row n / 50;
// - the menus' (font.tmr, the low bits): 20 x 20 cells from the top, 25 to a row, the digits, ( ) , . : < > * - +,
//   a to z then A to Z;
// - the full-width punctuation the English menus use (font.tmr, the low bits): the kana's last row, texel row 180,
//   its ! ? and / (cell 243, the slash of "50 / 50"), known with the menus' set.
// A set is known by its cells' bits as the disc has them, and drawn where the CLUT shows its picture: each letter on the
// baseline the game's are on, across where the game's ink is in its cell.
//
// One thread at a time: nothing kept between calls.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pcr_textures.h"

namespace pcr {

// How many times as fine across and down a sharper sheet is than the game's
constexpr uint32_t font_sheet_scale = 4;

// Whether a decoded texture is one of the game's font sheets, whole (512 x 512, 4-bit, one level)
bool is_font_sheet(const DecodedTexture &texture);

// A font sheet's colours made font_sheet_scale times as fine, rows top down (the sampler's RGBA8, as colour_texels
// makes them): colours is the sheet's own (width x height), clut the CLUT's colours it was made with. Empty when the
// texture is not a font sheet or its CLUT shows no set of letters (then the sheet is drawn as it is).
std::vector<uint32_t> sharper_font_sheet(const DecodedTexture &texture, std::span<const uint32_t> colours,
                                         std::span<const uint32_t, 256> clut);

// sharper_font_sheet without the check against the disc's sheets (the tests'): any whole 512 x 512 4-bit sheet, each
// set of letters drawn where the CLUT shows its picture, whatever its cells held
std::vector<uint32_t> sharpened_sheet(const DecodedTexture &texture, std::span<const uint32_t> colours,
                                      std::span<const uint32_t, 256> clut);

} // namespace pcr
