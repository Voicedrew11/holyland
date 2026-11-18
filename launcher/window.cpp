// The launcher's window (window.h)
#include "window.h"

#include <SDL3/SDL.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

namespace launcher {

namespace {

constexpr int kWidth = 720, kHeight = 480;

// the screen's content scale (150% is 1.5), for a readable window on a high-density screen
float display_scale() {
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale < 1.0f)
        return 1.0f;
    return scale > 3.0f ? 3.0f : scale;
}

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 10);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.07f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.30f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.42f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.52f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.45f, 0.70f, 0.45f, 1.0f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontScaleMain = 1.25f;
}

} // namespace

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
    SDL_SetRenderVSync(renderer, 1);
    apply_style(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    return std::unique_ptr<Window>(new Window(window, renderer));
}

Window::Window(SDL_Window *window, SDL_Renderer *renderer) : window_(window), renderer_(renderer) {}

Window::~Window() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool Window::begin_frame() {
    SDL_Event event;
    bool open = true;
    // idle while nothing happens; the build's progress still shows within a tenth of a second
    if (SDL_WaitEventTimeout(&event, 100)) {
        do {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                open = false;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_))
                open = false;
        } while (SDL_PollEvent(&event));
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return open;
}

void Window::end_frame() {
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer_, 18, 20, 18, 255);
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
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);
}

void Window::end_panel() {
    ImGui::End();
}

} // namespace launcher
