// The launcher's window (window.h)
#include "window.h"

#include <SDL3/SDL.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "rml_text.h"
#include "ui_frame.hpp"
#include "ui_render.hpp"
#include "ui_ties.hpp"

namespace launcher {

namespace {

constexpr int kWidth = 900, kHeight = 660; // in the screen's units (its content scale: 150% makes it larger)
constexpr Uint32 kIdleWaitMs = 100;        // the build's progress and the chooser's answer still show this soon
constexpr int kShotFrame = 10;             // KF4_LAUNCHER_SHOT: the frame saved, once the page has settled

// KF4_LAUNCHER_SHOT=FILE.bmp (tests, documentation): the window's picture at its tenth frame, saved to FILE
void save_shot(SDL_Renderer *renderer, int frame) {
    const char *file = SDL_getenv("KF4_LAUNCHER_SHOT");
    if (file == nullptr || *file == 0 || frame != kShotFrame)
        return;
    SDL_Surface *picture = SDL_RenderReadPixels(renderer, nullptr);
    if (picture == nullptr || !SDL_SaveBMP(picture, file))
        std::fprintf(stderr, "[launcher] KF4_LAUNCHER_SHOT: %s\n", SDL_GetError());
    else
        std::fprintf(stderr, "[launcher] saved the window's picture to %s\n", file);
    SDL_DestroySurface(picture);
}

// the screen's content scale (150% is 1.5), for a readable window on a high-density screen
float display_scale() {
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale < 1.0f)
        return 1.0f;
    return scale > 3.0f ? 3.0f : scale;
}

// SDL's keys that RmlUi's text fields and focus use
Rml::Input::KeyIdentifier rml_key(SDL_Keycode key) {
    struct Key {
        SDL_Keycode sdl;
        Rml::Input::KeyIdentifier rml;
    };
    static const Key kKeys[] = {
        {SDLK_BACKSPACE, Rml::Input::KI_BACK}, {SDLK_DELETE, Rml::Input::KI_DELETE}, {SDLK_LEFT, Rml::Input::KI_LEFT},
        {SDLK_RIGHT, Rml::Input::KI_RIGHT},    {SDLK_UP, Rml::Input::KI_UP},         {SDLK_DOWN, Rml::Input::KI_DOWN},
        {SDLK_HOME, Rml::Input::KI_HOME},      {SDLK_END, Rml::Input::KI_END},       {SDLK_RETURN, Rml::Input::KI_RETURN},
        {SDLK_KP_ENTER, Rml::Input::KI_RETURN}, {SDLK_TAB, Rml::Input::KI_TAB},      {SDLK_ESCAPE, Rml::Input::KI_ESCAPE},
        {SDLK_A, Rml::Input::KI_A},            {SDLK_C, Rml::Input::KI_C},           {SDLK_V, Rml::Input::KI_V},
        {SDLK_X, Rml::Input::KI_X},            {SDLK_Z, Rml::Input::KI_Z},
    };
    for (const Key &k : kKeys)
        if (k.sdl == key)
            return k.rml;
    return Rml::Input::KI_UNKNOWN;
}

int rml_modifiers(SDL_Keymod mod) {
    int out = 0;
    if (mod & SDL_KMOD_CTRL)
        out |= Rml::Input::KM_CTRL;
    if (mod & SDL_KMOD_SHIFT)
        out |= Rml::Input::KM_SHIFT;
    if (mod & SDL_KMOD_ALT)
        out |= Rml::Input::KM_ALT;
    if (mod & SDL_KMOD_GUI)
        out |= Rml::Input::KM_META;
    return out;
}

// the value of `name` on the element or its nearest parent that has it, or ""
std::string attribute_up(Rml::Element *element, const char *name) {
    for (; element != nullptr; element = element->GetParentNode())
        if (const Rml::Variant *value = element->GetAttribute(name))
            return value->Get<Rml::String>();
    return {};
}

// Clicks on data-action elements, and Enter in data-enter fields, as the frame's actions
class Actions final : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event &event) override {
        Rml::Element *target = event.GetTargetElement();
        if (event.GetId() == Rml::EventId::Click) {
            std::string action = attribute_up(target, "data-action");
            if (!action.empty())
                taken_.push_back(std::move(action));
            return;
        }
        if (event.GetId() == Rml::EventId::Keydown &&
            event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_RETURN) {
            std::string action = attribute_up(target, "data-enter");
            if (!action.empty())
                taken_.push_back(std::move(action));
        }
    }
    void clear() { taken_.clear(); }
    bool take(std::string_view action) {
        for (auto it = taken_.begin(); it != taken_.end(); ++it) {
            if (*it == action) {
                taken_.erase(it);
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> taken_;
};

// Written by SDL's callback, on whatever thread SDL calls it; read by the main thread. SDL keeps the filters'
// pointers until it calls back, so they are kept here, which lives that long.
struct ChooserAnswer {
    std::mutex mutex;
    bool open = false;
    bool failed = false;
    std::optional<std::filesystem::path> file;
    std::string what, extensions;
    SDL_DialogFileFilter filters[2] = {};
};

// SDL's callback with the chooser's answer: the chosen files, an empty list when the player chose none, or
// nullptr when the chooser could not open. The answer outlives the window if SDL calls this late.
void chooser_done(void *answer, const char *const *files, int) {
    auto *kept = static_cast<std::shared_ptr<ChooserAnswer> *>(answer);
    {
        std::lock_guard<std::mutex> lock((*kept)->mutex);
        (*kept)->open = false;
        if (files == nullptr) {
            std::fprintf(stderr, "[launcher] the system's file chooser: %s\n", SDL_GetError());
            (*kept)->failed = true;
        } else if (files[0] != nullptr) {
            (*kept)->file = std::filesystem::u8path(files[0]);
        }
    }
    delete kept; // (the one owner of this copy: SDL calls the callback once per chooser)
}

} // namespace

struct Window::State {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool rml_started = false;
    ui::SystemTies system;
    ui::AssetFiles files;
    ui::Recorder recorder;
    Rml::Context *context = nullptr;
    Actions actions;
    std::map<std::string, Rml::ElementDocument *> pages;
    Rml::ElementDocument *page = nullptr;
    std::string page_name;
    std::unordered_map<std::string, std::string> given; // "page|id|kind": what each part was last given
    std::unordered_map<ui::TextureId, SDL_Texture *> textures;
    int frames_drawn = 0;
    bool file_over = false;
    std::optional<std::filesystem::path> dropped;
    std::shared_ptr<ChooserAnswer> chooser = std::make_shared<ChooserAnswer>();

    State() = default;
    ~State();
    State(const State &) = delete;
    State &operator=(const State &) = delete;

    Rml::Element *part(const char *id) { return page != nullptr ? page->GetElementById(id) : nullptr; }
    // true when `value` differs from what this part was last given (and now it has been)
    bool changed(const char *id, const char *kind, const std::string &value);
    void process(const SDL_Event &event);
    void draw(const ui::Frame &frame);
};

Window::State::~State() {
    if (rml_started)
        Rml::Shutdown(); // (releases RmlUi's textures into the recorder: the SDL ones go below)
    for (auto &[id, texture] : textures)
        SDL_DestroyTexture(texture);
    if (renderer != nullptr)
        SDL_DestroyRenderer(renderer);
    if (window != nullptr) {
        SDL_StopTextInput(window);
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

bool Window::State::changed(const char *id, const char *kind, const std::string &value) {
    std::string key = page_name + "|" + id + "|" + kind;
    auto found = given.find(key);
    if (found != given.end() && found->second == value)
        return false;
    given[key] = value;
    return true;
}

std::unique_ptr<Window> Window::open(const std::string &title, std::string &why) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        why = SDL_GetError();
        return nullptr;
    }
    auto state = std::make_unique<State>();
    float scale = display_scale();
    state->window = SDL_CreateWindow(title.c_str(), int(kWidth * scale), int(kHeight * scale),
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (state->window == nullptr) {
        why = SDL_GetError();
        return nullptr;
    }
    state->renderer = SDL_CreateRenderer(state->window, nullptr);
    if (state->renderer == nullptr)
        state->renderer = SDL_CreateRenderer(state->window, SDL_SOFTWARE_RENDERER);
    if (state->renderer == nullptr) {
        why = SDL_GetError();
        return nullptr;
    }
    if (!SDL_SetRenderVSync(state->renderer, 1))
        std::fprintf(stderr, "[launcher] no vsync for the window: %s\n", SDL_GetError());
    if (!SDL_StartTextInput(state->window))
        std::fprintf(stderr, "[launcher] no text input: %s\n", SDL_GetError());

    Rml::SetSystemInterface(&state->system);
    Rml::SetFileInterface(&state->files);
    Rml::SetRenderInterface(&state->recorder);
    if (!Rml::Initialise()) {
        why = "RmlUi did not start";
        return nullptr;
    }
    state->rml_started = true;
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(state->window, &width, &height)) {
        width = kWidth;
        height = kHeight;
    }
    state->context = Rml::CreateContext("launcher", Rml::Vector2i(width, height));
    if (state->context == nullptr || !ui::load_fonts()) {
        why = "RmlUi's context or fonts did not load";
        return nullptr;
    }
    return std::unique_ptr<Window>(new Window(std::move(state)));
}

Window::Window(std::unique_ptr<State> state) : state_(std::move(state)) {}

Window::~Window() = default;

void Window::State::process(const SDL_Event &event) {
    float density = SDL_GetWindowPixelDensity(window); // (pointer positions are in the window's units)
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        context->ProcessMouseMove(int(event.motion.x * density), int(event.motion.y * density), 0);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        context->ProcessMouseMove(int(event.button.x * density), int(event.button.y * density), 0);
        int button = event.button.button == SDL_BUTTON_LEFT ? 0 : event.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            context->ProcessMouseButtonDown(button, 0);
        else
            context->ProcessMouseButtonUp(button, 0);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        context->ProcessMouseWheel(Rml::Vector2f(-event.wheel.x, -event.wheel.y), 0);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        context->ProcessMouseLeave();
        break;
    case SDL_EVENT_KEY_DOWN:
        context->ProcessKeyDown(rml_key(event.key.key), rml_modifiers(event.key.mod));
        break;
    case SDL_EVENT_KEY_UP:
        context->ProcessKeyUp(rml_key(event.key.key), rml_modifiers(event.key.mod));
        break;
    case SDL_EVENT_TEXT_INPUT:
        context->ProcessTextInput(Rml::String(event.text.text));
        break;
    // a drag over the window: begun, moving, then dropped or gone (complete)
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_POSITION:
        file_over = true;
        break;
    case SDL_EVENT_DROP_FILE:
        file_over = false;
        if (event.drop.data != nullptr && !dropped)
            dropped = std::filesystem::u8path(event.drop.data); // (the first of several)
        break;
    case SDL_EVENT_DROP_COMPLETE:
        file_over = false;
        break;
    default:
        break;
    }
}

bool Window::begin_frame() {
    State &s = *state_;
    s.actions.clear();
    SDL_Event event;
    bool open = true;
    if (SDL_WaitEventTimeout(&event, kIdleWaitMs)) {
        do {
            if (event.type == SDL_EVENT_QUIT)
                open = false;
            else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(s.window))
                open = false;
            else
                s.process(event);
        } while (SDL_PollEvent(&event));
    }
    return open;
}

void Window::end_frame() {
    State &s = *state_;
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(s.window, &width, &height) || width <= 0 || height <= 0)
        return; // (minimised)
    float dp = SDL_GetWindowDisplayScale(s.window);
    s.recorder.begin_frame(width, height);
    s.context->SetDimensions(Rml::Vector2i(width, height));
    s.context->SetDensityIndependentPixelRatio(dp > 0.0f ? dp : 1.0f);
    s.context->Update();
    s.context->Render();
    s.recorder.end_frame();
    s.draw(s.recorder.frame());
}

// RmlUi's recorded frame (port/ui/ui_frame.hpp) on SDL's renderer, in the window's pixels: its new textures made,
// its triangles drawn (colours and textures premultiplied), its textures no longer used let go
void Window::State::draw(const ui::Frame &frame) {
    for (const ui::TextureUpload &upload : frame.uploads) {
        SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                                 upload.width, upload.height);
        if (texture == nullptr || !SDL_UpdateTexture(texture, nullptr, upload.rgba.data(), upload.width * 4) ||
            !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED) ||
            !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR)) {
            std::fprintf(stderr, "[launcher] a texture of the window: %s\n", SDL_GetError());
            if (texture != nullptr)
                SDL_DestroyTexture(texture);
            continue;
        }
        textures[upload.id] = texture;
    }
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderDrawColor(renderer, 15, 13, 12, 255); // (launcher.rcss: the window's charcoal)
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    std::vector<float> xy;
    std::vector<SDL_FColor> colours;
    std::vector<float> uv;
    for (const ui::Draw &draw : frame.draws) {
        SDL_Texture *texture = nullptr;
        if (draw.texture != 0) {
            auto found = textures.find(draw.texture);
            if (found == textures.end())
                continue;
            texture = found->second;
        }
        if (draw.index_count == 0 || std::size_t(draw.first_index) + draw.index_count > frame.indices.size())
            continue;
        // the draw's vertices: those its indices reach, from vertex_offset (indices are relative to it)
        std::uint32_t highest = 0;
        for (std::uint32_t i = 0; i < draw.index_count; i++)
            highest = std::max(highest, frame.indices[draw.first_index + i]);
        std::size_t count = std::size_t(highest) + 1;
        if (draw.vertex_offset + count > frame.vertices.size())
            continue;
        xy.resize(count * 2);
        colours.resize(count);
        uv.resize(count * 2);
        for (std::size_t i = 0; i < count; i++) {
            const ui::Vertex &v = frame.vertices[draw.vertex_offset + i];
            xy[i * 2] = v.x + draw.translate_x;
            xy[i * 2 + 1] = v.y + draw.translate_y;
            colours[i] = SDL_FColor{float(v.r) / 255.0f, float(v.g) / 255.0f, float(v.b) / 255.0f, float(v.a) / 255.0f};
            uv[i * 2] = v.u;
            uv[i * 2 + 1] = v.v;
        }
        if (draw.clipped) {
            SDL_Rect clip{draw.clip.x, draw.clip.y, draw.clip.width, draw.clip.height};
            SDL_SetRenderClipRect(renderer, &clip);
        } else {
            SDL_SetRenderClipRect(renderer, nullptr);
        }
        if (!SDL_RenderGeometryRaw(renderer, texture, xy.data(), sizeof(float) * 2, colours.data(), sizeof(SDL_FColor),
                                   uv.data(), sizeof(float) * 2, int(count), &frame.indices[draw.first_index],
                                   int(draw.index_count), sizeof(std::uint32_t)))
            std::fprintf(stderr, "[launcher] drawing the window: %s\n", SDL_GetError());
    }
    SDL_SetRenderClipRect(renderer, nullptr);
    save_shot(renderer, ++frames_drawn);
    SDL_RenderPresent(renderer);
    for (ui::TextureId id : frame.releases) {
        auto found = textures.find(id);
        if (found != textures.end()) {
            SDL_DestroyTexture(found->second);
            textures.erase(found);
        }
    }
}

void Window::show_page(const char *document) {
    State &s = *state_;
    if (s.page_name == document)
        return;
    Rml::ElementDocument *&page = s.pages[document];
    if (page == nullptr) {
        page = s.context->LoadDocument(document);
        if (page == nullptr) {
            std::fprintf(stderr, "[launcher] the page %s did not load\n", document);
            return;
        }
        page->AddEventListener(Rml::EventId::Click, &s.actions);
        page->AddEventListener(Rml::EventId::Keydown, &s.actions);
    }
    if (s.page != nullptr)
        s.page->Hide();
    page->Show();
    s.page = page;
    s.page_name = document;
}

void Window::set_rml(const char *id, const std::string &rml) {
    Rml::Element *element = state_->part(id);
    if (element != nullptr && state_->changed(id, "rml", rml))
        element->SetInnerRML(rml);
}

void Window::set_text(const char *id, std::string_view text) {
    set_rml(id, rml_escaped(text));
}

void Window::set_classes(const char *id, const std::string &classes) {
    Rml::Element *element = state_->part(id);
    if (element != nullptr && state_->changed(id, "class", classes))
        element->SetClassNames(classes);
}

void Window::scroll_to_end(const char *id) {
    if (Rml::Element *element = state_->part(id))
        element->SetScrollTop(element->GetScrollHeight());
}

std::string Window::field(const char *id) {
    auto *control = dynamic_cast<Rml::ElementFormControl *>(state_->part(id));
    return control != nullptr ? control->GetValue() : std::string();
}

void Window::set_field(const char *id, const std::string &value) {
    if (auto *control = dynamic_cast<Rml::ElementFormControl *>(state_->part(id)))
        control->SetValue(value);
}

bool Window::take_action(std::string_view action) {
    return state_->actions.take(action);
}

void Window::choose_file(const char *what, const char *extensions) {
    std::shared_ptr<ChooserAnswer> &chooser = state_->chooser;
    {
        std::lock_guard<std::mutex> lock(chooser->mutex);
        if (chooser->open)
            return;
        chooser->open = true;
        chooser->what = what;
        chooser->extensions = extensions;
        chooser->filters[0] = SDL_DialogFileFilter{chooser->what.c_str(), chooser->extensions.c_str()};
        chooser->filters[1] = SDL_DialogFileFilter{"All files", "*"};
    }
    // (unlocked: where SDL answers at once, on this thread, its callback takes the lock.) SDL holds a copy of the
    // shared answer; if it never calls back (the program ends with the chooser open) that copy lasts to the end.
    auto *for_sdl = new std::shared_ptr<ChooserAnswer>(chooser);
    SDL_ShowOpenFileDialog(&chooser_done, for_sdl, state_->window, chooser->filters, 2, SDL_getenv("HOME"), false);
}

bool Window::choosing_file() const {
    std::lock_guard<std::mutex> lock(state_->chooser->mutex);
    return state_->chooser->open;
}

bool Window::file_over() const {
    return state_->file_over;
}

std::optional<std::filesystem::path> Window::take_file() {
    if (state_->dropped) {
        auto file = std::move(state_->dropped);
        state_->dropped.reset();
        return file;
    }
    std::lock_guard<std::mutex> lock(state_->chooser->mutex);
    auto file = std::move(state_->chooser->file);
    state_->chooser->file.reset();
    return file;
}

bool Window::take_chooser_failure() {
    std::lock_guard<std::mutex> lock(state_->chooser->mutex);
    bool failed = state_->chooser->failed;
    state_->chooser->failed = false;
    return failed;
}

} // namespace launcher
