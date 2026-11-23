// Saves brought over from PCSX2 or RetroArch, for the first-start setup's Emulator Saves page (setup_pages.hpp).
// The setup knows only SaveImports; the game's is make_emulator_saves, tests give their own.
//
// The game's finds the emulators' memory cards where those keep them (PCSX2's memcards folder, installed or
// Flatpak, and its Documents folder on Windows; RetroArch's system/pcsx2/memcards and saves folders), or the card the
// player picks with the system's file chooser, and lists the King's Field IV saves on them (port/include/port/
// card_import.h). A save is imported into a free Holyland save, never over one. The game's thread; the chooser's
// answer may come on another (kept behind a mutex until update takes it).
#pragma once

#include <memory>
#include <string>

#include "settings_rows.hpp"

struct SDL_Window;

namespace ui {

class SaveImports {
public:
    virtual ~SaveImports() = default;

    // Finds the cards again and lists their saves (the cards chosen with the chooser are kept)
    virtual void look() = 0;
    // Takes the chooser's answer, if it came: that card's saves join the list. Each picture, while the setup shows.
    virtual void update() = 0;

    // The saves listed
    virtual int count() const = 0;
    // A save's row: its label (level, play time) and help (where it is from, where it would go)
    virtual const SettingRow &row(int save) const = 0;
    // What happens to it: "Slot 2", "Imported to slot 2", "Already in slot 1", "No free slot"
    virtual std::string state(int save) const = 0;
    virtual bool importable(int save) const = 0;
    // Into a free Holyland save; false when it was not (state says why)
    virtual bool import(int save) = 0;

    // Opens the system's file chooser for a card elsewhere; its answer comes to update
    virtual void choose_file() = 0;
    // The chooser's row's value: what the last chosen file held ("2 saves found", "Not a memory card"), or empty
    virtual std::string chosen_text() const = 0;
};

// The game's: the chooser over the window `window` gives when it opens (the menu learns its window after it is made)
std::unique_ptr<SaveImports> make_emulator_saves(SDL_Window *(*window)());

} // namespace ui
