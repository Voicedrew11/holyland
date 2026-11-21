// The player menu's alpha masks (alpha_mask.h)
#include "alpha_mask.h"

#include <cstdint>

namespace launcher {

namespace {

constexpr std::size_t kHeaderBytes = 12; // "KF4A", width, height

std::uint32_t little_endian_u32(const unsigned char *bytes) {
    return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 | std::uint32_t(bytes[2]) << 16 |
           std::uint32_t(bytes[3]) << 24;
}

} // namespace

std::optional<AlphaMask> read_alpha_mask(std::string_view file) {
    if (file.size() < kHeaderBytes || file.substr(0, 4) != "KF4A")
        return std::nullopt;
    const auto *bytes = reinterpret_cast<const unsigned char *>(file.data());
    std::uint64_t width = little_endian_u32(bytes + 4), height = little_endian_u32(bytes + 8);
    // (sizes in 64 bits: two 32-bit sizes cannot overflow their product there)
    if (width == 0 || height == 0 || width > 65536 || height > 65536 || width * height != file.size() - kHeaderBytes)
        return std::nullopt;
    return AlphaMask{int(width), int(height), bytes + kHeaderBytes};
}

} // namespace launcher
