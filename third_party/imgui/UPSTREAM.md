# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, release `v1.92.9b`, copied from the
release's source archive. MIT licence: see `LICENSE.txt`.

It draws the settings overlay (`port/overlay/overlay.cpp`, F1) over the game's picture.

| file | what it is |
| --- | --- |
| `imgui*.cpp`, `imgui*.h`, `imconfig.h`, `imstb_*.h` | the library |
| `backends/imgui_impl_sdl3.*` | the platform backend: SDL3's events, the mouse, the keyboard and game controllers |
| `backends/imgui_impl_sdlrenderer3.*` | the renderer backend: draws with the window's `SDL_Renderer` (the launcher, and the game without Vulkan) |
| `backends/imgui_impl_vulkan.*` | the renderer backend of the Vulkan presenter (`port/platform/present_vk.cpp`, `--presenter vulkan`), built with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` (its functions are loaded from the native GS's Vulkan instance); copied from the same release |

Not copied: the demo, the examples, the misc folder and the other backends.

Keep these files as they are upstream, so they can be updated by copying a newer release over them.
Darker Reality's own code goes in `port/overlay/`.
