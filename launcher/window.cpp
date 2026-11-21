// The launcher's window (window.h)
#include "window.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <mutex>
#include <vector>

#include "alpha_mask.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "ui_assets.hpp"

namespace launcher {

namespace {

constexpr int kWidth = 900, kHeight = 660;
constexpr float kTextPx = 22.0f; // (the menu's 17dp is for a full screen; a small window needs larger)

// the screen's content scale (150% is 1.5), for a readable window on a high-density screen
float display_scale() {
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale < 1.0f)
        return 1.0f;
    return scale > 3.0f ? 3.0f : scale;
}

ImVec4 rgb(unsigned hex) {
    return ImVec4(float((hex >> 16) & 0xff) / 255.0f, float((hex >> 8) & 0xff) / 255.0f, float(hex & 0xff) / 255.0f, 1.0f);
}

// kf4.rcss's colours: charcoal windows, iron frames, bone-white text, one accent (amber)
void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.FramePadding = ImVec2(14, 8);
    style.ItemSpacing = ImVec2(10, 10);
    style.WindowPadding = ImVec2(24, 18);
    ImVec4 *colours = style.Colors;
    colours[ImGuiCol_Text] = rgb(0xe2dccf);
    colours[ImGuiCol_TextDisabled] = rgb(0x5e574c);
    colours[ImGuiCol_WindowBg] = rgb(0x0f0d0c);
    colours[ImGuiCol_ChildBg] = rgb(0x0a0908);
    colours[ImGuiCol_PopupBg] = rgb(0x13110f);
    colours[ImGuiCol_MenuBarBg] = rgb(0x181512);
    colours[ImGuiCol_Border] = rgb(0x2a2620);
    colours[ImGuiCol_FrameBg] = rgb(0x181512);
    colours[ImGuiCol_FrameBgHovered] = rgb(0x221e19);
    colours[ImGuiCol_FrameBgActive] = rgb(0x2a2620);
    colours[ImGuiCol_Button] = rgb(0x221e19);
    colours[ImGuiCol_ButtonHovered] = rgb(0x3a3227);
    colours[ImGuiCol_ButtonActive] = rgb(0x5a4a2e);
    colours[ImGuiCol_Header] = rgb(0x2a2620);
    colours[ImGuiCol_HeaderHovered] = rgb(0x3a3227);
    colours[ImGuiCol_HeaderActive] = rgb(0x5a4a2e);
    colours[ImGuiCol_CheckMark] = kAmber;
    colours[ImGuiCol_PlotHistogram] = kAmber;
    colours[ImGuiCol_TitleBg] = rgb(0x13110f);
    colours[ImGuiCol_TitleBgActive] = rgb(0x221e19);
    colours[ImGuiCol_NavCursor] = kAmber;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// a font carried in the launcher (port/ui/res/fonts/), or nullptr
ImFont *add_font(const char *path) {
    const UiAsset *asset = ui_asset(path);
    if (asset == nullptr)
        return nullptr;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false; // the launcher's own array, for the whole run (ImGui only reads it)
    std::snprintf(config.Name, sizeof(config.Name), "%s", path);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(asset->data), int(asset->size),
                                                      kTextPx, &config);
}

} // namespace

// Written by SDL's callback, on whatever thread SDL calls it; read by the main thread. SDL keeps the filters'
// pointers until it calls back, so they are kept here, which lives that long.
struct Window::ChooserAnswer {
    std::mutex mutex;
    bool open = false;
    bool failed = false;
    std::optional<std::filesystem::path> file;
    std::string what, extensions;
    SDL_DialogFileFilter filters[2] = {};
};

std::unique_ptr<Window> Window::open(const std::string &title, std::string &why) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        why = SDL_GetError();
        return nullptr;
    }
    float scale = display_scale();
    // (as Dear ImGui's SDL3 example: the screen's own pixels where it has more, sizes scaled by the content scale)
    SDL_Window *window = SDL_CreateWindow(title.c_str(), (int)(kWidth * scale), (int)(kHeight * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        why = SDL_GetError();
        SDL_Quit();
        return nullptr;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr)
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (renderer == nullptr) {
        why = SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return nullptr;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // nothing of the launcher's window is kept
    if (!SDL_SetRenderVSync(renderer, 1))
        std::fprintf(stderr, "[launcher] no vsync for the window: %s\n", SDL_GetError());
    apply_style(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    std::unique_ptr<Window> opened(new Window(window, renderer, scale));
    opened->load_look();
    return opened;
}

Window::Window(SDL_Window *window, SDL_Renderer *renderer, float scale)
    : window_(window), renderer_(renderer), scale_(scale), chooser_(std::make_shared<ChooserAnswer>()) {}

// The fonts (Spectral for text, Cinzel for titles) and the logo, coloured amber; without them the window still
// works, in ImGui's own font and with no logo
void Window::load_look() {
    if (add_font("fonts/Spectral-Regular.ttf") == nullptr)
        ImGui::GetIO().Fonts->AddFontDefault();
    title_font_ = add_font("fonts/Cinzel-Regular.ttf");
    if (title_font_ == nullptr)
        title_font_ = ImGui::GetIO().Fonts->Fonts[0];

    const UiAsset *asset = ui_asset("logo.alpha");
    auto mask = asset ? read_alpha_mask(std::string_view(reinterpret_cast<const char *>(asset->data), asset->size))
                      : std::nullopt;
    if (!mask) {
        std::fprintf(stderr, "[launcher] the logo's mask is missing or broken\n");
        return;
    }
    std::vector<unsigned char> pixels(std::size_t(mask->width) * std::size_t(mask->height) * 4);
    const unsigned char amber[3] = {0xd4, 0xa9, 0x5a};
    for (std::size_t i = 0; i < std::size_t(mask->width) * std::size_t(mask->height); i++) {
        pixels[i * 4 + 0] = amber[0];
        pixels[i * 4 + 1] = amber[1];
        pixels[i * 4 + 2] = amber[2];
        pixels[i * 4 + 3] = mask->alpha[i];
    }
    logo_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, mask->width, mask->height);
    if (logo_ == nullptr) {
        std::fprintf(stderr, "[launcher] no texture for the logo: %s\n", SDL_GetError());
        return;
    }
    if (!SDL_UpdateTexture(logo_, nullptr, pixels.data(), mask->width * 4) ||
        !SDL_SetTextureBlendMode(logo_, SDL_BLENDMODE_BLEND) || !SDL_SetTextureScaleMode(logo_, SDL_SCALEMODE_LINEAR)) {
        std::fprintf(stderr, "[launcher] the logo's texture: %s\n", SDL_GetError());
        SDL_DestroyTexture(logo_);
        logo_ = nullptr;
        return;
    }
    logo_width_ = mask->width;
    logo_height_ = mask->height;
}

Window::~Window() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (logo_ != nullptr)
        SDL_DestroyTexture(logo_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool Window::begin_frame() {
    SDL_Event event;
    bool open = true;
    // idle while nothing happens; the build's progress and the chooser's answer still show within a tenth of a second
    if (SDL_WaitEventTimeout(&event, 100)) {
        do {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                open = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == SDL_GetWindowID(window_))
                    open = false;
                break;
            // a drag over the window: begun, moving, then dropped or gone (complete)
            case SDL_EVENT_DROP_BEGIN:
            case SDL_EVENT_DROP_POSITION:
                file_over_ = true;
                break;
            case SDL_EVENT_DROP_FILE:
                file_over_ = false;
                if (event.drop.data != nullptr && dropped_ == std::nullopt)
                    dropped_ = std::filesystem::u8path(event.drop.data); // (the first of several)
                break;
            case SDL_EVENT_DROP_COMPLETE:
                file_over_ = false;
                break;
            default:
                break;
            }
        } while (SDL_PollEvent(&event));
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return open;
}

void Window::end_frame() {
    ImGui::Render();
    // ImGui draws in the window's size; a screen scaled by the desktop (115% on Wayland) has more pixels than that:
    // SDL scales the drawing up to them, or it covers only part of the window (as ImGui's SDL3 example)
    const ImVec2 pixels_per_point = ImGui::GetIO().DisplayFramebufferScale;
    if (!SDL_SetRenderScale(renderer_, pixels_per_point.x, pixels_per_point.y))
        std::fprintf(stderr, "[launcher] the window's scale: %s\n", SDL_GetError());
    SDL_SetRenderDrawColor(renderer_, 15, 13, 12, 255);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    SDL_RenderPresent(renderer_);
}

void Window::begin_panel() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
}

void Window::end_panel() {
    ImGui::End();
}

void Window::draw_logo(float width_dp) {
    if (logo_ == nullptr)
        return;
    float width = width_dp * scale_;
    float available = ImGui::GetContentRegionAvail().x;
    if (width > available)
        width = available;
    float height = width * float(logo_height_) / float(logo_width_);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - width) * 0.5f);
    ImGui::Image(ImTextureRef((ImTextureID)(intptr_t)logo_), ImVec2(width, height));
}

// SDL's callback with the chooser's answer: the chosen files, an empty list when the player chose none, or
// nullptr when the chooser could not open. The answer outlives the window if SDL calls this late.
void Window::chooser_done(void *answer, const char *const *files, int) {
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

void Window::choose_file(const char *what, const char *extensions) {
    {
        std::lock_guard<std::mutex> lock(chooser_->mutex);
        if (chooser_->open)
            return;
        chooser_->open = true;
        chooser_->what = what;
        chooser_->extensions = extensions;
        chooser_->filters[0] = SDL_DialogFileFilter{chooser_->what.c_str(), chooser_->extensions.c_str()};
        chooser_->filters[1] = SDL_DialogFileFilter{"All files", "*"};
    }
    // (unlocked: where SDL answers at once, on this thread, its callback takes the lock.) SDL holds a copy of the
    // shared answer; if it never calls back (the program ends with the chooser open) that copy lasts to the end.
    auto *for_sdl = new std::shared_ptr<ChooserAnswer>(chooser_);
    SDL_ShowOpenFileDialog(&Window::chooser_done, for_sdl, window_, chooser_->filters, 2, SDL_getenv("HOME"), false);
}

bool Window::choosing_file() const {
    std::lock_guard<std::mutex> lock(chooser_->mutex);
    return chooser_->open;
}

std::optional<std::filesystem::path> Window::take_file() {
    if (dropped_) {
        auto file = std::move(dropped_);
        dropped_.reset();
        return file;
    }
    std::lock_guard<std::mutex> lock(chooser_->mutex);
    auto file = std::move(chooser_->file);
    chooser_->file.reset();
    return file;
}

bool Window::take_chooser_failure() {
    std::lock_guard<std::mutex> lock(chooser_->mutex);
    bool failed = chooser_->failed;
    chooser_->failed = false;
    return failed;
}

} // namespace launcher
