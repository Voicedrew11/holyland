// The player menu's files (port/ui/res/), carried inside the game: made into arrays by cmake/ui_assets.cmake
#pragma once

#include <cstddef>
#include <string_view>

struct UiAsset {
    const char *path; // under port/ui/res/: "menu.rml", "fonts/Spectral-Regular.ttf"
    const unsigned char *data;
    size_t size;
};

extern const UiAsset ui_assets[];
extern const size_t ui_asset_count;

// The file of that path ("menu.rml", "/logo.alpha", "./fonts/..."), or nullptr
inline const UiAsset *ui_asset(std::string_view path) {
    while (!path.empty() && (path.front() == '/' || path.front() == '.')) {
        path.remove_prefix(1);
    }
    for (size_t i = 0; i < ui_asset_count; i++) {
        if (path == ui_assets[i].path) {
            return &ui_assets[i];
        }
    }
    return nullptr;
}
