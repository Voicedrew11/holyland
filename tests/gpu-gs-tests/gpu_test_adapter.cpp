// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/gs/gs_vulkan_backend.h"
#include <memory>

std::unique_ptr<GSRasterBackend> createGpuTestBackend()
{
    return std::make_unique<GSVulkanBackend>();
}
