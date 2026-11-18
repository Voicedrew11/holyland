// The launcher's window: SDL2 with Dear ImGui on SDL's own renderer (no Vulkan, so it opens even where
// the game's GPU needs cannot be met). Main thread.
#pragma once

#include <memory>
#include <string>

struct SDL_Window;
struct SDL_Renderer;

namespace launcher {

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

    // the frame's full-window panel, for a screen's contents
    void begin_panel();
    void end_panel();

private:
    Window(SDL_Window *window, SDL_Renderer *renderer);
    SDL_Window *window_;
    SDL_Renderer *renderer_;
};

} // namespace launcher
