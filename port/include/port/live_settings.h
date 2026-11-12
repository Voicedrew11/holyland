/* The settings that change while the game runs (port/overlay/live_settings.c): what the overlay's switches set, and
   the test changes of KF4_SET_AT. A change sets port_options (this run) and port_settings (settings.json, written a
   second later) and passes it on to what applies it, the same way from either. The game's thread. */
#ifndef PORT_LIVE_SETTINGS_H
#define PORT_LIVE_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PORT_LIVE_WIDESCREEN,        /* 0 or 1: the 16:9 picture */
    PORT_LIVE_NATIVE_WIDESCREEN, /* 0 or 1: with widescreen, the renderer's picture drawn with more pixels across */
    PORT_LIVE_NATIVE_SCALE,      /* 0 (auto) or 1 to 4: the renderer's resolution, times the PS2's */
    PORT_LIVE_FRAME_RATE,        /* pictures a second: 30 to 240, PORT_FRAME_RATE_MONITOR or _UNLOCKED */
    PORT_LIVE_DRAW_DISTANCE,     /* PORT_DRAW_DISTANCE_ */
    PORT_LIVE_NATIVE_DRAW,       /* PORT_ND_* kinds; those the renderer does not draw are left out */
    PORT_LIVE_SKY,               /* PORT_SKY_: how the sky is drawn natively */
    PORT_LIVE_MSAA,              /* 1, 2, 4 or 8: the samples a pixel (the most the GPU has up to it) */
    PORT_LIVE_CUTOUT_AA,         /* 0 or 1: with MSAA, the cut-outs shaded at every sample */
    PORT_LIVE_COLOUR,            /* 0 exact or 1 high: the renderer's colour precision */
    PORT_LIVE_NATIVE_2D,         /* 0 ps2 or 1 smooth: the 2D at a larger scale */
    PORT_LIVE_COUNT
} PortLiveSetting;

/* The setting changed to value, from the next vblank */
void port_live_set(PortLiveSetting setting, int value);

/* KF4_SET_AT=VBLANK:NAME=VALUE,... (each vblank) the test changes due by now, as the overlay makes them: NAME is the
   setting's in lower case without PORT_LIVE_ (native_widescreen, native_scale, ...), VALUE a number (0x... too) */
void port_live_vblank(void);

#ifdef __cplusplus
}
#endif

#endif
