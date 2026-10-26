// What the temporal upscalers are given (--upscaler fsr3, dlss: docs/port/UPSCALING.md): a picture, its inputs and
// output, as gsn_upscale.inc hands them to upscale_ffx.cpp and upscale_dlss.cpp. Plain Vulkan handles, so that
// neither library's headers (nor windows.h) come into gsn.cpp.
#pragma once
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h> // (not vulkan.h: its Windows header would come into gsn.cpp)

// An image the upscaler reads or writes, and a view of its one level and layer: in VK_IMAGE_LAYOUT_GENERAL, as all
// the native GS's are, and left so
struct UpscaleImage {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
};

// A picture to upscale. color is the scene's picture (perceptual colours: the PS2's), depth its depth as 1 / the
// view's depth times near_plane (R32F: 1 at near_plane, 0 far away, inverted and infinite), motion where each pixel
// was in the last picture less where it is now (RG16F, the scene's pixels, without the jitter), output at the
// upscaled size (with VK_IMAGE_USAGE_STORAGE_BIT). render_* is the part of color drawn (its whole, here).
struct UpscaleFrame {
    VkCommandBuffer cmd;
    UpscaleImage color, depth, motion, output;
    uint32_t render_w, render_h;
    float jitter[2]; // the picture's jitter, in the scene's pixels (down: +y)
    float frame_ms;  // since the last picture
    float near_plane, fov_y;
    bool reset; // nothing to take from the pictures before (the first, after a cut or a while without)
};
