// FSR 3.1 through the FidelityFX API (upscale_ffx.h). The headers are the SDK's (third_party/ffx-api, MIT); the DLL
// is AMD's signed build, not kept here: CMake's KF4_FFX_DLL copies one beside kf4.exe.
#include "upscale_ffx.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/vk/ffx_api_vk.h"

namespace {

ffxFunctions ffx = {};
int loaded = 0; // 1 loaded, -1 not there

void ffx_message(uint32_t type, const wchar_t *message) {
    fprintf(stderr, "[gsn] FSR %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message);
}

// (FFX's device functions) Its build asks for some by their KHR names (vkGetBufferMemoryRequirements2KHR) where the
// device has them as Vulkan 1.1's own, without the extension enabled: there, the core function of that name less KHR
PFN_vkGetDeviceProcAddr device_proc = nullptr;
PFN_vkVoidFunction VKAPI_CALL device_proc_core(VkDevice device, const char *name) {
    PFN_vkVoidFunction f = device_proc(device, name);
    size_t n = strlen(name);
    if (!f && n > 3 && n < 128 && strcmp(name + n - 3, "KHR") == 0) {
        char core[128];
        memcpy(core, name, n - 3);
        core[n - 3] = 0;
        f = device_proc(device, core);
    }
    return f;
}

bool debug_checking() {
    const char *on = getenv("KF4_FFX_DEBUG");
    return on && atoi(on) > 0;
}

uint32_t surface_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R32_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default:
        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// (the image as FFX takes it: described by hand, in GENERAL, which FFX's UNORDERED_ACCESS state is, before and after)
FfxApiResource resource(const UpscaleImage &i, bool written) {
    FfxApiResource r = {};
    r.resource = reinterpret_cast<void *>(i.image);
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = surface_format(i.format);
    r.description.width = i.width;
    r.description.height = i.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return r;
}

} // namespace

bool ffx_upscale_load() {
    if (loaded) {
        return loaded > 0;
    }
    loaded = -1;
    // (by its whole path: beside kf4.exe, or KF4_FFX_DLL's; never the search path's)
    wchar_t path[MAX_PATH] = {};
    const char *env = getenv("KF4_FFX_DLL");
    if (env && *env) {
        MultiByteToWideChar(CP_UTF8, 0, env, -1, path, MAX_PATH);
    } else {
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return false;
        }
        wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash || size_t(slash + 1 - path) + 22 >= MAX_PATH) {
            return false;
        }
        wcscpy(slash + 1, L"amd_fidelityfx_vk.dll");
    }
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        fprintf(stderr, "[gsn] --upscaler fsr3: %ls is not there (or would not load): bilinear instead\n", path);
        return false;
    }
    ffxLoadFunctions(&ffx, module);
    if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Dispatch) {
        fprintf(stderr, "[gsn] --upscaler fsr3: %ls has not the FidelityFX API's functions: bilinear instead\n", path);
        return false;
    }
    loaded = 1;
    return true;
}

void *ffx_upscale_make(VkDevice device, VkPhysicalDevice gpu, PFN_vkGetDeviceProcAddr proc, uint32_t render_w,
                       uint32_t render_h, uint32_t out_w, uint32_t out_h) {
    if (!ffx_upscale_load()) {
        return nullptr;
    }
    ffxCreateBackendVKDesc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = device;
    backend.vkPhysicalDevice = gpu;
    device_proc = proc;
    backend.vkDeviceProcAddr = device_proc_core;
    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    // (the scene's colours are the PS2's: perceptual, not linear; its depth as upscale_ffx.h says)
    desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                 FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | (debug_checking() ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0);
    desc.maxRenderSize = {render_w, render_h};
    desc.maxUpscaleSize = {out_w, out_h};
    desc.fpMessage = ffx_message;
    ffxContext context = nullptr;
    ffxReturnCode_t r = ffx.CreateContext(&context, &desc.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        fprintf(stderr, "[gsn] --upscaler fsr3: no context for %ux%u -> %ux%u (%u): bilinear instead\n", render_w,
                render_h, out_w, out_h, r);
        return nullptr;
    }
    ffxQueryGetProviderVersion version = {};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx.Query && ffx.Query(&context, &version.header) == FFX_API_RETURN_OK && version.versionName) {
        fprintf(stderr, "[gsn] --upscaler fsr3: %s, %ux%u -> %ux%u\n", version.versionName, render_w, render_h, out_w,
                out_h);
    }
    return context;
}

bool ffx_upscale_dispatch(void *context, const UpscaleFrame &f) {
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = reinterpret_cast<void *>(f.cmd);
    d.color = resource(f.color, false);
    d.depth = resource(f.depth, false);
    d.motionVectors = resource(f.motion, false);
    d.output = resource(f.output, true);
    d.jitterOffset = {f.jitter[0], f.jitter[1]};
    d.motionVectorScale = {1.0f, 1.0f}; // (the scene's pixels already)
    d.renderSize = {f.render_w, f.render_h};
    d.upscaleSize = {f.output.width, f.output.height};
    d.enableSharpening = false;
    d.sharpness = 0.0f;
    d.frameTimeDelta = f.frame_ms;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = FLT_MAX; // (inverted and infinite: FSR's way, the near plane given as far)
    d.cameraFar = f.near_plane;
    d.cameraFovAngleVertical = f.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    ffxContext c = context;
    ffxReturnCode_t r = ffx.Dispatch(&c, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int said;
        if (!said++) {
            fprintf(stderr, "[gsn] --upscaler fsr3: a picture was not upscaled (%u)\n", r);
        }
        return false;
    }
    return true;
}

void ffx_upscale_free(void *context) {
    if (context && loaded > 0) {
        ffxContext c = context;
        ffx.DestroyContext(&c, nullptr);
    }
}

#else // (FSR here through its Windows DLL only)

bool ffx_upscale_load() {
    return false;
}
void *ffx_upscale_make(VkDevice, VkPhysicalDevice, PFN_vkGetDeviceProcAddr, uint32_t, uint32_t, uint32_t, uint32_t) {
    return nullptr;
}
bool ffx_upscale_dispatch(void *, const UpscaleFrame &) {
    return false;
}
void ffx_upscale_free(void *) {}

#endif
