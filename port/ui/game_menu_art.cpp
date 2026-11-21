// The game's own menu art as the player menu's images (port/ui/game_menu_art.hpp)
#include "game_menu_art.hpp"

#include <charconv>
#include <cstdio>

extern "C" {
#include "port/platform.h"
}

namespace ui {

namespace {

// menu.tmx: a 16-byte header (the texture count at 4), then 16 bytes a texture (its offset from the file's start at 0);
// at each offset a 32-byte header (the image's bytes at 4, the GS TEX0 at 16), the image, then its CLUT
const size_t FILE_HEADER = 16;
const size_t ENTRY_SIZE = 16;
const size_t TEXTURE_HEADER = 32;
const uint32_t TEXTURE_COUNT_MAX = 256;
const uint32_t SIDE_LOG2_MAX = 10;
const uint32_t PSMT8 = 0x13;
const uint32_t PSMT4 = 0x14;
const uint32_t CPSM_CT32 = 0;

// The file on the disc, and the most it may be (it is 887 KB)
const char *const TMX_NAME = "menu/menu.tmx";
const uint32_t TMX_SIZE_MAX = 4u << 20;

const std::string_view SOURCE_PREFIX = "game-menu/";
const std::string_view SOURCE_SUFFIX = ".tex";

uint32_t read_u32(std::span<const uint8_t> bytes, size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
           uint32_t(bytes[at + 3]) << 24;
}

uint64_t read_u64(std::span<const uint8_t> bytes, size_t at) {
    return uint64_t(read_u32(bytes, at)) | uint64_t(read_u32(bytes, at + 4)) << 32;
}

// The fields of TEX0 a menu texture needs
struct Tex0 {
    uint32_t tbp, psm, width_log2, height_log2, cpsm;
};

Tex0 unpack_tex0(uint64_t tex0) {
    return {uint32_t(tex0 & 0x3FFF), uint32_t(tex0 >> 20 & 0x3F), uint32_t(tex0 >> 26 & 0xF),
            uint32_t(tex0 >> 30 & 0xF), uint32_t(tex0 >> 51 & 0xF)};
}

// A 256-entry CLUT is stored in GS memory with entries 8-15 and 16-23 of every 32 swapped: the colour of index i
size_t clut8_slot(size_t index) {
    return (index & ~size_t(0x18)) | (index & 0x08) << 1 | (index & 0x10) >> 1;
}

// One CLUT colour (alpha 0..0x80 meaning 0..1) as premultiplied RGBA in the look
void put_texel(std::span<const uint8_t> clut_entry, MenuArtLook look, uint8_t *out) {
    uint32_t alpha = clut_entry[3] >= 0x80 ? 255 : uint32_t(clut_entry[3]) * 2;
    uint32_t scale = look == MenuArtLook::add ? 255 : alpha;
    for (int c = 0; c < 3; c++) {
        out[c] = uint8_t((uint32_t(clut_entry[c]) * scale + 127) / 255);
    }
    out[3] = look == MenuArtLook::over ? uint8_t(alpha) : 0;
}

std::optional<MenuArtLook> look_named(std::string_view name) {
    if (name == "over") {
        return MenuArtLook::over;
    }
    if (name == "add") {
        return MenuArtLook::add;
    }
    if (name == "add_alpha") {
        return MenuArtLook::add_alpha;
    }
    return std::nullopt;
}

std::string_view without_leading_dots(std::string_view source) {
    while (!source.empty() && (source.front() == '/' || source.front() == '.')) {
        source.remove_prefix(1);
    }
    return source;
}

// menu.tmx from the disc; empty when the disc has none or it does not read
std::vector<uint8_t> read_tmx() {
    uint32_t lsn = 0, size = 0;
    if (!port_disc_find_member(TMX_NAME, &lsn, &size) || size == 0 || size > TMX_SIZE_MAX) {
        return {};
    }
    uint32_t sectors = (size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE;
    std::vector<uint8_t> bytes(size_t(sectors) * DISC_SECTOR_SIZE);
    if (!port_disc_read(lsn, sectors, bytes.data())) {
        return {};
    }
    bytes.resize(size);
    return bytes;
}

} // namespace

std::optional<MenuArtImage> decode_menu_texture(std::span<const uint8_t> tmx, uint32_t tbp, MenuArtLook look) {
    if (tmx.size() < FILE_HEADER) {
        return std::nullopt;
    }
    uint32_t count = read_u32(tmx, 4);
    if (count > TEXTURE_COUNT_MAX || FILE_HEADER + size_t(count) * ENTRY_SIZE > tmx.size()) {
        return std::nullopt;
    }
    for (uint32_t i = 0; i < count; i++) {
        size_t at = read_u32(tmx, FILE_HEADER + size_t(i) * ENTRY_SIZE);
        if (at > tmx.size() || tmx.size() - at < TEXTURE_HEADER) {
            return std::nullopt;
        }
        Tex0 tex0 = unpack_tex0(read_u64(tmx, at + 16));
        if (tex0.tbp != tbp) {
            continue;
        }
        if ((tex0.psm != PSMT8 && tex0.psm != PSMT4) || tex0.cpsm != CPSM_CT32 || tex0.width_log2 > SIDE_LOG2_MAX ||
            tex0.height_log2 > SIDE_LOG2_MAX) {
            return std::nullopt;
        }
        size_t width = size_t(1) << tex0.width_log2, height = size_t(1) << tex0.height_log2;
        size_t pixels = width * height;
        size_t image_size = tex0.psm == PSMT8 ? pixels : pixels / 2;
        size_t clut_size = (tex0.psm == PSMT8 ? 256 : 16) * 4;
        size_t image_at = at + TEXTURE_HEADER;
        if (read_u32(tmx, at + 4) != image_size || tmx.size() - image_at < image_size + clut_size) {
            return std::nullopt;
        }
        std::span<const uint8_t> image = tmx.subspan(image_at, image_size);
        std::span<const uint8_t> clut = tmx.subspan(image_at + image_size, clut_size);
        MenuArtImage out;
        out.width = int32_t(width);
        out.height = int32_t(height);
        out.rgba.resize(pixels * 4);
        for (size_t p = 0; p < pixels; p++) {
            size_t index = tex0.psm == PSMT8 ? clut8_slot(image[p]) : size_t(image[p / 2] >> (p % 2 * 4) & 0xF);
            put_texel(clut.subspan(index * 4, 4), look, &out.rgba[p * 4]);
        }
        return out;
    }
    return std::nullopt;
}

bool is_game_menu_source(std::string_view source) {
    return without_leading_dots(source).starts_with(SOURCE_PREFIX);
}

std::optional<MenuArtImage> load_game_menu_art(std::string_view source) {
    std::string_view name = without_leading_dots(source);
    if (!name.starts_with(SOURCE_PREFIX) || !name.ends_with(SOURCE_SUFFIX)) {
        std::fprintf(stderr, "[ui] %.*s is not a game menu texture's name\n", int(source.size()), source.data());
        return std::nullopt;
    }
    name.remove_prefix(SOURCE_PREFIX.size());
    name.remove_suffix(SOURCE_SUFFIX.size());
    uint32_t tbp = 0;
    auto [after, error] = std::from_chars(name.data(), name.data() + name.size(), tbp);
    std::optional<MenuArtLook> look;
    if (error == std::errc() && after < name.data() + name.size() && *after == '-') {
        look = look_named(name.substr(size_t(after - name.data()) + 1));
    }
    if (!look) {
        std::fprintf(stderr, "[ui] %.*s is not a game menu texture's name\n", int(source.size()), source.data());
        return std::nullopt;
    }
    std::vector<uint8_t> tmx = read_tmx();
    std::optional<MenuArtImage> image = decode_menu_texture(tmx, tbp, *look);
    if (!image) {
        std::fprintf(stderr, "[ui] the disc's %s has no texture at %u\n", TMX_NAME, tbp);
    }
    return image;
}

bool game_menu_art_on_disc() {
    return port_disc_find_member(TMX_NAME, nullptr, nullptr) != 0;
}

} // namespace ui
