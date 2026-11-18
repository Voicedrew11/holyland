// The build key and SHA-256 (build_key.h). SHA-256 as FIPS 180-4 has it; tested against its examples
// (tests/launcher_test.cpp).
#include "build_key.h"

#include <cstring>

namespace launcher {

namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

} // namespace

Sha256Hasher::Sha256Hasher()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, pending_{} {}

void Sha256Hasher::block(const std::uint8_t *p) {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; i++)
        w[i] = (std::uint32_t)p[i * 4] << 24 | (std::uint32_t)p[i * 4 + 1] << 16 | (std::uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; i++) {
        std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256Hasher::add(std::string_view bytes) {
    total_bytes_ += bytes.size();
    const auto *p = reinterpret_cast<const std::uint8_t *>(bytes.data());
    std::size_t left = bytes.size();
    while (left > 0) {
        std::size_t take = 64 - pending_size_ < left ? 64 - pending_size_ : left;
        std::memcpy(pending_.data() + pending_size_, p, take);
        pending_size_ += take;
        p += take;
        left -= take;
        if (pending_size_ == 64) {
            block(pending_.data());
            pending_size_ = 0;
        }
    }
}

Sha256 Sha256Hasher::finish() {
    std::uint64_t bits = total_bytes_ * 8;
    std::uint8_t pad[72] = {0x80};
    std::size_t pad_size = (pending_size_ < 56 ? 56 : 120) - pending_size_;
    for (int i = 0; i < 8; i++)
        pad[pad_size + i] = (std::uint8_t)(bits >> (56 - 8 * i));
    std::uint64_t kept = total_bytes_;
    add(std::string_view(reinterpret_cast<const char *>(pad), pad_size + 8));
    total_bytes_ = kept;
    Sha256 digest{};
    for (std::size_t i = 0; i < 8; i++) {
        digest[i * 4] = (std::uint8_t)(state_[i] >> 24);
        digest[i * 4 + 1] = (std::uint8_t)(state_[i] >> 16);
        digest[i * 4 + 2] = (std::uint8_t)(state_[i] >> 8);
        digest[i * 4 + 3] = (std::uint8_t)state_[i];
    }
    return digest;
}

std::string to_hex(const Sha256 &digest) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    for (std::uint8_t byte : digest) {
        text += kDigits[byte >> 4];
        text += kDigits[byte & 15];
    }
    return text;
}

std::string build_key(std::string_view version, std::string_view manifest, std::string_view toolchain_id) {
    Sha256Hasher hasher;
    // each part's length first, so that no two different sets of parts hash the same text
    for (std::string_view part : {std::string_view("darker-reality build 1"), version, manifest, toolchain_id}) {
        hasher.add(std::to_string(part.size()));
        hasher.add(":");
        hasher.add(part);
    }
    return to_hex(hasher.finish()).substr(0, 16);
}

} // namespace launcher
