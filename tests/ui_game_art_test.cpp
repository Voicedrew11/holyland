// The game's menu art (port/ui/game_menu_art.hpp), on a made-up menu.tmx: an 8-bit texture's CLUT is read with
// entries 8-15 and 16-23 swapped, a 4-bit texture takes the low nibble first, alpha 0x80 is whole, the looks
// premultiply as the menu blends, and a texture that is not there or not whole gives nothing.
#include "game_menu_art.hpp"

#include <cstdio>

extern "C" {
// (the disc is not read here)
int port_disc_find_member(const char *, uint32_t *, uint32_t *) {
    return 0;
}
int port_disc_read(uint32_t, uint32_t, void *) {
    return 0;
}
}

using namespace ui;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

void put_u32(std::vector<uint8_t> &bytes, size_t at, uint32_t value) {
    CHECK(at + 4 <= bytes.size());
    if (at + 4 > bytes.size()) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        bytes[at + size_t(i)] = uint8_t(value >> (8 * i));
    }
}

// TEX0 with a base pointer, a pixel format and 2x2 texels, its CLUT in 32-bit colour
uint64_t tex0(uint32_t tbp, uint32_t psm) {
    return uint64_t(tbp) | uint64_t(psm) << 20 | uint64_t(1) << 26 | uint64_t(1) << 30;
}

// One texture's 32-byte header, image and CLUT, appended; the file's table gets its offset
void add_texture(std::vector<uint8_t> &file, int index, uint64_t tex, const std::vector<uint8_t> &image,
                 const std::vector<uint8_t> &clut) {
    size_t at = file.size();
    put_u32(file, 16 + size_t(index) * 16, uint32_t(at));
    file.resize(at + 32);
    put_u32(file, at + 4, uint32_t(image.size()));
    put_u32(file, at + 16, uint32_t(tex));
    put_u32(file, at + 20, uint32_t(tex >> 32));
    file.insert(file.end(), image.begin(), image.end());
    file.insert(file.end(), clut.begin(), clut.end());
}

// A file of two textures: 100, 8 bits a pixel (indices 0, 8, 16, 255); 200, 4 bits a pixel (indices 1, 2, 3, 15)
std::vector<uint8_t> made_up_tmx() {
    std::vector<uint8_t> file(16 + 2 * 16);
    put_u32(file, 4, 2);
    std::vector<uint8_t> clut8(256 * 4);
    auto colour = [](std::vector<uint8_t> &clut, size_t slot, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        clut[slot * 4] = r;
        clut[slot * 4 + 1] = g;
        clut[slot * 4 + 2] = b;
        clut[slot * 4 + 3] = a;
    };
    colour(clut8, 0, 10, 20, 30, 0x80);
    colour(clut8, 16, 200, 0, 0, 0x40); // index 8 is kept in slot 16
    colour(clut8, 8, 0, 200, 0, 0x80);  // index 16 in slot 8
    colour(clut8, 255, 255, 255, 255, 0);
    add_texture(file, 0, tex0(100, 0x13), {0, 8, 16, 255}, clut8);
    std::vector<uint8_t> clut4(16 * 4);
    colour(clut4, 1, 100, 100, 100, 0x80);
    colour(clut4, 2, 50, 0, 0, 0x80);
    colour(clut4, 3, 0, 50, 0, 0x80);
    colour(clut4, 15, 0, 0, 50, 0x80);
    add_texture(file, 1, tex0(200, 0x14), {0x21, 0xF3}, clut4);
    return file;
}

bool texel_is(const MenuArtImage &image, int p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t *t = &image.rgba[size_t(p) * 4];
    return t[0] == r && t[1] == g && t[2] == b && t[3] == a;
}

void test_eight_bits() {
    std::vector<uint8_t> file = made_up_tmx();
    std::optional<MenuArtImage> image = decode_menu_texture(file, 100, MenuArtLook::over);
    CHECK(image && image->width == 2 && image->height == 2 && image->rgba.size() == 16);
    if (!image) {
        return;
    }
    CHECK(texel_is(*image, 0, 10, 20, 30, 255));
    CHECK(texel_is(*image, 1, 100, 0, 0, 128)); // half alpha, premultiplied
    CHECK(texel_is(*image, 2, 0, 200, 0, 255));
    CHECK(texel_is(*image, 3, 0, 0, 0, 0));
}

void test_four_bits() {
    std::vector<uint8_t> file = made_up_tmx();
    std::optional<MenuArtImage> image = decode_menu_texture(file, 200, MenuArtLook::over);
    CHECK(image && image->width == 2 && image->height == 2);
    if (!image) {
        return;
    }
    CHECK(texel_is(*image, 0, 100, 100, 100, 255)); // the low nibble first
    CHECK(texel_is(*image, 1, 50, 0, 0, 255));
    CHECK(texel_is(*image, 2, 0, 50, 0, 255));
    CHECK(texel_is(*image, 3, 0, 0, 50, 255));
}

void test_added_looks() {
    std::vector<uint8_t> file = made_up_tmx();
    std::optional<MenuArtImage> add = decode_menu_texture(file, 100, MenuArtLook::add);
    std::optional<MenuArtImage> add_alpha = decode_menu_texture(file, 100, MenuArtLook::add_alpha);
    CHECK(add && add_alpha);
    if (!add || !add_alpha) {
        return;
    }
    CHECK(texel_is(*add, 1, 200, 0, 0, 0));       // the texture's alpha unused
    CHECK(texel_is(*add, 3, 255, 255, 255, 0));
    CHECK(texel_is(*add_alpha, 1, 100, 0, 0, 0)); // scaled by it
    CHECK(texel_is(*add_alpha, 3, 0, 0, 0, 0));
}

void test_nothing() {
    std::vector<uint8_t> file = made_up_tmx();
    CHECK(!decode_menu_texture(file, 300, MenuArtLook::over));
    CHECK(!decode_menu_texture({}, 100, MenuArtLook::over));
    std::vector<uint8_t> cut(file.begin(), file.end() - 1); // the second texture's CLUT a byte short
    CHECK(decode_menu_texture(cut, 100, MenuArtLook::over));
    CHECK(!decode_menu_texture(cut, 200, MenuArtLook::over));
    std::vector<uint8_t> many = file;
    put_u32(many, 4, 1000); // more textures than the file has room to list
    CHECK(!decode_menu_texture(many, 100, MenuArtLook::over));
    std::vector<uint8_t> past = file;
    put_u32(past, 16, uint32_t(file.size())); // a texture at the file's end
    CHECK(!decode_menu_texture(past, 100, MenuArtLook::over));
}

void test_sources() {
    CHECK(is_game_menu_source("game-menu/11584-add.tex"));
    CHECK(is_game_menu_source("/game-menu/11584-add.tex"));
    CHECK(!is_game_menu_source("logo.alpha"));
    CHECK(!load_game_menu_art("game-menu/11584-sideways.tex"));
    CHECK(!load_game_menu_art("game-menu/x-add.tex"));
    CHECK(!load_game_menu_art("game-menu/11584-add.tex")); // (no disc here)
}

} // namespace

int main() {
    test_eight_bits();
    test_four_bits();
    test_added_looks();
    test_nothing();
    test_sources();
    if (failures != 0) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("ui_game_art: all passed\n");
    return 0;
}
