// VK_KHR_synchronization2 on a driver without it: the calls on the system (pcr_sync2.h has the why and the choices)

#include "context.hpp" // (Granite's Vulkan headers before any other)
#include "pcr_sync2.h"

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pcr::sync2 {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
// The program's own file; empty when the system cannot say
fs::path program_file() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n == 0) {
            return {};
        }
        if (n < path.size()) {
            path.resize(n);
            return fs::path(path);
        }
        path.resize(path.size() * 2); // (cut short: a longer path than the buffer)
    }
}

// folder put first in the variable, before what it held (the loader reads it with GetEnvironmentVariable)
bool add_to_variable(const char *variable, const fs::path &folder) {
    std::wstring name(variable, variable + strlen(variable)); // (ASCII)
    std::wstring held;
    DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size > 0) {
        held.resize(size);
        DWORD n = GetEnvironmentVariableW(name.c_str(), held.data(), size);
        held.resize(n < size ? n : 0);
    }
    std::wstring value = layer_path_value(folder.wstring(), held.c_str(), L';');
    return SetEnvironmentVariableW(name.c_str(), value.c_str()) != 0;
}
#else
fs::path program_file() {
    std::error_code error;
    fs::path path = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path() : path;
}

bool add_to_variable(const char *variable, const fs::path &folder) {
    std::string value = layer_path_value(folder.native(), getenv(variable), ':');
    return setenv(variable, value.c_str(), 1) == 0;
}
#endif

// Granite's instance, made with the layer added to the layers it asked for
struct LayerInstanceFactory final : Vulkan::InstanceFactory {
    VkInstance create_instance(const VkInstanceCreateInfo *info) override {
        std::vector<const char *> layers(info->ppEnabledLayerNames, info->ppEnabledLayerNames + info->enabledLayerCount);
        layers.push_back(LAYER_NAME);
        VkInstanceCreateInfo with_layer = *info;
        with_layer.enabledLayerCount = uint32_t(layers.size());
        with_layer.ppEnabledLayerNames = layers.data();
        VkInstance instance = VK_NULL_HANDLE;
        VkResult result = vkCreateInstance(&with_layer, nullptr, &instance);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "[pcr] no Vulkan instance with %s (code %d)\n", LAYER_NAME, int(result));
            return VK_NULL_HANDLE;
        }
        return instance;
    }
};

// A GPU's device extensions (empty when the driver cannot list them)
std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice gpu) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return {};
    }
    extensions.resize(count);
    return extensions;
}

} // namespace

bool let_loader_find_layer() {
    fs::path program = program_file();
    if (program.empty()) {
        return false;
    }
    fs::path folder = program.parent_path() / LAYERS_FOLDER;
    std::error_code error;
    if (!fs::is_regular_file(folder / MANIFEST, error)) {
        return false;
    }
    if (!Vulkan::Context::init_loader(nullptr)) {
        return false;
    }
    const char *variable = layer_path_variable(volkGetInstanceVersion());
    if (!add_to_variable(variable, folder)) {
        fprintf(stderr, "[pcr] could not set %s for the Vulkan layers beside the game\n", variable);
        return false;
    }
    return true;
}

bool loader_has_layer() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    layers.resize(count);
    for (const VkLayerProperties &layer : layers) {
        if (std::strcmp(layer.layerName, LAYER_NAME) == 0) {
            return true;
        }
    }
    return false;
}

bool instance_needs_layer(VkInstance instance, uint32_t instance_version) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    VkResult listed = vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    if (listed != VK_SUCCESS && listed != VK_INCOMPLETE) {
        return false;
    }
    gpus.resize(count);
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gpu, &props);
        uint32_t usable = std::min(props.apiVersion, instance_version);
        if (gpu_needs_layer(usable, device_extensions(gpu))) {
            fprintf(stderr, "[pcr] %s (Vulkan %u.%u) has no VK_KHR_synchronization2 of its own\n", props.deviceName,
                    VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
            return true;
        }
    }
    return false;
}

Vulkan::InstanceFactory &layer_instance_factory() {
    static LayerInstanceFactory factory;
    return factory;
}

} // namespace pcr::sync2

namespace {

// AMD's switchable graphics layer (VK_LAYER_AMD_switchable_graphics, an implicit layer AMD's Windows driver installs on
// a computer with an AMD GPU and another one, as on laptops with a Ryzen and an NVIDIA card) hands the game a GPU the
// Vulkan loader does not know: the loader stops the program at the first question about it ("vkGetPhysicalDeviceProperties:
// Invalid physicalDevice", then a fast fail 0xC0000409; issue 268). The layer has its own variable that turns it off,
// set here unless the player set it. The GPU is then chosen by the loader's order and Granite's (a discrete GPU first).
void turn_off_broken_layers() {
#ifdef _WIN32
    const wchar_t *variable = L"DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1";
    if (GetEnvironmentVariableW(variable, nullptr, 0) > 0) {
        return;
    }
    if (!SetEnvironmentVariableW(variable, L"1")) {
        fprintf(stderr, "[pcr] could not turn off AMD's switchable graphics layer (error %lu)\n",
                static_cast<unsigned long>(GetLastError()));
    }
#endif
}

} // namespace

// (pcr.h) The main thread, at start
extern "C" void pcr_find_vulkan_layers(void) {
    turn_off_broken_layers();
    pcr::sync2::let_loader_find_layer();
}
