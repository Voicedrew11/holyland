// The project's Ed25519 release key: the public half of the key the release workflow signs each release's SHA256SUMS
// with (tools/release_sign.sh, the RELEASE_SIGNING_KEY secret). The same 32 bytes as deploy/release-key.pub. A new
// key is an edit of both; launchers built before it never update to a release signed with the new one.
#pragma once

#include <array>
#include <cstdint>

namespace launcher {

inline constexpr std::array<std::uint8_t, 32> kReleaseKey = {
    0x40, 0x32, 0xf8, 0xd2, 0xe4, 0x22, 0x35, 0x6a, 0x5b, 0xc4, 0x4d, 0xe6, 0x72, 0xb5, 0x87, 0x6a, 0x3d, 0x84, 0x40, 0x5f, 0x22, 0xed, 0x38, 0x96, 0x55, 0xd2, 0x20, 0xe8, 0x82, 0x1f, 0x01, 0x73};

} // namespace launcher
