// RmlUi's ties to the program (ui_ties.hpp)
#include "ui_ties.hpp"

#include <SDL3/SDL.h>

#include <RmlUi/Core/Core.h>

#include <cstdio>
#include <cstring>

#include "ui_assets.hpp"

namespace ui {

SystemTies::~SystemTies() {
    if (hand_ != nullptr) {
        SDL_DestroyCursor(hand_);
    }
    if (arrow_ != nullptr) {
        SDL_DestroyCursor(arrow_);
    }
}

double SystemTies::GetElapsedTime() {
    return double(SDL_GetTicks()) / 1000.0;
}

bool SystemTies::LogMessage(Rml::Log::Type type, const Rml::String &message) {
    if (type <= Rml::Log::LT_WARNING) {
        std::fprintf(stderr, "[ui] %s\n", message.c_str());
    }
    return true;
}

void SystemTies::SetMouseCursor(const Rml::String &cursor_name) {
    bool hand = cursor_name == "pointer";
    SDL_Cursor *&cursor = hand ? hand_ : arrow_;
    if (cursor == nullptr) {
        cursor = SDL_CreateSystemCursor(hand ? SDL_SYSTEM_CURSOR_POINTER : SDL_SYSTEM_CURSOR_DEFAULT);
    }
    if (cursor != nullptr && !SDL_SetCursor(cursor)) {
        std::fprintf(stderr, "[ui] the pointer's shape: %s\n", SDL_GetError());
    }
}

void SystemTies::SetClipboardText(const Rml::String &text) {
    if (!SDL_SetClipboardText(text.c_str())) {
        std::fprintf(stderr, "[ui] the clipboard: %s\n", SDL_GetError());
    }
}

void SystemTies::GetClipboardText(Rml::String &text) {
    char *got = SDL_GetClipboardText();
    text = got != nullptr ? got : "";
    SDL_free(got);
}

Rml::FileHandle AssetFiles::Open(const Rml::String &path) {
    if (const UiAsset *file = ui_asset(path)) {
        open_.push_back({file, 0});
        return Rml::FileHandle(open_.size());
    }
    std::fprintf(stderr, "[ui] no file %s in the carried files\n", path.c_str());
    return 0;
}

void AssetFiles::Close(Rml::FileHandle file) {
    if (Reading *r = reading(file)) {
        r->asset = nullptr;
    }
}

size_t AssetFiles::Read(void *buffer, size_t size, Rml::FileHandle file) {
    Reading *r = reading(file);
    if (r == nullptr) {
        return 0;
    }
    size_t left = r->asset->size - r->at;
    size = size < left ? size : left;
    std::memcpy(buffer, r->asset->data + r->at, size);
    r->at += size;
    return size;
}

bool AssetFiles::Seek(Rml::FileHandle file, long offset, int origin) {
    Reading *r = reading(file);
    if (r == nullptr) {
        return false;
    }
    long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(r->at) : long(r->asset->size);
    long at = base + offset;
    if (at < 0 || size_t(at) > r->asset->size) {
        return false;
    }
    r->at = size_t(at);
    return true;
}

size_t AssetFiles::Tell(Rml::FileHandle file) {
    Reading *r = reading(file);
    return r != nullptr ? r->at : 0;
}

AssetFiles::Reading *AssetFiles::reading(Rml::FileHandle file) {
    if (file == 0 || file > open_.size() || open_[file - 1].asset == nullptr) {
        return nullptr;
    }
    return &open_[file - 1];
}

bool load_fonts() {
    struct Face {
        const char *path;
        const char *family;
        Rml::Style::FontStyle style;
        Rml::Style::FontWeight weight;
    };
    // (one face: the menus write in the game's Mincho, tools/make_menu_font.py, in one weight and no italic)
    static const Face faces[] = {
        {"fonts/HolylandMincho.ttf", "Mincho", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal},
    };
    for (const Face &face : faces) {
        const UiAsset *file = ui_asset(face.path);
        if (file == nullptr ||
            !Rml::LoadFontFace(Rml::Span<const Rml::byte>(file->data, file->size), face.family, face.style, face.weight)) {
            std::fprintf(stderr, "[ui] the font %s did not load\n", face.path);
            return false;
        }
    }
    return true;
}

} // namespace ui
