/* The Vulkan presenter (port/platform/present_vk.cpp, --presenter vulkan): the window's swapchain on
   the PC renderer's own Vulkan device. It draws the PC renderer's picture straight from the GPU (no
   read-back), or the pixels paraLLEl-GS gave, scaled into the window as port_video_fit says, then
   the settings overlay (Dear ImGui's Vulkan backend) over it. All of it on the game's thread. */
#ifndef PORT_PRESENT_VK_H
#define PORT_PRESENT_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* (port/platform/video.c) the picture's place in a window of out_w x out_h pixels */
void port_video_fit(int out_w, int out_h, int *x, int *y, int *width, int *height);

#ifdef KF4_HAVE_VULKAN_PRESENTER
/* The swapchain on the window (an SDL_Window made with SDL_WINDOW_VULKAN): 1 when it is up. 0 when
   it cannot be (no PC renderer, or its device has no swapchain, or the surface fails), and the window
   is then SDL_Renderer's. */
int port_vk_open(void *sdl_window);
/* A picture: pixels (width x height, R in the low byte) to upload, or with image the PC renderer's of
   that tag (pcr_scanout_take_tag); with neither, the last one again, and only if the overlay draws.
   With overlay (the game's thread), presented with the overlay over it; without, as it is (the pictures
   between vblanks, from a thread of their own, port/platform/video.c). One thread at a time. */
void port_vk_present(const uint32_t *pixels, uint32_t width, uint32_t height, int image, uint64_t tag, int overlay);
/* Waits for the screen's refresh (FIFO) or not (immediate, else mailbox), from the next picture */
void port_vk_set_vsync(int on);
/* (port/overlay/overlay.cpp) Dear ImGui's Vulkan backend on the presenter's render pass: 1 when up */
int port_vk_imgui_init(void);
/* The flash recorder (port_options.flash_seconds; docs/port/README.md "Debugging"): each picture presented
   is copied, scaled to port_options.flash_height rows, into a ring of images on the GPU. Writes the ring's
   pictures, oldest first, as folder/NNNN.png and folder/pictures.txt, on a thread of its own (the ring
   stops taking pictures until they are written, then port_debug_set_message says so): the number of
   pictures it will write, or 0 with why not in why. Any thread. */
int port_vk_flash_write(const char *folder, char *why, size_t size);
/* What the ring holds now: pictures, their size, and the GPU memory it takes (MB); 1 while writing */
int port_vk_flash_info(int *pictures, int *width, int *height, double *megabytes);
#endif

#ifdef __cplusplus
}
#endif

#endif
