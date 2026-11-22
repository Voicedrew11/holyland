/* The player's settings (port/overlay/settings.c): what the player menu (F1) changes, kept in
   settings.json in the user's data folder. Options on the command line override them for that run.
   A run that is headless or plays a pad script (--input) reads no settings.json, so tests stay the
   same whatever the player has chosen. */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASPECT_4_3 0     /* the TV's 4:3, whatever the window's shape */
#define ASPECT_STRETCH 1 /* the whole window */

/* --game-speed, settings.json's mods.game_speed, the player menu's Game speed and KF4_SET_AT all take and
   clamp the game's whole clock to this range, in quarters (1: the PS2's rate, 2: twice as fast) */
#define PORT_GAME_SPEED_LOW  1.0
#define PORT_GAME_SPEED_HIGH 2.0

typedef struct {
    /* Display */
    int window_height;     /* the window's height in pixels; 896 by default */
    int window_width;      /* its width in pixels (a monitor's size: 2560 x 1600, 3440 x 1440...), or 0 to follow
                              the picture's shape (4:3, or 16:9 with widescreen); 0 by default */
    int fullscreen;        /* borderless, on the whole screen */
    int aspect;            /* ASPECT_ */
    int vsync;             /* wait for the screen's refresh to show each picture */
    int smooth;            /* scale the picture with bilinear filtering (else nearest) */
    int scaling;           /* PORT_SCALING_: sharp (nearest), smooth (bilinear) or best (port_video_scaling) */
    float sharpen;         /* the sharpening after the scaling, 0 (none) to 1 */
    int deinterlace;       /* the ways the PC picture differs from the GS's (port_options) */
    int progressive;
    int anti_blur;
    int adapt_width;
    int whole_frame;
    int seamless_scroll;   /* the scrolling textures without a seam (port_options.seamless_scroll; on) */
    int flicker_filter;    /* the game's flicker filter, as on the PS2 (port_options.flicker_filter; off) */
    int widescreen;        /* the opt-in 16:9 picture (port_options.widescreen) */
    int widescreen_hud_centred; /* with it, the HUD in the 4:3 middle instead of the 16:9 corner */
    int fov;               /* the 3D view's horizontal field, degrees at 4:3 (port_options.fov; 64, the game's) */
    int frame_rate;        /* 30 (the PS2's), or the opt-in higher rates, interpolated: pictures a second, or
                              PORT_FRAME_RATE_MONITOR or _UNLOCKED (port_options.frame_rate) */
    int tearing;           /* unlocked's swapchain immediate, not mailbox (port_options.tearing; off) */
    int adaptive_sync;     /* for a VRR screen: FIFO, rates under its own (port_options.adaptive_sync; off) */
    int draw_distance;     /* the opt-in longer view: PORT_DRAW_DISTANCE_ (port_options.draw_distance) */
    int draw_distance_sky; /* with it, the sky's fog pushed out too (port_options.draw_distance_sky) */
    int native_scale;      /* the renderer's resolution, 1 to 4 times the PS2's (port_options.native_scale) */
    int native_widescreen; /* with widescreen, the renderer's picture drawn wider (port_options.native_widescreen) */
    int antialias;         /* the picture's anti-aliasing: PORT_AA_*, port/antialias.h (port_options.antialias) */
    int native_lod;        /* the renderer's mipmap levels: 0 ps2, 1 scale, 2 full (port_options.native_lod) */
    int native_aniso;      /* the renderer's anisotropic filtering: 1 off, 2 to 16 (port_options.native_aniso) */
    int native_2d;         /* at a larger scale, the renderer's 2D: 0 ps2, 1 smooth (port_options.native_2d) */
    int font;              /* the game's font: 0 ps2, 1 sharp (port_options.font) */
    int native_layer_z;    /* the renderer's Z: 0 ps2, 1 tolerant (port_options.native_layer_z) */
    int native_draw;       /* native draw, the whole path: 0 off (the PS2 path), 1 every accepted kind
                              (port_options.native_draw = PORT_ND_ACCEPTED; docs/port/NATIVE_DRAW.md) */
    int native_sky;        /* the native sky: PORT_SKY_PS2 or PORT_SKY_MODERN (port_options.native_sky) */
    int reflection_clip;   /* the water's reflection cut at the water: 1 on, 0 the PS2's (port_options.reflection_clip) */
    int presenter;         /* how the picture reaches the window, from the next start: PORT_PRESENTER_ */
    int scene_depth;       /* the scene renderer's depth, from the next start: PORT_SCENE_DEPTH_ */
    /* Audio */
    int volume;            /* 0 to 100 */
    int mute;
    /* Mods */
    float game_speed;      /* the whole game's clock, this many times the host's: PORT_GAME_SPEED_LOW to
                              PORT_GAME_SPEED_HIGH, in quarters (port_options.game_speed;
                              changed while the game runs, from the next vblank, and only while the
                              game simulates: the menus, the pauses and the cinematics keep 1x) */
    int myus_necklace;     /* the effect of Myu's Necklace, always (port_options.myus_necklace) */
    int separate_run;      /* a run key apart from the action button (port_options.separate_run) */
    int no_hit_jolt;       /* a hard hit does not throw the view down (port_options.no_hit_jolt) */
    int door_speed;        /* doors run their opening and closing animation this many times a game frame, 1 (the
                              PS2's) to 8 (port_door_speed; 1, the PS2's, by default) */
    int auto_loot_gold;    /* the gold an enemy drops when the player kills it goes into his purse at once, with the
                              pickup's notice (port_auto_loot_gold; off by default) */
    int equipped_mark;     /* the shops' and the smith's lists show " (E)" after what is equipped (port_equipped_mark;
                              on by default) */
    /* The overlay */
    int pause_when_open;
    int show_fps;
    float ui_scale;        /* the overlay's size, 0.75 to 2 */
    int save_slot;         /* the slot the Save states page has chosen */
    int quick_slot;        /* the slot of the quick save and quick load keys (port/include/port/quicksave.h) */
} PortSettings;
extern PortSettings port_settings;

/* (main) Reads settings.json, unless the run is headless or plays a pad script, and puts its values
   into port_options where the command line left them alone. Writes it back at exit if changed. */
void port_settings_load(void);
/* 1 when settings.json was read (a run with a window and no pad script) */
int port_settings_active(void);
/* Something changed: written a second after the last change, and at exit */
void port_settings_changed(void);
/* (once a vblank, the game's thread) writes settings.json if it changed a while ago */
void port_settings_poll(void);
void port_settings_save(void);
/* The user's data folder: KF4_DATA_DIR, else %APPDATA%\holyland on Windows, $XDG_DATA_HOME/holyland
   (or ~/.local/share/holyland) elsewhere (port/include/port/data_dir.h). Made when first written to. */
const char *port_data_dir(void);
/* settings.json's path */
const char *port_settings_path(void);
/* Puts the audio settings into the sound output (port_audio_set_volume) */
void port_settings_apply_audio(void);
/* Puts the display settings into the window: its size, full screen, vsync (port/platform/video.c) */
void port_video_apply_settings(void);

#ifdef __cplusplus
}
#endif

#endif
