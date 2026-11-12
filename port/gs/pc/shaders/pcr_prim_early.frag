#version 450
#extension GL_GOOGLE_include_directive : require
// pcr_prim.frag for the draws that discard nothing: the depth test before the shader (pcr_prim.h's PCR_EARLY)
#define PCR_EARLY
#include "pcr_prim.h"
