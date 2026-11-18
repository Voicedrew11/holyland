// The player menu (port/include/port/player_ui.h): RmlUi with the menu's document (port/ui/res/menu.rml) and look
// (kf4.rcss), the settings catalogue's rows (settings_rows.hpp) moved about by MenuModel (menu_model.hpp), drawn
// through ui::Recorder (ui_render.hpp) by the Vulkan presenter.
//
// Each picture the page's parts (the tabs, the rows, the help, the key prompts) are written as RML from the rows
// and the model, and given to RmlUi only when their text changed. The keyboard and the controller go to the model;
// the mouse goes to RmlUi, whose events on the rows (data-row, data-step, data-choice, data-tab) go to the model. All
// on the game's thread.
#include <SDL.h>

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include "menu_model.hpp"
#include "settings_rows.hpp"
#include "ui_assets.hpp"
#include "ui_frame.hpp"
#include "ui_render.hpp"

extern "C" {
#include "port/controls.h"
#include "port/player_ui.h"
#include "port/settings.h"
}

namespace {

// ---- RmlUi's ties to the program ------------------------------------------------------------------------------------
class SystemTies final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override {
        return double(SDL_GetTicks64()) / 1000.0;
    }
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
        if (type <= Rml::Log::LT_WARNING) {
            std::fprintf(stderr, "[ui] %s\n", message.c_str());
        }
        return true;
    }
};

// The menu's files, from the arrays the game carries (ui_assets.hpp)
class AssetFiles final : public Rml::FileInterface {
public:
    Rml::FileHandle Open(const Rml::String &path) override {
        std::string_view name = path;
        while (!name.empty() && (name.front() == '/' || name.front() == '.')) {
            name.remove_prefix(1);
        }
        for (size_t i = 0; i < ui_asset_count; i++) {
            if (name == ui_assets[i].path) {
                open_.push_back({&ui_assets[i], 0});
                return Rml::FileHandle(open_.size());
            }
        }
        std::fprintf(stderr, "[ui] no file %s in the menu's files\n", path.c_str());
        return 0;
    }
    void Close(Rml::FileHandle file) override {
        if (Reading *r = reading(file)) {
            r->asset = nullptr;
        }
    }
    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override {
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
    bool Seek(Rml::FileHandle file, long offset, int origin) override {
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
    size_t Tell(Rml::FileHandle file) override {
        Reading *r = reading(file);
        return r != nullptr ? r->at : 0;
    }

private:
    struct Reading {
        const UiAsset *asset;
        size_t at;
    };
    Reading *reading(Rml::FileHandle file) {
        if (file == 0 || file > open_.size() || open_[file - 1].asset == nullptr) {
            return nullptr;
        }
        return &open_[file - 1];
    }
    std::vector<Reading> open_; // (a handle is its place plus one; the menu opens a handful of files, once)
};

const UiAsset *asset(std::string_view path) {
    for (size_t i = 0; i < ui_asset_count; i++) {
        if (path == ui_assets[i].path) {
            return &ui_assets[i];
        }
    }
    return nullptr;
}

// ---- The menu's state -----------------------------------------------------------------------------------------------
enum class Device { keyboard, pad };

struct Menu;
class PointerEvents final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event &event) override;
};

struct Menu {
    bool ready = false;
    bool open = false;
    SDL_Window *window = nullptr;
    SystemTies system;
    AssetFiles files;
    ui::Recorder recorder;
    ui::SettingsRows rows;
    ui::MenuModel model{rows};
    PointerEvents pointer;
    Rml::Context *context = nullptr;
    Rml::ElementDocument *document = nullptr;
    // what each part of the page was last given, so RmlUi lays out again only what changed
    std::string tabs_rml, rows_rml, help_rml, prompts_rml;
    int scrolled_row = -2; // the row last scrolled into view
    bool scroll_wanted = false;
    Device device = Device::keyboard;
    // the controller's held direction, pressed again while held
    int pad_held = -1; // a MenuInput, or -1
    uint64_t pad_next_ms = 0;
    int stick_direction = -1; // the left stick's, as a MenuInput (or -1)
    bool l3 = false, r3 = false;
    // the window's pixels for each of SDL's window units (a HiDPI screen: 2)
    float pixels_per_unit = 1.0f;
    bool drawn = false;
};
Menu menu;

const uint64_t PAD_DELAY_MS = 350;  // a held direction repeats after this
const uint64_t PAD_REPEAT_MS = 85;  // every this

void set_open(bool open) {
    if (open == menu.open || !menu.ready) {
        return;
    }
    menu.open = open;
    port_controls_block(open);
    if (open) {
        menu.model.open_at(menu.model.tab());
        menu.scrolled_row = -2;
    } else {
        menu.pad_held = -1;
        port_settings_save();
    }
}

void apply(ui::MenuResult result) {
    menu.scroll_wanted = true;
    if (result == ui::MenuResult::close) {
        set_open(false);
    }
}

void press(ui::MenuInput input) {
    apply(menu.model.press(input));
}

// ---- The pointer ----------------------------------------------------------------------------------------------------
// The number in the attribute of the element or its nearest parent that has it
bool find_attribute(Rml::Element *element, const char *name, int &out) {
    for (; element != nullptr; element = element->GetParentNode()) {
        if (Rml::Variant *value = element->GetAttribute(name)) {
            out = value->Get<int>();
            return true;
        }
    }
    return false;
}

void PointerEvents::ProcessEvent(Rml::Event &event) {
    Rml::Element *target = event.GetTargetElement();
    int value = 0;
    menu.device = Device::keyboard; // (the mouse's prompts are the keyboard's)
    if (event.GetId() == Rml::EventId::Mouseover) {
        if (find_attribute(target, "data-row", value)) {
            menu.model.point_at_row(value);
        }
        return;
    }
    // (on the press, not the click: a page written again between press and release would lose the click)
    if (event.GetId() != Rml::EventId::Mousedown || event.GetParameter<int>("button", 0) != 0) {
        return;
    }
    int row = 0;
    if (find_attribute(target, "data-tab", value)) {
        menu.model.click_tab(value);
    } else if (find_attribute(target, "data-choice", value)) {
        menu.model.click_choice(value);
    } else if (find_attribute(target, "data-step", value) && find_attribute(target, "data-row", row)) {
        menu.model.click_step(row, value);
    } else if (find_attribute(target, "data-row", value)) {
        menu.model.click_row(value);
    } else {
        menu.model.click_row(-1); // outside the rows: closes an open list
    }
    menu.scroll_wanted = false;
}

// ---- The page, as RML -----------------------------------------------------------------------------------------------
void escaped(std::string &out, std::string_view text) {
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
}

std::string tabs_rml() {
    std::string out = menu.device == Device::pad ? "<tab class=\"end\">L1</tab>" : "<tab class=\"end\">Q</tab>";
    for (int t = 0; t < menu.rows.tab_count(); t++) {
        out += "<tab data-tab=\"" + std::to_string(t) + "\"";
        out += t == menu.model.tab() ? " class=\"selected\">" : ">";
        escaped(out, menu.rows.tab(t).name);
        out += "</tab>";
    }
    out += menu.device == Device::pad ? "<tab class=\"end\">R1</tab>" : "<tab class=\"end\">E</tab>";
    return out;
}

// A row's value: the arrows either side on the focused row (dim at the ends), a number's bar under it
void value_rml(std::string &out, int tab, int row, const ui::RowShape &shape, bool focused) {
    const ui::SettingRow &r = menu.rows.row(tab, row);
    std::string text = menu.rows.value_text(tab, row);
    out += shape.kind == ui::RowKind::toggle && shape.current == 0 ? "<value class=\"off\">" : "<value>";
    if (r.overridden != nullptr && r.overridden()) {
        out += "<note>command line</note>";
    }
    bool arrows = focused && shape.enabled && shape.kind != ui::RowKind::action;
    bool first = false, last = false;
    if (shape.kind == ui::RowKind::choice || shape.kind == ui::RowKind::toggle) {
        first = shape.current == 0;
        last = shape.current == shape.choice_count - 1;
    } else if (shape.kind == ui::RowKind::number) {
        float f = menu.rows.number_fraction(tab, row);
        first = f <= 0.0f;
        last = f >= 1.0f;
    }
    if (shape.kind == ui::RowKind::number) {
        char width[32];
        std::snprintf(width, sizeof(width), "%.1f%%", double(menu.rows.number_fraction(tab, row)) * 100.0);
        out += std::string("<bar><fill style=\"width: ") + width + "\"></fill></bar>";
    }
    if (arrows) {
        out += first ? "<arrow class=\"end\">&#x2039;</arrow>" : "<arrow data-step=\"-1\">&#x2039;</arrow>";
    }
    escaped(out, text);
    if (arrows) {
        out += last ? "<arrow class=\"end\">&#x203A;</arrow>" : "<arrow data-step=\"1\">&#x203A;</arrow>";
    }
    out += "</value>";
}

std::string rows_rml() {
    std::string out;
    int tab = menu.model.tab();
    for (int row = 0; row < menu.rows.row_count(tab); row++) {
        ui::RowShape shape = menu.rows.shape(tab, row);
        const ui::SettingRow &r = menu.rows.row(tab, row);
        if (shape.kind == ui::RowKind::heading) {
            out += row == 0 ? "<row class=\"heading first\"><label>" : "<row class=\"heading\"><label>";
            escaped(out, r.label);
            out += "</label></row>";
            continue;
        }
        bool focused = row == menu.model.row();
        bool open = focused && menu.model.list_open();
        out += "<row id=\"row-" + std::to_string(row) + "\" data-row=\"" + std::to_string(row) + "\" class=\"";
        out += focused ? "focused " : "";
        out += shape.enabled ? "" : "disabled ";
        out += shape.kind == ui::RowKind::action ? "action " : "";
        out += open ? "open" : "";
        out += "\"><label>";
        escaped(out, r.label);
        out += "</label>";
        if (shape.kind == ui::RowKind::action) {
            out += focused && shape.enabled ? "<value>Enter</value>" : "";
        } else {
            value_rml(out, tab, row, shape, focused);
        }
        out += "</row>";
        if (open) {
            out += "<list>";
            for (int c = 0; c < shape.choice_count; c++) {
                out += "<choice data-choice=\"" + std::to_string(c) + "\" class=\"";
                out += c == menu.model.list_choice() ? "highlighted " : "";
                out += c == shape.current ? "chosen" : "";
                out += "\">";
                escaped(out, r.choices[size_t(c)].label);
                out += "</choice>";
            }
            out += "</list>";
        }
    }
    return out;
}

std::string help_rml() {
    int tab = menu.model.tab(), row = menu.model.row();
    if (row < 0) {
        return "";
    }
    const ui::SettingRow &r = menu.rows.row(tab, row);
    std::string out = "<h2>";
    escaped(out, r.label);
    out += "</h2>";
    std::string_view help = r.help;
    while (!help.empty()) {
        size_t end = help.find('\n');
        out += "<p>";
        escaped(out, help.substr(0, end));
        out += "</p>";
        help = end == std::string_view::npos ? std::string_view() : help.substr(end + 1);
    }
    if (r.overridden != nullptr && r.overridden()) {
        out += "<p class=\"note\">This run's value was set on the command line. A change here is kept for the next "
               "start.</p>";
    }
    return out;
}

void prompt(std::string &out, const char *key_a, const char *key_b, const char *what) {
    out += "<prompt><key>";
    out += key_a;
    out += "</key>";
    if (key_b != nullptr) {
        out += "<key>";
        out += key_b;
        out += "</key>";
    }
    out += what;
    out += "</prompt>";
}

std::string prompts_rml() {
    std::string out;
    bool pad = menu.device == Device::pad;
    if (menu.model.list_open()) {
        prompt(out, pad ? "D-pad" : "&#x2191;", pad ? nullptr : "&#x2193;", "Choose");
        prompt(out, pad ? "Cross" : "Enter", nullptr, "Pick");
        prompt(out, pad ? "Circle" : "Esc", nullptr, "Cancel");
        return out;
    }
    prompt(out, pad ? "D-pad" : "&#x2191;", pad ? nullptr : "&#x2193;", "Move");
    prompt(out, "&#x2190;", "&#x2192;", "Change");
    prompt(out, pad ? "Cross" : "Enter", nullptr, "Select");
    prompt(out, pad ? "L1" : "Q", pad ? "R1" : "E", "Tabs");
    prompt(out, pad ? "Circle" : "Esc", nullptr, "Close");
    return out;
}

void set_part(const char *id, std::string &last, std::string now) {
    if (now == last) {
        return;
    }
    if (Rml::Element *part = menu.document->GetElementById(id)) {
        part->SetInnerRML(now);
    }
    last = std::move(now);
}

void write_page() {
    set_part("tabs", menu.tabs_rml, tabs_rml());
    set_part("rows", menu.rows_rml, rows_rml());
    set_part("help", menu.help_rml, help_rml());
    set_part("prompts", menu.prompts_rml, prompts_rml());
}

// The focused row (and its open list) into view, after a key moved it; not after the pointer did
void scroll_to_focus() {
    int row = menu.model.row();
    if (!menu.scroll_wanted || row < 0) {
        return;
    }
    menu.scroll_wanted = false;
    std::string id = "row-" + std::to_string(row);
    if (Rml::Element *element = menu.document->GetElementById(id)) {
        Rml::ScrollIntoViewOptions nearest(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest);
        if (menu.model.list_open()) {
            if (Rml::Element *list = element->GetNextSibling()) {
                list->ScrollIntoView(nearest);
            }
        }
        element->ScrollIntoView(nearest);
    }
}

// ---- Keys and the controller ----------------------------------------------------------------------------------------
int key_input(SDL_Scancode key, Uint16 modifiers) {
    switch (key) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_W:
        return int(ui::MenuInput::up);
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_S:
        return int(ui::MenuInput::down);
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_A:
        return int(ui::MenuInput::left);
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_D:
        return int(ui::MenuInput::right);
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_SPACE:
        return int(ui::MenuInput::confirm);
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_BACKSPACE:
        return int(ui::MenuInput::back);
    case SDL_SCANCODE_E:
    case SDL_SCANCODE_PAGEDOWN:
        return int(ui::MenuInput::next_tab);
    case SDL_SCANCODE_Q:
    case SDL_SCANCODE_PAGEUP:
        return int(ui::MenuInput::previous_tab);
    case SDL_SCANCODE_TAB:
        return int(modifiers & KMOD_SHIFT ? ui::MenuInput::previous_tab : ui::MenuInput::next_tab);
    default:
        return -1;
    }
}

bool repeats(int input) {
    return input == int(ui::MenuInput::up) || input == int(ui::MenuInput::down) ||
           input == int(ui::MenuInput::left) || input == int(ui::MenuInput::right);
}

int pad_input(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return int(ui::MenuInput::up);
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return int(ui::MenuInput::down);
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return int(ui::MenuInput::left);
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return int(ui::MenuInput::right);
    case SDL_CONTROLLER_BUTTON_A:
        return int(ui::MenuInput::confirm);
    case SDL_CONTROLLER_BUTTON_B:
        return int(ui::MenuInput::back);
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return int(ui::MenuInput::previous_tab);
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return int(ui::MenuInput::next_tab);
    default:
        return -1;
    }
}

void pad_hold(int input) {
    menu.pad_held = repeats(input) ? input : -1;
    menu.pad_next_ms = SDL_GetTicks64() + PAD_DELAY_MS;
}

// The left stick past half way, as a direction
int stick_direction() {
    SDL_GameController *pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && pad == nullptr; i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        }
    }
    if (pad == nullptr) {
        return -1;
    }
    int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    int y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    const int half = 16384;
    if (y < -half) {
        return int(ui::MenuInput::up);
    }
    if (y > half) {
        return int(ui::MenuInput::down);
    }
    if (x < -half) {
        return int(ui::MenuInput::left);
    }
    if (x > half) {
        return int(ui::MenuInput::right);
    }
    return -1;
}

// Each picture: a held direction (the d-pad's or the stick's) pressed again
void pad_repeat() {
    int stick = stick_direction();
    if (stick != menu.stick_direction) {
        menu.stick_direction = stick;
        if (stick >= 0) {
            menu.device = Device::pad;
            press(ui::MenuInput(stick));
        }
        pad_hold(stick);
    }
    uint64_t now = SDL_GetTicks64();
    if (menu.pad_held >= 0 && now >= menu.pad_next_ms) {
        menu.pad_next_ms = now + PAD_REPEAT_MS;
        press(ui::MenuInput(menu.pad_held));
    }
}

bool is_input(Uint32 type) {
    switch (type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
        return true;
    default:
        return false;
    }
}

// L3 + R3 together open and close the menu
bool pad_chord(const SDL_Event &event) {
    if (event.type != SDL_CONTROLLERBUTTONDOWN && event.type != SDL_CONTROLLERBUTTONUP) {
        return false;
    }
    bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
    if (event.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
        menu.l3 = down;
    } else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
        menu.r3 = down;
    } else {
        return false;
    }
    if (down && menu.l3 && menu.r3) {
        menu.device = Device::pad;
        set_open(!menu.open);
        return true;
    }
    return menu.open;
}

int mouse_x(int x) {
    return int(float(x) * menu.pixels_per_unit);
}

void quit() {
    if (!menu.ready) {
        return;
    }
    menu.ready = false;
    Rml::Shutdown();
}

bool load_fonts() {
    struct Face {
        const char *path;
        const char *family;
        Rml::Style::FontStyle style;
        Rml::Style::FontWeight weight;
    };
    static const Face faces[] = {
        {"fonts/Spectral-Regular.ttf", "Spectral", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal},
        {"fonts/Spectral-SemiBold.ttf", "Spectral", Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Bold},
        {"fonts/Spectral-Italic.ttf", "Spectral", Rml::Style::FontStyle::Italic, Rml::Style::FontWeight::Normal},
        {"fonts/IMFellEnglish-Regular.ttf", "IM FELL English", Rml::Style::FontStyle::Normal,
         Rml::Style::FontWeight::Normal},
        {"fonts/IMFellEnglish-Italic.ttf", "IM FELL English", Rml::Style::FontStyle::Italic,
         Rml::Style::FontWeight::Normal},
        {"fonts/IMFellEnglishSC-Regular.ttf", "IM FELL English SC", Rml::Style::FontStyle::Normal,
         Rml::Style::FontWeight::Normal},
    };
    for (const Face &face : faces) {
        const UiAsset *file = asset(face.path);
        if (file == nullptr ||
            !Rml::LoadFontFace(Rml::Span<const Rml::byte>(file->data, file->size), face.family, face.style, face.weight)) {
            std::fprintf(stderr, "[ui] the font %s did not load\n", face.path);
            return false;
        }
    }
    return true;
}

} // namespace

// ---- The port's side ------------------------------------------------------------------------------------------------
const ui::Frame *player_ui_frame() {
    return menu.drawn ? &menu.recorder.frame() : nullptr;
}

extern "C" void port_player_ui_init(void *sdl_window) {
    if (menu.ready || !port_settings_active()) {
        return;
    }
    menu.window = static_cast<SDL_Window *>(sdl_window);
    Rml::SetSystemInterface(&menu.system);
    Rml::SetFileInterface(&menu.files);
    Rml::SetRenderInterface(&menu.recorder);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "[ui] RmlUi did not start: no player menu\n");
        return;
    }
    menu.context = Rml::CreateContext("menu", Rml::Vector2i(1280, 720));
    if (menu.context == nullptr || !load_fonts() ||
        (menu.document = menu.context->LoadDocument("menu.rml")) == nullptr) {
        std::fprintf(stderr, "[ui] the menu's document did not load: no player menu\n");
        Rml::Shutdown();
        return;
    }
    menu.document->AddEventListener(Rml::EventId::Mouseover, &menu.pointer);
    menu.document->AddEventListener(Rml::EventId::Mousedown, &menu.pointer);
    menu.document->Show();
    menu.model.open_at(0);
    menu.ready = true;
    std::atexit(quit);
    std::fprintf(stderr, "[ui] F1 (or L3 + R3) opens the menu; Shift+F1 the developers' overlay\n");
}

extern "C" int port_player_ui_event(const void *sdl_event) {
    const SDL_Event &event = *static_cast<const SDL_Event *>(sdl_event);
    if (!menu.ready) {
        return 0;
    }
    if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F1) {
        if (event.key.keysym.mod & KMOD_SHIFT) {
            return 0; // (the developers' overlay)
        }
        if (!event.key.repeat) {
            menu.device = Device::keyboard;
            set_open(!menu.open);
        }
        return 1;
    }
    if (pad_chord(event)) {
        return 1;
    }
    if (!menu.open) {
        return 0;
    }
    switch (event.type) {
    case SDL_KEYDOWN: {
        if (event.key.keysym.scancode == SDL_SCANCODE_F11) {
            return 0; // (full screen: the overlay's key, open or not)
        }
        int input = key_input(event.key.keysym.scancode, event.key.keysym.mod);
        if (input >= 0 && (!event.key.repeat || repeats(input))) {
            menu.device = Device::keyboard;
            press(ui::MenuInput(input));
        }
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN: {
        menu.device = Device::pad;
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            set_open(false);
            break;
        }
        int input = pad_input(event.cbutton.button);
        if (input >= 0) {
            press(ui::MenuInput(input));
            pad_hold(input);
        }
        break;
    }
    case SDL_CONTROLLERBUTTONUP:
        if (pad_input(event.cbutton.button) == menu.pad_held) {
            menu.pad_held = -1;
        }
        break;
    case SDL_MOUSEMOTION:
        menu.context->ProcessMouseMove(mouse_x(event.motion.x), mouse_x(event.motion.y), 0);
        break;
    case SDL_MOUSEBUTTONDOWN:
        menu.context->ProcessMouseMove(mouse_x(event.button.x), mouse_x(event.button.y), 0);
        if (event.button.button == SDL_BUTTON_RIGHT) {
            press(ui::MenuInput::back);
        } else if (event.button.button == SDL_BUTTON_LEFT) {
            menu.context->ProcessMouseButtonDown(0, 0);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (event.button.button == SDL_BUTTON_LEFT) {
            menu.context->ProcessMouseButtonUp(0, 0);
        }
        break;
    case SDL_MOUSEWHEEL:
        menu.context->ProcessMouseWheel(Rml::Vector2f(0.0f, float(-event.wheel.y)), 0);
        break;
    default:
        break;
    }
    return is_input(event.type);
}

extern "C" int port_player_ui_open(void) {
    return menu.ready && menu.open;
}

extern "C" int port_player_ui_visible(void) {
    return menu.ready && menu.open;
}

extern "C" int port_player_ui_show(const char *tab_id) {
    if (!menu.ready) {
        return 0;
    }
    if (tab_id == nullptr) {
        set_open(false);
        return 1;
    }
    // "graphics:down.down.confirm": the tab, then those presses
    std::string_view spec = tab_id;
    size_t colon = spec.find(':');
    int tab = menu.rows.tab_of(std::string(spec.substr(0, colon)).c_str());
    if (tab < 0) {
        return 0;
    }
    set_open(true);
    menu.model.open_at(tab);
    static const char *const names[] = {"up", "down", "left", "right", "confirm", "back", "next", "previous"};
    for (spec = colon == std::string_view::npos ? std::string_view() : spec.substr(colon + 1); !spec.empty();) {
        size_t end = spec.find('.');
        std::string_view name = spec.substr(0, end);
        for (size_t i = 0; i < std::size(names); i++) {
            if (name == names[i]) {
                press(ui::MenuInput(i));
            }
        }
        spec = end == std::string_view::npos ? std::string_view() : spec.substr(end + 1);
    }
    return 1;
}

extern "C" void port_player_ui_draw(int width, int height) {
    menu.drawn = false;
    if (!port_player_ui_visible() || width <= 0 || height <= 0) {
        return;
    }
    int units_w = 0, units_h = 0;
    SDL_GetWindowSize(menu.window, &units_w, &units_h);
    menu.pixels_per_unit = units_w > 0 ? float(width) / float(units_w) : 1.0f;
    // the menu's 1000 x 680 dp window fitted to the game's window, with a margin
    float dp = std::max(std::min(float(width) / 1060.0f, float(height) / 720.0f), 0.4f);

    pad_repeat();
    menu.recorder.begin_frame(width, height);
    menu.context->SetDimensions(Rml::Vector2i(width, height));
    menu.context->SetDensityIndependentPixelRatio(dp);
    write_page();
    menu.context->Update();
    scroll_to_focus();
    menu.context->Render();
    menu.drawn = true;
}
