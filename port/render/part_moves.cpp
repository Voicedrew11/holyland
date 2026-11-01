// The native draws' moves in the pictures between (part_moves.hpp)
#include "part_moves.hpp"

#include <math.h>
#include <string.h>

extern "C" {
#include "port/host.h"
}

namespace {

const double MAX_MOVE = 400.0; // (frame_interp.c's INTERP_MAX_MOVE: more is not a move but a jump)

// a b, matrices of VU layout (a qword a column), in doubles
void multiply(double *out, const float *a, const float *b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double sum = 0.0;
            for (int i = 0; i < 4; i++) {
                sum += double(a[i * 4 + row]) * double(b[col * 4 + i]);
            }
            out[col * 4 + row] = sum;
        }
    }
}

// A matrix's linear part split into a rotation r and the rest s (r s; s is r's transpose times it), r[row][col]: its
// first column's direction, the second's square to it, the third their cross (so a mirror is s's)
bool split(const double *m, double r[3][3], double s[3][3]) {
    double c[3][3]; // (columns)
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            c[j][i] = m[j * 4 + i];
        }
    }
    double n0 = sqrt(c[0][0] * c[0][0] + c[0][1] * c[0][1] + c[0][2] * c[0][2]);
    if (!(n0 > 1e-12) || !std::isfinite(n0)) {
        return false;
    }
    double r0[3] = {c[0][0] / n0, c[0][1] / n0, c[0][2] / n0};
    double d = r0[0] * c[1][0] + r0[1] * c[1][1] + r0[2] * c[1][2];
    double u[3] = {c[1][0] - d * r0[0], c[1][1] - d * r0[1], c[1][2] - d * r0[2]};
    double n1 = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(n1 > 1e-12 * n0) || !std::isfinite(n1)) {
        return false;
    }
    double r1[3] = {u[0] / n1, u[1] / n1, u[2] / n1};
    double r2[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    const double *rc[3] = {r0, r1, r2};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i][j] = rc[j][i];
            s[i][j] = rc[i][0] * c[j][0] + rc[i][1] * c[j][1] + rc[i][2] * c[j][2];
            if (!std::isfinite(s[i][j])) {
                return false;
            }
        }
    }
    return true;
}

// The kinds moved by their own matrices; the map records and the items lying about keep the camera's move (the sky by
// its translation)
bool model_kind(uint32_t kind) {
    return kind == RD_KIND_MAP_OBJECT || kind == RD_KIND_PICKED || kind == RD_KIND_ENTITY || kind == RD_KIND_ARM ||
           kind == RD_KIND_GOLD || kind == RD_KIND_SHADOW;
}

} // namespace

bool vu1_between(Vu1Between &out, const float *cur, const float *prev, const float *view_cur, const float *view_prev) {
    double c[16], p[16];
    if (view_cur) {
        multiply(c, view_cur, cur);
        multiply(p, view_prev, prev);
    } else {
        for (int i = 0; i < 16; i++) {
            c[i] = cur[i];
            p[i] = prev[i];
        }
    }
    double dt[3] = {p[12] - c[12], p[13] - c[13], p[14] - c[14]};
    double d2 = dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2];
    // (the bound as a float, as gsn_native_draw.inc has it)
    if (!(d2 < double(float(MAX_MOVE)) * float(MAX_MOVE))) {
        return false;
    }
    double rc[3][3], sc[3][3], rp[3][3], sp[3][3];
    if (!split(c, rc, sc) || !split(p, rp, sp)) {
        return false;
    }
    // the turn q from rc to rp in rc's frame (rp = rc q): rc's transpose times rp, as a quaternion, the shorter way round
    double q[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            q[i][j] = rc[0][i] * rp[0][j] + rc[1][i] * rp[1][j] + rc[2][i] * rp[2][j];
        }
    }
    double w, x, y, z, tr = q[0][0] + q[1][1] + q[2][2];
    if (tr > 0.0) {
        double t = sqrt(tr + 1.0) * 2.0;
        w = t / 4.0, x = (q[2][1] - q[1][2]) / t, y = (q[0][2] - q[2][0]) / t, z = (q[1][0] - q[0][1]) / t;
    } else if (q[0][0] > q[1][1] && q[0][0] > q[2][2]) {
        double t = sqrt(1.0 + q[0][0] - q[1][1] - q[2][2]) * 2.0;
        w = (q[2][1] - q[1][2]) / t, x = t / 4.0, y = (q[0][1] + q[1][0]) / t, z = (q[0][2] + q[2][0]) / t;
    } else if (q[1][1] > q[2][2]) {
        double t = sqrt(1.0 + q[1][1] - q[0][0] - q[2][2]) * 2.0;
        w = (q[0][2] - q[2][0]) / t, x = (q[0][1] + q[1][0]) / t, y = t / 4.0, z = (q[1][2] + q[2][1]) / t;
    } else {
        double t = sqrt(1.0 + q[2][2] - q[0][0] - q[1][1]) * 2.0;
        w = (q[1][0] - q[0][1]) / t, x = (q[0][2] + q[2][0]) / t, y = (q[1][2] + q[2][1]) / t, z = t / 4.0;
    }
    if (w < 0.0) {
        w = -w, x = -x, y = -y, z = -z;
    }
    double vn = sqrt(x * x + y * y + z * z);
    double angle = 2.0 * atan2(vn, w);
    if (!std::isfinite(angle) || angle > 3.0) {
        return false;
    }
    memset(&out, 0, sizeof(out));
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            out.rot[col * 4 + row] = float(rc[row][col]);
            out.s[col * 4 + row] = float(sc[row][col]);
            out.ds[col * 4 + row] = float(sp[row][col] - sc[row][col]);
        }
        out.dt[col] = float(dt[col]);
    }
    if (vn > 1e-12) {
        out.axis[0] = float(x / vn);
        out.axis[1] = float(y / vn);
        out.axis[2] = float(z / vn);
        out.axis[3] = float(angle);
    } else {
        out.axis[0] = 1.0f;
    }
    return true;
}

bool PartMoves::Key::operator==(const Key &o) const {
    return memcmp(k, o.k, sizeof(k)) == 0;
}

size_t PartMoves::KeyHash::operator()(const Key &key) const {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t v : key.k) {
        h = (h ^ v) * 0x100000001b3ull;
    }
    return size_t(h ^ (h >> 29));
}

void PartMoves::frame(uint32_t serial, PartWalk walk) {
    section_ = Section::none;
    seen_.clear();
    // (a frame's packets drawn again, for a picture between, are the same frame)
    if (walk == PartWalk::picture || serial == serial_) {
        return;
    }
    serial_ = serial;
    before_.swap(now_);
    now_.clear();
    strip_before_.swap(strip_now_);
    strip_now_.clear();
}

void PartMoves::mark(uint64_t frame_interp_value) {
    uint32_t index = uint32_t(frame_interp_value >> 32) & 0xFF, v = uint32_t(frame_interp_value);
    if (index != PORT_FRAME_INTERP_HEADER) {
        return;
    }
    if (v == PORT_FRAME_INTERP_ARM) {
        section_ = Section::arm;
    } else if (v == PORT_FRAME_INTERP_END) {
        section_ = Section::other;
    } else if ((v & 0xFF) != 0) {
        section_ = (v >> 8) == 1 ? Section::reflect : Section::other;
    } else {
        section_ = (v >> 8) == 2 ? Section::other : Section::world;
    }
}

PartMoves::Key PartMoves::key_of(const RdDraw &d) {
    Key key = {{d.kind, d.id[0], d.id[1], d.id[2], uint32_t(section_), 0}};
    key.k[5] = seen_[key]++;
    return key;
}

// In a picture between, how a draw of the models' kinds moves: in the world with the camera and, paired with its draw
// in the frame before, between its two places (a: its matrix's move; b: its parent's, program 0x280's set B); in the
// reflection with the camera only; the arm and the held item between their two places in the eye's space, the
// camera's move left out; elsewhere, or the arm not paired, as the frame has it
int32_t PartMoves::model_flags(const Key &key, const RdDraw &d, const RdCamera &cam, Vu1Between *a, Vu1Between *b) {
    bool eye = section_ == Section::arm && d.kind == RD_KIND_ARM;
    if (!eye && section_ != Section::world && section_ != Section::reflect) {
        return 0;
    }
    int32_t flags = eye ? 0 : VU1_MOVE_CAMERA;
    auto it = before_.find(key);
    if (section_ == Section::reflect || it == before_.end()) {
        return flags;
    }
    const Place &was = it->second;
    if (memcmp(was.model, d.model, 64) == 0 && (!b || memcmp(was.parent, d.parent, 64) == 0) &&
        (!eye || memcmp(was.view, cam.view, 64) == 0)) {
        return flags; // (it did not move)
    }
    const float *vc = eye ? &cam.view[0][0] : nullptr, *vp = eye ? was.view : nullptr;
    // (set B, the parent's: a part with none sends a slot of no matrix, never read: no move then)
    if (b && !vu1_between(*b, &d.parent[0][0], was.parent, vc, vp)) {
        memset(b, 0, sizeof(*b));
    }
    if (!vu1_between(*a, &d.model[0][0], was.model, vc, vp)) {
        return flags;
    }
    return VU1_MOVE_CAMERA | VU1_MOVE_MODEL | (eye ? VU1_MOVE_EYE : 0);
}

// How a picture between moves a draw (the frame's places are kept by draw())
PartMove PartMoves::move_of(const Key &key, const RdDraw &d, const RdCamera &cam) {
    PartMove move;
    if (d.program == 0x280) {
        move.flags = model_flags(key, d, cam, &move.model, &move.parent);
        return move;
    }
    if (d.program == 0x700 || model_kind(d.kind)) {
        move.flags = model_flags(key, d, cam, &move.model, nullptr);
        return move;
    }
    if (section_ != Section::world && section_ != Section::reflect) {
        return move;
    }
    // the world and the reflection move with the picture's camera; a part of the world that moved since the frame
    // before (the sky) also with its model, as frame_interp.c's object_between
    move.flags = VU1_MOVE_CAMERA;
    auto it = before_.find(key);
    if (section_ == Section::world && it != before_.end()) {
        const float *m = &d.model[0][0], *was = &it->second.model[12];
        float dm[3] = {m[12] - was[0], m[13] - was[1], m[14] - was[2]};
        float d2 = dm[0] * dm[0] + dm[1] * dm[1] + dm[2] * dm[2];
        if (d2 > 0.0f && d2 < float(MAX_MOVE) * float(MAX_MOVE)) {
            memcpy(move.obj, dm, sizeof(dm));
            move.flags |= VU1_MOVE_OBJ;
        }
    }
    return move;
}

PartMove PartMoves::draw(const RdDraw &d, const RdCamera &cam, PartWalk walk) {
    PartMove move;
    Key key = key_of(d);
    if (walk != PartWalk::picture) {
        Place &p = now_[key];
        memcpy(p.model, d.model, 64);
        memcpy(p.parent, d.parent, 64);
        memcpy(p.view, cam.view, 64);
    }
    if (walk == PartWalk::frame) {
        return move;
    }
    move = move_of(key, d, cam);
    if (walk == PartWalk::picture && (move.flags & VU1_MOVE_MODEL)) {
        moved_models_++;
    }
    return move;
}

const PartMoves::StripPoints *PartMoves::strip(const RdDraw &d, std::span<const uint32_t> xy,
                                               std::span<const uint32_t> rgba, PartWalk walk,
                                               std::vector<uint32_t> &from) {
    Key key = key_of(d);
    if (walk != PartWalk::picture) {
        StripPoints &now = strip_now_[key];
        now.xy.assign(xy.begin(), xy.end());
        now.rgba.assign(rgba.begin(), rgba.end());
        if (walk == PartWalk::frame) {
            return nullptr;
        }
    }
    // Each point's partner in the frame before (from[i]). The trail is a ring of the blade's two ends, pushed a pair a
    // frame and kept where they were on the screen until they die: the frame before's points the same, from some point
    // on (the oldest gone), then this frame's new pair, which grows out of the frame before's last pair. Otherwise, as
    // frame_interp.c pairs the PS2 path's: point by point, as many as the frame before had, none more than 128 pixels
    // away.
    auto it = strip_before_.find(key);
    from.assign(xy.size(), 0);
    if (it == strip_before_.end() || it->second.xy.empty()) {
        return nullptr;
    }
    const std::vector<uint32_t> &was = it->second.xy;
    size_t n = was.size();
    bool paired = false;
    for (size_t o = 0; o + 2 <= n && !paired && n - o <= xy.size(); o++) {
        size_t overlap = n - o;
        paired = overlap % 2 == 0 && xy.size() - overlap <= 2 && memcmp(&xy[0], &was[o], overlap * 4) == 0;
        for (size_t i = 0; paired && i < xy.size(); i++) {
            from[i] = uint32_t(i < overlap ? i + o : n - 2 + (i - overlap) % 2);
        }
    }
    if (!paired && n == xy.size()) {
        paired = true;
        for (size_t i = 0; paired && i < xy.size(); i++) {
            int32_t dx = int32_t(xy[i] & 0xFFFF) - int32_t(was[i] & 0xFFFF);
            int32_t dy = int32_t(xy[i] >> 16) - int32_t(was[i] >> 16);
            paired = dx >= -2048 && dx <= 2048 && dy >= -2048 && dy <= 2048;
            from[i] = uint32_t(i);
        }
    }
    if (!paired) {
        return nullptr;
    }
    if (walk == PartWalk::picture) {
        paired_strips_++;
    }
    return &it->second;
}
