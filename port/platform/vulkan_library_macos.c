/* macOS: which Vulkan library the game loads (docs/port/MACOS.md). Built on Apple only (CMakeLists.txt).

   macOS has no Vulkan of its own: it is MoltenVK (Vulkan on Metal), through the Vulkan loader (libvulkan.1.dylib:
   the SDK's or Homebrew's, which finds MoltenVK's ICD) or on its own (libMoltenVK.dylib). Granite (the device,
   GRANITE_VULKAN_LIBRARY) and SDL (the window's surface, SDL_VULKAN_LIBRARY) each load it by themselves, from where
   neither looks: the default search path holds neither. They must load the same one, so before either does, the
   first that exists is given to both: beside the program (a release carries libMoltenVK.dylib there), then
   Homebrew's and /usr/local's loader, then their MoltenVK. Either variable already set is used for both. */
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "port/platform.h"

void port_vulkan_choose_library(void) {
    const char *granite = getenv("GRANITE_VULKAN_LIBRARY");
    const char *sdl = getenv("SDL_VULKAN_LIBRARY");
    char chosen[PATH_MAX] = "";
    char here[PATH_MAX];
    uint32_t size = sizeof(here);

    if (granite != NULL && *granite) {
        snprintf(chosen, sizeof(chosen), "%s", granite);
    } else if (sdl != NULL && *sdl) {
        snprintf(chosen, sizeof(chosen), "%s", sdl);
    } else {
        static const char *const beside[] = {"libMoltenVK.dylib", "libvulkan.1.dylib"};
        static const char *const system[] = {"/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib",
                                             "/opt/homebrew/lib/libMoltenVK.dylib", "/usr/local/lib/libMoltenVK.dylib"};
        size_t i;

        if (_NSGetExecutablePath(here, &size) == 0) {
            char *slash = strrchr(here, '/');

            for (i = 0; slash != NULL && i < sizeof(beside) / sizeof(beside[0]) && !chosen[0]; i++) {
                char path[PATH_MAX];

                snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - here), here, beside[i]);
                if (access(path, R_OK) == 0) {
                    snprintf(chosen, sizeof(chosen), "%s", path);
                }
            }
        }
        for (i = 0; i < sizeof(system) / sizeof(system[0]) && !chosen[0]; i++) {
            if (access(system[i], R_OK) == 0) {
                snprintf(chosen, sizeof(chosen), "%s", system[i]);
            }
        }
    }
    if (!chosen[0]) {
        fprintf(stderr, "[port] no Vulkan library found (libMoltenVK.dylib or libvulkan.1.dylib): brew install "
                        "molten-vk, or set GRANITE_VULKAN_LIBRARY\n");
        return;
    }
    setenv("GRANITE_VULKAN_LIBRARY", chosen, 1);
    setenv("SDL_VULKAN_LIBRARY", chosen, 1);
    fprintf(stderr, "[port] Vulkan: %s\n", chosen);
}
