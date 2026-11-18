// Which build of the game a release makes (docs/port/LAUNCHER.md, "Builds"): a new release, other tools or
// other source makes a new key, so a new build; the same ones find the last build and play it.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace launcher {

using Sha256 = std::array<std::uint8_t, 32>;

class Sha256Hasher {
public:
    Sha256Hasher();
    void add(std::string_view bytes);
    Sha256 finish(); // once
private:
    void block(const std::uint8_t *p);
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> pending_;
    std::size_t pending_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::string to_hex(const Sha256 &digest);

// 16 hex digits naming builds/<key>/: the version, the release's manifest (every shipped file and its
// SHA-256) and the tools' id
std::string build_key(std::string_view version, std::string_view manifest, std::string_view toolchain_id);

} // namespace launcher
