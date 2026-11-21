// The launcher's window: SDL3 with RmlUi, the player menu's UI library (port/ui/), drawn by SDL's own renderer (no
// Vulkan, so it opens even where the game's GPU needs cannot be met). Its pages are documents carried in the
// launcher (launcher/res/*.rml, in the menu's fonts and colours: launcher.rcss); a screen shows one page, fills its
// parts by their ids, and asks for the buttons clicked (data-action). It takes files the player gives it: dropped
// on it, or chosen in the system's own file chooser (SDL's: the desktop's portal on Linux, IFileOpenDialog on
// Windows). Main thread.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace launcher {

class Window {
public:
    // nullptr, with the reason in `why`, if SDL or RmlUi cannot start
    static std::unique_ptr<Window> open(const std::string &title, std::string &why);
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    // Starts a frame: the player's input since the last one (clicks become actions). False once the player closed
    // the window.
    bool begin_frame();
    // lays the page out and draws it
    void end_frame();

    // The page shown: a document of launcher/res/ ("setup.rml"), loaded the first time and kept
    void show_page(const char *document);
    // A part of the page shown, by its id: its inner RML, or its text (escaped), or its classes; each given to RmlUi
    // only when it changed. A part the page does not have is left alone.
    void set_rml(const char *id, const std::string &rml);
    void set_text(const char *id, std::string_view text); // (escaped: rml_text.h)
    void set_classes(const char *id, const std::string &classes);
    // a part scrolled to its end (a log's last lines)
    void scroll_to_end(const char *id);
    // A text field's value (an <input> by its id), and setting it
    std::string field(const char *id);
    void set_field(const char *id, const std::string &value);

    // True once if the player clicked an element whose data-action (its own or a parent's) is `action` this frame,
    // or pressed Enter in a text field whose data-enter is `action`
    bool take_action(std::string_view action);

    // Opens the system's file chooser over the window and returns at once; the file comes from take_file.
    // `what` names the filter ("King's Field IV disc"), `extensions` its extensions ("iso;chd;cnf").
    void choose_file(const char *what, const char *extensions);
    bool choosing_file() const;
    // true while the player drags a file over the window
    bool file_over() const;
    // the file dropped on the window or chosen since the last call, once
    std::optional<std::filesystem::path> take_file();
    // true once after the system's chooser could not open (none on this desktop)
    bool take_chooser_failure();

private:
    struct State; // window.cpp: SDL, RmlUi, the pages, the chooser
    explicit Window(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace launcher
