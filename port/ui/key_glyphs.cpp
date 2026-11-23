// The player menu's keys and mouse buttons as pictures (key_glyphs.hpp)
#include "key_glyphs.hpp"

#include "key_pictures.h"

namespace ui {
namespace {

constexpr std::string_view prefix = "keycap/", suffix = ".tex";
constexpr uint32_t picture_scale = 3; // (finer texels a key picture's texel: 69 pixels tall, the menu shows it 28 to 32 dp)

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// The key's name a source holds; nullopt when it is not one
std::optional<std::string> name_in(std::string_view source) {
    if (source.size() < prefix.size() + suffix.size() || source.substr(0, prefix.size()) != prefix ||
        source.substr(source.size() - suffix.size()) != suffix) {
        return std::nullopt;
    }
    std::string_view hex = source.substr(prefix.size(), source.size() - prefix.size() - suffix.size());
    if (hex.empty() || hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::string name;
    for (size_t i = 0; i < hex.size(); i += 2) {
        int high = hex_digit(hex[i]), low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        name += char(high * 16 + low);
    }
    return name;
}

} // namespace

std::string key_glyph_rml(std::string_view key_name) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "<keycap><img src=\"";
    out += prefix;
    for (char c : key_name) {
        out += digits[uint8_t(c) >> 4];
        out += digits[uint8_t(c) & 15];
    }
    out += suffix;
    return out + "\"/></keycap>";
}

bool is_key_glyph_source(std::string_view source) {
    return name_in(source).has_value();
}

std::optional<TextureUpload> load_key_glyph(std::string_view source) {
    std::optional<std::string> name = name_in(source);
    if (!name) {
        return std::nullopt;
    }
    pcr::KeyPicture picture = pcr::key_picture(*name, picture_scale);
    TextureUpload upload;
    upload.width = int32_t(picture.width);
    upload.height = int32_t(picture.height);
    upload.rgba.resize(picture.texels.size() * 4);
    for (size_t i = 0; i < picture.texels.size(); i++) {
        uint32_t texel = picture.texels[i];
        uint32_t alpha = texel >> 24;
        for (uint32_t c = 0; c < 3; c++) { // (premultiplied, as the menu blends)
            upload.rgba[i * 4 + c] = uint8_t((((texel >> (8 * c)) & 0xFF) * alpha + 127) / 255);
        }
        upload.rgba[i * 4 + 3] = uint8_t(alpha);
    }
    return upload;
}

} // namespace ui
