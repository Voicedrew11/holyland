// The game's fonts made sharper (font_sheet.h).
#include "font_sheet.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "finer_pictures.h"
#include "font_letters_data.h"

namespace pcr {
namespace {

constexpr uint32_t sheet_size = 512;
constexpr uint32_t k = font_sheet_scale, fine_size = sheet_size * k;
static_assert(font_letters::scale == font_sheet_scale, "font_letters_data.h made at another scale");

// A set of letters on a sheet
struct LetterSet {
    uint32_t x0, y0;              // (texels: the first cell's corner)
    uint32_t cell_width, cell_height, across;
    bool high_bits;               // (the picture in the indices' high two bits, else the low two)
    std::string_view characters;  // (in the cells' order; each drawn as font_letters' ASCII letter)
    std::array<uint64_t, 2> known; // (the cells' bits as the disc has them: FNV-1a, letters_hash; 0 none)
};

constexpr std::string_view half_width_characters =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz";

// SLUS-20318's sheets: the messages' set on font.tmr and menu/font.tmr, the menus' on font.tmr
constexpr std::array<LetterSet, 2> letter_sets = {{
    {0, 460, 10, 20, 50, true, half_width_characters, {0xcee01d3f1f8b8365ull, 0x5ce7fe8a58334073ull}},
    {0, 0, 20, 20, 25, false, "0123456789(),.:<>*-+abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ",
     {0x1a8f6d3e6aa4fb59ull, 0}},
}};

uint32_t rows_of(const LetterSet &set) {
    return uint32_t((set.characters.size() + set.across - 1) / set.across);
}

// A texel's level in the set's picture: 0 none to 3 a stroke's core (its bits 3 to 0)
uint32_t level(const LetterSet &set, uint8_t index) {
    return 3u - (set.high_bits ? index >> 2 : index & 3u);
}

// The CLUT's colour for a level of the set's picture
uint32_t level_colour(const LetterSet &set, std::span<const uint32_t, 256> clut, uint32_t lvl) {
    return set.high_bits ? clut[(3u - lvl) * 4] : clut[3u - lvl];
}

uint64_t letters_hash(const LetterSet &set, const DecodedTexture &texture) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t y = set.y0; y < set.y0 + rows_of(set) * set.cell_height; y++) {
        for (uint32_t x = set.x0; x < set.x0 + set.across * set.cell_width; x++) {
            uint8_t index = texture.indices[size_t(y) * sheet_size + x];
            h = (h ^ uint64_t(set.high_bits ? index >> 2 : index & 3u)) * 0x100000001b3ull;
        }
    }
    return h;
}

bool known(const LetterSet &set, const DecodedTexture &texture) {
    uint64_t h = letters_hash(set, texture);
    return std::find(set.known.begin(), set.known.end(), h) != set.known.end();
}

// Whether the CLUT shows the set's picture: each colour the same for the four indices sharing the bits it reads, and
// none not the same as the core
bool shown(const LetterSet &set, std::span<const uint32_t, 256> clut) {
    for (uint32_t i = 0; i < 16; i++) {
        if (clut[i] != clut[set.high_bits ? i & ~3u : i & 3u]) {
            return false;
        }
    }
    return level_colour(set, clut, 0) != level_colour(set, clut, 3);
}

bool whole_sheet(const DecodedTexture &texture) {
    return texture.kind == TexelKind::Index8 && texture.width_px == sheet_size && texture.height_px == sheet_size &&
           texture.levels == 1 && texture.indices.size() >= size_t(sheet_size) * sheet_size;
}

uint32_t channel(uint32_t colour, uint32_t c) {
    return (colour >> (8 * c)) & 0xFF;
}

// A fine texel of a letter from its coverage (0-255): the colours of the picture's four levels, none to the core,
// spread over coverage as the game's levels spread over its strokes (exact where coverage is none, a third, two thirds
// or whole; between, the two around blended)
uint32_t letter_texel(uint8_t coverage, const std::array<uint32_t, 4> &levels) {
    if (coverage < 8) {
        return levels[0];
    }
    uint32_t t = coverage * 3u; // (in 255ths of a level)
    uint32_t lo = std::min(t / 255u, 2u), f = t - lo * 255u;
    uint32_t texel = 0;
    for (uint32_t ch = 0; ch < 4; ch++) {
        uint32_t v = (channel(levels[lo], ch) * (255u - f) + channel(levels[lo + 1], ch) * f + 127u) / 255u;
        texel |= v << (8 * ch);
    }
    return texel;
}

// The middle across of a cell's ink, in fine texels from the cell's left (twice, so it stays whole); false for a cell
// without ink
bool ink_middle_x2(const LetterSet &set, const DecodedTexture &texture, uint32_t cx, uint32_t cy, uint32_t &middle) {
    uint32_t left = UINT32_MAX, right = 0;
    for (uint32_t y = 0; y < set.cell_height; y++) {
        for (uint32_t x = 0; x < set.cell_width; x++) {
            if (level(set, texture.indices[size_t(cy + y) * sheet_size + cx + x]) != 0) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    if (left == UINT32_MAX) {
        return false;
    }
    middle = (left + right + 1) * k; // (the ink from left's start to right's end, in fine texels, times 2)
    return true;
}

// font_letters' letter's middle across, in its cell's fine texels (twice)
uint32_t letter_middle_x2(const uint8_t *coverage) {
    uint32_t left = font_letters::cell_width, right = 0;
    for (uint32_t y = 0; y < font_letters::cell_height; y++) {
        for (uint32_t x = 0; x < font_letters::cell_width; x++) {
            if (coverage[y * font_letters::cell_width + x] >= 64) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    return left > right ? font_letters::cell_width : left + right + 1;
}

void draw_set(const LetterSet &set, const DecodedTexture &texture, std::span<const uint32_t, 256> clut,
              std::vector<uint32_t> &fine) {
    std::array<uint32_t, 4> levels{};
    for (uint32_t l = 0; l < 4; l++) {
        levels[l] = level_colour(set, clut, l);
    }
    uint32_t fine_w = set.cell_width * k, fine_h = set.cell_height * k;
    for (size_t n = 0; n < set.characters.size(); n++) {
        uint32_t letter = uint8_t(set.characters[n]) - font_letters::first;
        if (letter >= font_letters::count) {
            continue;
        }
        uint32_t cx = set.x0 + uint32_t(n % set.across) * set.cell_width;
        uint32_t cy = set.y0 + uint32_t(n / set.across) * set.cell_height;
        uint32_t middle = 0;
        if (!ink_middle_x2(set, texture, cx, cy, middle)) {
            continue; // (the game's cell is empty: the space)
        }
        const uint8_t *coverage = font_letters::coverage[letter];
        // (where the letter's column 0 lands in the cell: its middle on the game's ink's)
        int32_t shift = (int32_t(middle) - int32_t(letter_middle_x2(coverage))) / 2;
        uint32_t top = std::min(font_letters::cell_height, fine_h);
        for (uint32_t y = 0; y < fine_h; y++) {
            for (uint32_t x = 0; x < fine_w; x++) {
                int32_t lx = int32_t(x) - shift;
                uint8_t c = 0;
                if (y < top && lx >= 0 && lx < int32_t(font_letters::cell_width)) {
                    c = coverage[y * font_letters::cell_width + uint32_t(lx)];
                }
                fine[size_t(cy * k + y) * fine_size + cx * k + x] = letter_texel(c, levels);
            }
        }
    }
}

std::vector<uint32_t> drawn(const DecodedTexture &texture, std::span<const uint32_t> colours,
                            std::span<const uint32_t, 256> clut, bool only_known) {
    if (!whole_sheet(texture) || colours.size() != size_t(sheet_size) * sheet_size) {
        return {};
    }
    std::vector<const LetterSet *> sets;
    for (const LetterSet &set : letter_sets) {
        if (shown(set, clut) && (!only_known || known(set, texture))) {
            sets.push_back(&set);
        }
    }
    if (sets.empty()) {
        return {};
    }
    std::vector<uint32_t> fine = enlarged_as_read(colours, sheet_size, sheet_size, k);
    for (const LetterSet *set : sets) {
        draw_set(*set, texture, clut, fine);
    }
    return fine;
}

} // namespace

bool is_font_sheet(const DecodedTexture &texture) {
    if (!whole_sheet(texture)) {
        return false;
    }
    return std::any_of(letter_sets.begin(), letter_sets.end(),
                       [&](const LetterSet &set) { return known(set, texture); });
}

std::vector<uint32_t> sharper_font_sheet(const DecodedTexture &texture, std::span<const uint32_t> colours,
                                         std::span<const uint32_t, 256> clut) {
    return drawn(texture, colours, clut, true);
}

std::vector<uint32_t> sharpened_sheet(const DecodedTexture &texture, std::span<const uint32_t> colours,
                                      std::span<const uint32_t, 256> clut) {
    return drawn(texture, colours, clut, false);
}

} // namespace pcr
