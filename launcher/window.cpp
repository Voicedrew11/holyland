// The launcher's window (window.h)
#include "window.h"

#include <SDL.h>

#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "imgui.h"

namespace launcher {

namespace {

constexpr int kWidth = 720, kHeight = 480;

// the screen's scale over 96 dpi, for a readable window on a high-density screen
float display_scale() {
    float dpi = 96.0f;
    if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0 || dpi < 96.0f)
        return 1.0f;
    return dpi / 96.0f > 3.0f ? 3.0f : dpi / 96.0f;
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
    ImGui::GetIO().FontGlobalScale = scale * 1.25f;
}

} // namespace

std::unique_ptr<Window> Window::open(const std::string &title, std::string &why) {
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        why = SDL_GetError();
        return nullptr;
    }
    float scale = display_scale();
    SDL_Window *window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          (int)(kWidth * scale), (int)(kHeight * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        why = SDL_GetError();
        SDL_Quit();
        return nullptr;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (renderer == nullptr)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer == nullptr) {
        why = SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return nullptr;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // nothing of the launcher's window is kept
    apply_style(scale);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    return std::unique_ptr<Window>(new Window(window, renderer));
}

Window::Window(SDL_Window *window, SDL_Renderer *renderer) : window_(window), renderer_(renderer) {}

Window::~Window() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
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
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                open = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window_))
                open = false;
        } while (SDL_PollEvent(&event));
    }
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    return open;
}

void Window::end_frame() {
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer_, 18, 20, 18, 255);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
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
