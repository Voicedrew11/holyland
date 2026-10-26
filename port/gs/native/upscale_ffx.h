// The temporal upscalers' own calls (--upscaler, docs/port/UPSCALING.md): AMD's FSR 3.1 through the FidelityFX API,
// its signed DLL (amd_fidelityfx_vk.dll, beside kf4.exe or KF4_FFX_DLL) loaded at run time. Apart from gsn.cpp, so
// that windows.h and the FFX headers stay out of it; the back end's thread makes these calls (gsn_upscale.inc).
#pragma once
#include "upscale.h"

// The DLL and its functions, loaded once: false (said why, once) where it is not there
bool ffx_upscale_load();
// A context for pictures of render_* upscaled to out_*: null (said why) where it could not be made
void *ffx_upscale_make(VkDevice device, VkPhysicalDevice gpu, PFN_vkGetDeviceProcAddr proc, uint32_t render_w,
                       uint32_t render_h, uint32_t out_w, uint32_t out_h);
// Its calls for one picture, into f.cmd (compute: its pipelines and descriptor sets bound past Granite)
bool ffx_upscale_dispatch(void *context, const UpscaleFrame &f);
// (once the GPU is done with the pictures it made)
void ffx_upscale_free(void *context);
