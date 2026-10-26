// DLSS super resolution through NGX (upscale_dlss.h). The SDK's own way in is a static library that finds the
// driver's NGX core (_nvngx.dll) and forwards to it; its Windows build is MSVC's (its C and C++ runtime), which this
// build cannot link. So the core is loaded here and called directly, as that library calls it. Its Linux build
// (libnvsdk_ngx.a) shows how: every function forwards its arguments as they are, but the inits, which take the SDK's
// version before the feature info. NGX's headers come from the SDK (KF4_DLSS_SDK) and are not kept here.
#include "upscale_dlss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(KF4_HAVE_DLSS) && defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_defs_vk.h"

namespace {

// The core's functions (the names it exports)
typedef NVSDK_NGX_Result (*PfnInitProjectIdExt)(const char *project, NVSDK_NGX_EngineType engine,
                                                const char *engine_version, const wchar_t *data_path,
                                                VkInstance instance, VkPhysicalDevice gpu, VkDevice device,
                                                PFN_vkGetInstanceProcAddr gipa,
                                                PFN_vkGetDeviceProcAddr gdpa, NVSDK_NGX_Version version,
                                                const NVSDK_NGX_FeatureCommonInfo *info);
typedef NVSDK_NGX_Result (*PfnInstanceExtensions)(const NVSDK_NGX_FeatureDiscoveryInfo *info, uint32_t *count,
                                                  VkExtensionProperties **out);
typedef NVSDK_NGX_Result (*PfnDeviceExtensions)(VkInstance instance, VkPhysicalDevice gpu,
                                                const NVSDK_NGX_FeatureDiscoveryInfo *info, uint32_t *count,
                                                VkExtensionProperties **out);
typedef NVSDK_NGX_Result (*PfnParameters)(void **out);
typedef NVSDK_NGX_Result (*PfnCreateFeature1)(VkDevice device, VkCommandBuffer cmd, NVSDK_NGX_Feature feature,
                                              void *params, NVSDK_NGX_Handle **out);
typedef NVSDK_NGX_Result (*PfnEvaluateFeature)(VkCommandBuffer cmd, const NVSDK_NGX_Handle *handle, const void *params,
                                               void *progress);
typedef NVSDK_NGX_Result (*PfnReleaseFeature)(NVSDK_NGX_Handle *handle);

struct Core {
    int loaded = 0; // 1 loaded, -1 not there
    PfnInitProjectIdExt init = nullptr;
    PfnInstanceExtensions instance_extensions = nullptr;
    PfnDeviceExtensions device_extensions = nullptr;
    PfnParameters capabilities = nullptr, allocate = nullptr;
    PfnCreateFeature1 create = nullptr;
    PfnEvaluateFeature evaluate = nullptr;
    PfnReleaseFeature release = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    void *params = nullptr; // (the parameters the feature is made and run with)
    wchar_t exe_dir[MAX_PATH] = {};
    wchar_t data_dir[MAX_PATH] = {};
} core;

// (holyland's: a project ID as NGX wants of an engine of its own, GUID-like)
const char *const project_id = "9c3f2a71-5d4e-4b8a-a6f1-3e7b0c9d2f58";

bool debug() {
    const char *on = getenv("KF4_DLSS_DEBUG");
    return on && atoi(on) > 0;
}

void ngx_log(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    fprintf(stderr, "[gsn] NGX: %s", message);
    size_t n = strlen(message);
    if (n == 0 || message[n - 1] != '\n') {
        fputc('\n', stderr);
    }
}

// (the feature DLLs beside kf4.exe; NGX's logs in the temporary folder, never the player's)
NVSDK_NGX_FeatureCommonInfo common_info() {
    static const wchar_t *paths[1];
    paths[0] = core.exe_dir;
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = debug() ? ngx_log : nullptr;
    info.LoggingInfo.MinimumLoggingLevel = debug() ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = false;
    return info;
}

NVSDK_NGX_FeatureDiscoveryInfo discovery(const NVSDK_NGX_FeatureCommonInfo *common) {
    NVSDK_NGX_FeatureDiscoveryInfo d = {};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = project_id;
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    d.ApplicationDataPath = core.data_dir;
    d.FeatureInfo = common;
    return d;
}

// The driver's NGX core: where its installer says (the registry keys the SDK's loader reads)
bool load_core() {
    if (core.loaded) {
        return core.loaded > 0;
    }
    core.loaded = -1;
    DWORD n = GetModuleFileNameW(nullptr, core.exe_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return false;
    }
    if (wchar_t *slash = wcsrchr(core.exe_dir, L'\\')) {
        *slash = 0;
    }
    DWORD t = GetTempPathW(MAX_PATH, core.data_dir);
    if (t == 0 || t >= MAX_PATH) {
        wcscpy(core.data_dir, core.exe_dir);
    }
    static const struct {
        const wchar_t *key, *value;
    } places[] = {
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath"},
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath"},
        {L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath"},
    };
    HMODULE module = nullptr;
    for (const auto &p : places) {
        wchar_t dir[MAX_PATH] = {};
        DWORD size = sizeof(dir);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, p.key, p.value, RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) {
            continue;
        }
        for (const wchar_t *name : {L"\\_nvngx.dll", L"\\nvngx.dll"}) {
            wchar_t path[MAX_PATH];
            if (wcslen(dir) + wcslen(name) >= MAX_PATH) {
                continue;
            }
            wcscpy(path, dir);
            wcscat(path, name);
            module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module) {
                break;
            }
        }
        if (module) {
            break;
        }
    }
    if (!module) {
        fprintf(stderr, "[gsn] --upscaler dlss: no NVIDIA driver's NGX here\n");
        return false;
    }
    core.init = reinterpret_cast<PfnInitProjectIdExt>(GetProcAddress(module, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext"));
    core.instance_extensions = reinterpret_cast<PfnInstanceExtensions>(
        GetProcAddress(module, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements"));
    core.device_extensions = reinterpret_cast<PfnDeviceExtensions>(
        GetProcAddress(module, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements"));
    core.capabilities =
        reinterpret_cast<PfnParameters>(GetProcAddress(module, "NVSDK_NGX_VULKAN_GetCapabilityParameters"));
    core.allocate = reinterpret_cast<PfnParameters>(GetProcAddress(module, "NVSDK_NGX_VULKAN_AllocateParameters"));
    core.create = reinterpret_cast<PfnCreateFeature1>(GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1"));
    core.evaluate = reinterpret_cast<PfnEvaluateFeature>(GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    core.release = reinterpret_cast<PfnReleaseFeature>(GetProcAddress(module, "NVSDK_NGX_VULKAN_ReleaseFeature"));
    if (!core.init || !core.instance_extensions || !core.device_extensions || !core.capabilities || !core.allocate ||
        !core.create || !core.evaluate || !core.release) {
        fprintf(stderr, "[gsn] --upscaler dlss: the driver's NGX has not the functions this build calls\n");
        return false;
    }
    core.loaded = 1;
    return true;
}

// NGX's parameters: an object of the core's, made by MSVC's C++, used here through its table of virtual functions
// by slot as MSVC lays the table out: each name's overloads together, the last declared first. So Set's void *,
// ID3D12Resource *, ID3D11Resource *, int, unsigned int, double, float, unsigned long long (0 to 7), then Get's
// the same (8 to 15), then Reset. Never through the class: this compiler would lay the table out in declaration
// order. dlss_init proves the slots before anything is made with them. (A member function's call is the platform's
// usual one, the object first: a float goes in the third argument's register as for any function.)
enum Slot {
    SET_PTR = 0, SET_I = 3, SET_UI = 4, SET_F = 6, SET_ULL = 7,
    GET_PTR = 8, GET_I = 11, GET_UI = 12, GET_F = 14, GET_ULL = 15,
};
template <class T> void set(void *p, Slot slot, const char *name, T value) {
    auto f = reinterpret_cast<void (*)(void *, const char *, T)>((*static_cast<void ***>(p))[slot]);
    f(p, name, value);
}
template <class T> NVSDK_NGX_Result get(void *p, Slot slot, const char *name, T *out) {
    auto f = reinterpret_cast<NVSDK_NGX_Result (*)(void *, const char *, T *)>((*static_cast<void ***>(p))[slot]);
    return f(p, name, out);
}

// (dlss_init) Each kind set and read back the same through the slots above, and a name never set not found
bool slots_proved(void *p) {
    unsigned long long ull = 0;
    float f = 0;
    unsigned int ui = 0;
    int i = 0;
    void *ptr = nullptr;
    set(p, SET_ULL, "holyland.check.ull", 0x123456789abcdef0ull);
    set(p, SET_F, "holyland.check.f", 0.625f);
    set(p, SET_UI, "holyland.check.ui", 0xdeadbeefu);
    set(p, SET_I, "holyland.check.i", -12345);
    set(p, SET_PTR, "holyland.check.ptr", static_cast<void *>(&core));
    return get(p, GET_ULL, "holyland.check.ull", &ull) == NVSDK_NGX_Result_Success && ull == 0x123456789abcdef0ull &&
           get(p, GET_F, "holyland.check.f", &f) == NVSDK_NGX_Result_Success && f == 0.625f &&
           get(p, GET_UI, "holyland.check.ui", &ui) == NVSDK_NGX_Result_Success && ui == 0xdeadbeefu &&
           get(p, GET_I, "holyland.check.i", &i) == NVSDK_NGX_Result_Success && i == -12345 &&
           get(p, GET_PTR, "holyland.check.ptr", &ptr) == NVSDK_NGX_Result_Success && ptr == &core &&
           get(p, GET_UI, "holyland.check.never", &ui) != NVSDK_NGX_Result_Success;
}

std::vector<std::string> names(uint32_t count, const VkExtensionProperties *props) {
    std::vector<std::string> out;
    for (uint32_t i = 0; props && i < count; i++) {
        out.push_back(props[i].extensionName);
    }
    return out;
}

NVSDK_NGX_Resource_VK resource(const UpscaleImage &i, bool written) {
    NVSDK_NGX_Resource_VK r = {};
    r.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    r.Resource.ImageViewInfo.ImageView = i.view;
    r.Resource.ImageViewInfo.Image = i.image;
    r.Resource.ImageViewInfo.SubresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r.Resource.ImageViewInfo.Format = i.format;
    r.Resource.ImageViewInfo.Width = i.width;
    r.Resource.ImageViewInfo.Height = i.height;
    r.ReadWrite = written;
    return r;
}

} // namespace

bool dlss_instance_extensions(std::vector<std::string> &out) {
    if (!load_core()) {
        return false;
    }
    NVSDK_NGX_FeatureCommonInfo common = common_info();
    NVSDK_NGX_FeatureDiscoveryInfo d = discovery(&common);
    uint32_t count = 0;
    VkExtensionProperties *props = nullptr;
    NVSDK_NGX_Result r = core.instance_extensions(&d, &count, &props);
    if (r == NVSDK_NGX_Result_FAIL_NotImplemented) {
        out = {"VK_KHR_get_physical_device_properties2"}; // (the SDK's library's answer for a core without the query)
        return true;
    }
    if (r != NVSDK_NGX_Result_Success) {
        fprintf(stderr, "[gsn] --upscaler dlss: NGX names no instance extensions (%#x)\n", unsigned(r));
        return false;
    }
    out = names(count, props);
    return true;
}

bool dlss_device_extensions(VkInstance instance, VkPhysicalDevice gpu, std::vector<std::string> &out) {
    if (!load_core()) {
        return false;
    }
    NVSDK_NGX_FeatureCommonInfo common = common_info();
    NVSDK_NGX_FeatureDiscoveryInfo d = discovery(&common);
    uint32_t count = 0;
    VkExtensionProperties *props = nullptr;
    NVSDK_NGX_Result r = core.device_extensions(instance, gpu, &d, &count, &props);
    if (r == NVSDK_NGX_Result_FAIL_NotImplemented) {
        out = {"VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_buffer_device_address",
               "VK_KHR_push_descriptor"}; // (as above)
        return true;
    }
    if (r != NVSDK_NGX_Result_Success) {
        fprintf(stderr, "[gsn] --upscaler dlss: NGX names no device extensions for this GPU (%#x)\n", unsigned(r));
        return false;
    }
    out = names(count, props);
    return true;
}

bool dlss_init(VkInstance instance, VkPhysicalDevice gpu, VkDevice device, PFN_vkGetInstanceProcAddr gipa,
               PFN_vkGetDeviceProcAddr gdpa) {
    if (!load_core()) {
        return false;
    }
    NVSDK_NGX_FeatureCommonInfo common = common_info();
    NVSDK_NGX_Result r = core.init(project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", core.data_dir, instance, gpu,
                                   device, gipa, gdpa, NVSDK_NGX_Version_API, &common);
    if (r != NVSDK_NGX_Result_Success) {
        fprintf(stderr, "[gsn] --upscaler dlss: NGX would not start (%#x)\n", unsigned(r));
        return false;
    }
    void *caps = nullptr;
    int available = 0, needs_driver = 0;
    unsigned int major = 0, minor = 0;
    if (core.capabilities(&caps) != NVSDK_NGX_Result_Success || !caps) {
        fprintf(stderr, "[gsn] --upscaler dlss: NGX has no capabilities to read\n");
        return false;
    }
    if (core.allocate(&core.params) != NVSDK_NGX_Result_Success || !core.params || !slots_proved(core.params)) {
        fprintf(stderr, "[gsn] --upscaler dlss: NGX's parameters are not laid out as this build reads them\n");
        return false;
    }
    if (get(caps, GET_I, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver) ==
            NVSDK_NGX_Result_Success &&
        needs_driver) {
        get(caps, GET_UI, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        get(caps, GET_UI, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        fprintf(stderr, "[gsn] --upscaler dlss: DLSS wants a newer driver (%u.%u)\n", major, minor);
        return false;
    }
    if (get(caps, GET_I, NVSDK_NGX_Parameter_SuperSampling_Available, &available) != NVSDK_NGX_Result_Success ||
        !available) {
        fprintf(stderr, "[gsn] --upscaler dlss: DLSS is not available on this GPU (or nvngx_dlss.dll is not beside "
                        "kf4.exe)\n");
        return false;
    }
    core.device = device;
    return true;
}

void *dlss_make(VkCommandBuffer cmd, uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h) {
    void *p = core.params;
    if (!p) {
        return nullptr;
    }
    // (the quality the ratio is nearest: DLSS's own sizes for it bound the scene's)
    float ratio = float(out_w) / float(render_w);
    int quality = ratio >= 2.5f ? NVSDK_NGX_PerfQuality_Value_UltraPerformance
                  : ratio >= 1.85f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                  : ratio >= 1.6f  ? NVSDK_NGX_PerfQuality_Value_Balanced
                  : ratio >= 1.2f  ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                   : NVSDK_NGX_PerfQuality_Value_DLAA;
    set(p, SET_UI, NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    set(p, SET_UI, NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    set(p, SET_UI, NVSDK_NGX_Parameter_Width, render_w);
    set(p, SET_UI, NVSDK_NGX_Parameter_Height, render_h);
    set(p, SET_UI, NVSDK_NGX_Parameter_OutWidth, out_w);
    set(p, SET_UI, NVSDK_NGX_Parameter_OutHeight, out_h);
    set(p, SET_I, NVSDK_NGX_Parameter_PerfQualityValue, quality);
    // (the motion at the scene's size, without the jitter; the depth 1 near, 0 far; the PS2's colours, not HDR)
    set(p, SET_I, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted));
    set(p, SET_I, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    // (the presets the SDK's library gives a feature made without: its defaults, which the driver may change)
    static const struct {
        const char *name;
        unsigned int value;
    } presets[] = {
        {"DLSS.Hint.Render.Preset.DLAA", 0x8000000bu},
        {"DLSS.Hint.Render.Preset.Quality", 0x8000000bu},
        {"DLSS.Hint.Render.Preset.Balanced", 0x8000000bu},
        {"DLSS.Hint.Render.Preset.Performance", 0x8000000du},
        {"DLSS.Hint.Render.Preset.UltraQuality", 0x8000000bu},
        {"DLSS.Hint.Render.Preset.UltraPerformance", 0x8000000cu},
    };
    // (KF4_DLSS_PRESET=E, J, K, L, M: that model for every quality, to weigh them)
    const char *letter = getenv("KF4_DLSS_PRESET");
    unsigned int forced = letter && *letter >= 'A' && *letter <= 'Z' ? unsigned(*letter - 'A' + 1) : 0;
    for (const auto &preset : presets) {
        set(p, SET_UI, preset.name, forced ? forced : preset.value);
    }
    NVSDK_NGX_Handle *handle = nullptr;
    NVSDK_NGX_Result r = core.create(core.device, cmd, NVSDK_NGX_Feature_SuperSampling, p, &handle);
    if (r != NVSDK_NGX_Result_Success || !handle) {
        fprintf(stderr, "[gsn] --upscaler dlss: no feature for %ux%u -> %ux%u (%#x)\n", render_w, render_h, out_w, out_h,
                unsigned(r));
        return nullptr;
    }
    fprintf(stderr, "[gsn] --upscaler dlss: %ux%u -> %ux%u\n", render_w, render_h, out_w, out_h);
    return handle;
}

bool dlss_dispatch(void *feature, const UpscaleFrame &f) {
    void *p = core.params;
    NVSDK_NGX_Resource_VK color = resource(f.color, false), output = resource(f.output, true),
                          depth = resource(f.depth, false), motion = resource(f.motion, false);
    set(p, SET_PTR, NVSDK_NGX_Parameter_Color, static_cast<void *>(&color));
    set(p, SET_PTR, NVSDK_NGX_Parameter_Output, static_cast<void *>(&output));
    set(p, SET_PTR, NVSDK_NGX_Parameter_Depth, static_cast<void *>(&depth));
    set(p, SET_PTR, NVSDK_NGX_Parameter_MotionVectors, static_cast<void *>(&motion));
    set(p, SET_PTR, NVSDK_NGX_Parameter_TransparencyMask, static_cast<void *>(nullptr));
    set(p, SET_PTR, NVSDK_NGX_Parameter_ExposureTexture, static_cast<void *>(nullptr));
    set(p, SET_PTR, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, static_cast<void *>(nullptr));
    set(p, SET_F, NVSDK_NGX_Parameter_Jitter_Offset_X, f.jitter[0]);
    set(p, SET_F, NVSDK_NGX_Parameter_Jitter_Offset_Y, f.jitter[1]);
    set(p, SET_F, NVSDK_NGX_Parameter_Sharpness, 0.0f);
    set(p, SET_I, NVSDK_NGX_Parameter_Reset, f.reset ? 1 : 0);
    set(p, SET_F, NVSDK_NGX_Parameter_MV_Scale_X, 1.0f); // (the scene's pixels already)
    set(p, SET_F, NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    set(p, SET_UI, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, f.render_w);
    set(p, SET_UI, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, f.render_h);
    set(p, SET_F, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    set(p, SET_F, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    set(p, SET_F, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, f.frame_ms);
    NVSDK_NGX_Result r = core.evaluate(f.cmd, static_cast<NVSDK_NGX_Handle *>(feature), p, nullptr);
    if (r != NVSDK_NGX_Result_Success) {
        static int said;
        if (!said++) {
            fprintf(stderr, "[gsn] --upscaler dlss: a picture was not upscaled (%#x)\n", unsigned(r));
        }
        return false;
    }
    return true;
}

void dlss_free(void *feature) {
    if (feature && core.loaded > 0) {
        core.release(static_cast<NVSDK_NGX_Handle *>(feature));
    }
}

#else // (built without NGX's headers, or not on Windows: no DLSS)

bool dlss_instance_extensions(std::vector<std::string> &) {
    fprintf(stderr, "[gsn] --upscaler dlss: this build has no DLSS (CMake's KF4_DLSS_SDK names the SDK)\n");
    return false;
}
bool dlss_device_extensions(VkInstance, VkPhysicalDevice, std::vector<std::string> &) {
    return false;
}
bool dlss_init(VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr) {
    return false;
}
void *dlss_make(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {
    return nullptr;
}
bool dlss_dispatch(void *, const UpscaleFrame &) {
    return false;
}
void dlss_free(void *) {}

#endif
