// The synchronization2 layer's choices (port/gs/pc/pcr_sync2.h): which loader variable lets the loader find the layer,
// the variable's value, and which GPUs need the layer.
#include "pcr_sync2.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pcr::sync2;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

VkExtensionProperties extension(const char *name) {
    VkExtensionProperties e{};
    std::snprintf(e.extensionName, sizeof e.extensionName, "%s", name);
    e.specVersion = 1;
    return e;
}

void variable_by_loader() {
    // the player's loader, 2026-11-22
    CHECK(std::strcmp(layer_path_variable(VK_MAKE_API_VERSION(0, 1, 2, 170)), "VK_LAYER_PATH") == 0);
    CHECK(std::strcmp(layer_path_variable(VK_MAKE_API_VERSION(0, 1, 3, 206)), "VK_LAYER_PATH") == 0);
    CHECK(std::strcmp(layer_path_variable(VK_MAKE_API_VERSION(0, 1, 3, 207)), "VK_ADD_LAYER_PATH") == 0);
    CHECK(std::strcmp(layer_path_variable(VK_MAKE_API_VERSION(0, 1, 4, 341)), "VK_ADD_LAYER_PATH") == 0);
    // volk's answer for a loader without vkEnumerateInstanceVersion: 1.0
    CHECK(std::strcmp(layer_path_variable(VK_API_VERSION_1_0), "VK_LAYER_PATH") == 0);
}

void value_keeps_what_was_there() {
    CHECK(layer_path_value("/opt/kf4/vk_layers", nullptr, ':') == "/opt/kf4/vk_layers");
    CHECK(layer_path_value("/opt/kf4/vk_layers", "", ':') == "/opt/kf4/vk_layers");
    CHECK(layer_path_value("/opt/kf4/vk_layers", "/home/a/layers", ':') == "/opt/kf4/vk_layers:/home/a/layers");
    CHECK(layer_path_value("C:\\kf4\\vk_layers", "D:\\l;E:\\m", ';') == "C:\\kf4\\vk_layers;D:\\l;E:\\m");
    // (Windows's wide strings: a folder in any letters)
    CHECK(layer_path_value(L"C:\\Users\\\u5f20\\kf4\\vk_layers", nullptr, L';') == L"C:\\Users\\\u5f20\\kf4\\vk_layers");
    CHECK(layer_path_value(L"C:\\kf4\\vk_layers", L"D:\\l", L';') == L"C:\\kf4\\vk_layers;D:\\l");
    CHECK(layer_path_value(L"C:\\kf4\\vk_layers", L"", L';') == L"C:\\kf4\\vk_layers");
}

void gpus_that_need_it() {
    std::vector<VkExtensionProperties> none;
    std::vector<VkExtensionProperties> others = {extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME),
                                                 extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME)};
    std::vector<VkExtensionProperties> with = {extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME),
                                               extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)};
    // Vulkan 1.2 without the extension: the player's driver
    CHECK(gpu_needs_layer(VK_MAKE_API_VERSION(0, 1, 2, 170), others));
    CHECK(gpu_needs_layer(VK_API_VERSION_1_1, none));
    // the extension of its own, or 1.3's core
    CHECK(!gpu_needs_layer(VK_MAKE_API_VERSION(0, 1, 2, 170), with));
    CHECK(!gpu_needs_layer(VK_API_VERSION_1_3, none));
    CHECK(!gpu_needs_layer(VK_MAKE_API_VERSION(0, 1, 4, 354), others));
    // a name that only begins the same is not the extension
    CHECK(gpu_needs_layer(VK_API_VERSION_1_2, std::vector<VkExtensionProperties>{extension("VK_KHR_synchronization")}));
}

} // namespace

int main() {
    variable_by_loader();
    value_keeps_what_was_there();
    gpus_that_need_it();
    if (failures != 0) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("pcr_sync2: all passed\n");
    return 0;
}
