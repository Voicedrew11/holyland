#pragma once
#include <string_view>

namespace ps2_runtime_calls
{
    inline std::string_view resolveSyscallName(std::string_view) { return {}; }
    inline std::string_view resolveStubName(std::string_view name)
    {
        return name == "sceSifFreeIopHeap" ? name : std::string_view{};
    }
}
