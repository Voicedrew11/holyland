// VK_KHR_synchronization2 on a driver without it.
//
// Granite (third_party/parallel-gs/Granite, vulkan/context.cpp) makes no device without synchronization2: Vulkan 1.3's
// core, an extension before it. An older driver has neither (a player's, 2026-11-22: Vulkan 1.2.170, from early 2021),
// and the game stopped with "KHR_synchronization2 is not supported. This is a hard requirement for Granite." Khronos's
// synchronization2 layer (third_party/vk-sync2-layer) adds the extension on top of the driver's older barriers and
// submits. It is built with the game into vk_layers/ beside the program (CMakeLists.txt) and enabled only on an instance
// with a GPU that needs it (pcr.cpp's make_context); elsewhere the game runs as it did without it.
//
// The choices are inline here, with no Vulkan calls (tests/pcr_sync2_test.cpp). The calls that make them on the system
// are declared at the end, in pcr_sync2.cpp.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES // (the functions are volk's, as Granite loads them)
#endif
#include <vulkan/vulkan_core.h>

namespace Vulkan {
struct InstanceFactory;
}

namespace pcr::sync2 {

constexpr const char *LAYER_NAME = "VK_LAYER_KHRONOS_synchronization2";
constexpr const char *LAYERS_FOLDER = "vk_layers"; // beside the program
constexpr const char *MANIFEST = "VkLayer_khronos_synchronization2.json";

// The first loader with VK_ADD_LAYER_PATH (Vulkan-Loader v1.3.207)
constexpr uint32_t ADD_LAYER_PATH_LOADER = VK_MAKE_API_VERSION(0, 1, 3, 207);

// The loader's variable that adds a folder of explicit layers. VK_ADD_LAYER_PATH keeps the loader's own folders. An older
// loader has only VK_LAYER_PATH, which is searched in their place: the system's explicit layers (validation) are then
// not found, which matters only to a developer on a loader from before 2022.
inline const char *layer_path_variable(uint32_t loader_version) {
    return loader_version >= ADD_LAYER_PATH_LOADER ? "VK_ADD_LAYER_PATH" : "VK_LAYER_PATH";
}

// The variable's new value: folder first, then what it held (nullptr or "": nothing), separated by the system's list
// separator (':' on Linux, ';' on Windows)
inline std::string layer_path_value(std::string_view folder, const char *held, char separator) {
    std::string value(folder);
    if (held != nullptr && held[0] != '\0') {
        value += separator;
        value += held;
    }
    return value;
}
// (Windows: its own wide strings, so that a folder in any language's letters reaches the loader)
inline std::wstring layer_path_value(std::wstring_view folder, const wchar_t *held, wchar_t separator) {
    std::wstring value(folder);
    if (held != nullptr && held[0] != L'\0') {
        value += separator;
        value += held;
    }
    return value;
}

// Whether a GPU needs the layer: below Vulkan 1.3 as the instance uses it (usable_version: the lower of the GPU's and
// the instance's) and without VK_KHR_synchronization2 among its extensions
inline bool gpu_needs_layer(uint32_t usable_version, std::span<const VkExtensionProperties> extensions) {
    if (usable_version >= VK_API_VERSION_1_3) {
        return false;
    }
    for (const VkExtensionProperties &e : extensions) {
        if (std::strcmp(e.extensionName, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0) {
            return false;
        }
    }
    return true;
}

// ---- On the system (pcr_sync2.cpp) ----

// (the main thread, at start, before any other thread: it writes the process's environment) When the layer's manifest
// is in LAYERS_FOLDER beside the program, lets the Vulkan loader find it (layer_path_variable). Loads the loader, to
// ask its version. False, changing nothing, without the manifest or the loader.
bool let_loader_find_layer();

// (the thread that opens the renderer) Whether the loader lists the layer: let_loader_find_layer found it
bool loader_has_layer();

// (the thread that opens the renderer) Whether a GPU of the instance needs the layer (gpu_needs_layer), instance_version
// being the version the instance was made with
bool instance_needs_layer(VkInstance instance, uint32_t instance_version);

// Granite's instance with the layer enabled as well (Context::set_instance_factory). It holds nothing and lives as
// long as the program.
Vulkan::InstanceFactory &layer_instance_factory();

} // namespace pcr::sync2
