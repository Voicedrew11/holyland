// The player menu's triangles drawn with Vulkan (port/ui/ui_frame.hpp), in the Vulkan presenter's render pass over
// the picture (port/platform/present_vk.cpp). Its own pipeline, textures, buffers and descriptor sets, on the
// presenter's device; the game's thread, as the presenter.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>

#include "../ui/ui_frame.hpp"

struct UiVkSetup {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkRenderPass pass;     // the presenter's: one colour attachment, loaded and stored
    uint32_t frames;       // command buffers in flight: each has its own vertex buffer
};

// Makes the pipeline and what it needs; false (logged) when it cannot, and the menu is then not drawn
bool ui_vk_open(const UiVkSetup &setup);
// After vkQueueWaitIdle: everything let go
void ui_vk_close();
// Outside the render pass, in frame slot's command buffer once its fence has passed: the frame's textures made and
// uploaded, those it lets go of freed once no frame in flight uses them, its vertices and indices written
void ui_vk_prepare(VkCommandBuffer cmd, uint32_t slot, const ui::Frame &frame);
// Inside the render pass, on an extent of width x height pixels: the frame's draws
void ui_vk_draw(VkCommandBuffer cmd, uint32_t slot, const ui::Frame &frame, uint32_t width, uint32_t height);
