// Whether the device has Vulkan's dynamic depth, stencil, culling and topology state (cmake/granite_dynamic_state.cmake):
// Vulkan 1.3's own, or VK_EXT_extended_dynamic_state on an older driver (Granite's calls promoted to its names).
#pragma once

#include "context.hpp"

namespace pcr {

inline bool has_dynamic_depth_state(const Vulkan::DeviceFeatures &f) {
    return f.device_api_core_version >= VK_API_VERSION_1_3 || f.eds_features.extendedDynamicState;
}

} // namespace pcr
