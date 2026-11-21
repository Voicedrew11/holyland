// The launcher's window: SDL3 with Dear ImGui on SDL's own renderer (no Vulkan, so it opens even where
// the game's GPU needs cannot be met), in the player menu's look: dark iron and amber, Cinzel and Spectral, the
// Holyland logo (port/ui/res/kf4.rcss). It takes files the player gives it: dropped on it, or chosen in the
// system's own file chooser (SDL's: the desktop's portal on Linux, IFileOpenDialog on Windows). Main thread.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "imgui.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace launcher {

// the look's colours (port/ui/res/kf4.rcss)
inline constexpr ImVec4 kAmber(0.831f, 0.663f, 0.353f, 1.0f); // #d4a95a
inline constexpr ImVec4 kEmber(0.906f, 0.533f, 0.408f, 1.0f); // #e78868, warnings (the menu's ember, lifted to read on black)
inline constexpr ImVec4 kDim(0.753f, 0.725f, 0.675f, 1.0f);   // #c0b9ac, secondary text (lighter than the menu's: readable)
inline constexpr ImVec4 kFrame(0.541f, 0.498f, 0.431f, 1.0f); // #8a7f6e, borders (lighter than the menu's #6b6255: seen on black)

class Window {
public:
    // nullptr, with the reason in `why`, if SDL cannot open a window
    static std::unique_ptr<Window> open(const std::string &title, std::string &why);
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    // starts a frame: false once the player closed the window
    bool begin_frame();
    void end_frame();

    // the frame's full-window panel, for a screen's contents (under the main menu bar when a screen draws one)
    void begin_panel();
    void end_panel();

    // dp to pixels: the screen's content scale (150% is 1.5)
    float scale() const { return scale_; }
    // Cinzel, for titles; push it with ImGui::PushFont(window.title_font(), size)
    ImFont *title_font() const { return title_font_; }
    // the Holyland logo in amber, `width_dp` wide, centred on the panel's line
    void draw_logo(float width_dp);

    // Opens the system's file chooser over the window and returns at once; the file comes from take_file.
    // `what` names the filter ("King's Field IV disc"), `extensions` its extensions ("iso;chd;cnf").
    void choose_file(const char *what, const char *extensions);
    bool choosing_file() const;
    // true while the player drags a file over the window
    bool file_over() const { return file_over_; }
    // the file dropped on the window or chosen since the last call, once
    std::optional<std::filesystem::path> take_file();
    // true once after the system's chooser could not open (none on this desktop)
    bool take_chooser_failure();

private:
    // the chooser's question and answer (window.cpp), shared with SDL's callback until it is called
    struct ChooserAnswer;
    static void chooser_done(void *answer, const char *const *files, int filter);

    Window(SDL_Window *window, SDL_Renderer *renderer, float scale);
    void load_look();

    SDL_Window *window_;
    SDL_Renderer *renderer_;
    float scale_;
    SDL_Texture *logo_ = nullptr;
    int logo_width_ = 0, logo_height_ = 0;
    ImFont *title_font_ = nullptr;
    bool file_over_ = false;
    std::optional<std::filesystem::path> dropped_;
    std::shared_ptr<ChooserAnswer> chooser_;
};

} // namespace launcher
