// The GS's blend as Vulkan's (pcr_canvas.cpp's draws): ALPHA's (A - B) * C + D as a fixed-function blend's factors
// and op, with the part the fragment shader does (pcr_prim.h's flags: PCR_F_PREMUL, PCR_F_C_FIX,
// PCR_F_ALPHA_FACTOR), whether the colour is written at all, and whether the GS's own result is had (else the nearest
// Vulkan has, counted by the canvas, or drawn by the shader from a copy: shader_blend).
//
// Header only, no state. Unit test: pcr_blend.
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace pcr {

// pcr_common.h's flags this file sets (the canvas's PrimPush::flags)
constexpr uint32_t F_PREMUL = 0x1000;       // the colour out is Cs * C / 128 (Vulkan adds or takes it away)
constexpr uint32_t F_C_FIX = 0x2000;        // ... C is ALPHA's FIX (the push's clamp_hi bits 24-31), else As
constexpr uint32_t F_ALPHA_FACTOR = 0x4000; // the alpha out is As / 128, the blend's factor (the blend constant puts it back)

inline uint32_t blend_bits(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

// The blend: Vulkan's factors for ALPHA's (A - B) * C + D, with the shader's part (pcr_prim.frag's flags)
struct BlendChoice {
    bool enable = false;
    VkBlendFactor src = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ZERO, src_alpha = VK_BLEND_FACTOR_ONE;
    VkBlendOp op = VK_BLEND_OP_ADD;
    uint32_t flags = 0;
    bool write_rgb = true;
    bool supported = true;
};

inline BlendChoice choose_blend(uint64_t alpha, bool abe) {
    BlendChoice b;
    if (!abe) {
        return b;
    }
    uint32_t a = blend_bits(alpha, 0, 2), bb = blend_bits(alpha, 2, 2), c = blend_bits(alpha, 4, 2), d = blend_bits(alpha, 6, 2);
    bool fix = c == 2;
    if (c == 1) {
        b.supported = false; // (C = Ad: by As)
    }
    uint32_t c_flag = fix ? F_C_FIX : 0;
    // the factor C as Vulkan reads it: the alpha out (As / 128, put back by the constant) or the constant (FIX / 128)
    VkBlendFactor by_c = fix ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor by_one_minus_c = fix ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    uint32_t c_out = fix ? 0 : F_ALPHA_FACTOR;
    VkBlendFactor alpha_src = fix ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_CONSTANT_ALPHA;
    if (a == bb) {
        // (A - B) is 0: D alone
        if (d == 0) {
            return b; // Cs
        }
        if (d == 1) {
            b.write_rgb = false; // Cd: the colour kept, the alpha written
            return b;
        }
        b.flags = F_PREMUL | F_C_FIX; // 0: Cs * 0 (FIX read as 0 by the caller's push)
        b.flags |= 0x80000000u;      // (the caller gives FIX 0)
        return b;
    }
    if (fix && blend_bits(alpha, 32, 8) == 0 && d == 1 && ((a == 0 && bb == 2) || (a == 2 && bb == 0))) {
        // Cs * 0 + Cd, Cd - Cs * 0: Cd, as A the same as B. (Drawn as a blend, every pixel's colour was read to write
        // it back: the game's pass that sets the alpha to 0 over the whole screen, 0.08 ms a picture at 063.)
        b.write_rgb = false;
        return b;
    }
    if (a == 0 && bb == 1 && d == 1) { // Cs * C + Cd * (1 - C)
        b.enable = true;
        b.src = by_c;
        b.dst = by_one_minus_c;
        b.src_alpha = alpha_src;
        b.flags = c_out;
        return b;
    }
    if (a == 1 && bb == 0 && d == 0) { // Cd * C + Cs * (1 - C)
        b.enable = true;
        b.src = by_one_minus_c;
        b.dst = by_c;
        b.src_alpha = alpha_src;
        b.flags = c_out;
        return b;
    }
    if (a == 0 && bb == 2) { // Cs * C + D
        b.flags = F_PREMUL | c_flag;
        if (d == 1) {
            b.enable = true;
            b.dst = VK_BLEND_FACTOR_ONE;
        } else if (d == 0) {
            b.enable = true; // Cs * C + Cs: as Cs * (C + 1), not had: Cs * C (counted)
            b.supported = false;
        }
        return b;
    }
    if (a == 2 && bb == 0) { // D - Cs * C
        b.flags = F_PREMUL | c_flag;
        if (d == 1) {
            b.enable = true;
            b.dst = VK_BLEND_FACTOR_ONE;
            b.op = VK_BLEND_OP_REVERSE_SUBTRACT;
        } else {
            b.supported = false; // (0 or Cs less Cs * C: drawn as 0 - nothing)
            b.flags = F_PREMUL | F_C_FIX | 0x80000000u;
        }
        return b;
    }
    if (a == 1 && bb == 2) { // Cd * C + D
        b.enable = true;
        b.src = d == 0 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        b.dst = by_c;
        b.src_alpha = alpha_src;
        b.flags = c_out;
        b.supported = d != 1; // (Cd * (1 + C): drawn as Cd * C)
        return b;
    }
    // the rest ((Cs - Cd) * C + 0 or + Cs, (Cd - Cs) * C + Cd or + 0, 0 - Cd ...): drawn as Cs * C + Cd * (1 - C)
    b.supported = false;
    b.enable = true;
    b.src = by_c;
    b.dst = by_one_minus_c;
    b.src_alpha = alpha_src;
    b.flags = c_out;
    return b;
}

} // namespace pcr
