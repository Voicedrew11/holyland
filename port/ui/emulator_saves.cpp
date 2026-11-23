// Saves brought over from PCSX2 or RetroArch (port/ui/emulator_saves.hpp)
#include "emulator_saves.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <system_error>
#include <vector>

extern "C" {
#include "port/card_import.h"
}

namespace fs = std::filesystem;

namespace ui {

namespace {

// ---- Where the emulators keep their cards -------------------------------------------------------------------------
struct Place {
    fs::path folder;
    const char *emulator;
    int depth; // folders below it looked in too (RetroArch's cards are a folder or two down in saves/)
};

fs::path env_path(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != 0 ? fs::path(value) : fs::path();
}

std::vector<Place> card_places() {
    std::vector<Place> places;
#ifdef _WIN32
    fs::path home = env_path("USERPROFILE");
    fs::path appdata = env_path("APPDATA");
    if (!home.empty()) {
        places.push_back({home / "Documents" / "PCSX2" / "memcards", "PCSX2", 0});
        places.push_back({home / "OneDrive" / "Documents" / "PCSX2" / "memcards", "PCSX2", 0});
    }
    std::vector<fs::path> retroarch = {"C:/RetroArch-Win64", "C:/RetroArch"};
    if (!appdata.empty()) {
        retroarch.insert(retroarch.begin(), appdata / "RetroArch");
    }
#else
    fs::path home = env_path("HOME");
    fs::path config = env_path("XDG_CONFIG_HOME");
    if (config.empty() && !home.empty()) {
        config = home / ".config";
    }
    std::vector<fs::path> retroarch;
    if (!config.empty()) {
        places.push_back({config / "PCSX2" / "memcards", "PCSX2", 0});
        retroarch.push_back(config / "retroarch");
    }
    if (!home.empty()) {
        places.push_back({home / ".var/app/net.pcsx2.PCSX2/config/PCSX2/memcards", "PCSX2", 0});
        places.push_back({home / "Emulation/saves/pcsx2/saves", "PCSX2", 0}); // (EmuDeck)
        retroarch.push_back(home / ".var/app/org.libretro.RetroArch/config/retroarch");
    }
#endif
    // RetroArch's PS2 cores: system/pcsx2/memcards (older), saves/pcsx2/Slot 1, saves/lrps2 (newer)
    for (const fs::path &base : retroarch) {
        places.push_back({base / "system" / "pcsx2" / "memcards", "RetroArch", 0});
        places.push_back({base / "saves", "RetroArch", 2});
    }
    return places;
}

bool named_ps2(const fs::path &path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".ps2";
}

// Every *.ps2 in the folder (a file, or a folder card), and in the folders below it to `depth`
void cards_in(const fs::path &folder, int depth, std::vector<fs::path> &out) {
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const fs::path &path = it->path();
        if (named_ps2(path) && (it->is_regular_file(error) || it->is_directory(error))) {
            out.push_back(path);
        } else if (depth > 0 && it->is_directory(error)) {
            cards_in(path, depth - 1, out);
        }
    }
}

std::string utf8(const fs::path &path) {
    std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// "31h 05m"
std::string played(int minutes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%dh %02dm", minutes / 60, minutes % 60);
    return text;
}

std::string slot_text(const char *what, int slot) {
    return std::string(what) + " " + std::to_string(slot + 1);
}

// ---- The list ------------------------------------------------------------------------------------------------------
// One save listed. Not moved once made (its row points into its texts).
struct Listed {
    fs::path card;
    std::unique_ptr<PortCardSave> save;
    std::string label, help;
    SettingRow row{"", "", RowKind::action};
    int imported_to = -1; // the save it went to this run
    int held_in = -1;     // a Holyland save that already holds it
    int slot = -1;        // the free save it would go to
};

// The system file chooser's answer, from SDL's callback (which may come on another thread)
struct ChooserAnswer {
    std::mutex mutex;
    bool open = false;
    std::optional<std::string> file;
    bool failed = false;
};

const SDL_DialogFileFilter card_filters[] = {{"PS2 memory cards", "ps2"}, {"All files", "*"}};

// (SDL's thread or the game's)
void SDLCALL chooser_done(void *userdata, const char *const *files, int) {
    auto *answer = static_cast<ChooserAnswer *>(userdata);
    std::lock_guard<std::mutex> lock(answer->mutex);
    answer->open = false;
    if (files == nullptr) {
        answer->failed = true;
    } else if (files[0] != nullptr) {
        answer->file = files[0];
    }
}

class EmulatorSaves final : public SaveImports {
public:
    explicit EmulatorSaves(SDL_Window *(*window)()) : window_(window) {
    }

    void look() override {
        std::vector<std::unique_ptr<Listed>> kept;
        for (auto &listed : listed_) {
            if (std::find(chosen_.begin(), chosen_.end(), listed->card) != chosen_.end()) {
                kept.push_back(std::move(listed));
            }
        }
        listed_ = std::move(kept);
        std::vector<fs::path> seen;
        for (const Place &place : card_places()) {
            std::vector<fs::path> cards;
            cards_in(place.folder, place.depth, cards);
            std::sort(cards.begin(), cards.end());
            for (const fs::path &card : cards) {
                std::error_code error;
                fs::path same = fs::weakly_canonical(card, error);
                if (std::find(seen.begin(), seen.end(), same) == seen.end()) {
                    seen.push_back(same);
                    add_card(card, place.emulator);
                }
            }
        }
        refresh();
        std::fprintf(stderr, "[ui] emulator saves: %d found\n", count());
    }

    void update() override {
        std::optional<std::string> file;
        bool failed;
        {
            std::lock_guard<std::mutex> lock(answer_.mutex);
            file = std::move(answer_.file);
            answer_.file.reset();
            failed = answer_.failed;
            answer_.failed = false;
        }
        if (failed) {
            std::fprintf(stderr, "[ui] emulator saves: no file chooser: %s\n", SDL_GetError());
            chosen_text_ = "No file chooser";
        }
        if (file) {
            take_chosen(fs::path(std::u8string(file->begin(), file->end()))); // (SDL's names are UTF-8)
        }
    }

    int count() const override {
        return int(listed_.size());
    }

    const SettingRow &row(int save) const override {
        return listed_[size_t(save)]->row;
    }

    std::string state(int save) const override {
        const Listed &listed = *listed_[size_t(save)];
        if (listed.imported_to >= 0) {
            return slot_text("Imported to slot", listed.imported_to);
        }
        if (listed.held_in >= 0) {
            return slot_text("Already in slot", listed.held_in);
        }
        if (listed.slot < 0) {
            return "No free slot";
        }
        return slot_text("Import to slot", listed.slot);
    }

    bool importable(int save) const override {
        const Listed &listed = *listed_[size_t(save)];
        return listed.imported_to < 0 && listed.held_in < 0 && listed.slot >= 0;
    }

    bool import(int save) override {
        if (save < 0 || save >= count() || !importable(save)) {
            return false;
        }
        Listed &listed = *listed_[size_t(save)];
        int slot = port_card_save_import(listed.save.get());
        if (slot < 0) {
            std::fprintf(stderr, "[ui] emulator saves: %s save %d not imported: %s\n", utf8(listed.card).c_str(),
                         listed.save->number + 1, port_saves_message());
            refresh();
            return false;
        }
        std::fprintf(stderr, "[ui] emulator saves: %s save %d imported to slot %d\n", utf8(listed.card).c_str(),
                     listed.save->number + 1, slot + 1);
        listed.imported_to = slot;
        refresh();
        return true;
    }

    void choose_file() override {
        {
            std::lock_guard<std::mutex> lock(answer_.mutex);
            if (answer_.open) {
                return;
            }
            answer_.open = true;
        }
        fs::path start = first_place();
        std::string location = start.empty() ? std::string() : utf8(start);
        // (unlocked: where SDL answers at once, on this thread, the callback takes the lock.) The answer is a member:
        // the menu that owns this lives until the program ends.
        SDL_ShowOpenFileDialog(&chooser_done, &answer_, window_(), card_filters, int(std::size(card_filters)),
                               location.empty() ? nullptr : location.c_str(), false);
    }

    std::string chosen_text() const override {
        return chosen_text_;
    }

private:
    // the first emulator folder that is there, for the chooser to open in
    static fs::path first_place() {
        for (const Place &place : card_places()) {
            std::error_code error;
            if (fs::is_directory(place.folder, error)) {
                return place.folder;
            }
        }
        return {};
    }

    // Reads a card and lists its saves; how many, or -1 with why in *why
    int add_card(const fs::path &card, const char *emulator, PortCardResult *why = nullptr) {
        auto saves = std::make_unique<PortCardSaves>();
        PortCardResult result = port_card_read(utf8(card).c_str(), saves.get());
        if (why != nullptr) {
            *why = result;
        }
        if (result != PORT_CARD_OK) {
            return -1;
        }
        std::string name = utf8(card.filename());
        for (int i = 0; i < saves->count; i++) {
            auto listed = std::make_unique<Listed>();
            listed->card = card;
            listed->save = std::make_unique<PortCardSave>(saves->saves[i]);
            const PortCardSave &save = *listed->save;
            listed->label = "Save " + std::to_string(save.number + 1) + ", Level " +
                            std::to_string(port_card_save_level(&save));
            listed->help = "Imports save " + std::to_string(save.number + 1) + " from " + name +
                           (emulator != nullptr ? std::string(" (") + emulator + ")" : std::string()) + ": level " +
                           std::to_string(port_card_save_level(&save)) + ", " + played(port_card_save_minutes(&save)) +
                           " played. Goes into an empty slot; never replaces a Holyland save.";
            listed->row.label = listed->label.c_str();
            listed->row.help = listed->help.c_str();
            listed_.push_back(std::move(listed));
        }
        if (saves->damaged != 0) {
            std::fprintf(stderr, "[ui] emulator saves: %s: %d damaged save(s) left out\n", utf8(card).c_str(),
                         saves->damaged);
        }
        return saves->count;
    }

    void take_chosen(const fs::path &card) {
        std::error_code error;
        fs::path same = fs::weakly_canonical(card, error);
        for (const auto &listed : listed_) {
            if (fs::weakly_canonical(listed->card, error) == same) {
                chosen_text_ = "Already listed";
                return;
            }
        }
        PortCardResult why = PORT_CARD_OK;
        int found = add_card(card, nullptr, &why);
        std::fprintf(stderr, "[ui] emulator saves: chosen %s: %d save(s) (%d)\n", utf8(card).c_str(), found, int(why));
        switch (why) {
        case PORT_CARD_UNREADABLE:
            chosen_text_ = "Cannot read the file";
            return;
        case PORT_CARD_NOT_CARD:
            chosen_text_ = "Not a PS2 memory card";
            return;
        case PORT_CARD_NO_GAME:
            chosen_text_ = "No King's Field IV saves";
            return;
        case PORT_CARD_DAMAGED:
            chosen_text_ = "Saves damaged";
            return;
        case PORT_CARD_OK:
            break;
        }
        chosen_.push_back(card);
        chosen_text_ = found == 0 ? "No King's Field IV saves" : found == 1 ? "1 save found"
                                                                            : std::to_string(found) + " saves found";
        refresh();
    }

    // Where each save would go, now
    void refresh() {
        for (auto &listed : listed_) {
            listed->held_in = listed->imported_to >= 0 ? -1 : port_card_save_held_in(listed->save.get());
            listed->slot = port_card_save_slot(listed->save.get());
        }
    }

    SDL_Window *(*window_)();
    std::vector<std::unique_ptr<Listed>> listed_;
    std::vector<fs::path> chosen_; // cards chosen with the chooser: kept by look
    std::string chosen_text_;
    ChooserAnswer answer_;
};

} // namespace

std::unique_ptr<SaveImports> make_emulator_saves(SDL_Window *(*window)()) {
    return std::make_unique<EmulatorSaves>(window);
}

} // namespace ui
