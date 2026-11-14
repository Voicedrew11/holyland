#version 450
#extension GL_GOOGLE_include_directive : require
// pcr_prim.frag for a tolerant layers' pass that writes Z alone and discards nothing (pcr_canvas.cpp's tolerant passes):
// the pixel's Z (pcr_prim.h's pcr_exact_depth) and nothing else, no texture read, no colour
#define PCR_ZONLY
#include "pcr_prim.h"
