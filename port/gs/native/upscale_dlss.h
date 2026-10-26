// --upscaler dlss (docs/port/UPSCALING.md): NVIDIA's DLSS super resolution through NGX, the driver's own NGX core
// (_nvngx.dll) loaded at run time and called directly, and nvngx_dlss.dll beside kf4.exe. Built with NGX's headers
// only where CMake's KF4_DLSS_SDK names the SDK (its licence keeps it out of this repository); without, every call
// here says no. NGX wants Vulkan extensions of its own when the instance and the device are made (gsn_open), and the
// rest is the back end's thread's (gsn_upscale.inc).
#pragma once
#include <string>
#include <vector>
#include "upscale.h"

// (gsn_open, before the instance) Whether DLSS can be had here (built with it, an NVIDIA driver's NGX core), and the
// instance extensions it wants
bool dlss_instance_extensions(std::vector<std::string> &out);
// (gsn_open, between the instance and the device) The device extensions it wants of this GPU
bool dlss_device_extensions(VkInstance instance, VkPhysicalDevice gpu, std::vector<std::string> &out);
// (the back end's thread, once) NGX for the device made with them: false (said why) where DLSS cannot be had
bool dlss_init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
               PFN_vkGetDeviceProcAddr gdpa);
// A feature for pictures of render_* upscaled to out_*, its making recorded into cmd: null (said why) where it could
// not be made
void *dlss_make(VkCommandBuffer cmd, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h);
// Its calls for one picture, into f.cmd (compute: its pipelines and descriptor sets bound past Granite)
bool dlss_dispatch(void *feature, const UpscaleFrame &f);
// (once the GPU is done with the pictures it made)
void dlss_free(void *feature);
