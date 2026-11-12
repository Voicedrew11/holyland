#version 450
#extension GL_GOOGLE_include_directive : require
// pcr_prim_sample.frag for the draws that discard nothing (pcr_prim.h's PCR_SAMPLE and PCR_EARLY)
#define PCR_SAMPLE
#define PCR_EARLY
#include "pcr_prim.h"
