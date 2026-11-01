/* The screen's vblanks, as the system knows them: when the screen that shows the window last began a refresh.

   The screen's refresh clock (port/include/port/refresh_clock.h) is fed the times of the screen's refreshes.
   The window's swapchain cannot give them under a compositor: the Vulkan drivers note no time a picture reached
   the screen there (VK_EXT_present_timing's first pixel out is 0 under KWin with RADV, and X11 surfaces have no
   timing), and VK_KHR_present_wait's waits may not run beside the presents. So they come from the system's own
   record of the display's vblanks, where a compositor's or a browser's pacing takes them from:
   - Linux: the kernel's (DRM, libdrm: drmWaitVBlank for no vblank more answers at once with the last one's
     time), on the CRTC whose mode is the window's screen's (its size and rate, else its rate alone);
   - Windows: the desktop compositor's (DwmGetCompositionTimingInfo), which keeps to the main screen's;
   - elsewhere, or without libdrm: none.

   port/platform/screen_vblank.c. Any thread. */
#ifndef PORT_SCREEN_VBLANK_H
#define PORT_SCREEN_VBLANK_H

#ifdef __cplusplus
extern "C" {
#endif

/* The screen showing the window now: width x height pixels at refresh_hz (as SDL names them). 1 when its
   vblanks can be known. */
int port_screen_vblank_open(int width, int height, int refresh_hz);
/* When that screen's last vblank began (port_wall_seconds): 1 when known */
int port_screen_vblank_last(double *at_s);

#ifdef __cplusplus
}
#endif

#endif
