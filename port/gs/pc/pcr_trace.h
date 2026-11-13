/* The developers' pixel trace's records (pcr.h's pcr_debug_trace*; kf4-gsreplay --renderer pc --pixel): plain types,
   laid out as the shaders write them (shaders/pcr_prim.h's Trace, pcr_probe.h's probes). pcr.h says what they hold. */
#ifndef PORT_GS_PC_PCR_TRACE_H
#define PORT_GS_PC_PCR_TRACE_H

#include <stdint.h>

typedef struct {
    uint32_t fbp;
    int32_t x, y, sub_x, sub_y;
} PcrTraceSpot;

enum { PCR_TRACE_SHADED = 0, PCR_TRACE_DATE_DISCARD = 1, PCR_TRACE_ALPHA_DISCARD = 2 };
#define PCR_TRACE_PASS(how) ((how) & 0xFu)
#define PCR_TRACE_OUTCOME(how) (((how) >> 4) & 0xFu)
#define PCR_TRACE_DECAL(how) (((how) >> 8) & 1u)

typedef struct {
    uint32_t draw;      /* pcr_debug_trace_draw's number */
    uint32_t primitive; /* its batch's triangle (gl_PrimitiveID) */
    float z;            /* its Z (depth / the Z scale): at the pixel's centre, or at its sample when shaded a sample */
    float dz_dx;        /* Z's change a surface pixel across and down */
    float dz_dy;
    uint32_t coverage;  /* the samples it covers (gl_SampleMaskIn), before the depth test */
    int32_t sample;     /* shaded once a sample: which (cut-out AA), else -1 */
    uint32_t rgba;      /* the colour it outputs, the GS's integers before the blend (0 when DATE discarded it) */
    /* bits 0-3: the pass (0 every pixel, 1 the alpha test's passing pixels, 2 its failing ones); 4-7: PCR_TRACE_*;
       8: a decal (pcr_canvas.cpp's is_decal) */
    uint32_t how;
    uint32_t reserved[3];
} PcrTraceFragment;

typedef struct {
    uint32_t draw;     /* the last pcr_debug_trace_draw before it */
    uint32_t samples;  /* the surfaces' */
    uint32_t has_depth; /* bit 0: z holds the depth surface's; bit 1: rgba not read (the colour's samples differ) */
    uint32_t reserved;
    float z[8];        /* each sample's Z in the depth surface (depth / the Z scale) */
    uint32_t rgba[8];  /* each sample's colour, 8 bits a channel (R in the low byte) */
} PcrTraceProbe;

#endif
