// The player menu's files (port/ui/res/), carried inside the game: made into arrays by cmake/ui_assets.cmake
#pragma once

#include <cstddef>

struct UiAsset {
    const char *path; // under port/ui/res/: "menu.rml", "fonts/Spectral-Regular.ttf"
    const unsigned char *data;
    size_t size;
};

extern const UiAsset ui_assets[];
extern const size_t ui_asset_count;
