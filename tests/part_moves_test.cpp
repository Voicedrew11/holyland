// The native draws' moves in the pictures between (port/render/part_moves.hpp): a part's move between its two matrices
// worked out as the shaders do (vu1_000_maths.h's between_move) against the matrices themselves at t 0, 1 and between;
// the moves refused (a jump, nearly half a turn, no rotation); then which draws move how, by section and identity, and
// the trail's points paired with the frame before's.
#include "part_moves.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "port/host.h"
}

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

struct Vec3 {
    double x, y, z;
};

Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// A matrix of VU layout (a qword a column): a turn of angle about z, a scale, a translation
void matrix(float *m, double angle, double scale, Vec3 t) {
    std::memset(m, 0, 64);
    double c = std::cos(angle), s = std::sin(angle);
    m[0] = float(c * scale);
    m[1] = float(s * scale);
    m[4] = float(-s * scale);
    m[5] = float(c * scale);
    m[10] = float(scale);
    m[12] = float(t.x);
    m[13] = float(t.y);
    m[14] = float(t.z);
    m[15] = 1.0f;
}

Vec3 apply(const float *m, Vec3 p) {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

// vu1_000_maths.h's between_move (nd_between.h), in doubles: how far the part is at u (1 - t) from where it is at t 1
Vec3 between_move(const Vu1Between &b, Vec3 p, double u) {
    auto col = [](const float *c, int i) { return Vec3{c[i * 4], c[i * 4 + 1], c[i * 4 + 2]}; };
    Vec3 sp = {0, 0, 0}, dsp = {0, 0, 0};
    double pv[3] = {p.x, p.y, p.z};
    for (int i = 0; i < 3; i++) {
        Vec3 s = col(b.s, i), ds = col(b.ds, i);
        sp = {sp.x + (s.x + ds.x * u) * pv[i], sp.y + (s.y + ds.y * u) * pv[i], sp.z + (s.z + ds.z * u) * pv[i]};
        dsp = {dsp.x + ds.x * pv[i], dsp.y + ds.y * pv[i], dsp.z + ds.z * pv[i]};
    }
    double phi = b.axis[3] * u, h = std::sin(0.5 * phi);
    Vec3 a = {b.axis[0], b.axis[1], b.axis[2]}, c = cross(a, sp), cc = cross(a, c);
    Vec3 inner = {std::sin(phi) * c.x + 2 * h * h * cc.x + dsp.x * u, std::sin(phi) * c.y + 2 * h * h * cc.y + dsp.y * u,
                  std::sin(phi) * c.z + 2 * h * h * cc.z + dsp.z * u};
    Vec3 r0 = col(b.rot, 0), r1 = col(b.rot, 1), r2 = col(b.rot, 2);
    return {r0.x * inner.x + r1.x * inner.y + r2.x * inner.z + b.dt[0] * u,
            r0.y * inner.x + r1.y * inner.y + r2.y * inner.z + b.dt[1] * u,
            r0.z * inner.x + r1.z * inner.y + r2.z * inner.z + b.dt[2] * u};
}

bool near(Vec3 a, Vec3 b, double tolerance) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

void test_between() {
    float cur[16], prev[16];
    matrix(cur, 0.5, 2.0, {10, 20, 30});
    matrix(prev, 0.2, 1.5, {12, 18, 33});
    Vu1Between b;
    CHECK(vu1_between(b, cur, prev, nullptr, nullptr));
    const Vec3 points[] = {{1, 0, 0}, {0, 1, 0}, {3, -2, 5}, {-7, 4, 1}};
    for (Vec3 p : points) {
        Vec3 here = apply(cur, p), there = apply(prev, p);
        // t 1: where it is; t 0: where it was
        CHECK(near(between_move(b, p, 0.0), {0, 0, 0}, 1e-9));
        Vec3 back = between_move(b, p, 1.0);
        CHECK(near({here.x + back.x, here.y + back.y, here.z + back.z}, there, 1e-4));
        // half way: turned half way (about z), scaled and moved half way
        float mid[16];
        matrix(mid, 0.35, 1.75, {11, 19, 31.5});
        Vec3 half = between_move(b, p, 0.5), want = apply(mid, p);
        CHECK(near({here.x + half.x, here.y + half.y, here.z + half.z}, want, 1e-4));
    }
    // the same matrix: no move at any time
    CHECK(vu1_between(b, cur, cur, nullptr, nullptr));
    CHECK(near(between_move(b, {3, 4, 5}, 0.7), {0, 0, 0}, 1e-6));
    // refused: a jump of 400 or more, nearly half a turn, no rotation in it
    matrix(prev, 0.5, 2.0, {410, 20, 30});
    CHECK(!vu1_between(b, cur, prev, nullptr, nullptr));
    matrix(prev, 0.5 + 3.1, 2.0, {10, 20, 30});
    CHECK(!vu1_between(b, cur, prev, nullptr, nullptr));
    matrix(prev, 0.0, 0.0, {10, 20, 30});
    CHECK(!vu1_between(b, cur, prev, nullptr, nullptr));
    // in the eye's space: the views' products
    float view_cur[16], view_prev[16];
    matrix(view_cur, 0.1, 1.0, {0, 0, 0});
    matrix(view_prev, 0.3, 1.0, {1, 0, 0});
    matrix(prev, 0.5, 2.0, {10, 20, 30});
    CHECK(vu1_between(b, cur, prev, view_cur, view_prev));
    Vec3 p = {2, 1, 0};
    Vec3 here = apply(view_cur, apply(cur, p)), there = apply(view_prev, apply(prev, p));
    Vec3 back = between_move(b, p, 1.0);
    CHECK(near({here.x + back.x, here.y + back.y, here.z + back.z}, there, 1e-4));
}

RdDraw draw_of(uint32_t kind, uint32_t id, uint32_t program, Vec3 at, double angle) {
    RdDraw d;
    std::memset(&d, 0, sizeof(d));
    d.kind = kind;
    d.id[0] = id;
    d.program = program;
    matrix(&d.model[0][0], angle, 1.0, at);
    matrix(&d.parent[0][0], 0.0, 1.0, {0, 0, 0});
    return d;
}

RdCamera camera_of(double angle) {
    RdCamera c;
    std::memset(&c, 0, sizeof(c));
    matrix(&c.view[0][0], angle, 1.0, {0, 0, 0});
    return c;
}

// frame_interp.c's header marks (PORT_GS_REG_FRAME_INTERP's value)
uint64_t header(uint32_t v) {
    return uint64_t(PORT_FRAME_INTERP_HEADER) << 32 | v;
}
const uint64_t WORLD = header(0), REFLECT = header(1 << 8 | 1), BEFORE_EYE = header(2 << 8);
const uint64_t ARM = header(PORT_FRAME_INTERP_ARM), END = header(PORT_FRAME_INTERP_END);

void test_draws() {
    PartMoves moves;
    RdCamera cam = camera_of(0.0);
    RdDraw sky0 = draw_of(RD_KIND_MAP_RECORD, 1, 0, {0, 0, 0}, 0.0), sky1 = draw_of(RD_KIND_MAP_RECORD, 1, 0, {1, 2, 3}, 0.0);
    RdDraw man0 = draw_of(RD_KIND_ENTITY, 2, 0, {5, 0, 0}, 0.0), man1 = draw_of(RD_KIND_ENTITY, 2, 0, {6, 0, 0}, 0.2);
    RdDraw still = draw_of(RD_KIND_MAP_OBJECT, 3, 0, {9, 9, 9}, 0.0);
    RdDraw arm0 = draw_of(RD_KIND_ARM, 4, 0x280, {0, 1, 0}, 0.0), arm1 = draw_of(RD_KIND_ARM, 4, 0x280, {0, 1, 0}, 0.1);
    RdDraw fx0 = draw_of(RD_KIND_EFFECT, 5, 0x700, {0, 0, 0}, 0.0), fx1 = draw_of(RD_KIND_EFFECT, 5, 0x700, {0, 0, 1}, 0.0);
    // the frame before
    moves.frame(1, PartWalk::frame);
    moves.mark(WORLD);
    moves.draw(sky0, cam, PartWalk::frame);
    moves.draw(man0, cam, PartWalk::frame);
    moves.draw(man0, cam, PartWalk::frame); // (a second draw of the same identity: paired with its own)
    moves.draw(still, cam, PartWalk::frame);
    moves.draw(fx0, cam, PartWalk::frame);
    moves.mark(ARM);
    moves.draw(arm0, cam, PartWalk::frame);
    // this frame, its own drawing: nothing moves
    moves.frame(2, PartWalk::frame);
    moves.mark(WORLD);
    CHECK(moves.draw(sky1, cam, PartWalk::frame).flags == 0);
    moves.draw(man1, cam, PartWalk::frame);
    moves.draw(man0, cam, PartWalk::frame);
    moves.draw(still, cam, PartWalk::frame);
    moves.draw(fx1, cam, PartWalk::frame);
    moves.mark(ARM);
    moves.draw(arm1, cam, PartWalk::frame);
    // a picture between's walk of this frame (the same serial, in a picture: the frame before kept)
    for (int walk = 0; walk < 2; walk++) {
        moves.frame(2, PartWalk::picture);
        moves.mark(WORLD);
        PartMove m = moves.draw(sky1, cam, PartWalk::picture);
        CHECK(m.flags == (VU1_MOVE_CAMERA | VU1_MOVE_OBJ) && m.obj[0] == 1.0f && m.obj[1] == 2.0f && m.obj[2] == 3.0f);
        CHECK(moves.draw(man1, cam, PartWalk::picture).flags == (VU1_MOVE_CAMERA | VU1_MOVE_MODEL));
        CHECK(moves.draw(man0, cam, PartWalk::picture).flags == VU1_MOVE_CAMERA); // (the second draw: it did not move)
        CHECK(moves.draw(still, cam, PartWalk::picture).flags == VU1_MOVE_CAMERA);
        PartMove fx = moves.draw(fx1, cam, PartWalk::picture);
        CHECK(fx.flags == (VU1_MOVE_CAMERA | VU1_MOVE_MODEL) && fx.model.dt[2] == -1.0f);
        moves.mark(ARM);
        CHECK(moves.draw(arm1, cam, PartWalk::picture).flags == (VU1_MOVE_CAMERA | VU1_MOVE_MODEL | VU1_MOVE_EYE));
        moves.mark(END);
        CHECK(moves.draw(sky1, cam, PartWalk::picture).flags == 0);
    }
    CHECK(moves.moved_models() == 6);
    // the reflection: with the camera only; before the eye: not moved; a draw not in the frame before: the camera's
    moves.frame(2, PartWalk::picture);
    moves.mark(REFLECT);
    CHECK(moves.draw(man1, cam, PartWalk::picture).flags == VU1_MOVE_CAMERA);
    CHECK(moves.draw(sky1, cam, PartWalk::picture).flags == VU1_MOVE_CAMERA);
    moves.mark(BEFORE_EYE);
    CHECK(moves.draw(man1, cam, PartWalk::picture).flags == 0);
    moves.mark(WORLD);
    CHECK(moves.draw(draw_of(RD_KIND_ENTITY, 99, 0, {0, 0, 0}, 0.0), cam, PartWalk::picture).flags == VU1_MOVE_CAMERA);
    // a new frame: the one before is this one
    moves.frame(3, PartWalk::frame);
    moves.mark(WORLD);
    moves.draw(sky1, cam, PartWalk::frame);
    moves.frame(3, PartWalk::picture);
    moves.mark(WORLD);
    CHECK(moves.draw(sky1, cam, PartWalk::picture).flags == VU1_MOVE_CAMERA);
}

void test_strips() {
    PartMoves moves;
    RdDraw trail = draw_of(RD_KIND_ARM, 7, RD_PROGRAM_GS_STRIP, {0, 0, 0}, 0.0);
    std::vector<uint32_t> from;
    const std::vector<uint32_t> was = {0x00100010, 0x00200020, 0x00300030, 0x00400040};
    const std::vector<uint32_t> rgba = {1, 2, 3, 4};
    moves.frame(1, PartWalk::frame);
    CHECK(moves.strip(trail, was, rgba, PartWalk::frame, from) == nullptr);
    // the oldest pair gone, a new pair pushed: the frame before's last pair is where the new one grows from
    const std::vector<uint32_t> now = {0x00300030, 0x00400040, 0x00500050, 0x00600060};
    moves.frame(2, PartWalk::frame);
    moves.strip(trail, now, rgba, PartWalk::frame, from);
    moves.frame(2, PartWalk::picture);
    const PartMoves::StripPoints *before = moves.strip(trail, now, rgba, PartWalk::picture, from);
    CHECK(before != nullptr && before->xy == was && from == std::vector<uint32_t>({2, 3, 2, 3}));
    // as many as before, nearby: point by point
    const std::vector<uint32_t> shifted = {0x00110011, 0x00210021, 0x00310031, 0x00410041};
    moves.frame(3, PartWalk::frame);
    moves.strip(trail, shifted, rgba, PartWalk::frame, from);
    moves.frame(3, PartWalk::picture);
    before = moves.strip(trail, shifted, rgba, PartWalk::picture, from);
    CHECK(before != nullptr && from == std::vector<uint32_t>({0, 1, 2, 3}));
    // far apart: not paired
    const std::vector<uint32_t> far = {0x0F000F00, 0x00210021, 0x00310031, 0x00410041};
    moves.frame(4, PartWalk::frame);
    moves.strip(trail, far, rgba, PartWalk::frame, from);
    moves.frame(5, PartWalk::frame);
    moves.strip(trail, shifted, rgba, PartWalk::frame, from);
    moves.frame(5, PartWalk::picture);
    CHECK(moves.strip(trail, shifted, rgba, PartWalk::picture, from) == nullptr);
    CHECK(moves.paired_strips() == 2);
}

bool same_move(const PartMove &a, const PartMove &b) {
    return a.flags == b.flags && memcmp(a.obj, b.obj, sizeof(a.obj)) == 0 &&
           memcmp(&a.model, &b.model, sizeof(a.model)) == 0 && memcmp(&a.parent, &b.parent, sizeof(a.parent)) == 0;
}

// A frame's draws and trail: the sky and a still object, a man whose parent moves (program 0x280), an effect, the arm
// in the eye's space, and a trail whose points move a little each frame
struct Scene {
    RdCamera cam;
    RdDraw sky, man, still, fx, arm, trail;
    std::vector<uint32_t> xy, rgba;
};

Scene scene_of(int f) {
    double t = f;
    Scene s;
    s.cam = camera_of(0.02 * t);
    s.sky = draw_of(RD_KIND_MAP_RECORD, 1, 0, {t, 2.0 * t, 3.0}, 0.0);
    s.man = draw_of(RD_KIND_ENTITY, 2, 0x280, {5.0 + 0.5 * t, 0.0, 0.0}, 0.2 * t);
    matrix(&s.man.parent[0][0], 0.05 * t, 1.0, {0.0, t, 0.0});
    s.still = draw_of(RD_KIND_MAP_OBJECT, 3, 0, {9.0, 9.0, 9.0}, 0.0);
    s.fx = draw_of(RD_KIND_EFFECT, 5, 0x700, {0.0, 0.0, 0.5 * t}, 0.0);
    s.arm = draw_of(RD_KIND_ARM, 4, 0x280, {0.0, 1.0, 0.1 * t}, 0.05 * t);
    s.trail = draw_of(RD_KIND_ARM, 7, RD_PROGRAM_GS_STRIP, {0.0, 0.0, 0.0}, 0.0);
    for (uint32_t i = 0; i < 4; i++) {
        s.xy.push_back((0x100 + 0x10 * i + uint32_t(f)) | (0x200 + 0x10 * i) << 16);
        s.rgba.push_back(0x01020304u + i);
    }
    return s;
}

struct Met {
    std::vector<PartMove> moves;
    std::vector<uint32_t> was, from; // the trail's frame before's points, and the partner of each point (when paired)
    bool paired = false;
};

// One walk of a frame as the renderer meets it: its serial, the marks, the draws, then the trail
Met meet(PartMoves &moves, const Scene &s, PartWalk walk, uint32_t serial) {
    Met met;
    moves.frame(serial, walk);
    moves.mark(WORLD);
    met.moves.push_back(moves.draw(s.sky, s.cam, walk));
    met.moves.push_back(moves.draw(s.man, s.cam, walk));
    met.moves.push_back(moves.draw(s.still, s.cam, walk));
    met.moves.push_back(moves.draw(s.fx, s.cam, walk));
    moves.mark(ARM);
    met.moves.push_back(moves.draw(s.arm, s.cam, walk));
    moves.mark(END);
    const PartMoves::StripPoints *before = moves.strip(s.trail, s.xy, s.rgba, walk, met.from);
    if (before) {
        met.paired = true;
        met.was = before->xy;
    }
    return met;
}

// The frame's walk kept (frame_kept) gives each draw and the trail the same answer as the frame's walk followed by a
// picture walk of that frame, frame after frame: the places kept carry on from one frame to the next
void test_frame_kept() {
    PartMoves kept, plain;
    int frames_moved = 0;
    for (uint32_t serial = 1; serial <= 5; serial++) {
        Scene s = scene_of(int(serial));
        Met k = meet(kept, s, PartWalk::frame_kept, serial);
        meet(plain, s, PartWalk::frame, serial);
        Met p = meet(plain, s, PartWalk::picture, serial);
        CHECK(k.moves.size() == p.moves.size());
        bool moved = false;
        for (size_t i = 0; i < k.moves.size() && i < p.moves.size(); i++) {
            CHECK(same_move(k.moves[i], p.moves[i]));
            moved = moved || (p.moves[i].flags & VU1_MOVE_MODEL) != 0;
        }
        CHECK(serial == 1 || p.paired);
        CHECK(k.paired == p.paired);
        if (k.paired && p.paired) {
            CHECK(k.was == p.was && k.from == p.from);
        }
        frames_moved += moved;
    }
    CHECK(frames_moved >= 3);
    // the picture's answers count; the frame_kept walk's own places are not moves
    CHECK(kept.moved_models() == 0 && kept.paired_strips() == 0);
    CHECK(plain.moved_models() > 0 && plain.paired_strips() > 0);
}

} // namespace

int main() {
    test_between();
    test_draws();
    test_strips();
    test_frame_kept();
    if (failures) {
        std::printf("part_moves: %d failed\n", failures);
        return 1;
    }
    std::printf("part_moves: all passed\n");
    return 0;
}
