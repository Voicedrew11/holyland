/* The picture's anti-aliasing: the methods the PC renderer has (port/gs/pc/pcr_antialias.h), by number and by name
   (settings.json's display "antialias", --antialias, the overlay, KF4_SET_AT's antialias). A method is one entry here
   and one implementation of pcr::AntiAliasMethod there. */
#ifndef PORT_ANTIALIAS_H
#define PORT_ANTIALIAS_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PORT_AA_OFF,  /* the picture as drawn */
    PORT_AA_FXAA, /* FXAA over the picture at the renderer's resolution (port/gs/pc/shaders/pcr_fxaa.frag) */
    PORT_AA_SMAA, /* SMAA 1x over the picture at the renderer's resolution (port/gs/pc/shaders/pcr_smaa.h) */
    PORT_AA_SMAA_T2X, /* SMAA T2x: SMAA 1x of two pictures jittered in turn, the previous reprojected
                         (pcr_antialias.cpp's SmaaT2x) */
    PORT_AA_COUNT
};

typedef struct {
    const char *name;  /* settings.json's and the command line's */
    const char *label; /* the overlay's */
} PortAntiAlias;

static inline const PortAntiAlias *port_antialias(int method) {
    static const PortAntiAlias methods[PORT_AA_COUNT] = {
        {"off", "Off"},   /* PORT_AA_OFF (in the enum's order: C++ includes this too) */
        {"fxaa", "FXAA"}, /* PORT_AA_FXAA */
        {"smaa", "SMAA"}, /* PORT_AA_SMAA */
        {"smaa_t2x", "SMAA T2x"}, /* PORT_AA_SMAA_T2X */
    };
    return method >= 0 && method < PORT_AA_COUNT ? &methods[method] : NULL;
}

/* The method named name, or -1 for none */
static inline int port_antialias_from_name(const char *name) {
    for (int m = 0; name != NULL && m < PORT_AA_COUNT; m++) {
        if (strcmp(port_antialias(m)->name, name) == 0) {
            return m;
        }
    }
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif
