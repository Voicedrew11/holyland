// The Vulkan presenter (port/include/port/present_vk.h, --presenter vulkan, docs/port/STEP3.md).
//
// The window's swapchain is made on the PC renderer's own Vulkan device (pcr_enable_present turns the
// surface and swapchain extensions on for it). Each picture the window shows is
// - the PC renderer's: the image pcr_scanout_take hands out, which the PC renderer wrote on the GPU at the
//   vblank, so nothing comes back to the CPU;
// - or paraLLEl-GS's (--renderer gs): its pixels, copied into a buffer
//   and from there into an image of the presenter's own;
// blitted into the swapchain's image, scaled and placed as port_video_fit says (4:3 or the widescreen
// 16:9, with black bars, or stretched), filtered as "Smooth scaling" says. Dear ImGui's draw lists
// (the settings overlay, port/overlay/overlay.cpp) go over it in a render
// pass, with ImGui's own Vulkan backend (third_party/imgui/backends/imgui_impl_vulkan.cpp), and the player menu's
// triangles (port/include/port/player_ui.h) with its own pipeline (present_vk_ui.cpp), under ImGui's.
//
// The presenter submits to the PC renderer's queue, under its lock (pcr_queue_lock): the queue's order
// and a barrier at the start of each command buffer are all the synchronisation the PC renderer's
// picture needs. The PC renderer draws on a thread of its own (port/gs/pc/pcr.cpp): it writes the
// picture into its image at the vblank and then says so (pcr_set_presenter's image callback,
// port_video_submit_image), so a picture is never taken half made; pcr_scanout_take and
// pcr_scanout_release keep the image alive while this uses it. Everything here runs on the game's
// thread, with command pools of its own; the queue is shared under Granite's device lock.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include "flash_ring.h"
#include "port/zone_scope.hpp"
#ifdef KF4_HAVE_PLAYER_UI
#include "present_vk_ui.hpp"
#endif

extern "C" {
#include "gs_api.h"
#include "port/mouse_lead.h"
#include "port/overlay.h"
#include "port/debug.h"
#include "port/platform.h"
#include "port/player_ui.h"
#include "port/present_vk.h"
#include "port/profile.h"
#include "port/settings.h"
}

namespace {

// ---- Vulkan's functions, from the PC renderer's instance and device -------------------------------
#define INSTANCE_FUNCTIONS(X)                                                                              \
    X(vkGetDeviceProcAddr)                                                                                 \
    X(vkDestroySurfaceKHR)                                                                                 \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                           \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                           \
    X(vkGetPhysicalDeviceFormatProperties)                                                                 \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                 \
    X(vkGetPhysicalDeviceProperties)
#define DEVICE_FUNCTIONS(X)                                                                                \
    X(vkCreateSwapchainKHR)                                                                                \
    X(vkDestroySwapchainKHR)                                                                               \
    X(vkGetSwapchainImagesKHR)                                                                             \
    X(vkAcquireNextImageKHR)                                                                               \
    X(vkQueuePresentKHR)                                                                                   \
    X(vkQueueSubmit)                                                                                       \
    X(vkQueueWaitIdle)                                                                                     \
    X(vkCreateCommandPool)                                                                                 \
    X(vkDestroyCommandPool)                                                                                \
    X(vkAllocateCommandBuffers)                                                                            \
    X(vkBeginCommandBuffer)                                                                                \
    X(vkEndCommandBuffer)                                                                                  \
    X(vkCreateFence)                                                                                       \
    X(vkDestroyFence)                                                                                      \
    X(vkWaitForFences)                                                                                     \
    X(vkResetFences)                                                                                       \
    X(vkCreateSemaphore)                                                                                   \
    X(vkDestroySemaphore)                                                                                  \
    X(vkCreateImageView)                                                                                   \
    X(vkDestroyImageView)                                                                                  \
    X(vkCreateFramebuffer)                                                                                 \
    X(vkDestroyFramebuffer)                                                                                \
    X(vkCreateRenderPass)                                                                                  \
    X(vkDestroyRenderPass)                                                                                 \
    X(vkCmdBeginRenderPass)                                                                                \
    X(vkCmdEndRenderPass)                                                                                  \
    X(vkCmdPipelineBarrier)                                                                                \
    X(vkCmdClearColorImage)                                                                                \
    X(vkCmdBlitImage)                                                                                      \
    X(vkCmdClearAttachments)                                                                               \
    X(vkCmdCopyBufferToImage)                                                                              \
    X(vkCmdCopyImageToBuffer)                                                                              \
    X(vkCreateBuffer)                                                                                      \
    X(vkDestroyBuffer)                                                                                     \
    X(vkCreateImage)                                                                                       \
    X(vkDestroyImage)                                                                                      \
    X(vkAllocateMemory)                                                                                    \
    X(vkFreeMemory)                                                                                        \
    X(vkBindBufferMemory)                                                                                  \
    X(vkBindImageMemory)                                                                                   \
    X(vkGetBufferMemoryRequirements)                                                                       \
    X(vkGetImageMemoryRequirements)                                                                        \
    X(vkMapMemory)                                                                                         \
    X(vkCreateQueryPool)                                                                                   \
    X(vkDestroyQueryPool)                                                                                  \
    X(vkCmdResetQueryPool)                                                                                 \
    X(vkCmdWriteTimestamp)                                                                                 \
    X(vkGetQueryPoolResults)                                                                               \
    X(vkResetCommandPool)                                                                                  \
    X(vkCreateShaderModule)                                                                                \
    X(vkDestroyShaderModule)                                                                               \
    X(vkCreateSampler)                                                                                     \
    X(vkDestroySampler)                                                                                    \
    X(vkCreateDescriptorSetLayout)                                                                         \
    X(vkDestroyDescriptorSetLayout)                                                                        \
    X(vkCreateDescriptorPool)                                                                              \
    X(vkDestroyDescriptorPool)                                                                             \
    X(vkAllocateDescriptorSets)                                                                            \
    X(vkUpdateDescriptorSets)                                                                              \
    X(vkCreatePipelineLayout)                                                                              \
    X(vkDestroyPipelineLayout)                                                                             \
    X(vkCreateGraphicsPipelines)                                                                           \
    X(vkDestroyPipeline)                                                                                   \
    X(vkCmdBindPipeline)                                                                                   \
    X(vkCmdBindDescriptorSets)                                                                             \
    X(vkCmdPushConstants)                                                                                  \
    X(vkCmdSetViewport)                                                                                    \
    X(vkCmdSetScissor)                                                                                     \
    X(vkCmdDraw)

#define DECLARE(name) PFN_##name name = nullptr;
struct Functions {
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
};
#undef DECLARE
Functions vk;

// ---- The presenter's state ---------------------------------------------------------------------
const uint32_t FRAMES = 2; // command buffers in flight

struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;          // its command buffer has run (made signalled)
    VkSemaphore acquired = VK_NULL_HANDLE;  // the swapchain's image is free to draw into
    // the CPU's pixels for this frame's upload
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *staging_map = nullptr;
    VkDeviceSize staging_size = 0;
    // KF4_FLASH_TIME: the flash recorder's copy timed (two timestamps), read when the frame comes round
    VkQueryPool timer = VK_NULL_HANDLE;
    bool timed = false;
    // The presenter's own GPU time (own_timing): three timestamps, at its start, after the picture's copy into the
    // swapchain's image, at its end; read when the frame comes round
    VkQueryPool own_timer = VK_NULL_HANDLE;
    bool own_timed = false;
    // (KF4_PRESENT_FOLD_CHECK) the picture as sharpened here (a, the swapchain's format) and as the PC renderer
    // sharpens it (b, RGBA), read back to compare when the frame comes round (fold_check_read)
    VkBuffer check_a = VK_NULL_HANDLE, check_b = VK_NULL_HANDLE;
    VkDeviceMemory check_a_memory = VK_NULL_HANDLE, check_b_memory = VK_NULL_HANDLE;
    void *check_a_map = nullptr, *check_b_map = nullptr;
    VkDeviceSize check_size = 0;
    uint32_t check_w = 0, check_h = 0;
    bool check_pending = false;
};

struct SwapImage {
    VkImage image;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkSemaphore rendered; // drawn: the presentation engine may show it
};

// What the window shows
enum Source { SOURCE_NONE, SOURCE_UPLOAD, SOURCE_NATIVE };

struct Presenter {
    bool up = false;
    GsnPresentDevice gsn = {};
    PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    SDL_Window *window = nullptr;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSurfaceFormatKHR format = {};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    bool can_read = false; // the swapchain's images can be copied from (KF4_OVERLAY_SHOT)
    std::vector<SwapImage> images;
    bool stale = true;     // the swapchain is to be made again (a new size, vsync, or out of date)
    int vsync_made = -1;
    int vsync_wanted = 0;
    // the mode chosen over vsync's (wanted_mode): 0 none, 1 mailbox (unlocked), 2 immediate (unlocked --tearing),
    // 3 FIFO (--adaptive-sync)
    int mode_made = -1;
    int mode_wanted = 0;

    VkRenderPass pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    Frame frames[FRAMES];
    uint32_t frame = 0;

    // the CPU's picture, as an image (GENERAL layout)
    VkImage upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory = VK_NULL_HANDLE;
    uint32_t upload_width = 0, upload_height = 0;
    bool upload_new = false; // made, and not yet in its layout
    Source source = SOURCE_NONE;
    uint64_t tag = 0; // the PC renderer's picture shown (PORT_PICTURE_TAG)

    // the window read back for KF4_OVERLAY_SHOT
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    void *readback_map = nullptr;
    VkDeviceSize readback_size = 0;

    bool imgui = false;

    // The sharpening drawn here (make_sharpen): the PC renderer's pcr_present_sharpen.frag straight into the swapchain's
    // image at the picture's place, in place of the copy of a picture it sharpened itself (GsnScanout's sharpen)
    struct Sharpen {
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[FRAMES] = {};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    } sharpen;
};
Presenter pr;

void fold_check_print(); // (KF4_PRESENT_FOLD_CHECK's, below)

void check(VkResult result) {
    if (result < 0) {
        std::fprintf(stderr, "[present] Vulkan error %d\n", int(result));
    }
}

// ---- Memory --------------------------------------------------------------------------------------
int memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vk.vkGetPhysicalDeviceMemoryProperties(pr.gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
            return int(i);
        }
    }
    return -1;
}

// The largest device-local heap's size (an integrated GPU's carve-out; 0: none)
uint64_t device_local_heap_bytes() {
    VkPhysicalDeviceMemoryProperties props;
    vk.vkGetPhysicalDeviceMemoryProperties(pr.gpu, &props);
    uint64_t most = 0;
    for (uint32_t i = 0; i < props.memoryHeapCount; i++) {
        if ((props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && props.memoryHeaps[i].size > most) {
            most = props.memoryHeaps[i].size;
        }
    }
    return most;
}

// A buffer the CPU writes or reads, mapped for good
bool make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory,
                      void **map) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.vkCreateBuffer(pr.device, &info, nullptr, buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(pr.device, *buffer, &req);
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) {
        type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(type);
    if (type < 0 || vk.vkAllocateMemory(pr.device, &alloc, nullptr, memory) != VK_SUCCESS) {
        vk.vkDestroyBuffer(pr.device, *buffer, nullptr);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    vk.vkBindBufferMemory(pr.device, *buffer, *memory, 0);
    vk.vkMapMemory(pr.device, *memory, 0, VK_WHOLE_SIZE, 0, map);
    return true;
}

void free_buffer(VkBuffer *buffer, VkDeviceMemory *memory) {
    if (*buffer != VK_NULL_HANDLE) {
        vk.vkDestroyBuffer(pr.device, *buffer, nullptr);
        vk.vkFreeMemory(pr.device, *memory, nullptr); // (unmapped with it)
    }
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
}

// Every command buffer of the presenter's has run
void wait_frames() {
    for (Frame &f : pr.frames) {
        vk.vkWaitForFences(pr.device, 1, &f.done, VK_TRUE, UINT64_MAX);
    }
}

// The image the CPU's pictures go into, width x height
bool make_upload(uint32_t width, uint32_t height) {
    if (pr.upload != VK_NULL_HANDLE && pr.upload_width == width && pr.upload_height == height) {
        return true;
    }
    wait_frames();
    if (pr.upload != VK_NULL_HANDLE) {
        vk.vkDestroyImage(pr.device, pr.upload, nullptr);
        vk.vkFreeMemory(pr.device, pr.upload_memory, nullptr);
        pr.upload = VK_NULL_HANDLE;
    }
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(pr.device, &info, nullptr, &pr.upload) != VK_SUCCESS) {
        pr.upload = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(pr.device, pr.upload, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    alloc.memoryTypeIndex = uint32_t(type < 0 ? memory_type(req.memoryTypeBits, 0) : type);
    if (vk.vkAllocateMemory(pr.device, &alloc, nullptr, &pr.upload_memory) != VK_SUCCESS) {
        vk.vkDestroyImage(pr.device, pr.upload, nullptr);
        pr.upload = VK_NULL_HANDLE;
        return false;
    }
    vk.vkBindImageMemory(pr.device, pr.upload, pr.upload_memory, 0);
    pr.upload_width = width;
    pr.upload_height = height;
    pr.upload_new = true;
    return true;
}

// ---- The swapchain -------------------------------------------------------------------------------
void drop_swapchain_images() {
    for (SwapImage &s : pr.images) {
        vk.vkDestroyFramebuffer(pr.device, s.framebuffer, nullptr);
        vk.vkDestroyImageView(pr.device, s.view, nullptr);
        vk.vkDestroySemaphore(pr.device, s.rendered, nullptr);
    }
    pr.images.clear();
}

// Waits for the queue to be done with the swapchain (its images, their semaphores)
void wait_queue() {
    wait_frames();
    gsr_queue_lock();
    vk.vkQueueWaitIdle(pr.queue);
    gsr_queue_unlock();
}

// The swapchain mode asked for over vsync's (Presenter::mode_wanted), from the options as they are now
int wanted_mode() {
    if (port_options.adaptive_sync) {
        return 3;
    }
    if (port_options.frame_rate != PORT_FRAME_RATE_UNLOCKED) {
        return 0;
    }
    return port_options.tearing ? 2 : 1;
}

// The swapchain for the window's size and the vsync setting: false if there is none now (the window
// is minimised, or it failed)
bool make_swapchain() {
    int drawable_w = 0, drawable_h = 0;
    SDL_GetWindowSizeInPixels(pr.window, &drawable_w, &drawable_h);
    VkSurfaceCapabilitiesKHR caps;
    if (vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pr.gpu, pr.surface, &caps) != VK_SUCCESS) {
        return false;
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = uint32_t(drawable_w < 0 ? 0 : drawable_w);
        extent.height = uint32_t(drawable_h < 0 ? 0 : drawable_h);
        extent.width = extent.width < caps.minImageExtent.width ? caps.minImageExtent.width : extent.width;
        extent.width = extent.width > caps.maxImageExtent.width ? caps.maxImageExtent.width : extent.width;
        extent.height = extent.height < caps.minImageExtent.height ? caps.minImageExtent.height : extent.height;
        extent.height = extent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : extent.height;
    }
    if (extent.width == 0 || extent.height == 0) {
        return false; // minimised
    }

    // vsync: FIFO; without: immediate, else mailbox (no tearing, never waits), else FIFO after all.
    // --frame-rate unlocked, whatever vsync: mailbox (each refresh shows the newest picture, no tearing), else FIFO;
    // with --tearing, immediate first (the owner's choice: PC_RENDERER.md, smooth play step 3).
    // --adaptive-sync, whatever the rest: FIFO (a VRR screen waits for each picture; port/include/port/adaptive_sync.h)
    uint32_t count = 0;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(pr.gpu, pr.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(pr.gpu, pr.surface, &count, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> wants;
    if (pr.mode_wanted == 3) {
        wants = {}; // FIFO, which every surface has
    } else if (pr.mode_wanted == 1) {
        wants = {VK_PRESENT_MODE_MAILBOX_KHR};
    } else if (pr.mode_wanted == 2) {
        wants = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    } else if (!pr.vsync_wanted) {
        wants = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    }
    {
        for (VkPresentModeKHR want : wants) {
            bool found = false;
            for (VkPresentModeKHR m : modes) {
                found |= m == want;
            }
            if (found) {
                mode = want;
                break;
            }
        }
    }

    uint32_t images = caps.minImageCount + 1 < 2 ? 2 : caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && images > caps.maxImageCount) {
        images = caps.maxImageCount;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    pr.can_read = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (pr.can_read) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if ((caps.supportedUsageFlags & usage) != usage) {
        std::fprintf(stderr, "[present] the swapchain's images cannot be drawn into by a copy\n");
        return false;
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                alpha = VkCompositeAlphaFlagBitsKHR(bit);
                break;
            }
        }
    }

    // the old one's images may still be in use: everything finished first
    wait_queue();
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = pr.surface;
    info.minImageCount = images;
    info.imageFormat = pr.format.format;
    info.imageColorSpace = pr.format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = pr.swapchain;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkResult result = vk.vkCreateSwapchainKHR(pr.device, &info, nullptr, &swapchain);
    drop_swapchain_images();
    if (pr.swapchain != VK_NULL_HANDLE) {
        vk.vkDestroySwapchainKHR(pr.device, pr.swapchain, nullptr);
        pr.swapchain = VK_NULL_HANDLE;
    }
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[present] no swapchain (%d)\n", int(result));
        return false;
    }
    pr.swapchain = swapchain;
    pr.extent = extent;

    vk.vkGetSwapchainImagesKHR(pr.device, swapchain, &count, nullptr);
    std::vector<VkImage> list(count);
    vk.vkGetSwapchainImagesKHR(pr.device, swapchain, &count, list.data());
    for (VkImage image : list) {
        SwapImage s = {image, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = pr.format.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.vkCreateImageView(pr.device, &view, nullptr, &s.view);
        VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = pr.pass;
        fb.attachmentCount = 1;
        fb.pAttachments = &s.view;
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        vk.vkCreateFramebuffer(pr.device, &fb, nullptr, &s.framebuffer);
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk.vkCreateSemaphore(pr.device, &sem, nullptr, &s.rendered);
        pr.images.push_back(s);
    }
    pr.vsync_made = pr.vsync_wanted;
    pr.mode_made = pr.mode_wanted;
    pr.stale = false;
    static const char *const mode_names[] = {"immediate", "mailbox", "FIFO", "relaxed FIFO"};
    std::fprintf(stderr, "[present] swapchain %ux%u, %u images, %s\n", extent.width, extent.height, count,
                 mode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR ? mode_names[mode] : "other");
    return true;
}

// The swapchain's format: 8-bit UNORM (the picture's values as they are, as SDL_Renderer shows them),
// one a copy can scale into and ImGui can draw on
bool choose_format() {
    uint32_t count = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(pr.gpu, pr.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(pr.gpu, pr.surface, &count, formats.data());
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32}) {
        for (const VkSurfaceFormatKHR &f : formats) {
            if (f.format != want || f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                continue;
            }
            VkFormatProperties props;
            vk.vkGetPhysicalDeviceFormatProperties(pr.gpu, want, &props);
            VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
            if ((props.optimalTilingFeatures & need) == need) {
                pr.format = f;
                return true;
            }
        }
    }
    return false;
}

bool make_render_pass() {
    // ImGui's pass: over the picture (LOAD), left in COLOR_ATTACHMENT_OPTIMAL for the barrier after it
    VkAttachmentDescription colour = {};
    colour.format = pr.format.format;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &colour;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    return vk.vkCreateRenderPass(pr.device, &info, nullptr, &pr.pass) == VK_SUCCESS;
}

// The sharpening's pipeline (Presenter::Sharpen), in the render pass, for each frame a set (the picture's view)
bool make_sharpen() {
    Presenter::Sharpen &sh = pr.sharpen;
    size_t vert_bytes = 0, frag_bytes = 0;
    const uint32_t *frag_code = nullptr;
    const uint32_t *vert_code = gsr_present_sharpen_spirv(&vert_bytes, &frag_code, &frag_bytes);
    VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST; // (texelFetch: unused but for its being there)
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                            nullptr};
    VkDescriptorSetLayoutCreateInfo set_layout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES};
    VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = FRAMES;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    // (pcr_present_sharpen.frag's: the size, the origin, the strength)
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 5 * sizeof(int32_t)};
    if (vk.vkCreateSampler(pr.device, &sampler, nullptr, &sh.sampler) != VK_SUCCESS ||
        vk.vkCreateDescriptorSetLayout(pr.device, &set_layout, nullptr, &sh.set_layout) != VK_SUCCESS ||
        vk.vkCreateDescriptorPool(pr.device, &pool, nullptr, &sh.pool) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorSetLayout layouts[FRAMES];
    for (uint32_t i = 0; i < FRAMES; i++) {
        layouts[i] = sh.set_layout;
    }
    VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = sh.pool;
    alloc.descriptorSetCount = FRAMES;
    alloc.pSetLayouts = layouts;
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &sh.set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (vk.vkAllocateDescriptorSets(pr.device, &alloc, sh.sets) != VK_SUCCESS ||
        vk.vkCreatePipelineLayout(pr.device, &layout, nullptr, &sh.layout) != VK_SUCCESS) {
        return false;
    }
    auto module = [](const uint32_t *code, size_t bytes) {
        VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        return vk.vkCreateShaderModule(pr.device, &info, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
    };
    VkShaderModule vert = module(vert_code, vert_bytes), frag = module(frag_code, frag_bytes);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = sh.layout;
    info.renderPass = pr.pass;
    info.subpass = 0;
    bool made = vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE &&
                vk.vkCreateGraphicsPipelines(pr.device, VK_NULL_HANDLE, 1, &info, nullptr, &sh.pipeline) == VK_SUCCESS;
    if (vert != VK_NULL_HANDLE) {
        vk.vkDestroyShaderModule(pr.device, vert, nullptr);
    }
    if (frag != VK_NULL_HANDLE) {
        vk.vkDestroyShaderModule(pr.device, frag, nullptr);
    }
    if (!made) {
        sh.pipeline = VK_NULL_HANDLE;
    }
    return made;
}

void free_sharpen() {
    Presenter::Sharpen &sh = pr.sharpen;
    if (sh.pipeline != VK_NULL_HANDLE) {
        vk.vkDestroyPipeline(pr.device, sh.pipeline, nullptr);
    }
    if (sh.layout != VK_NULL_HANDLE) {
        vk.vkDestroyPipelineLayout(pr.device, sh.layout, nullptr);
    }
    if (sh.pool != VK_NULL_HANDLE) {
        vk.vkDestroyDescriptorPool(pr.device, sh.pool, nullptr);
    }
    if (sh.set_layout != VK_NULL_HANDLE) {
        vk.vkDestroyDescriptorSetLayout(pr.device, sh.set_layout, nullptr);
    }
    if (sh.sampler != VK_NULL_HANDLE) {
        vk.vkDestroySampler(pr.device, sh.sampler, nullptr);
    }
    sh = Presenter::Sharpen();
}

// The picture (view, w x h, not sharpened) sharpened by strength into the render pass's image at x, y (the pass begun)
void draw_sharpened(VkCommandBuffer cmd, uint32_t frame, VkImageView view, int x, int y, int w, int h, float strength) {
    Presenter::Sharpen &sh = pr.sharpen;
    VkDescriptorImageInfo image = {sh.sampler, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sh.sets[frame];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vk.vkUpdateDescriptorSets(pr.device, 1, &write, 0, nullptr);
    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh.pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh.layout, 0, 1, &sh.sets[frame], 0, nullptr);
    VkViewport viewport = {float(x), float(y), float(w), float(h), 0.0f, 1.0f};
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min(x + w, int(pr.extent.width)), y1 = std::min(y + h, int(pr.extent.height));
    VkRect2D scissor = {{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);
    struct {
        int32_t size[2], origin[2];
        float strength;
    } push = {{w, h}, {x, y}, strength};
    vk.vkCmdPushConstants(cmd, sh.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);
}

bool make_frames() {
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = pr.family;
    if (vk.vkCreateCommandPool(pr.device, &pool, nullptr, &pr.pool) != VK_SUCCESS) {
        return false;
    }
    for (Frame &f : pr.frames) {
        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pr.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vk.vkAllocateCommandBuffers(pr.device, &alloc, &f.cmd) != VK_SUCCESS ||
            vk.vkCreateFence(pr.device, &fence, nullptr, &f.done) != VK_SUCCESS ||
            vk.vkCreateSemaphore(pr.device, &sem, nullptr, &f.acquired) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

// ---- The flash recorder (port_vk_flash_write; docs/port/README.md "Debugging") ---------------------
// Each picture presented is blitted, scaled to port_options.flash_height rows (its width the picture's
// shape), into the next layer of a ring: one image of seconds x pictures a second layers, on the GPU. The
// flash recorder's key (F8) has the presenter hand the ring to a thread of its own, which reads it back a
// few dozen layers at a time and writes them as PNGs, oldest first; the ring takes no pictures meanwhile.
// The cost while playing is the one blit a picture (KF4_FLASH_TIME=1 prints its GPU time now and then).
struct FlashPicture {
    uint64_t tag;   // PORT_PICTURE_TAG: the vblank and the phase
    double seconds; // when it was presented (port_wall_seconds)
};

struct Flash {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, layers = 0;
    bool fresh = false; // made, and not yet in its layout (GENERAL)
    uint32_t head = 0, count = 0;
    std::vector<FlashPicture> pictures;
    double megabytes = 0;
    float seconds_made = 0;
    int height_made = 0;
    int hz_made = 0;           // (the pictures a second it was made for: flash_ring_keeps_rate)
    double made_at = 0;        // (port_wall_seconds when it was made)
    uint64_t most_bytes = 0;   // (flash_ring_most_bytes of the GPU's device-local heap; 0: not yet looked at)
    bool failed = false; // could not be made with these options: not tried again until they change
    // the key's request (any thread) and the writer (its thread)
    char folder[1100] = {};
    std::atomic<int> wanted{0};
    std::atomic<int> writing{0};
    SDL_Thread *thread = nullptr;
    // the writer's copy of the ring's order
    uint32_t first = 0, n = 0;
    std::vector<FlashPicture> written;
    // KF4_FLASH_TIME
    int timing = -1;
    double gpu_ns = 0;
    uint64_t timed = 0;
    float period = 1.0f;
};
Flash fl;

void flash_free() {
    if (fl.thread != nullptr) {
        SDL_WaitThread(fl.thread, nullptr);
        fl.thread = nullptr;
    }
    if (fl.image != VK_NULL_HANDLE) {
        wait_queue();
        vk.vkDestroyImage(pr.device, fl.image, nullptr);
        vk.vkFreeMemory(pr.device, fl.memory, nullptr);
    }
    fl.image = VK_NULL_HANDLE;
    fl.memory = VK_NULL_HANDLE;
    fl.layers = fl.width = fl.height = fl.head = fl.count = 0;
    fl.megabytes = 0;
}

// The ring for a picture of w x h (its place in the window): false when there is none (off, or not made)
bool flash_ring(int w, int h) {
    float seconds = port_options.flash_seconds;
    int rows = port_options.flash_height;
    if (seconds <= 0 || rows <= 0 || w <= 0 || h <= 0) {
        if (fl.image != VK_NULL_HANDLE && !fl.writing.load()) {
            flash_free();
        }
        fl.failed = false;
        return false;
    }
    // (held to flash_ring_most_bytes: smaller pictures, then fewer, at a high frame rate; the rate it was made for kept
    // while the rate found stays near it)
    if (fl.most_bytes == 0) {
        VkPhysicalDeviceProperties props;
        vk.vkGetPhysicalDeviceProperties(pr.gpu, &props);
        fl.most_bytes =
            flash_ring_most_bytes(device_local_heap_bytes(), props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    }
    int hz = port_frame_rate_hz();
    // (nothing shown, a load or a teleport: unlocked's rate is its ceiling then, not a rate pictures come at; the ring
    // is kept as it is)
    const bool showing = port_video_pictures_per_second() > 0;
    if (fl.image != VK_NULL_HANDLE &&
        (!showing || flash_ring_keeps_rate(fl.hz_made, hz, port_wall_seconds() - fl.made_at))) {
        hz = fl.hz_made;
    }
    const FlashRingSize size = flash_ring_size(seconds, hz, rows, w, h, fl.most_bytes);
    const uint32_t layers = size.layers, width = size.width, height = size.height;
    if (fl.image != VK_NULL_HANDLE && fl.layers == layers && fl.width == width && fl.height == height) {
        return true;
    }
    if (fl.writing.load() || (fl.failed && fl.seconds_made == seconds && fl.height_made == rows)) {
        return false;
    }
    flash_free();
    fl.seconds_made = seconds;
    fl.height_made = rows;
    fl.hz_made = hz;
    fl.made_at = port_wall_seconds();
    fl.failed = true;
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.vkCreateImage(pr.device, &info, nullptr, &fl.image) != VK_SUCCESS) {
        fl.image = VK_NULL_HANDLE;
        std::fprintf(stderr, "[present] the flash recorder's ring (%u x %u x %u) could not be made\n", width, height, layers);
        return false;
    }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(pr.device, fl.image, &req);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    int type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    alloc.memoryTypeIndex = uint32_t(type < 0 ? memory_type(req.memoryTypeBits, 0) : type);
    if (vk.vkAllocateMemory(pr.device, &alloc, nullptr, &fl.memory) != VK_SUCCESS) {
        vk.vkDestroyImage(pr.device, fl.image, nullptr);
        fl.image = VK_NULL_HANDLE;
        std::fprintf(stderr, "[present] no GPU memory for the flash recorder's ring (%.0f MB)\n", double(req.size) / 1048576.0);
        return false;
    }
    vk.vkBindImageMemory(pr.device, fl.image, fl.memory, 0);
    fl.failed = false;
    fl.fresh = true;
    fl.width = width;
    fl.height = height;
    fl.layers = layers;
    fl.head = fl.count = 0;
    fl.megabytes = double(req.size) / 1048576.0;
    fl.pictures.assign(layers, FlashPicture());
    std::fprintf(stderr,
                 "[present] flash recorder: the last %u pictures kept on the GPU at %u x %u (%.0f MB; at most %.0f MB, "
                 "for %d a second)\n",
                 layers, width, height, fl.megabytes, double(fl.most_bytes) / 1048576.0, hz);
    return true;
}

// (the writer's thread) fl.n pictures of the ring from fl.first, to fl.folder
int flash_writer(void *) {
    const uint32_t bytes = fl.width * fl.height * 4;
    uint32_t chunk = (64u << 20) / bytes;
    chunk = chunk < 1 ? 1 : chunk > fl.n ? fl.n : chunk;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *map = nullptr;
    uint32_t done = 0;
    double start = port_wall_seconds();
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = pr.family;
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    bool ok = vk.vkCreateCommandPool(pr.device, &pool_info, nullptr, &pool) == VK_SUCCESS &&
              vk.vkCreateFence(pr.device, &fence_info, nullptr, &fence) == VK_SUCCESS &&
              make_host_buffer(VkDeviceSize(chunk) * bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer, &memory, &map);
    if (ok) {
        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        ok = vk.vkAllocateCommandBuffers(pr.device, &alloc, &cmd) == VK_SUCCESS;
    }
    std::vector<uint32_t> rgba(size_t(fl.width) * fl.height);
    std::vector<VkBufferImageCopy> regions(chunk);
    for (uint32_t at = 0; ok && at < fl.n; at += chunk) {
        uint32_t n = fl.n - at < chunk ? fl.n - at : chunk;
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk.vkBeginCommandBuffer(cmd, &begin);
        VkMemoryBarrier before = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0,
                                nullptr, 0, nullptr);
        for (uint32_t k = 0; k < n; k++) {
            VkBufferImageCopy &r = regions[k];
            r = {};
            r.bufferOffset = VkDeviceSize(k) * bytes;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (fl.first + at + k) % fl.layers, 1};
            r.imageExtent = {fl.width, fl.height, 1};
        }
        vk.vkCmdCopyImageToBuffer(cmd, fl.image, VK_IMAGE_LAYOUT_GENERAL, buffer, n, regions.data());
        VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                                nullptr);
        vk.vkEndCommandBuffer(cmd);
        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        gsr_queue_lock();
        ok = vk.vkQueueSubmit(pr.queue, 1, &submit, fence) == VK_SUCCESS;
        gsr_queue_unlock();
        ok = ok && vk.vkWaitForFences(pr.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        vk.vkResetFences(pr.device, 1, &fence);
        vk.vkResetCommandPool(pr.device, pool, 0);
        for (uint32_t k = 0; ok && k < n; k++) {
            char path[1200];
            std::snprintf(path, sizeof(path), "%s/%04u.png", fl.folder, at + k);
            std::memcpy(rgba.data(), static_cast<const uint8_t *>(map) + size_t(k) * bytes, bytes);
            if (port_png_write(path, rgba.data(), fl.width, fl.height)) {
                done++;
            }
        }
    }
    char path[1200];
    std::snprintf(path, sizeof(path), "%s/pictures.txt", fl.folder);
    FILE *list = std::fopen(path, "w");
    if (list != nullptr) {
        std::fprintf(list, "# the flash recorder's pictures, oldest first: the file, the vblank, the phase (0: the vblank's\n"
                           "# own picture, else a picture between), seconds before the last\n");
        double last = fl.n ? fl.written[fl.n - 1].seconds : 0;
        for (uint32_t k = 0; k < fl.n; k++) {
            const FlashPicture &p = fl.written[k];
            std::fprintf(list, "%04u.png %llu %.3f %.4f\n", k, (unsigned long long)(p.tag >> 16), double(p.tag & 0xFFFF) / 65536.0,
                         last - p.seconds);
        }
        std::fclose(list);
    }
    free_buffer(&buffer, &memory);
    if (fence != VK_NULL_HANDLE) {
        vk.vkDestroyFence(pr.device, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vk.vkDestroyCommandPool(pr.device, pool, nullptr); // (and its command buffer)
    }
    if (done == fl.n && ok) {
        port_debug_set_message("recorded %u pictures (the last %.1f s, %u x %u) in %s, written in %.1f s", done,
                               fl.n ? fl.written[fl.n - 1].seconds - fl.written[0].seconds : 0.0, fl.width, fl.height, fl.folder,
                               port_wall_seconds() - start);
    } else {
        port_debug_set_message("flash recorder: wrote %u of %u pictures to %s", done, fl.n, fl.folder);
    }
    port_overlay_notify_debug();
    fl.writing.store(0);
    return 0;
}

// (the presenter, before anything else of a picture) a write asked for: the ring's order kept, the writer
// started. Every blit into the ring before it is submitted already, and none is made until it is done.
void flash_start_writer() {
    if (!fl.wanted.load() || fl.writing.load()) {
        return;
    }
    fl.wanted.store(0);
    if (fl.thread != nullptr) {
        SDL_WaitThread(fl.thread, nullptr);
        fl.thread = nullptr;
    }
    if (fl.image == VK_NULL_HANDLE || fl.count == 0) {
        return;
    }
    fl.n = fl.count;
    fl.first = (fl.head + fl.layers - fl.count) % fl.layers;
    fl.written.resize(fl.n);
    for (uint32_t k = 0; k < fl.n; k++) {
        fl.written[k] = fl.pictures[(fl.first + k) % fl.layers];
    }
    fl.count = 0; // (the next pictures start a new ring)
    fl.writing.store(1);
    fl.thread = SDL_CreateThread(flash_writer, "flash recorder", nullptr);
    if (fl.thread == nullptr) {
        fl.writing.store(0);
        port_debug_set_message("flash recorder: no thread to write with");
    }
}

// (the presenter) the picture, source's sw x sh at sx, sy (GENERAL), into the ring's next layer
void flash_take(VkCommandBuffer cmd, Frame &f, VkImage source, int32_t sx, int32_t sy, uint32_t sw, uint32_t sh, int w,
                int h) {
    if (fl.writing.load() || source == VK_NULL_HANDLE || !flash_ring(w, h)) {
        return;
    }
    if (fl.timing < 0) {
        fl.timing = getenv("KF4_FLASH_TIME") != nullptr;
        VkPhysicalDeviceProperties props;
        vk.vkGetPhysicalDeviceProperties(pr.gpu, &props);
        fl.period = props.limits.timestampPeriod;
    }
    if (fl.timing && f.timer == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo q = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 2;
        vk.vkCreateQueryPool(pr.device, &q, nullptr, &f.timer);
    }
    bool timing = fl.timing && f.timer != VK_NULL_HANDLE;
    if (timing) {
        vk.vkCmdResetQueryPool(cmd, f.timer, 0, 2);
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.timer, 0);
    }
    uint32_t layer = fl.head;
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = fl.image;
    b.oldLayout = fl.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, fl.fresh ? 0 : layer, fl.fresh ? VK_REMAINING_ARRAY_LAYERS : 1};
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    fl.fresh = false;
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {sx, sy, 0};
    blit.srcOffsets[1] = {sx + int32_t(sw), sy + int32_t(sh), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    blit.dstOffsets[1] = {int32_t(fl.width), int32_t(fl.height), 1};
    vk.vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, fl.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    if (timing) {
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, f.timer, 1);
        f.timed = true;
    }
    fl.pictures[layer] = {pr.tag, port_wall_seconds()};
    fl.head = (layer + 1) % fl.layers;
    fl.count = fl.count < fl.layers ? fl.count + 1 : fl.layers;
}

// (the presenter, its frame's fence waited for) the last copy's GPU time, with KF4_FLASH_TIME
// The presenter's own GPU time, which the PC renderer's GpuTimes does not see: with KF4_PRESENT_TIME=1 printed now and
// then, and while a profile run records added to its GPU table as the kinds "presenter copy" (the picture into the
// swapchain's image, the flash recorder's copy, the menu's uploads) and "presenter overlay" (the counter, menu and
// overlay drawn over it). (The first timestamp is at the top of the pipe, before the wait for the swapchain's image:
// with vsync off that wait is next to none.)
struct OwnTiming {
    int printing = -1; // (KF4_PRESENT_TIME, read once)
    double period = 0;
    double ns_copy = 0, ns_overlay = 0;
    uint64_t timed = 0;
} own;

bool own_timing() {
    if (own.printing < 0) {
        own.printing = getenv("KF4_PRESENT_TIME") != nullptr && atoi(getenv("KF4_PRESENT_TIME")) >= 1;
        VkPhysicalDeviceProperties props;
        vk.vkGetPhysicalDeviceProperties(pr.gpu, &props);
        own.period = props.limits.timestampPeriod;
    }
    return own.printing || port_profile_gpu_recording();
}

void own_timer_begin(VkCommandBuffer cmd, Frame &f) {
    if (!own_timing()) {
        return;
    }
    if (f.own_timer == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo q = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 3;
        if (vk.vkCreateQueryPool(pr.device, &q, nullptr, &f.own_timer) != VK_SUCCESS) {
            f.own_timer = VK_NULL_HANDLE;
            return;
        }
    }
    vk.vkCmdResetQueryPool(cmd, f.own_timer, 0, 3);
    vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f.own_timer, 0);
    f.own_timed = true;
}

void own_timer_mark(VkCommandBuffer cmd, Frame &f, uint32_t query) {
    if (f.own_timed) {
        vk.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.own_timer, query);
    }
}

void own_read_timer(Frame &f) {
    if (!f.own_timed) {
        return;
    }
    f.own_timed = false;
    uint64_t t[3];
    if (vk.vkGetQueryPoolResults(pr.device, f.own_timer, 0, 3, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) !=
        VK_SUCCESS) {
        return;
    }
    double copy = double(t[1] - t[0]) * own.period, overlay = double(t[2] - t[1]) * own.period;
    if (port_profile_gpu_recording()) {
        port_profile_gpu_add(0x50524553454E5401ull, "presenter copy", PROFILE_GPU_KIND, uint64_t(copy), 1);
        port_profile_gpu_add(0x50524553454E5402ull, "presenter overlay", PROFILE_GPU_KIND, uint64_t(overlay), 1);
    }
    own.ns_copy += copy;
    own.ns_overlay += overlay;
    own.timed++;
    if (own.printing && own.timed % 500 == 0) {
        std::fprintf(stderr, "[present] the presenter's GPU time: copy %.3f, overlay %.3f ms a picture (%llu pictures)\n",
                     own.ns_copy / double(own.timed) / 1e6, own.ns_overlay / double(own.timed) / 1e6,
                     (unsigned long long)own.timed);
    }
}

void flash_read_timer(Frame &f) {
    if (!f.timed) {
        return;
    }
    f.timed = false;
    uint64_t t[2];
    if (vk.vkGetQueryPoolResults(pr.device, f.timer, 0, 2, sizeof(t), t, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) ==
        VK_SUCCESS) {
        fl.gpu_ns += double(t[1] - t[0]) * fl.period;
        fl.timed++;
        if (fl.timed % 500 == 0) {
            std::fprintf(stderr, "[present] flash recorder: %.1f us a picture on the GPU (%llu pictures)\n",
                         fl.gpu_ns / double(fl.timed) / 1000.0, (unsigned long long)fl.timed);
        }
    }
}

// Everything made so far, undone (a start that failed: the window goes back to SDL_Renderer)
void close_all() {
    if (pr.device != VK_NULL_HANDLE && vk.vkQueueWaitIdle != nullptr) {
        flash_free();
        for (Frame &f : pr.frames) {
            if (f.timer != VK_NULL_HANDLE) {
                vk.vkDestroyQueryPool(pr.device, f.timer, nullptr);
            }
            if (f.own_timer != VK_NULL_HANDLE) {
                vk.vkDestroyQueryPool(pr.device, f.own_timer, nullptr);
            }
        }
    }
    if (pr.device != VK_NULL_HANDLE && vk.vkQueueWaitIdle != nullptr) {
        wait_queue();
#ifdef KF4_HAVE_PLAYER_UI
        ui_vk_close();
#endif
        drop_swapchain_images();
        if (pr.swapchain != VK_NULL_HANDLE) {
            vk.vkDestroySwapchainKHR(pr.device, pr.swapchain, nullptr);
        }
        for (Frame &f : pr.frames) {
            if (f.done != VK_NULL_HANDLE) {
                vk.vkDestroyFence(pr.device, f.done, nullptr);
            }
            if (f.acquired != VK_NULL_HANDLE) {
                vk.vkDestroySemaphore(pr.device, f.acquired, nullptr);
            }
            free_buffer(&f.staging, &f.staging_memory);
        }
        if (pr.pool != VK_NULL_HANDLE) {
            vk.vkDestroyCommandPool(pr.device, pr.pool, nullptr);
        }
        gsr_present_sharpens(0);
        free_sharpen();
        fold_check_print();
        for (Frame &f : pr.frames) {
            free_buffer(&f.check_a, &f.check_a_memory);
            free_buffer(&f.check_b, &f.check_b_memory);
        }
        if (pr.pass != VK_NULL_HANDLE) {
            vk.vkDestroyRenderPass(pr.device, pr.pass, nullptr);
        }
    }
    if (pr.surface != VK_NULL_HANDLE) {
        vk.vkDestroySurfaceKHR(pr.instance, pr.surface, nullptr);
    }
    pr = Presenter();
}

// (developers', KF4_PRESENT_FOLD_CHECK=1: the PC renderer also sharpens each picture it leaves to the presenter, as
// before, into GsnScanout's check) Every 15th such picture, the presenter's own sharpening read back from the swapchain's
// image and compared with it, byte for byte
struct FoldCheck {
    int on = -1;
    uint64_t seen = 0, compared = 0, differ = 0, pixels = 0;
} fold_check;

bool fold_checking() {
    if (fold_check.on < 0) {
        const char *e = getenv("KF4_PRESENT_FOLD_CHECK");
        fold_check.on = e && std::strcmp(e, "0") != 0;
    }
    return fold_check.on != 0;
}

void fold_check_print() {
    if (fold_check.compared) {
        std::fprintf(stderr, "[present] the sharpening here against the PC renderer's (KF4_PRESENT_FOLD_CHECK): %llu "
                     "pictures compared, %llu differ (%llu pixels)\n",
                     (unsigned long long)fold_check.compared, (unsigned long long)fold_check.differ,
                     (unsigned long long)fold_check.pixels);
    }
}

// (the frame's fence waited for) its pictures compared
void fold_check_read(Frame &f) {
    if (!f.check_pending) {
        return;
    }
    f.check_pending = false;
    const uint32_t *a = static_cast<const uint32_t *>(f.check_a_map), *b = static_cast<const uint32_t *>(f.check_b_map);
    const bool bgra = pr.format.format == VK_FORMAT_B8G8R8A8_UNORM;
    uint64_t wrong = 0;
    for (size_t i = 0, n = size_t(f.check_w) * f.check_h; i < n; i++) {
        uint32_t v = a[i];
        if (bgra) {
            v = (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);
        }
        wrong += (v & 0xFFFFFFu) != (b[i] & 0xFFFFFFu) ? 1 : 0;
    }
    fold_check.compared++;
    fold_check.differ += wrong ? 1 : 0;
    fold_check.pixels += wrong;
    if (fold_check.compared % 20 == 0) {
        fold_check_print();
    }
}

// Whether this picture is compared: its buffers ready (w x h), the copies to do once the pass is ended
bool fold_check_now(Frame &f, const GsnScanout &scanout, int w, int h) {
    if (!fold_checking() || scanout.check == 0 || fold_check.seen++ % 15 != 0) {
        return false;
    }
    VkDeviceSize size = VkDeviceSize(w) * h * 4;
    if (f.check_size < size) {
        free_buffer(&f.check_a, &f.check_a_memory);
        free_buffer(&f.check_b, &f.check_b_memory);
        f.check_size = 0;
        if (!make_host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f.check_a, &f.check_a_memory, &f.check_a_map) ||
            !make_host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &f.check_b, &f.check_b_memory, &f.check_b_map)) {
            return false;
        }
        f.check_size = size;
    }
    f.check_w = uint32_t(w);
    f.check_h = uint32_t(h);
    return true;
}

// ---- A picture -----------------------------------------------------------------------------------
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage,
                   VkAccessFlags src_access, VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ImGui's textures (the font atlas) are made with a submit of ImGui's own and a wait for the queue:
// those go under the PC renderer's queue lock
bool imgui_textures_pending(ImDrawData *data) {
    if (data->Textures == nullptr) {
        return false;
    }
    for (ImTextureData *tex : *data->Textures) {
        if (tex->Status != ImTextureStatus_OK) {
            return true;
        }
    }
    return false;
}

// The frame-rate counter's squares (port_video_counter_draw), to be cleared into the swapchain's image
// in the render pass: opaque, and no pipeline of its own, so the pictures' own thread draws it too
struct Squares {
    VkClearAttachment clears[256];
    VkClearRect rects[256];
    uint32_t count;
};

void add_square(void *context, int x, int y, int width, int height, unsigned int rgba) {
    Squares *s = static_cast<Squares *>(context);
    int right = x + width, bottom = y + height;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    right = right > int(pr.extent.width) ? int(pr.extent.width) : right;
    bottom = bottom > int(pr.extent.height) ? int(pr.extent.height) : bottom;
    if (s->count == 256 || right <= x || bottom <= y) {
        return;
    }
    VkClearAttachment &c = s->clears[s->count];
    c.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.colorAttachment = 0;
    c.clearValue.color.float32[0] = float(rgba & 0xFF) / 255.0f;
    c.clearValue.color.float32[1] = float(rgba >> 8 & 0xFF) / 255.0f;
    c.clearValue.color.float32[2] = float(rgba >> 16 & 0xFF) / 255.0f;
    c.clearValue.color.float32[3] = 1.0f;
    VkClearRect &r = s->rects[s->count];
    r.rect.offset = {x, y};
    r.rect.extent = {uint32_t(right - x), uint32_t(bottom - y)};
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    s->count++;
}

// The window read back after the frame (KF4_OVERLAY_SHOT), as RGBA
void read_window(Frame &f) {
    vk.vkWaitForFences(pr.device, 1, &f.done, VK_TRUE, UINT64_MAX);
    uint32_t w = pr.extent.width, h = pr.extent.height;
    std::vector<uint32_t> pixels(size_t(w) * h);
    const uint32_t *in = static_cast<const uint32_t *>(pr.readback_map);
    bool bgra = pr.format.format == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < pixels.size(); i++) {
        uint32_t v = in[i];
        pixels[i] = bgra ? (v & 0xFF00FF00u) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16) : v;
    }
    port_overlay_shot(pixels.data(), int(w), int(h));
}

} // namespace

// (developers', KF4_LATE_TURN_LOG=FILE: slice 4b's measurement, docs/plans/scene-slice4-results.md) For each picture
// presented that shows a picture between, as its commands are recorded here: what the mouse's hand has moved since the
// hand the picture was turned to (its newest reading now), the late turn a reprojection here would add. A line a
// picture: now_s turned_s shown_s yaw_rad pitch_rad sx sy newest_age_s (sx, sy: GS pixels a unit of x / z, y / z;
// newest_age_s: how old the newest reading of the hand is now).
void log_late_turn(const GsnPictureTurn &turn) {
    static FILE *log = [] {
        const char *path = getenv("KF4_LATE_TURN_LOG");
        return path && *path ? fopen(path, "w") : nullptr;
    }();
    if (!log) {
        return;
    }
    constexpr double VBLANK_S = 1001.0 / 60000.0;
    const double now = port_wall_seconds();
    MouseTurn since = port_mouse_lead_since({turn.hand_yaw_rad, turn.hand_pitch_rad}, HUGE_VAL);
    const double newest_v = port_mouse_lead_newest_vblanks(), now_v = port_video_vblanks_at(now);
    const double age = newest_v >= 0 && now_v >= 0 ? (now_v - newest_v) * VBLANK_S : -1;
    fprintf(log, "%.6f %.6f %.6f %.7f %.7f %.2f %.2f %.6f\n", now, turn.turned_s, turn.shown_s, since.yaw_rad,
            since.pitch_rad, turn.sx, turn.sy, age);
}

// ---- The port's side -----------------------------------------------------------------------------
extern "C" int port_vk_open(void *sdl_window) {
    pr = Presenter();
    if (!gsr_present_device(&pr.gsn)) {
        std::fprintf(stderr, "[present] the Vulkan presenter needs the PC renderer's device with a swapchain\n");
        return 0;
    }
    pr.window = static_cast<SDL_Window *>(sdl_window);
    pr.instance = static_cast<VkInstance>(pr.gsn.instance);
    pr.gpu = static_cast<VkPhysicalDevice>(pr.gsn.gpu);
    pr.device = static_cast<VkDevice>(pr.gsn.device);
    pr.queue = static_cast<VkQueue>(pr.gsn.queue);
    pr.family = pr.gsn.queue_family;
    pr.get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(pr.gsn.get_instance_proc_addr);
    bool loaded = pr.get_instance_proc_addr != nullptr;
#define LOAD_INSTANCE(name)                                                                                \
    vk.name = loaded ? reinterpret_cast<PFN_##name>(pr.get_instance_proc_addr(pr.instance, #name)) : nullptr; \
    loaded = loaded && vk.name != nullptr;
#define LOAD_DEVICE(name)                                                                                  \
    vk.name = loaded ? reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(pr.device, #name)) : nullptr;   \
    loaded = loaded && vk.name != nullptr;
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_INSTANCE
#undef LOAD_DEVICE
    if (!loaded) {
        std::fprintf(stderr, "[present] Vulkan's swapchain functions are missing\n");
        pr = Presenter();
        return 0;
    }
    if (!SDL_Vulkan_CreateSurface(pr.window, pr.instance, nullptr, &pr.surface)) {
        std::fprintf(stderr, "[present] no Vulkan surface for the window: %s\n", SDL_GetError());
        pr.surface = VK_NULL_HANDLE;
        close_all();
        return 0;
    }
    VkBool32 supported = VK_FALSE;
    vk.vkGetPhysicalDeviceSurfaceSupportKHR(pr.gpu, pr.family, pr.surface, &supported);
    if (!supported) {
        std::fprintf(stderr, "[present] the PC renderer's queue cannot present to the window\n");
        close_all();
        return 0;
    }
    if (!choose_format()) {
        std::fprintf(stderr, "[present] the window has no 8-bit format to present in\n");
        close_all();
        return 0;
    }
    pr.vsync_wanted = port_settings.vsync;
    pr.mode_wanted = wanted_mode(); // (the first swapchain the one wanted, not made twice)
    if (!make_render_pass() || !make_frames() || !make_swapchain()) {
        std::fprintf(stderr, "[present] the swapchain could not be made\n");
        close_all();
        return 0;
    }
    pr.up = true;
    if (make_sharpen()) {
        gsr_present_sharpens(1); // (the PC renderer leaves its pictures to sharpen here)
    } else {
        std::fprintf(stderr, "[present] no pipeline for the sharpening: the PC renderer sharpens the pictures\n");
        free_sharpen();
    }
#ifdef KF4_HAVE_PLAYER_UI
    UiVkSetup ui_setup = {pr.get_instance_proc_addr, pr.instance, pr.gpu, pr.device, pr.pass, FRAMES};
    ui_vk_open(ui_setup); // (logged when it cannot: the player menu is then not drawn)
#endif
    static bool waits;
    if (!waits) {
        waits = true;
        std::atexit([] {
            if (fl.thread != nullptr) {
                SDL_WaitThread(fl.thread, nullptr); // (the flash recorder's pictures written before the device goes)
                fl.thread = nullptr;
            }
        });
    }
    std::fprintf(stderr, "[present] the window is presented with Vulkan, on the PC renderer's device\n");
    return 1;
}

extern "C" void port_vk_set_vsync(int on) {
    pr.vsync_wanted = on != 0;
}

extern "C" int port_vk_imgui_init(void) {
    if (!pr.up) {
        return 0;
    }
    auto loader = [](const char *name, void *) -> PFN_vkVoidFunction { return pr.get_instance_proc_addr(pr.instance, name); };
    uint32_t api = pr.gsn.api_version ? pr.gsn.api_version : VK_API_VERSION_1_1;
    if (!ImGui_ImplVulkan_LoadFunctions(api, loader)) {
        return 0;
    }
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = api;
    info.Instance = pr.instance;
    info.PhysicalDevice = pr.gpu;
    info.Device = pr.device;
    info.QueueFamily = pr.family;
    info.Queue = pr.queue;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = FRAMES;
    info.ImageCount = FRAMES;
    info.PipelineInfoMain.RenderPass = pr.pass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check;
    pr.imgui = ImGui_ImplVulkan_Init(&info);
    return pr.imgui;
}

// The frame profiler's zones (port/include/port/zones.h)
static const ZoneId present_zone = port_zone_id("present", ZONE_WORK);
static const ZoneId present_wait_zone = port_zone_id("present: wait for its frame and a swapchain image", ZONE_WAIT);
static const ZoneId present_overlay_zone = port_zone_id("present: the overlay drawn (ImGui)", ZONE_WORK);
static const ZoneId present_queue_zone = port_zone_id("present: submit and vkQueuePresentKHR", ZONE_WAIT);

extern "C" void port_vk_present(const uint32_t *pixels, uint32_t width, uint32_t height, int image, uint64_t tag,
                                int overlay) {
    if (!pr.up) {
        return;
    }
    ZoneScope zone(present_zone);
    flash_start_writer(); // (F8)
    bool fresh = pixels != nullptr || image;
    // with nothing new, the window is drawn again only for the overlay
    if (!fresh && (pr.source == SOURCE_NONE || !port_overlay_visible())) {
        return;
    }
    if (SDL_GetWindowFlags(pr.window) & SDL_WINDOW_MINIMIZED) {
        return;
    }
    int drawable_w = 0, drawable_h = 0;
    SDL_GetWindowSizeInPixels(pr.window, &drawable_w, &drawable_h);
    if (drawable_w <= 0 || drawable_h <= 0) {
        return;
    }
    pr.mode_wanted = wanted_mode();
    if (pr.stale || pr.vsync_made != pr.vsync_wanted || pr.mode_made != pr.mode_wanted ||
        pr.extent.width != uint32_t(drawable_w) ||
        pr.extent.height != uint32_t(drawable_h)) {
        if (!make_swapchain()) {
            return;
        }
    }

    Frame &f = pr.frames[pr.frame];
    int waiting = port_zone_begin(present_wait_zone);
    vk.vkWaitForFences(pr.device, 1, &f.done, VK_TRUE, UINT64_MAX);
    flash_read_timer(f);
    own_read_timer(f);
    fold_check_read(f);
    uint32_t index = 0;
    VkResult result = vk.vkAcquireNextImageKHR(pr.device, pr.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &index);
    if (waiting) {
        port_zone_end(present_wait_zone);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        pr.stale = true;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        pr.stale = true; // (drawn all the same, and made again for the next)
    } else if (result != VK_SUCCESS) {
        check(result);
        return;
    }
    SwapImage &target = pr.images[index];

    // the CPU's pixels into this frame's buffer, then (below) into the upload image
    if (pixels != nullptr) {
        VkDeviceSize size = VkDeviceSize(width) * height * 4;
        if (f.staging_size < size) {
            free_buffer(&f.staging, &f.staging_memory);
            f.staging_size = 0;
            if (make_host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &f.staging, &f.staging_memory, &f.staging_map)) {
                f.staging_size = size;
            }
        }
        if (f.staging_size >= size && make_upload(width, height)) {
            std::memcpy(f.staging_map, pixels, size_t(size));
            pr.source = SOURCE_UPLOAD;
        } else {
            pixels = nullptr;
        }
    } else if (image) {
        pr.source = SOURCE_NATIVE;
        pr.tag = tag;
    }
    // the PC renderer's picture of that tag (with nothing new, the one shown last again)
    GsnScanout scanout = {};
    bool have_scanout = pr.source == SOURCE_NATIVE && gsr_scanout_take_tag(&scanout, pr.tag);
    if (fresh && have_scanout && scanout.turn.known) {
        log_late_turn(scanout.turn);
    }

    // the picture's place; the PC renderer scales its next pictures to it with the best scaling (or the
    // sharpening), this one when it was made at this size
    int x, y, w, h;
    port_video_fit(int(pr.extent.width), int(pr.extent.height), &x, &y, &w, &h);
    {
        int scaling = port_video_scaling();
        float sharpen = port_video_sharpen();

        if (scaling == PORT_SCALING_BEST || sharpen > 0) {
            gsr_set_present_scale(w, h, sharpen);
        } else {
            gsr_set_present_scale(0, 0, 0);
        }
    }
    bool scaled = have_scanout && scanout.scaled != 0 && scanout.scaled_width == uint32_t(w) &&
                  scanout.scaled_height == uint32_t(h);

    // the overlay's draw lists over the picture's place
    ImDrawData *draw = nullptr;
    const ui::Frame *menu = nullptr;
    bool shot = false;
    if (overlay) {
        ZoneScope overlay_zone(present_overlay_zone);
#ifdef KF4_HAVE_PLAYER_UI
        port_player_ui_draw(int(pr.extent.width), int(pr.extent.height));
        menu = player_ui_frame();
#endif
        port_overlay_draw(x, y, w, h);
        draw = pr.imgui ? static_cast<ImDrawData *>(port_overlay_draw_data()) : nullptr;
        shot = port_overlay_shot_due() && pr.can_read;
    }
    if (shot) {
        VkDeviceSize size = VkDeviceSize(pr.extent.width) * pr.extent.height * 4;
        if (pr.readback_size < size) {
            free_buffer(&pr.readback, &pr.readback_memory);
            pr.readback_size = 0;
            if (make_host_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &pr.readback, &pr.readback_memory, &pr.readback_map)) {
                pr.readback_size = size;
            }
        }
        shot = pr.readback_size >= size;
    }

    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &begin);
    own_timer_begin(cmd, f);
    // what the queue wrote before (the PC renderer's picture, this presenter's copies) before it is read
    VkMemoryBarrier memory = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0,
                            nullptr, 0, nullptr);
#ifdef KF4_HAVE_PLAYER_UI
    if (menu != nullptr) {
        ui_vk_prepare(cmd, pr.frame, *menu); // (its textures' uploads: outside the render pass)
    }
#endif
    if (pixels != nullptr) {
        if (pr.upload_new) {
            image_barrier(cmd, pr.upload, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            pr.upload_new = false;
        }
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vk.vkCmdCopyBufferToImage(cmd, f.staging, pr.upload, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        image_barrier(cmd, pr.upload, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }
    VkImage source = VK_NULL_HANDLE;
    uint32_t source_w = 0, source_h = 0;
    if (pr.source == SOURCE_UPLOAD && !pr.upload_new) {
        source = pr.upload;
        source_w = pr.upload_width;
        source_h = pr.upload_height;
    } else if (scaled) {
        source = VkImage(scanout.scaled); // (a pixel a pixel)
        source_w = scanout.scaled_width;
        source_h = scanout.scaled_height;
    } else if (have_scanout) {
        source = VkImage(scanout.image);
        source_w = scanout.width;
        source_h = scanout.height;
    }
    // (the bars beside or above the picture black: with a picture, the bars alone, in the render pass below (a 16:9
    // picture in a 16:10 window, 1920 x 1200: the whole image cleared was 0.2 ms a picture on a Vega 7, 2026-11-20);
    // the whole image when there is none. Nothing when the picture covers the window.)
    const bool pictured = source != VK_NULL_HANDLE && w > 0 && h > 0;
    const bool covered = pictured && x <= 0 && y <= 0 && x + w >= int(pr.extent.width) && y + h >= int(pr.extent.height);
    // The picture left to sharpen here (GsnScanout's sharpen): drawn sharpened straight into the swapchain's image, in
    // place of the copy (one full window's write and read fewer than the PC renderer sharpening it into its own image)
    const bool sharpen_here = pictured && source == VkImage(scanout.scaled) && scanout.sharpen > 0 &&
                              scanout.scaled_view != 0 && pr.sharpen.pipeline != VK_NULL_HANDLE;
    const bool inside = x >= 0 && y >= 0 && x + w <= int(pr.extent.width) && y + h <= int(pr.extent.height);
    bool pass_open = false;
    auto begin_pass = [&] {
        VkRenderPassBeginInfo pass = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = pr.pass;
        pass.framebuffer = target.framebuffer;
        pass.renderArea.extent = pr.extent;
        vk.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        pass_open = true;
    };
    if (sharpen_here) {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        // (the PC renderer's picture written before, read by the fragment shader)
        VkMemoryBarrier read = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        read.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &read,
                                0, nullptr, 0, nullptr);
        begin_pass();
        draw_sharpened(cmd, pr.frame, VkImageView(scanout.scaled_view), x, y, w, h, scanout.sharpen);
        const bool readable = inside && pr.can_read;
        const bool check = readable && fold_check_now(f, scanout, w, h);
        if ((fresh && readable) || check) {
            // (the flash recorder's ring: the picture as it is shown, sharpened, from the swapchain's image)
            vk.vkCmdEndRenderPass(cmd);
            pass_open = false;
            image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            if (fresh) {
                flash_take(cmd, f, target.image, x, y, uint32_t(w), uint32_t(h), w, h);
            }
            if (check) {
                VkBufferImageCopy copy = {};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageOffset = {x, y, 0};
                copy.imageExtent = {uint32_t(w), uint32_t(h), 1};
                vk.vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_GENERAL, f.check_a, 1, &copy);
                copy.imageOffset = {0, 0, 0};
                vk.vkCmdCopyImageToBuffer(cmd, VkImage(scanout.check), VK_IMAGE_LAYOUT_GENERAL, f.check_b, 1, &copy);
                VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                                        nullptr, 0, nullptr);
                f.check_pending = true;
            }
            image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        } else if (fresh) {
            flash_take(cmd, f, source, 0, 0, source_w, source_h, w, h); // (not sharpened: no other way)
        }
        own_timer_mark(cmd, f, 1);
    } else {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (!pictured) {
            VkClearColorValue black = {};
            black.float32[3] = 1.0f;
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        }
        if (pictured) {
            image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageBlit blit = {};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(source_w), int32_t(source_h), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {x, y, 0};
            blit.dstOffsets[1] = {x + w, y + h, 1};
            vk.vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                              &blit, !scaled && port_video_scaling() != PORT_SCALING_SHARP ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        if (fresh && source != VK_NULL_HANDLE) {
            flash_take(cmd, f, source, 0, 0, source_w, source_h, w, h); // (the flash recorder's ring)
        }
        own_timer_mark(cmd, f, 1);
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    // the frame-rate counter (either thread), then the overlay over it (the game's thread)
    static Squares squares; // (under the window's present lock, port/platform/video.c)
    squares.count = 0;
    if (pictured && !covered) {
        const int ew = int(pr.extent.width), eh = int(pr.extent.height);
        add_square(&squares, 0, 0, ew, y, 0xFF000000u);                     // above
        add_square(&squares, 0, y + h, ew, eh - (y + h), 0xFF000000u);     // below
        add_square(&squares, 0, y, x, h, 0xFF000000u);                      // left
        add_square(&squares, x + w, y, ew - (x + w), h, 0xFF000000u);       // right
    }
    port_video_counter_draw(add_square, &squares, x, y, w, h);
    if (draw != nullptr || menu != nullptr || squares.count > 0) {
        if (!pass_open) {
            begin_pass();
        }
        for (uint32_t i = 0; i < squares.count; i++) {
            vk.vkCmdClearAttachments(cmd, 1, &squares.clears[i], 1, &squares.rects[i]);
        }
        // ImGui (the full map, the minimap, the extended stats beside the game's menus, messages; the developers'
        // overlay) first, then the player menu over all of it: the menu is the top layer, as it has the input
        if (draw != nullptr) {
            bool textures = imgui_textures_pending(draw);
            if (textures) {
                gsr_queue_lock();
            }
            ImGui_ImplVulkan_RenderDrawData(draw, cmd);
            if (textures) {
                gsr_queue_unlock();
            }
        }
#ifdef KF4_HAVE_PLAYER_UI
        if (menu != nullptr) {
            ui_vk_draw(cmd, pr.frame, *menu, pr.extent.width, pr.extent.height);
        }
#endif
    }
    if (pass_open) {
        vk.vkCmdEndRenderPass(cmd);
    }
    own_timer_mark(cmd, f, 2);
    if (shot) {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {pr.extent.width, pr.extent.height, 1};
        vk.vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pr.readback, 1, &copy);
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
        VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                                nullptr);
    } else {
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    vk.vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &target.rendered;
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &target.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &pr.swapchain;
    present.pImageIndices = &index;
    vk.vkResetFences(pr.device, 1, &f.done);
    int queueing = port_zone_begin(present_queue_zone);
    gsr_queue_lock();
    check(vk.vkQueueSubmit(pr.queue, 1, &submit, f.done));
    if (have_scanout) {
        gsr_scanout_release(); // (submitted: whatever the PC renderer writes into it next comes after)
    }
    result = vk.vkQueuePresentKHR(pr.queue, &present);
    gsr_queue_unlock();
    if (queueing) {
        port_zone_end(present_queue_zone);
    }
    port_zone_picture(); // (the frame profiler's frames end here)
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        pr.stale = true;
    } else {
        check(result);
    }
    if (shot) {
        read_window(f);
    }
    pr.frame = (pr.frame + 1) % FRAMES;
}

extern "C" int port_vk_flash_write(const char *folder, char *why, size_t size) {
    if (!pr.up) {
        std::snprintf(why, size, "the Vulkan presenter is not up");
        return 0;
    }
    if (port_options.flash_seconds <= 0) {
        std::snprintf(why, size, "the flash recorder is off: --flash-recorder on, or the overlay's Debug page");
        return 0;
    }
    if (fl.writing.load() || fl.wanted.load()) {
        std::snprintf(why, size, "still writing the last ones");
        return 0;
    }
    if (fl.image == VK_NULL_HANDLE || fl.count == 0) {
        std::snprintf(why, size, "none presented yet");
        return 0;
    }
    std::snprintf(fl.folder, sizeof(fl.folder), "%s", folder);
    int pictures = int(fl.count);
    fl.wanted.store(1);
    return pictures;
}

extern "C" int port_vk_flash_info(int *pictures, int *width, int *height, double *megabytes) {
    *pictures = int(fl.count);
    *width = int(fl.width);
    *height = int(fl.height);
    *megabytes = fl.megabytes;
    return fl.writing.load() || fl.wanted.load();
}
