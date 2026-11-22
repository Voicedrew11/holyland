// RmlUi's ties to a program that carries its files inside it (ui_assets.hpp), shared by the player menu
// (player_ui.cpp) and the launcher (launcher/window.cpp): the clock, the log, the pointer's shape and the clipboard
// through SDL; files from the carried arrays; the fonts. Set them before Rml::Initialise, keep them until
// Rml::Shutdown. On the thread that runs RmlUi.
#pragma once

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <vector>

struct SDL_Cursor;
struct UiAsset;

namespace ui {

class SystemTies final : public Rml::SystemInterface {
public:
    SystemTies() = default;
    ~SystemTies() override;
    SystemTies(const SystemTies &) = delete;
    SystemTies &operator=(const SystemTies &) = delete;

    double GetElapsedTime() override;
    // warnings and errors to stderr, as "[ui] ..."
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override;
    // the style sheet's cursor: "pointer" (a hand) or the arrow
    void SetMouseCursor(const Rml::String &cursor_name) override;
    void SetClipboardText(const Rml::String &text) override;
    void GetClipboardText(Rml::String &text) override;

private:
    SDL_Cursor *hand_ = nullptr;
    SDL_Cursor *arrow_ = nullptr;
};

// The program's files, from the arrays it carries (ui_assets.hpp)
class AssetFiles final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String &path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;

private:
    struct Reading {
        const UiAsset *asset;
        size_t at;
    };
    Reading *reading(Rml::FileHandle file);
    std::vector<Reading> open_; // (a handle is its place plus one; a handful of files are opened, once each)
};

// Holyland Mincho (the game's Mincho, tools/make_menu_font.py) from the carried fonts/; false if it is missing
bool load_fonts();

} // namespace ui
