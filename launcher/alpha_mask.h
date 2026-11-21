// The player menu's pictures as alpha masks (port/ui/res/*.alpha, made by tools/ui_masks.py): "KF4A", the width
// and the height as little-endian 32-bit numbers, then a byte of alpha for each pixel, rows top to bottom. The
// launcher carries the logo's (cmake/ui_assets.cmake) and colours it.
#pragma once

#include <optional>
#include <string_view>

namespace launcher {

struct AlphaMask {
    int width = 0, height = 0;
    const unsigned char *alpha = nullptr; // width * height bytes, inside the file read_alpha_mask was given
};

// the mask in `file`, or nullopt when it is not one (another tag, no pixels, sizes its length does not hold)
std::optional<AlphaMask> read_alpha_mask(std::string_view file);

} // namespace launcher
