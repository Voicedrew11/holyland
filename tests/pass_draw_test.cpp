// Checks port/render/pass_draw.cpp (a screen pass as GIF packets) against the code it stands for: the game's own passes
// (decomp src/game/core/game.c, src/game/gfx/gs_draw.c: Game_ApplyBrightness, Game_TintScreen, GsDraw_FillScreen,
// Game_DrawFade, Game_BlendScreenOffsetDown and _UpDown, Game_BlendPreviousFrame, Game_DrawHazeLayers,
// Game_ShiftScreenByViewTurn's strips, Game_DrawWobblingScreen's cells), their packets written out here as the game
// builds them (GsDraw_QueueTexturedSprite's and _QueueFlatSprite's layouts, the register lists, the loops). Both go
// through the PC renderer's GIF front end (port/gs/pc/pcr_gif.h): the register file they leave must be the same, and so
// must the GS pixels their primitives cover with what colour and texel (a sprite drawn as the game's strips or as one).
// Then the pictures between's moves against port/render/shaders/gif_moves.h's kick_now (the PS2 path's 2D moved on the
// GPU): a colour t of the way, the frame shown before by A^t, a texel t of the way in 12.4, the pairing by identity.
//
//     out/<preset>/pass_draw_test     prints the failures, and exits 1 if any
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <tuple>
#include <vector>

#include "pass_draw.hpp"
#include "pcr_gif.h"

using namespace pcr;

namespace {

int failures, checks;

void check(bool ok, const char *what) {
    checks++;
    if (!ok) {
        failures++;
        if (failures <= 30) {
            printf("FAIL: %s\n", what);
        }
    }
}

// ---- The game's packets, as it builds them ----
constexpr uint32_t TEX0_1 = 0x06, CLAMP_1 = 0x08, TEX1_1 = 0x14, TEXFLUSH = 0x3F, ALPHA_1 = 0x42, TEST_1 = 0x47,
                   ZBUF_1 = 0x4E;

struct GameCorner { // (GsSpritePacket's corner, ScreenSprite's vertex)
    uint32_t x, y, z;
    int32_t texel_x, texel_y;
};
struct GameSprite {
    uint64_t tex0 = 0, tex1 = 0;
    uint32_t prim = 0;
    uint32_t rgba[4] = {};
    GameCorner corner[2] = {};
};

// Packet_BeginGsRegisters ... _End
void game_regs(PassStream &out, std::vector<std::pair<uint32_t, uint64_t>> list) {
    out.tag(uint64_t(list.size()) | (1ull << 15) | (1ull << 60), 0xE);
    for (auto &r : list) {
        out.ad(r.first, r.second);
    }
}

// GsDraw_QueueTexturedSprite (its GIF part, past the DMA and VIF words)
void queue_textured(PassStream &out, const GameSprite &s) {
    out.tag((uint64_t(s.prim | 6) << 47) | 0x8000400000008001ull, 0x513513EE);
    out.ad(TEX0_1, s.tex0);
    out.ad(TEX1_1, s.tex1);
    for (int k = 0; k < 2; k++) {
        const GameCorner &c = s.corner[k];
        out.words(uint32_t(c.texel_x), uint32_t(c.texel_y), 0, 0);
        out.words(s.rgba[0], s.rgba[1], s.rgba[2], s.rgba[3]);
        out.words(c.x, c.y, c.z, k == 0 ? 1 : 0);
    }
}

// GsDraw_QueueFlatSprite
void queue_flat(PassStream &out, const GameSprite &s) {
    uint32_t prim = (s.prim & 0xFFFFFFE9u) | 6;
    out.tag((uint64_t(prim) << 47) | 0x4000400000008001ull, 0x5151);
    for (int k = 0; k < 2; k++) {
        const GameCorner &c = s.corner[k];
        out.words(s.rgba[0], s.rgba[1], s.rgba[2], s.rgba[3]);
        out.words(c.x, c.y, c.z, k == 0 ? 1 : 0);
    }
}

// Game_ApplyBrightness and Game_TintScreen (the same packets, the tint's colour its own)
void game_scale(PassStream &out, uint32_t base, uint32_t r, uint32_t g, uint32_t b) {
    game_regs(out, {{TEXFLUSH, 0xFFFFFFFFull}});
    game_regs(out, {{ALPHA_1, 10}, {TEST_1, 0x30000}, {ZBUF_1, 0x10A000118ull}});
    GameSprite s;
    s.tex0 = (base | 0x28028000ull) | (9ull << 30);
    s.prim = 0x116;
    s.rgba[0] = r, s.rgba[1] = g, s.rgba[2] = b, s.rgba[3] = 0x80;
    s.corner[0] = {0x6C00, 0x7200, 0xFFFFFFFFu, 0, 0};
    s.corner[1] = {0x7000, 0x8DF8, 0xFFFFFFFFu, 0x400, 0x1C00};
    int remaining = 9;
    do {
        remaining--;
        queue_textured(out, s);
        s.corner[0].x += 0x400, s.corner[1].x += 0x400, s.corner[0].texel_x += 0x400, s.corner[1].texel_x += 0x400;
    } while (remaining >= 0);
    game_regs(out, {{ZBUF_1, 0xA000118}});
    game_regs(out, {{TEXFLUSH, 0xFFFFFFFFull}});
}

void game_fill(PassStream &out, uint32_t r, uint32_t g, uint32_t b, uint32_t a, uint64_t blend) {
    game_regs(out, {{ALPHA_1, blend}, {TEST_1, 0x31001}});
    GameSprite s;
    s.prim = 0x46;
    s.rgba[0] = r, s.rgba[1] = g, s.rgba[2] = b, s.rgba[3] = a;
    s.corner[0] = {0x6C00, 0x7200, 0, 0, 0};
    s.corner[1] = {0x7000, 0x8E00, 0, 0, 0};
    int left = 9;
    do {
        left--;
        queue_flat(out, s);
        s.corner[0].x += 0x400, s.corner[1].x += 0x400;
    } while (left >= 0);
}

void game_fade(PassStream &out, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    game_regs(out, {{TEST_1, 0x30000}, {ZBUF_1, 0x10A000118ull}});
    game_fill(out, r, g, b, a, 0x44);
    game_regs(out, {{ZBUF_1, 0xA000118}});
}

void game_flicker(PassStream &out, uint32_t base, bool updown) {
    game_regs(out, {{TEXFLUSH, 0xFFFFFFFFull}, {ALPHA_1, 0x44}, {TEST_1, 0x30000}, {ZBUF_1, 0x10A000118ull}});
    GameSprite s;
    s.tex0 = (base | 0x28028000ull) | (9ull << 30);
    s.rgba[0] = s.rgba[1] = s.rgba[2] = 0x80, s.rgba[3] = 0x40;
    s.prim = 0x156;
    s.corner[1] = {0x6C00, 0x7210, 0xFFFFFFFFu, 0, 0};
    s.corner[0] = {0x6FF8, 0x8E08, 0xFFFFFFFFu, 0x400, 0x1C00};
    int remaining = 9;
    do {
        remaining--;
        queue_textured(out, s);
        s.corner[0].x += 0x400, s.corner[1].x += 0x400, s.corner[0].texel_x += 0x400, s.corner[1].texel_x += 0x400;
    } while (remaining >= 0);
    if (updown) {
        s.corner[1].x = 0x7000, s.corner[0].x = 0x6BF8, s.corner[1].y = 0x8DF0, s.corner[0].y = 0x71E8;
        s.corner[1].texel_x = 0x400, s.corner[1].texel_y = 0x1C00, s.corner[0].texel_x = 0, s.corner[0].texel_y = 0;
        remaining = 9;
        do {
            remaining--;
            queue_textured(out, s);
            s.corner[0].x += 0x400, s.corner[1].x += 0x400, s.corner[0].texel_x += 0x400, s.corner[1].texel_x += 0x400;
        } while (remaining >= 0);
    }
    game_regs(out, {{ZBUF_1, 0xA000118}});
}

// Game_BlendPreviousFrame and Game_DrawHazeLayers' own 13-qword packet
void own_packet(PassStream &out, uint64_t tex0, uint64_t tex1, const uint32_t rgba[4], const uint32_t c0[5],
                const uint32_t c1[5]) {
    out.tag(0x80AF400000008001ull, 0x513513EE);
    out.ad(TEX0_1, tex0);
    out.ad(TEX1_1, tex1);
    out.words(c0[3], c0[4], 0, 0);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(c0[0], c0[1], c0[2], 0);
    out.words(c1[3], c1[4], 0, 0);
    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
    out.words(c1[0], c1[1], c1[2], 0);
}

void game_previous(PassStream &out, uint32_t base, uint32_t alpha, const int32_t rect[4]) {
    game_regs(out, {{ALPHA_1, 0x44}, {TEST_1, 0x51001}});
    uint32_t colour[4] = {0x80, 0x80, 0x80, alpha};
    uint32_t c0[5] = {uint32_t((rect[0] + 0x6C0) << 4), uint32_t((rect[1] + 0x720) << 4), 0xFFFFFFFFu,
                      uint32_t(rect[0] << 4), uint32_t(rect[1] << 4)};
    uint32_t c1[5] = {uint32_t((rect[2] << 4) + 0x6BF8), uint32_t((rect[3] << 4) + 0x71F8), 0xFFFFFFFFu,
                      uint32_t(rect[2] << 4), uint32_t(rect[3] << 4)};
    own_packet(out, (base | 0x28028000ull) | (9ull << 30), 0, colour, c0, c1);
}

void game_haze(PassStream &out, uint32_t base, const uint32_t colour[4], const int32_t *depths, uint32_t count) {
    game_regs(out, {{ALPHA_1, 0x80000000A8ull}, {TEST_1, 0x51001}, {CLAMP_1, 5}});
    for (uint32_t i = 0; i < count; i++) {
        uint32_t c0[5] = {0x6BF8, 0x7200, uint32_t(depths[i]), 0, 0};
        uint32_t c1[5] = {0x93F8, 0x8E00, uint32_t(depths[i]), 0x2800, 0x1C00};
        own_packet(out, (base | 0x28028000ull) | (9ull << 30), 0x60, colour, c0, c1);
    }
    game_regs(out, {{CLAMP_1, 0}});
}

// Game_ShiftScreenByViewTurn's strips (its loop; x a u16 of the game's struct)
void game_view_shift(PassStream &out, GameSprite s, int32_t step) {
    int strip = 0x13;
    do {
        strip--;
        queue_textured(out, s);
        for (auto &c : s.corner) {
            c.x = (c.x + uint32_t(step)) & 0xFFFF;
            c.texel_x += step;
        }
    } while (strip >= 0);
}

// Game_DrawWobblingScreen's cells: a PACKED strip of four corners each (UV, RGBAQ, XYZ2)
void game_wobble(PassStream &out, const uint32_t uv[88], const uint32_t rgba[4]) {
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 10; j++) {
            out.tag(0xC0AE400000008001ull, 0x513513513513ull);
            for (int k = 0; k < 2; k++) {
                for (int c = 0; c < 2; c++) {
                    uint32_t w = uv[(i + k) * 11 + j + c];
                    out.words(w & 0x3FFF, w >> 16, 0, 0);
                    out.words(rgba[0], rgba[1], rgba[2], rgba[3]);
                    out.words(uint32_t((((j + c) << 6) + 0x6C0) << 4), uint32_t((((i + k) << 6) + 0x720) << 4), 0, 0);
                }
            }
        }
    }
}

// ---- Through the GIF front end ----

// What the primitives drew: for each GS pixel (XYOFFSET 0x6C00, 0x7200 off) the last primitive's colour, texel and Z
// there and how many drew it; a triangle's corners as they are (their own list: compared as given)
struct Drawn {
    std::map<std::pair<int, int>, std::tuple<uint32_t, int32_t, int32_t, uint32_t, int>> pixels;
    std::vector<std::vector<uint64_t>> triangles;
};

class Recorder : public GifEvents {
public:
    Drawn drawn;
    GifFrontEnd *front = nullptr;
    void register_written(uint32_t, uint64_t, uint64_t) override {}
    void texture_written(uint32_t, uint64_t, uint64_t) override {}
    void primitive(const GifPrimitive &p) override {
        if (p.shape != PrimitiveShape::sprite) {
            std::vector<uint64_t> t = {p.prim};
            for (uint32_t i = 0; i < p.vertex_count; i++) {
                const GifVertex &v = p.vertices[i];
                t.push_back(uint64_t(v.x) | uint64_t(v.y) << 16 | uint64_t(v.u) << 32 | uint64_t(v.v) << 48);
                t.push_back(uint64_t(v.rgba) | uint64_t(v.z) << 32);
            }
            drawn.triangles.push_back(t);
            return;
        }
        // a sprite: the GS pixels (whole, at their points) from its smaller corner to before its larger, each its texel
        // by the line between the corners' (UV in 12.4: the texel at the pixel's point, in 1/16ths rounded down)
        const GifVertex &a = p.vertices[0], &b = p.vertices[1];
        int32_t ax = int32_t(a.x) - 0x6C00, ay = int32_t(a.y) - 0x7200, bx = int32_t(b.x) - 0x6C00, by = int32_t(b.y) - 0x7200;
        int32_t x0 = std::min(ax, bx), x1 = std::max(ax, bx), y0 = std::min(ay, by), y1 = std::max(ay, by);
        for (int32_t py = (y0 + 15) / 16; py * 16 < y1; py++) {
            for (int32_t px = (x0 + 15) / 16; px * 16 < x1; px++) {
                int32_t u = bx == ax ? a.u : int32_t(floor(a.u + double(px * 16 - ax) * (double(b.u) - a.u) / (bx - ax)));
                int32_t v = by == ay ? a.v : int32_t(floor(a.v + double(py * 16 - ay) * (double(b.v) - a.v) / (by - ay)));
                auto &e = drawn.pixels[{px, py}];
                int n = std::get<4>(e) + 1;
                e = {b.rgba, u, v, b.z, n};
            }
        }
    }
    void image_upload(const TransferRegisters &, std::span<const uint8_t>, UploadEnd) override {}
    void local_copy(const TransferRegisters &) override {}
    void local_to_host(const TransferRegisters &) override {}
    void native_draw(uint64_t) override {}
    void upload_again(uint64_t) override {}
    void frame_mark(uint64_t) override {}
    void widescreen(uint64_t) override {}
    void gs_sync(uint32_t, uint64_t) override {}
    void kick_numbered(const GifVertex &, uint32_t) override {}
};

// Packets fed to a front end as PATH3's bytes; its register file and what it drew
struct Result {
    GsRegisterFile file;
    Drawn drawn;
};

Result run(const PassStream &s) {
    Recorder r;
    GifFrontEnd front(r);
    r.front = &front;
    std::vector<uint8_t> bytes(s.q.size() * 8);
    memcpy(bytes.data(), s.q.data(), bytes.size());
    front.feed(3, bytes);
    return {front.registers(), r.drawn};
}

void compare(const char *what, const PassStream &native, const PassStream &game) {
    Result a = run(native), b = run(game);
    char text[160];
    snprintf(text, sizeof(text), "%s: the register file left", what);
    check(a.file == b.file, text);
    snprintf(text, sizeof(text), "%s: the pixels drawn (%zu and %zu)", what, a.drawn.pixels.size(), b.drawn.pixels.size());
    check(a.drawn.pixels == b.drawn.pixels, text);
    snprintf(text, sizeof(text), "%s: the triangles drawn (%zu and %zu)", what, a.drawn.triangles.size(),
             b.drawn.triangles.size());
    check(a.drawn.triangles == b.drawn.triangles, text);
}

RdPass pass_of(uint32_t kind, uint32_t site, uint32_t base) {
    RdPass p;
    memset(&p, 0, sizeof(p));
    p.pass = kind;
    p.site = site;
    p.frame_tbp = base;
    return p;
}

// The vertex kicks a stream makes, in order (as the frame's own walk kept numbers a native draw's)
struct KickRecorder : Recorder {
    std::vector<GifVertex> kicks;
    void kick_numbered(const GifVertex &v, uint32_t) override { kicks.push_back(v); }
};

std::vector<GifVertex> kicks_of(const PassStream &s) {
    KickRecorder r;
    GifFrontEnd front(r);
    r.front = &front;
    front.number_kicks();
    std::vector<uint8_t> bytes(s.q.size() * 8);
    memcpy(bytes.data(), s.q.data(), bytes.size());
    front.feed(3, bytes);
    return r.kicks;
}

// A kick of the frame's own pass moved as gif_moves.h's kick_now moves it by its FLAT move (pcr_native.cpp's
// walk_pass_by_kicks makes the move from the PassKickMove: U and V in texels, and what is added at t 0)
GifVertex moved_on_gpu(GifVertex v, const PassKickMove &k, float u) {
    if (k.flags & PASS_KICK_XY) {
        int ax = int(k.xy_was & 0xFFFF), ay = int(k.xy_was >> 16), bx = int(k.xy_now & 0xFFFF), by = int(k.xy_now >> 16);
        v.x = uint16_t(bx + int(floorf(float(ax - bx) * u + 0.5f)));
        v.y = uint16_t(by + int(floorf(float(ay - by) * u + 0.5f)));
    }
    if (k.flags & PASS_KICK_RGBA) {
        uint32_t rgba = 0;
        for (uint32_t i = 0; i < 32; i += 8) {
            int ac = int((k.rgba_was >> i) & 0xFF), bc = int((k.rgba_now >> i) & 0xFF);
            rgba |= (uint32_t(bc + int(floorf(float(ac - bc) * u + 0.5f))) & 0xFF) << i;
        }
        v.rgba = rgba;
    }
    if (k.flags & PASS_KICK_UV) {
        float bu = float(k.uv_now & 0x3FFF) / 16.0f, bv = float((k.uv_now >> 16) & 0x3FFF) / 16.0f;
        float du = float(k.uv_was & 0x3FFF) / 16.0f - bu, dv = float((k.uv_was >> 16) & 0x3FFF) / 16.0f - bv;
        v.u = uint16_t(int(floorf((bu + du * u) * 16.0f + 0.5f)) & 0x3FFF);
        v.v = uint16_t(int(floorf((bv + dv * u) * 16.0f + 0.5f)) & 0x3FFF);
    }
    return v;
}

// pass_stream_moves against pass_stream: the frame's own packets, one move a kick, and each kick moved by its move at
// t as the picture's packets have it
void check_moves(const char *what, const RdPass &p, const PassMove &m, float t) {
    char text[160];
    PassStream own, with, picture;
    pass_stream(p, PassMove(), own);
    std::vector<PassKickMove> moves;
    bool ok = pass_stream_moves(p, m, with, moves);
    snprintf(text, sizeof(text), "%s: moved by its kicks", what);
    check(ok, text);
    snprintf(text, sizeof(text), "%s: the frame's own packets", what);
    check(with.q == own.q, text);
    PassMove at = m;
    at.t = t;
    pass_stream(p, at, picture);
    std::vector<GifVertex> a = kicks_of(with), b = kicks_of(picture);
    snprintf(text, sizeof(text), "%s: a move a kick (%zu kicks, %zu moves)", what, a.size(), moves.size());
    check(a.size() == moves.size() && a.size() == b.size() && !a.empty(), text);
    int moved = 0, wrong = 0;
    for (size_t i = 0; i < a.size() && i < moves.size() && i < b.size(); i++) {
        GifVertex v = moved_on_gpu(a[i], moves[i], 1.0f - t);
        moved += moves[i].flags != 0;
        wrong += v.x != b[i].x || v.y != b[i].y || v.rgba != b[i].rgba || v.u != b[i].u || v.v != b[i].v;
    }
    snprintf(text, sizeof(text), "%s: kicks moved as the picture's (%d moved, %d wrong)", what, moved, wrong);
    check(moved > 0 && wrong == 0, text);
}

uint32_t rng_state = 0x9E3779B9u;
uint32_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

} // namespace

int main() {
    for (uint32_t base : {0u, 0x1180u}) {
        // the brightness and the tint
        for (uint32_t c : {0x80u, 0x40u, 0xFFu}) {
            RdPass p = pass_of(RD_PASS_SCALE, RD_PASS_SITE_BRIGHTNESS, base);
            p.arg[0] = p.arg[1] = p.arg[2] = c;
            PassStream n, g;
            pass_stream(p, PassMove(), n);
            game_scale(g, base, c, c, c);
            compare("brightness", n, g);
        }
        {
            RdPass p = pass_of(RD_PASS_SCALE, RD_PASS_SITE_TINT, base);
            p.arg[0] = 0x5E, p.arg[1] = 0x87, p.arg[2] = 0x92;
            PassStream n, g;
            pass_stream(p, PassMove(), n);
            game_scale(g, base, 0x5E, 0x87, 0x92);
            compare("tint", n, g);
        }
        // a fill and the fade
        {
            RdPass p = pass_of(RD_PASS_FILL, RD_PASS_SITE_FILL, base);
            p.arg[0] = 0x40, p.arg[1] = 0x80, p.arg[2] = 0x40, p.arg[3] = 0x78, p.arg[4] = 0x44, p.arg[5] = 1;
            PassStream n, g;
            pass_stream(p, PassMove(), n);
            game_fill(g, 0x40, 0x80, 0x40, 0x78, 0x100000044ull);
            compare("fill", n, g);
            p = pass_of(RD_PASS_FADE, RD_PASS_SITE_FADE, base);
            p.arg[0] = 1, p.arg[1] = 2, p.arg[2] = 3, p.arg[3] = 0x50;
            PassStream n2, g2;
            pass_stream(p, PassMove(), n2);
            game_fade(g2, 1, 2, 3, 0x50);
            compare("fade", n2, g2);
        }
        // the flicker filter
        for (int updown = 0; updown < 2; updown++) {
            RdPass p = pass_of(RD_PASS_FLICKER, RD_PASS_SITE_FLICKER, base);
            p.arg[0] = uint32_t(updown), p.arg[1] = 1;
            PassStream n, g;
            pass_stream(p, PassMove(), n);
            game_flicker(g, base, updown != 0);
            compare(updown ? "flicker filter (up and down)" : "flicker filter (down)", n, g);
        }
        // the frame shown before (the other buffer)
        {
            int32_t rect[4] = {0, 0, 640, 448};
            RdPass p = pass_of(RD_PASS_PREVIOUS, RD_PASS_SITE_PREVIOUS, base);
            p.arg[0] = 0x60;
            for (int i = 0; i < 4; i++) {
                p.arg[1 + i] = uint32_t(rect[i]);
                p.arg[5 + i] = uint32_t(rect[i]);
            }
            PassStream n, g;
            pass_stream(p, PassMove(), n);
            game_previous(g, base, 0x60, rect);
            compare("the frame shown before", n, g);
        }
        // haze
        {
            int32_t depths[3] = {0xF000, 0xE000, 0x8000};
            uint32_t colour[4] = {0x80, 0x70, 0x60, 0x30};
            RdPass p = pass_of(RD_PASS_HAZE, RD_PASS_SITE_HAZE, base);
            p.arg[0] = 3;
            for (int i = 0; i < 4; i++) {
                p.arg[1 + i] = colour[i];
            }
            for (int i = 0; i < 3; i++) {
                p.data[i] = uint32_t(depths[i]);
            }
            PassStream n, g;
            pass_stream(p, PassMove(), n);
            game_haze(g, base, colour, depths, 3);
            compare("haze", n, g);
        }
    }
    // (the comparison sees a difference: another colour, the flicker filter's second half)
    {
        RdPass p = pass_of(RD_PASS_SCALE, RD_PASS_SITE_BRIGHTNESS, 0);
        p.arg[0] = p.arg[1] = p.arg[2] = 0x80;
        PassStream n, g, g2;
        pass_stream(p, PassMove(), n);
        game_scale(g, 0, 0x7F, 0x80, 0x80);
        Result a = run(n), b = run(g);
        check(!a.drawn.pixels.empty() && a.drawn.pixels.size() == 640 * 448 && a.drawn.pixels != b.drawn.pixels,
              "another colour differs");
        RdPass f = pass_of(RD_PASS_FLICKER, RD_PASS_SITE_FLICKER, 0);
        f.arg[1] = 1;
        pass_stream(f, PassMove(), n = PassStream());
        game_flicker(g2, 0, true);
        check(run(n).drawn.pixels != run(g2).drawn.pixels, "the flicker filter's halves differ");
    }
    // the view-turn shift: its first strip as Game_ShiftScreenByViewTurn makes it, either way of the turn
    for (int trial = 0; trial < 64; trial++) {
        int32_t ox = int32_t(rng() % 257) - 128, oy = int32_t(rng() % 257) - 128;
        GameSprite s;
        s.prim = 0x116;
        s.tex0 = (0x28028000ull | (trial & 1 ? 0 : 0x1180)) | (0xA400ull << 20);
        s.rgba[0] = s.rgba[1] = s.rgba[2] = s.rgba[3] = 0x80;
        s.corner[0].z = s.corner[1].z = 0xFFFFFFFFu;
        if (ox < 1) {
            s.corner[0].x = 0x6BF8, s.corner[1].x = 0x6E00, s.corner[0].texel_x = 0, s.corner[1].texel_x = 0x200;
        } else {
            s.corner[0].x = 0x9400, s.corner[1].x = 0x91F8, s.corner[0].texel_x = 0x2800, s.corner[1].texel_x = 0x2600;
        }
        if (oy < 1) {
            s.corner[0].y = 0x71F8, s.corner[1].y = 0x8E00, s.corner[0].texel_y = 0, s.corner[1].texel_y = 0x1C00;
        } else {
            s.corner[0].y = 0x8E00, s.corner[1].y = 0x71F8, s.corner[0].texel_y = 0x1C00, s.corner[1].texel_y = 0;
        }
        for (auto &c : s.corner) {
            c.x = (c.x + uint32_t(ox)) & 0xFFFF;
            c.y = (c.y + uint32_t(oy)) & 0xFFFF;
        }
        int32_t step = ox < 1 ? 0x200 : -0x200;
        RdPass p = pass_of(RD_PASS_VIEW_SHIFT, RD_PASS_SITE_VIEW_SHIFT, 0);
        // (nd_passes.c copies the GsSpritePacket: TEX0, TEX1, PRIM, RGBA bytes, then the corners: X | Y << 16, Z, texels)
        p.data[0] = uint32_t(s.tex0), p.data[1] = uint32_t(s.tex0 >> 32), p.data[2] = 0, p.data[3] = 0;
        p.data[4] = s.prim;
        p.data[5] = 0x80808080u;
        for (int k = 0; k < 2; k++) {
            p.data[8 + k * 4] = s.corner[k].x | s.corner[k].y << 16;
            p.data[9 + k * 4] = s.corner[k].z;
            p.data[10 + k * 4] = uint32_t(s.corner[k].texel_x);
            p.data[11 + k * 4] = uint32_t(s.corner[k].texel_y);
        }
        p.arg[0] = uint32_t(step);
        PassStream n, g;
        pass_stream(p, PassMove(), n);
        game_view_shift(g, s, step);
        compare("view-turn shift", n, g);
    }
    // the wobble grid's cells
    {
        RdPass p = pass_of(RD_PASS_WOBBLE, RD_PASS_SITE_WOBBLE, 0);
        uint32_t rgba[4] = {0x5E, 0x87, 0x92, 0x80};
        for (int i = 0; i < 4; i++) {
            p.arg[i] = rgba[i];
        }
        for (int i = 0; i < 88; i++) {
            p.data[i] = (rng() & 0x3FFF) | (rng() & 0x3FFF) << 16;
        }
        PassStream n, g;
        pass_stream(p, PassMove(), n);
        game_wobble(g, p.data, rgba);
        compare("wobble grid", n, g);
    }

    // ---- The pictures between ----
    {
        // a fill whose alpha changed: paired, its colour t of the way (gif_moves.h: b + floor((a - b) u + 0.5))
        PassMoves moves;
        RdPass was = pass_of(RD_PASS_FILL, RD_PASS_SITE_FILL, 0), now = was;
        was.arg[3] = 0x40, now.arg[3] = 0x80, was.arg[4] = now.arg[4] = 0x44;
        moves.frame(1, PartWalk::frame);
        moves.pass(was, 0, false, PartWalk::frame, 1.0f);
        moves.frame(2, PartWalk::frame);
        moves.pass(now, 0, false, PartWalk::frame, 1.0f);
        moves.frame(2, PartWalk::picture);
        PassMove m = moves.pass(now, 0, false, PartWalk::picture, 0.25f);
        check(m.mode == PASS_MOVE_RGBA && m.prev_rgba == 0x40000000u, "a fill's colour change pairs it");
        PassStream s;
        pass_stream(now, m, s);
        Result r = run(s);
        auto px = r.drawn.pixels.find({10, 10});
        float u = 1.0f - 0.25f;
        uint32_t want = 0x80 + uint32_t(int32_t(floorf(float(0x40 - 0x80) * u + 0.5f)));
        check(px != r.drawn.pixels.end() && (std::get<0>(px->second) >> 24) == want, "a fill's alpha t of the way");
        // a second fill of the same site is paired with the second of the frame before
        RdPass other = now;
        other.arg[0] = 9;
        PassMove m2 = moves.pass(other, 0, false, PartWalk::picture, 0.25f);
        check(m2.mode == 0, "the second fill of the site has no partner in the frame before");
    }
    {
        // the frame shown before: A^t when the frame shown is kept, from A t of the way when its alpha changed
        PassMoves moves;
        RdPass was = pass_of(RD_PASS_PREVIOUS, RD_PASS_SITE_AIM, 0), now = was;
        int32_t rect[4] = {0, 0, 640, 448};
        for (int i = 0; i < 4; i++) {
            was.arg[1 + i] = now.arg[1 + i] = was.arg[5 + i] = now.arg[5 + i] = uint32_t(rect[i]);
        }
        was.arg[0] = 0x40, now.arg[0] = 0x60;
        moves.frame(1, PartWalk::frame);
        moves.pass(was, 0, true, PartWalk::frame, 1.0f);
        moves.frame(2, PartWalk::frame);
        moves.pass(now, 0, true, PartWalk::frame, 1.0f);
        moves.frame(2, PartWalk::picture);
        float t = 0.5f, u = 1.0f - t;
        PassMove m = moves.pass(now, 0, true, PartWalk::picture, t);
        check(m.mode == (PASS_MOVE_PREV | PASS_MOVE_PAIRED), "the frame shown before: kept and paired");
        PassStream s;
        pass_stream(now, m, s);
        Result r = run(s);
        float base = 96.0f + (64.0f - 96.0f) * u, a = powf(base / 128.0f, t);
        uint32_t want = uint32_t(int32_t(floorf(a * 128.0f + 0.5f)));
        auto px = r.drawn.pixels.find({100, 100});
        check(px != r.drawn.pixels.end() && (std::get<0>(px->second) >> 24) == want, "the frame shown before by A^t");
        PassMove none = moves.pass(now, 0, false, PartWalk::picture, t);
        check(none.mode == 0, "the frame shown before not kept: as the frame");
    }
    {
        // the wobble: a cell whose springs moved a little pairs, its texels t of the way in 12.4; the frame's own walk
        // kept answers as a picture would
        PassMoves moves;
        RdPass was = pass_of(RD_PASS_WOBBLE, RD_PASS_SITE_WOBBLE, 0), now = was;
        for (int i = 0; i < 4; i++) {
            was.arg[i] = now.arg[i] = 0x80;
        }
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 11; j++) {
                uint32_t uu = uint32_t(j * 64 * 16 + 100), vv = uint32_t(i * 64 * 16 + 100);
                was.data[i * 11 + j] = uu | vv << 16;
                now.data[i * 11 + j] = (uu + (i + j) % 3 * 8) | vv << 16;
            }
        }
        uint64_t tex0 = (10ull << 26) | (9ull << 30); // (1024 x 512)
        moves.frame(1, PartWalk::frame);
        moves.pass(was, tex0, false, PartWalk::frame, 1.0f);
        moves.frame(2, PartWalk::frame_kept);
        PassMove kept = moves.pass(now, tex0, false, PartWalk::frame_kept, 0.75f);
        check((kept.mode & PASS_MOVE_GRID) != 0, "the frame's own walk kept: the wobble's move answered");
        moves.frame(2, PartWalk::picture);
        float t = 0.75f, u = 1.0f - t;
        PassMove m = moves.pass(now, tex0, false, PartWalk::picture, t);
        check((m.mode & PASS_MOVE_GRID) != 0 && m.cells[0] != 0, "the wobble's cells pair");
        PassStream s;
        pass_stream(now, m, s);
        Result r = run(s);
        // cell (0, 1)'s second corner: point (0, 2)
        uint32_t g = 2, nu = now.data[g] & 0x3FFF, wu = was.data[g] & 0x3FFF;
        float b = float(nu) / 16.0f, a = float(wu) / 16.0f;
        uint32_t want = uint32_t(int32_t(floorf((b + (a - b) * u) * 16.0f + 0.5f)));
        bool found = false;
        for (const auto &tri : r.drawn.triangles) {
            for (size_t k = 1; k + 1 < tri.size(); k += 2) {
                uint32_t x = uint32_t(tri[k] & 0xFFFF), y = uint32_t((tri[k] >> 16) & 0xFFFF);
                if (x == ((2 << 6) + 0x6C0) << 4 && y == 0x720 << 4) {
                    found = true;
                    check(uint32_t((tri[k] >> 32) & 0xFFFF) == want, "a wobble point's texel t of the way");
                }
            }
        }
        check(found, "the wobble's point drawn");
    }
    {
        // the frame's own walk kept: a pass that its pictures move drawn at the frame's time, each kick with the move
        // the GPU makes (pass_stream_moves): the wobble (texels and colour), the view-turn shift (places and texels),
        // a fill's colour and the tint's; the frame shown before (it reads the frame shown kept) is not
        for (float t : {0.25f, 0.5f, 0.8125f}) {
            PassMoves moves;
            RdPass was = pass_of(RD_PASS_WOBBLE, RD_PASS_SITE_WOBBLE, 0), now = was;
            for (int i = 0; i < 4; i++) {
                was.arg[i] = 0x80;
                now.arg[i] = 0x60 + uint32_t(i) * 8;
            }
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 11; j++) {
                    uint32_t uu = uint32_t(j * 64 * 16 + 100), vv = uint32_t(i * 64 * 16 + 100);
                    was.data[i * 11 + j] = uu | vv << 16;
                    now.data[i * 11 + j] = (uu + uint32_t((i + j) % 3 * 8)) | (vv + uint32_t((i * j) % 5 * 3)) << 16;
                }
            }
            uint64_t tex0 = (10ull << 26) | (9ull << 30);
            RdPass fill_was = pass_of(RD_PASS_FILL, RD_PASS_SITE_FILL, 0), fill_now = fill_was;
            uint32_t fw[6] = {0x10, 0x40, 0x30, 0x20, 0x44, 0}, fn[6] = {0x30, 0x20, 0x31, 0x70, 0x44, 0};
            memcpy(fill_was.arg, fw, sizeof(fw));
            memcpy(fill_now.arg, fn, sizeof(fn));
            RdPass tint_was = pass_of(RD_PASS_SCALE, RD_PASS_SITE_TINT, 0x1180), tint_now = tint_was;
            tint_was.arg[0] = 0x5E, tint_was.arg[1] = 0x87, tint_was.arg[2] = 0x92;
            tint_now.arg[0] = 0x60, tint_now.arg[1] = 0x80, tint_now.arg[2] = 0x99;
            RdPass shift_was = pass_of(RD_PASS_VIEW_SHIFT, RD_PASS_SITE_VIEW_SHIFT, 0), shift_now;
            uint64_t shift_tex0 = 0x1180 | (10ull << 26) | (9ull << 30);
            shift_was.data[0] = uint32_t(shift_tex0), shift_was.data[1] = uint32_t(shift_tex0 >> 32);
            shift_was.data[4] = 0x156, shift_was.data[5] = 0x80808080u;
            uint32_t corners[8] = {0x6C00u | 0x7200u << 16, 0, 0, 0, 0x6E00u | 0x8E00u << 16, 0, 32 * 16, 448 * 16};
            memcpy(shift_was.data + 8, corners, sizeof(corners));
            shift_was.arg[0] = 32 * 16;
            shift_now = shift_was;
            shift_now.data[8] += 3 * 16, shift_now.data[12] += 3 * 16; // (the strips 3 pixels on, their texels too)
            shift_now.data[10] += 3 * 16, shift_now.data[14] += 3 * 16;
            moves.frame(1, PartWalk::frame);
            moves.pass(was, tex0, false, PartWalk::frame, 1.0f);
            moves.pass(fill_was, 0, false, PartWalk::frame, 1.0f);
            moves.pass(tint_was, 0, false, PartWalk::frame, 1.0f);
            moves.pass(shift_was, 0, false, PartWalk::frame, 1.0f);
            moves.frame(2, PartWalk::frame_kept);
            PassMove wobble = moves.pass(now, tex0, false, PartWalk::frame_kept, t);
            PassMove fill = moves.pass(fill_now, 0, false, PartWalk::frame_kept, t);
            PassMove tint = moves.pass(tint_now, 0, false, PartWalk::frame_kept, t);
            PassMove shift = moves.pass(shift_now, 0, false, PartWalk::frame_kept, t);
            check((wobble.mode & PASS_MOVE_GRID) && (wobble.mode & PASS_MOVE_RGBA), "the wobble pairs, colour too");
            check(fill.mode == PASS_MOVE_RGBA && tint.mode == PASS_MOVE_RGBA, "a fill's colour and the tint's pair");
            check(shift.mode == PASS_MOVE_SHIFT && shift.strips == 0xFFFFF, "the view-turn shift's strips pair");
            check_moves("the wobble", now, wobble, t);
            check_moves("a fill", fill_now, fill, t);
            check_moves("the tint", tint_now, tint, t);
            check_moves("the view-turn shift", shift_now, shift, t);
            RdPass prev = pass_of(RD_PASS_PREVIOUS, RD_PASS_SITE_PREVIOUS, 0x1180);
            PassMove prev_move;
            prev_move.mode = PASS_MOVE_PREV;
            PassStream s;
            std::vector<PassKickMove> none;
            check(!pass_stream_moves(prev, prev_move, s, none) && s.q.empty() && none.empty(),
                  "the frame shown before blended in: not moved by its kicks");
        }
    }
    printf("pass_draw_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
