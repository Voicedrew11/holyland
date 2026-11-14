/* Adaptive sync (VRR: FreeSync, G-Sync, HDMI VRR): the screen refreshes when a picture comes, at any rate in
   its range, instead of on its own clock. Each picture is then on the screen at the time it was handed over,
   so any rate the game draws at is shown evenly (no judder at 144 on a 170 Hz screen), and nothing tears.

   With --adaptive-sync or settings.json's display.adaptive_sync (port_options.adaptive_sync):
   - the swapchain is FIFO whatever VSync and --tearing say (port/platform/present_vk.cpp): the screen waits
     for each picture and shows every one;
   - no rate goes over adaptive_sync_cap_hz of the screen's (port/platform/frame_interp.c): over the top of
     the range FIFO would hold pictures back a refresh each, the latency of plain VSync;
   - the screen's rate is not timed by its refreshes (port/platform/video.c's by_refresh): with VRR they
     follow the game's own pictures.
   Whether the screen and the system have VRR on is theirs to say (Linux: the compositor's setting, KWin's
   for a full-screen window by default; Windows: the driver's); without it the setting is plain VSync.

   port/platform/adaptive_sync.c; no state. */
#ifndef PORT_ADAPTIVE_SYNC_H
#define PORT_ADAPTIVE_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

/* The most pictures a second that keep a VRR screen of refresh_hz in its range with room for the pictures'
   unevenness: refresh_hz less refresh_hz^2 / 3600, rounded down (59 at 60, 138 at 144, 161 at 170, 224 at
   240). 0 when refresh_hz is not known (0 or less): no cap. */
int adaptive_sync_cap_hz(int refresh_hz);

#ifdef __cplusplus
}
#endif

#endif
