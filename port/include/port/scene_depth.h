/* How the scene renderer keeps the 3D's depth (port/gs/scene/scn_draw.h, docs/plans/scene-renderer-design.md "Depth's
   exactness and DATE"), by number and by name: settings.json's display "scene_depth", --scene-depth, the player
   menu's Depth row (High, Medium, Low; unsettled is the developers'). Chosen as the renderer opens (the depth's
   format, its shaders and the pipelines made before play), so a change takes effect at the next start. A mode is one
   entry here. Each also has the name it had before the menu's levels (exact, nearer, gpu), still read. */
#ifndef PORT_SCENE_DEPTH_H
#define PORT_SCENE_DEPTH_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PORT_SCENE_DEPTH_EXACT,     /* the GS's whole Z at each pixel (exact Z), decals settled by the stencil: the default */
    PORT_SCENE_DEPTH_NEARER,    /* exact Z, decals settled in one pass where they are nearer (no stencil) */
    PORT_SCENE_DEPTH_UNSETTLED, /* exact Z, decals left with the top fine part they were tested with */
    PORT_SCENE_DEPTH_GPU,       /* the GPU's own depth, interpolated (D16_UNORM for the 16-bit Z buffers) */
    PORT_SCENE_DEPTH_COUNT
};

typedef struct {
    const char *name;     /* settings.json's and the command line's */
    const char *label;    /* the player menu's (unsettled is not in it) */
    const char *old_name; /* its name before the menu's levels (2026-11-22), still read */
} PortSceneDepth;

static inline const PortSceneDepth *port_scene_depth(int mode) {
    static const PortSceneDepth modes[PORT_SCENE_DEPTH_COUNT] = {
        {"high", "High", "exact"},                     /* PORT_SCENE_DEPTH_EXACT (in the enum's order) */
        {"medium", "Medium", "nearer"},                /* PORT_SCENE_DEPTH_NEARER */
        {"unsettled", "Exact, layers unsettled", NULL}, /* PORT_SCENE_DEPTH_UNSETTLED (the developers') */
        {"low", "Low", "gpu"},                         /* PORT_SCENE_DEPTH_GPU */
    };
    return mode >= 0 && mode < PORT_SCENE_DEPTH_COUNT ? &modes[mode] : NULL;
}

/* The mode named name (its name or its old one), or -1 for none */
static inline int port_scene_depth_from_name(const char *name) {
    for (int m = 0; name != NULL && m < PORT_SCENE_DEPTH_COUNT; m++) {
        const PortSceneDepth *d = port_scene_depth(m);
        if (strcmp(d->name, name) == 0 || (d->old_name != NULL && strcmp(d->old_name, name) == 0)) {
            return m;
        }
    }
    return -1;
}

/* The mode the renderer opens with: mode (the settings' or --scene-depth's) unless the developers' variables say
   otherwise: KF4_SCN_DEPTH=gpu (depth_env), else KF4_SCN_SETTLE=0 or nearer (settle_env); NULL for one not set */
static inline int port_scene_depth_with_env(int mode, const char *depth_env, const char *settle_env) {
    if (depth_env != NULL && strcmp(depth_env, "gpu") == 0) {
        return PORT_SCENE_DEPTH_GPU;
    }
    if (settle_env != NULL && strcmp(settle_env, "0") == 0) {
        return PORT_SCENE_DEPTH_UNSETTLED;
    }
    if (settle_env != NULL && strcmp(settle_env, "nearer") == 0) {
        return PORT_SCENE_DEPTH_NEARER;
    }
    return port_scene_depth(mode) != NULL ? mode : PORT_SCENE_DEPTH_EXACT;
}

#ifdef __cplusplus
}
#endif

#endif
