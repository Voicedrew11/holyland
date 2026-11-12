// A screen pass as the PS2 path's packets (pass_draw.hpp)
#include "pass_draw.hpp"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace {

// the GS's registers the passes write
constexpr uint32_t TEX0_1 = 0x06, CLAMP_1 = 0x08, TEX1_1 = 0x14, TEXFLUSH = 0x3F, ALPHA_1 = 0x42, TEST_1 = 0x47,
                   ZBUF_1 = 0x4E;

// A register list (Packet_BeginGsRegisters ... _End): an A+D packet of its own
void regs(PassStream &out, std::initializer_list<std::pair<uint32_t, uint64_t>> list) {
    out.tag(uint64_t(list.size()) | (1ull << 15) | (1ull << 60), 0xE);
    for (const auto &r : list) {
        out.ad(r.first, r.second);
    }
}

// A corner of a sprite, as ScreenSprite's (gfx/packet.h): X and Y (12.4), Z, and its texel (12.4)
struct Corner {
    uint32_t x, y, z;
    int32_t s, t;
};

// GsDraw_QueueTexturedSprite's packet: PRIM (with the sprite type) by PRE, TEX0 and TEX1 by A+D, then each corner's UV,
// RGBAQ and XYZ2 (the first corner's XYZ2 with a 1 in its last word, as the game writes it)
void textured_sprite(PassStream &out, uint32_t prim, uint64_t tex0, uint64_t tex1, const uint32_t rgba[4],
                     const Corner &a, const Corner &b) {
    out.tag((uint64_t(prim | 6) << 47) | 0x8000400000008001ull, 0x513513EE);
    out.ad(TEX0_1, tex0);
    out.ad(TEX1_1, tex1);
    out.words(uint32_t(a.s), uint32_t(a.t), 0, 0);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(a.x, a.y, a.z, 1);
    out.words(uint32_t(b.s), uint32_t(b.t), 0, 0);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(b.x, b.y, b.z, 0);
}

// GsDraw_QueueFlatSprite's packet: PRIM by PRE (texture, fog and ST off, the sprite type), then each corner's RGBAQ and
// XYZ2
void flat_sprite(PassStream &out, uint32_t prim, const uint32_t rgba[4], const Corner &a, const Corner &b) {
    out.tag((uint64_t((prim & 0xFFFFFFE9u) | 6) << 47) | 0x4000400000008001ull, 0x5151);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(a.x, a.y, a.z, 1);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(b.x, b.y, b.z, 0);
}

// The 13-qword sprite packet Game_BlendPreviousFrame and Game_DrawHazeLayers build themselves (tag 0x80AF400000008001: a
// textured, blended sprite by PRE; TEX0 and TEX1 by A+D, then UV, RGBAQ and XYZ2 twice, each XYZ2's last word 0)
void own_sprite(PassStream &out, uint64_t tex0, uint64_t tex1, const uint32_t rgba[4], const Corner &a, const Corner &b) {
    out.tag(0x80AF400000008001ull, 0x513513EE);
    out.ad(TEX0_1, tex0);
    out.ad(TEX1_1, tex1);
    out.words(uint32_t(a.s), uint32_t(a.t), 0, 0);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(a.x, a.y, a.z, 0);
    out.words(uint32_t(b.s), uint32_t(b.t), 0, 0);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(b.x, b.y, b.z, 0);
}

// The frame (640 x 448 of a 1024 x 512 CT32 texture) as the passes read it
uint64_t frame_tex0(uint32_t tbp) {
    return uint64_t(tbp | 0x28028000u) | (9ull << 30);
}

uint32_t rgba_of(const uint32_t *a) {
    return (a[0] & 0xFF) | (a[1] & 0xFF) << 8 | (a[2] & 0xFF) << 16 | (a[3] & 0xFF) << 24;
}

// ---- The pictures between's moves: gif_moves.h's kick_now, in the same floats ----

// now moved t of the way from was (u = 1 - t): now + floor((was - now) u + 0.5)
int32_t lerped(int32_t now, int32_t was, float u) {
    return now + int32_t(floorf(float(was - now) * u + 0.5f));
}

// A colour's four channels t of the way from was's
void lerp_rgba(uint32_t rgba[4], uint32_t was, float u) {
    for (int i = 0; i < 4; i++) {
        rgba[i] = uint32_t(lerped(int32_t(rgba[i] & 0xFF), int32_t((was >> (8 * i)) & 0xFF), u)) & 0xFF;
    }
}

// A texel (12.4, 14 bits) t of the way from was's: in texels, then rounded to 12.4
uint32_t lerp_texel(uint32_t now, uint32_t was, float u) {
    float b = float(now & 0x3FFF) / 16.0f, a = float(was & 0x3FFF) / 16.0f;
    return uint32_t(int32_t(floorf((b + (a - b) * u) * 16.0f + 0.5f))) & 0x3FFF;
}

// The previous frame's alpha at the picture's time: A^t (from A t of the way when paired)
uint32_t prev_alpha(uint32_t a_now, const PassMove &m, float u) {
    float vb = float(a_now & 0xFF), va = float(m.prev_rgba >> 24);
    float base = (m.mode & PASS_MOVE_PAIRED) ? vb + (va - vb) * u : vb;
    float a = base / 128.0f;
    a = a <= 0.0f ? 0.0f : a >= 1.0f ? 1.0f : powf(a, m.t);
    return uint32_t(int32_t(floorf(a * 128.0f + 0.5f))) & 0xFF;
}

// ---- The passes ----

// Game_ApplyBrightness and Game_TintScreen: the frame times RGB / 128, in the game's ten strips (each reads its own
// pixels, one texel a pixel, nearest; as one sprite the PC renderer's GPU puts the texel at a GS pixel's point a little
// under the whole texel now and then, farther from the first corner, and read the pixel to the left: a picture moved
// by a pixel in places)
void scale(PassStream &out, const RdPass &p, const PassMove &m, float u) {
    regs(out, {{TEXFLUSH, 0xFFFFFFFFull}}); // (GsDraw_QueueTexFlush; GsDraw_QueueFlush is the VIF's)
    regs(out, {{ALPHA_1, 10}, {TEST_1, 0x30000}, {ZBUF_1, 0x10A000118ull}});
    uint32_t rgba[4] = {p.arg[0], p.arg[1], p.arg[2], 0x80};
    if (m.mode & PASS_MOVE_RGBA) {
        lerp_rgba(rgba, m.prev_rgba, u);
    }
    uint64_t tex0 = frame_tex0(p.frame_tbp);
    Corner a = {0x6C00, 0x7200, 0xFFFFFFFFu, 0, 0}, b = {0x7000, 0x8DF8, 0xFFFFFFFFu, 0x400, 0x1C00};
    for (int i = 0; i < 10; i++) {
        textured_sprite(out, 0x116, tex0, 0, rgba, a, b);
        a.x += 0x400, b.x += 0x400, a.s += 0x400, b.s += 0x400;
    }
    regs(out, {{ZBUF_1, 0xA000118}});
    regs(out, {{TEXFLUSH, 0xFFFFFFFFull}});
}

// GsDraw_FillScreen: a flat colour over the screen, blended by ALPHA (one sprite for the game's ten strips)
void fill(PassStream &out, const uint32_t colour[4], uint64_t blend, const PassMove &m, float u) {
    regs(out, {{ALPHA_1, blend}, {TEST_1, 0x31001}});
    uint32_t rgba[4] = {colour[0], colour[1], colour[2], colour[3]};
    if (m.mode & PASS_MOVE_RGBA) {
        lerp_rgba(rgba, m.prev_rgba, u);
    }
    flat_sprite(out, 0x46, rgba, {0x6C00, 0x7200, 0, 0, 0}, {0x9400, 0x8E00, 0, 0, 0});
}

// Game_DrawFade's drawing: Z off around a fill
void fade(PassStream &out, const RdPass &p, const PassMove &m, float u) {
    regs(out, {{TEST_1, 0x30000}, {ZBUF_1, 0x10A000118ull}});
    fill(out, p.arg, 0x44, m, u);
    regs(out, {{ZBUF_1, 0xA000118}});
}

// Game_BlendScreenOffsetDown (and _UpDown): the frame over itself a row lower (and a row higher) at half, in the game's
// strips (each 63.5 pixels wide over 64 texels); its strips only with --flicker-filter
void flicker(PassStream &out, const RdPass &p) {
    regs(out, {{TEXFLUSH, 0xFFFFFFFFull}, {ALPHA_1, 0x44}, {TEST_1, 0x30000}, {ZBUF_1, 0x10A000118ull}});
    uint64_t tex0 = frame_tex0(p.frame_tbp);
    uint32_t rgba[4] = {0x80, 0x80, 0x80, 0x40};
    if (p.arg[1]) {
        Corner a = {0x6FF8, 0x8E08, 0xFFFFFFFFu, 0x400, 0x1C00}, b = {0x6C00, 0x7210, 0xFFFFFFFFu, 0, 0};
        for (int i = 0; i < 10; i++) {
            textured_sprite(out, 0x156, tex0, 0, rgba, a, b);
            a.x += 0x400, b.x += 0x400, a.s += 0x400, b.s += 0x400;
        }
        if (p.arg[0]) {
            a = {0x6BF8, 0x71E8, 0xFFFFFFFFu, 0, 0};
            b = {0x7000, 0x8DF0, 0xFFFFFFFFu, 0x400, 0x1C00};
            for (int i = 0; i < 10; i++) {
                textured_sprite(out, 0x156, tex0, 0, rgba, a, b);
                a.x += 0x400, b.x += 0x400, a.s += 0x400, b.s += 0x400;
            }
        }
    }
    regs(out, {{ZBUF_1, 0xA000118}});
}

// Game_BlendPreviousFrame: the frame shown before (the other buffer) over the whole screen at the alpha
void previous(PassStream &out, const RdPass &p, const PassMove &m, float u) {
    regs(out, {{ALPHA_1, 0x44}, {TEST_1, 0x51001}});
    int32_t r[8];
    memcpy(r, &p.arg[1], sizeof(r)); // (the screen's, then the texels' rectangle)
    uint32_t rgba[4] = {0x80, 0x80, 0x80, p.arg[0]};
    if (m.mode & PASS_MOVE_PREV) {
        rgba[3] = prev_alpha(p.arg[0], m, u);
    }
    own_sprite(out, frame_tex0(p.frame_tbp), 0, rgba,
               {uint32_t((r[0] + 0x6C0) << 4), uint32_t((r[1] + 0x720) << 4), 0xFFFFFFFFu, r[4] << 4, r[5] << 4},
               {uint32_t((r[2] << 4) + 0x6BF8), uint32_t((r[3] << 4) + 0x71F8), 0xFFFFFFFFu, r[6] << 4, r[7] << 4});
}

// (the view-turn shift) strip i's corners: X and Y (12.4, the u16 the game writes), U and V (12.4)
struct ShiftStrip {
    int32_t x[2], y[2], u[2], v[2];
};

ShiftStrip shift_strip(const RdPass &p, int i) {
    ShiftStrip s;
    uint32_t step = p.arg[0];
    for (int k = 0; k < 2; k++) {
        const uint32_t *c = p.data + 8 + k * 4; // (GsSpriteCorner: X and Y, Z, the texel's X and Y)
        s.x[k] = int32_t(((c[0] & 0xFFFF) + step * uint32_t(i)) & 0xFFFF);
        s.y[k] = int32_t(c[0] >> 16);
        s.u[k] = int32_t(c[2] + step * uint32_t(i));
        s.v[k] = int32_t(c[3]);
    }
    return s;
}

// Game_ShiftScreenByViewTurn's twenty strips (its registers are the game's, before): the first strip's sprite as the
// game made it (GsSpritePacket), each next one a step along; a strip that pairs t of the way from the frame before's
void view_shift(PassStream &out, const RdPass &p, const PassMove &m, float u) {
    const uint32_t *w = p.data;
    uint64_t tex0 = w[0] | uint64_t(w[1]) << 32, tex1 = w[2] | uint64_t(w[3]) << 32;
    uint32_t prim = w[4];
    uint32_t rgba[4] = {w[5] & 0xFF, (w[5] >> 8) & 0xFF, (w[5] >> 16) & 0xFF, w[5] >> 24};
    for (int i = 0; i < 20; i++) {
        ShiftStrip s = shift_strip(p, i);
        Corner c[2];
        for (int k = 0; k < 2; k++) {
            c[k] = {uint32_t(s.x[k]), uint32_t(s.y[k]), w[8 + k * 4 + 1], s.u[k], s.v[k]};
        }
        if ((m.mode & PASS_MOVE_SHIFT) && ((m.strips >> i) & 1)) {
            ShiftStrip a = shift_strip(m.before, i);
            for (int k = 0; k < 2; k++) {
                c[k].x = uint32_t(lerped(s.x[k], a.x[k], u)) & 0xFFFF;
                c[k].y = uint32_t(lerped(s.y[k], a.y[k], u)) & 0xFFFF;
                c[k].s = int32_t(lerp_texel(uint32_t(s.u[k]), uint32_t(a.u[k]), u));
                c[k].t = int32_t(lerp_texel(uint32_t(s.v[k]), uint32_t(a.v[k]), u));
            }
        }
        textured_sprite(out, prim, tex0, tex1, rgba, c[0], c[1]);
    }
}

// Game_DrawHazeLayers: a sprite of the frame at each layer's depth, bilinear and half a pixel over, redrawing the pixels
// farther than it
void haze(PassStream &out, const RdPass &p) {
    regs(out, {{ALPHA_1, 0x80000000A8ull}, {TEST_1, 0x51001}, {CLAMP_1, 5}});
    uint32_t rgba[4] = {p.arg[1], p.arg[2], p.arg[3], p.arg[4]};
    for (uint32_t i = 0; i < p.arg[0] && i < 8; i++) {
        own_sprite(out, frame_tex0(p.frame_tbp), 0x60, rgba, {0x6BF8, 0x7200, p.data[i], 0, 0},
                   {0x93F8, 0x8E00, p.data[i], 0x2800, 0x1C00});
    }
    regs(out, {{CLAMP_1, 0}});
}

// Game_DrawWobblingScreen's 70 cells (its registers are the game's, before and after): each a strip of four corners, the
// grid's points on the screen with their texels in the frame grabbed at 0x2BC0; a cell that pairs t of the way from the
// frame before's, colour and texels
void wobble(PassStream &out, const RdPass &p, const PassMove &m, float u) {
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 10; j++) {
            int cell = i * 10 + j;
            bool moves = (m.mode & PASS_MOVE_GRID) && ((m.cells[cell >> 5] >> (cell & 31)) & 1);
            uint32_t rgba[4] = {p.arg[0], p.arg[1], p.arg[2], p.arg[3]};
            if (moves && (m.mode & PASS_MOVE_RGBA)) {
                lerp_rgba(rgba, m.prev_rgba, u);
            }
            out.tag(0xC0AE400000008001ull, 0x513513513513ull);
            for (int k = 0; k < 2; k++) {
                for (int c = 0; c < 2; c++) {
                    uint32_t g = uint32_t((i + k) * 11 + j + c);
                    uint32_t uv = p.data[g];
                    uint32_t us = uv & 0x3FFF, vs = uv >> 16;
                    if (moves) {
                        uint32_t was = m.before.data[g];
                        us = lerp_texel(us, was & 0x3FFF, u);
                        vs = lerp_texel(vs, (was >> 16) & 0x3FFF, u);
                    }
                    out.words(us, vs, 0, 0);
                    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
                    out.words(uint32_t((((j + c) << 6) + 0x6C0) << 4), uint32_t((((i + k) << 6) + 0x720) << 4), 0, 0);
                }
            }
        }
    }
}

// ---- Pairing: frame_interp.c's items_pair, for a pass's primitives ----

// frame_interp.c's items_pair for a strip of the view-turn shift (a sprite: two corners, UV, its colour the same), b this
// frame's, a the frame before's: whether the pictures between move it
bool shift_pairs(const ShiftStrip &a, const ShiftStrip &b, uint64_t tex0) {
    int changed = 0, uniform = 1, moved_only = 1;
    float ds0 = 0, dt0 = 0, smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f;
    float tw = float(1u << ((tex0 >> 26) & 0xF)), th = float(1u << ((tex0 >> 30) & 0xF)), eps = 0.07f;
    for (int k = 0; k < 2; k++) {
        int32_t dx = b.x[k] - a.x[k], dy = b.y[k] - a.y[k];
        if (dx > 2048 || dx < -2048 || dy > 2048 || dy < -2048) {
            return false;
        }
        changed |= dx != 0 || dy != 0;
        float bs = float(b.u[k] & 0x3FFF) / 16.0f, bt = float(b.v[k] & 0x3FFF) / 16.0f;
        float ds = bs - float(a.u[k] & 0x3FFF) / 16.0f, dt = bt - float(a.v[k] & 0x3FFF) / 16.0f;
        if (k == 0) {
            ds0 = ds, dt0 = dt;
        } else if (fabsf(ds - ds0) > eps || fabsf(dt - dt0) > eps) {
            uniform = 0;
        }
        if ((fabsf(ds) > eps || fabsf(dt) > eps) && dx == 0 && dy == 0) {
            moved_only = 0;
        }
        smin = std::min(smin, bs), smax = std::max(smax, bs), tmin = std::min(tmin, bt), tmax = std::max(tmax, bt);
    }
    if (uniform && (fabsf(ds0) > eps || fabsf(dt0) > eps)) {
        float s = ds0 - tw * floorf(ds0 / tw + 0.5f), t = dt0 - th * floorf(dt0 / th + 0.5f);
        if (fabsf(s) > 0.25f * (smax - smin) + eps || fabsf(t) > 0.25f * (tmax - tmin) + eps) {
            return false;
        }
        if (ds0 - s != 0 || dt0 - t != 0) {
            return false; // (a scroll past the texture's wrap: never, the strips' texels move with their places)
        }
        changed = 1;
    } else if (!uniform) {
        if (!moved_only) {
            return false; // (a sprite's texture changed in place)
        }
        changed = 1;
    }
    return changed != 0;
}

// frame_interp.c's items_pair for the wobble's cell (i, j) (a strip of four corners, UV, the pass's colour; its places
// the same each frame), b this frame's, a the frame before's: whether the pictures between move it
bool wobble_pairs(const RdPass &a, const RdPass &b, int i, int j, uint64_t tex0) {
    int changed = 0, uniform = 1, moved_only = 1;
    float ds0 = 0, dt0 = 0, smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f, dsmost = 0, dtmost = 0;
    float tw = float(1u << ((tex0 >> 26) & 0xF)), th = float(1u << ((tex0 >> 30) & 0xF)), eps = 0.07f;
    for (int k = 0; k < 4; k++) {
        uint32_t g = uint32_t((i + (k >> 1)) * 11 + j + (k & 1));
        uint32_t ua = a.data[g], ub = b.data[g];
        changed |= memcmp(a.arg, b.arg, 16) != 0;
        float bs = float(ub & 0x3FFF) / 16.0f, bt = float((ub >> 16) & 0x3FFF) / 16.0f;
        float ds = bs - float(ua & 0x3FFF) / 16.0f, dt = bt - float((ua >> 16) & 0x3FFF) / 16.0f;
        if (k == 0) {
            ds0 = ds, dt0 = dt;
        } else if (fabsf(ds - ds0) > eps || fabsf(dt - dt0) > eps) {
            uniform = 0;
        }
        if (fabsf(ds) > eps || fabsf(dt) > eps) {
            moved_only = 0; // (its places never move)
        }
        dsmost = std::max(dsmost, fabsf(ds)), dtmost = std::max(dtmost, fabsf(dt));
        smin = std::min(smin, bs), smax = std::max(smax, bs), tmin = std::min(tmin, bt), tmax = std::max(tmax, bt);
    }
    if (uniform && (fabsf(ds0) > eps || fabsf(dt0) > eps)) {
        float s = ds0 - tw * floorf(ds0 / tw + 0.5f), t = dt0 - th * floorf(dt0 / th + 0.5f);
        if (fabsf(s) > 0.25f * (smax - smin) + eps || fabsf(t) > 0.25f * (tmax - tmin) + eps) {
            return false;
        }
        if (ds0 - s != 0 || dt0 - t != 0) {
            return false; // (a scroll past the texture's wrap: never, the springs move a few texels)
        }
        changed = 1;
    } else if (!uniform) {
        if (!moved_only && (dsmost > 0.25f * (smax - smin) + eps || dtmost > 0.25f * (tmax - tmin) + eps)) {
            return false;
        }
        changed = 1;
    }
    return changed != 0;
}

} // namespace

void pass_stream(const RdPass &p, const PassMove &m, PassStream &out) {
    float u = 1.0f - m.t; // (the picture's 1 - t, as pcr_between.cpp makes it)
    switch (p.pass) {
    case RD_PASS_SCALE:
        scale(out, p, m, u);
        break;
    case RD_PASS_FILL:
        fill(out, p.arg, p.arg[4] | uint64_t(p.arg[5]) << 32, m, u);
        break;
    case RD_PASS_FADE:
        fade(out, p, m, u);
        break;
    case RD_PASS_FLICKER:
        flicker(out, p);
        break;
    case RD_PASS_PREVIOUS:
        previous(out, p, m, u);
        break;
    case RD_PASS_VIEW_SHIFT:
        view_shift(out, p, m, u);
        break;
    case RD_PASS_HAZE:
        haze(out, p);
        break;
    case RD_PASS_WOBBLE:
        wobble(out, p, m, u);
        break;
    default:
        break;
    }
}

void PassMoves::frame(uint32_t serial, PartWalk walk) {
    seen_.clear();
    if (walk == PartWalk::picture || serial == serial_) {
        return;
    }
    serial_ = serial;
    before_.swap(now_);
    now_.clear();
}

PassMove PassMoves::pass(const RdPass &p, uint64_t tex0, bool shown_kept, PartWalk walk, float t) {
    Key key = {p.pass, p.site, seen_[uint64_t(p.pass) << 32 | p.site]++};
    if (walk != PartWalk::picture) {
        now_[key] = p;
    }
    PassMove m;
    m.t = t;
    if (walk == PartWalk::frame) {
        return m;
    }
    auto it = before_.find(key);
    const RdPass *was = it != before_.end() ? &it->second : nullptr;
    switch (p.pass) {
    case RD_PASS_FILL:
    case RD_PASS_FADE:
    case RD_PASS_WOBBLE:
        if (was && rgba_of(was->arg) != rgba_of(p.arg)) {
            m.mode |= PASS_MOVE_RGBA;
            m.prev_rgba = rgba_of(was->arg);
        }
        if (p.pass == RD_PASS_WOBBLE && was && (memcmp(was->data, p.data, 88 * 4) != 0 || (m.mode & PASS_MOVE_RGBA))) {
            // (each cell a 2D item of frame_interp.c's, moved, colour and texels, only when items_pair takes it)
            for (int i = 0; i < 7; i++) {
                for (int j = 0; j < 10; j++) {
                    if (wobble_pairs(*was, p, i, j, tex0)) {
                        m.cells[(i * 10 + j) >> 5] |= 1u << ((i * 10 + j) & 31);
                    }
                }
            }
            m.mode |= PASS_MOVE_GRID;
            m.before = *was;
        }
        break;
    case RD_PASS_SCALE:
        if (was && memcmp(was->arg, p.arg, 12) != 0) {
            m.mode |= PASS_MOVE_RGBA;
            m.prev_rgba = rgba_of(was->arg) | 0x80000000u; // (its A 0x80, the sprite's: arg 3 is 0)
        }
        break;
    case RD_PASS_VIEW_SHIFT:
        // (each strip a 2D item, paired with the frame before's in order, moved when items_pair takes the pair; not with
        // a bilinear TEX1: frame_interp.c's snap_sprite_uv's, not here)
        if (was && !((p.data[2] >> 5) & 1)) {
            uint64_t shift_tex0 = p.data[0] | uint64_t(p.data[1]) << 32;
            for (int i = 0; i < 20; i++) {
                if (shift_pairs(shift_strip(*was, i), shift_strip(p, i), shift_tex0)) {
                    m.strips |= 1u << i;
                }
            }
            if (m.strips) {
                m.mode |= PASS_MOVE_SHIFT;
                m.before = *was;
            }
        }
        break;
    case RD_PASS_PREVIOUS:
        // (when the frame shown before was kept for the pictures; paired when its alpha changed)
        if (shown_kept) {
            m.mode |= PASS_MOVE_PREV;
            if (was && (was->arg[0] & 0xFF) != (p.arg[0] & 0xFF)) {
                m.mode |= PASS_MOVE_PAIRED;
                m.prev_rgba = (was->arg[0] & 0xFF) << 24;
            }
        }
        break;
    default:
        break;
    }
    return m;
}
