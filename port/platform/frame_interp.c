/* --frame-rate 60 to 240: pictures between the game's frames in play, where it draws one every second
   vblank (port_frame_interp(), port/include/port/host.h; docs/port/STEP3.md, "Higher frame rates").

   What the game does. In play, GameLoop_Run's loop runs once every two vblanks (30 frames a second):
   Display_SwapBuffers waits for the odd field's vblank (sceGsSyncV until it returns 1), shows the
   buffer drawn last time and sets the other to be drawn (libgraph's draw environment, by PATH3, with
   its clear of colour and Z), then Packet_SendChain sends the packet chain the previous pass built,
   which draws the whole frame into it; then the game moves everything on by one frame (input, the
   player, the camera, the entities) and builds the next chain. The display registers a vblank scans
   out are those it found, before the game's swap after it: the chain drawn after vblank 2k - 1 is
   shown at 2k + 2 and 2k + 3, and the one shown at 2k and 2k + 1 was drawn after 2k - 3. A frame's
   camera is that of the pass before the one that sends it. (Pictures dumped in play change at even
   vblanks only.) The menus, the title and the movies have loops of their own and no world.

   What is done here. The GIF packets that reach the GS between two vblanks are kept (the clear and
   the whole chain). When, at vblank 2k, they drew the game's world into the buffer not shown, they are
   kept on as the pending frame, through vblank 2k + 1 while nothing else has been sent and the same
   buffer is shown, and each picture between (port_interp_picture: at 60, the one of vblank 2k + 1; above
   60, those port_interp_schedule times) sends them to the GS again, drawn into the buffer shown, with
   everything moved to the picture's time t, from 0 (the frame shown) to 1 (the frame drawn): the 3D
   world to the camera t of the way, the paired model parts and 2D items t of the way between their two
   places. The frame drawn is shown at 2k + 2 as before, so nothing is shown later than on the PS2 (no
   more input lag): the pictures between show the camera's move early.
   - Where the world is, and its cameras, the game says in the chain (game/frame_interp.c): a mark
     after each GameLoop_SetCameraMatrices of GameLoop_DrawFrame, with world_view_matrix and
     view_screen_matrix, and one after the world. Within the world (projection 0, modes 0 and 1: the
     world and its mirror image), the vertices VU1's programs 0x000, 0x280, 0x5A0 and 0x700 send
     (not 0x276's GIF packets: the 2D) in triangles are moved: from each one's screen position and its
     depth (1/Q, or the Z) back to the camera's space, by the change of camera, and projected again,
     with S, T and Q for the new depth. A vertex the new camera puts behind it, or past the guard band,
     drops its triangles (ADC), as the VU1 programs do. Models drawn at a fixed place before the eye
     (mode 2) and the arm move only as they move ("What moves by itself"); the reflection's view
     (projection 1, mode 1) by the camera too, by its own projection; the 2D by its pairs ("The 2D").
   - The camera t of the way: the rotation by quaternion slerp, the position along the line. A
     cut (a turn of more than INTERP_MAX_TURN, a move of more than INTERP_MAX_MOVE) is not
     interpolated: the vblank shows the frame before, as on the PS2.
   - Nothing of the replay is left behind. What it draws into the frame goes into the shown buffer,
     which the next frame's clear wipes (FRAME, and TEX0 and BITBLTBUF where they read the frame, point
     there instead). Everything else it draws (the scrolling textures' 128x128 targets, the
     reflection's 256x256 view) is drawn again from the same packets, and the uploads go again in their
     order (asked for again from the native GS, which recorded them: PORT_GS_REG_REPLAY_UPLOAD), so
     every draw finds the textures it found the first time. A buffer a picture between drew differently
     that the frame reads before it draws it is drawn again exact after the pictures
     (port_interp_end_vblank). Local-to-local transfers into anything but the frame are not started,
     but for copies of the frame the frame reads after them (the haze, the wobble: made again from the
     picture's frame), and copies it does not read over pages the pictures between upload into first (the
     map change's last frame, faded out by the frames after: made again from the game's frame by the exact
     restore, COPY_RESTORE); local-to-host ones neither; FINISH, SIGNAL and LABEL are not sent. The registers
     the replay changed (FRAME, TEX0, BITBLTBUF) are written back with what the game last wrote (TEX0
     without a CLUT load). So the game's own frames are the same pixels with the switch on or off, at
     every rate (checked: --dump-frames of the even vblanks). (Dropping the draws into the other
     targets instead does not work: on map 000 the sky samples a 128x128 target at FBP 0x1D4 that the
     frame draws, then draws over with another at 0x1D2.)
   - The previous frame blended in (Game_BlendPreviousFrame) reads, in a picture between, the native
     GS's copy of the frame shown, by a^t (replay_pending, move_flat_vertex).
   - Not interpolated: pictures the CPU makes anew (the HUD orb's swirl, flipbook animations: a new
     texture, not a move), models that come or go, a cut.
   With the switch off nothing here runs: the game sends no marks, and nothing is kept.

   Only the DMA controller's thread runs here (port/platform/gs.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "port/host.h"
#include "port/platform.h"
#include "port/render.h"
#include "port/unlocked_rate.h"
#include "../gs/native/gsn.h"

#define INTERP_MAX_TURN 0.8   /* radians between two frames: more is a cut */
#define INTERP_MAX_MOVE 400.0 /* world units between two frames: more is a cut */

#define GS_PRIM 0x00
#define GS_XYZF2 0x04
#define GS_XYZ2 0x05
#define GS_TEX0_1 0x06
#define GS_TEX0_2 0x07
#define GS_XYZF3 0x0C
#define GS_XYZ3 0x0D
#define GS_PRMODECONT 0x1A
#define GS_PRMODE 0x1B
#define GS_FRAME_1 0x4C
#define GS_FRAME_2 0x4D
#define GS_BITBLTBUF 0x50
#define GS_TRXDIR 0x53
#define GS_SIGNAL 0x60
#define GS_FINISH 0x61
#define GS_LABEL 0x62

int port_frame_interp(void) {
    return port_options.frame_rate > 30;
}

/* ---- How many pictures, and when -------------------------------------------------------------
   The game's frames come every second vblank, at 59.94 vblanks a second. At F pictures a second there
   are F / 59.94 a vblank: when that is a whole number n (to 1%: 60, 120, 240, 360 Hz), the vblank's own
   picture and n - 1 more between it and the next, evenly; otherwise (144, 165 Hz) a picture every
   1 / (F / 59.94) of a vblank from vblank 0 on, wherever it falls. Each is the time the window shows it
   at (port/platform/video.c), and what the picture shows is the game's world at that time.
   With the screen's rate (PORT_FRAME_RATE_MONITOR), once the screen's refresh clock is sure of its
   refreshes (port_video_refresh_schedule), one picture each refresh instead, timed for it: the screen
   keeps its own time (169.83 Hz where the system names 170), and pictures on any other clock beat
   against it, a refresh with two steps of the world or none every few seconds, which vsync hides
   (each refresh takes one picture) and nothing else does.
   Unlocked (PORT_FRAME_RATE_UNLOCKED): as many a second as the renderer keeps up with, found as it plays
   (port/include/port/unlocked_rate.h: gs.c says at each game vblank whether the native GS was behind), up to the
   cap and UNLOCKED_MOST_A_VBLANK a vblank, and timed as a fixed rate is (the window shows each at its time; the
   swapchain mailbox, so that each refresh shows the newest, or immediate with --tearing). */
#define UNLOCKED_MOST_A_VBLANK 16 /* (gs.c's schedule's room) */
#define UNLOCKED_FLOOR_HZ 60
static UnlockedRate unlocked;
static int unlocked_on;            /* (the controller's thread) unlocked was on at the last vblank judged */
static int unlocked_cap_made = -1; /* the cap its bounds were made with */
static volatile int unlocked_hz;   /* its rate now, for any thread (port_frame_rate_hz) */

/* (the controller's thread) the rate's bounds: UNLOCKED_FLOOR_HZ to the cap, and the schedule's most */
static double unlocked_ceiling_hz(void) {
    double most = UNLOCKED_MOST_A_VBLANK * 60000.0 / 1001.0;
    int cap = port_options.frame_rate_cap;

    return cap > 0 && cap < most ? cap : most;
}

void port_interp_unlocked_vblank(uint64_t vblank, int behind) {
    if (port_options.frame_rate != PORT_FRAME_RATE_UNLOCKED) {
        unlocked_on = 0;
        return;
    }
    if (!unlocked_on) {
        /* (from the screen's rate: the least a window shows smoothly, and up from there) */
        int screen = port_video_refresh_rate();

        unlocked_rate_init(&unlocked, screen > UNLOCKED_FLOOR_HZ ? screen : 2 * UNLOCKED_FLOOR_HZ, UNLOCKED_FLOOR_HZ,
                           unlocked_ceiling_hz(), vblank);
        unlocked_cap_made = port_options.frame_rate_cap;
        unlocked_on = 1;
    } else if (unlocked_cap_made != port_options.frame_rate_cap) {
        unlocked_rate_bound(&unlocked, UNLOCKED_FLOOR_HZ, unlocked_ceiling_hz());
        unlocked_cap_made = port_options.frame_rate_cap;
    }
    unlocked_rate_vblank(&unlocked, vblank, behind);
    unlocked_rate_presented(&unlocked, vblank, port_video_pictures_per_second());
    __atomic_store_n(&unlocked_hz, (int)(unlocked.rate_hz + 0.5), __ATOMIC_RELAXED);
}

int port_interp_unlocked_at_floor(void) {
    return !unlocked_on || unlocked_rate_at_floor(&unlocked);
}

int port_frame_rate_hz(void) {
    int hz = port_options.frame_rate;

    if (hz == PORT_FRAME_RATE_MONITOR) {
        hz = port_video_refresh_rate();
        hz = hz > 0 ? hz : 60;
    } else if (hz == PORT_FRAME_RATE_UNLOCKED) {
        hz = __atomic_load_n(&unlocked_hz, __ATOMIC_RELAXED);
        hz = hz > 0 ? hz : port_video_refresh_rate() > UNLOCKED_FLOOR_HZ ? port_video_refresh_rate() : 2 * UNLOCKED_FLOOR_HZ;
    }
    return hz < 30 ? 30 : hz > 1000 ? 1000 : hz;
}

static double pictures_a_vblank(void) {
    double p, n;

    if (port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED) {
        return port_frame_rate_hz() * 1001.0 / 60000.0; /* (a fraction as found: no whole number to keep) */
    }
    p = port_frame_rate_hz() * 1001.0 / 60000.0;
    n = floor(p + 0.5);
    return n >= 1 && fabs(p - n) <= 0.01 * n ? n : p;
}

int port_interp_schedule(uint64_t vblank, double *phases, int max) {
    double p = pictures_a_vblank(), i;
    int n = 0;

    if (port_frame_rate_hz() <= 30 || p < 1.0 || max < 1) {
        phases[0] = 0;
        return 1;
    }
    if (port_options.frame_rate == PORT_FRAME_RATE_MONITOR && (n = port_video_refresh_schedule(vblank, phases, max)) >= 0) {
        return n;
    }
    n = 0;
    if (p == floor(p)) {
        for (n = 0; n < (int)p && n < max; n++) {
            phases[n] = n / p;
        }
        return n;
    }
    for (i = ceil((double)vblank * p - 1e-6); i < (double)(vblank + 1) * p - 1e-6 && n < max; i++) {
        double phase = i / p - (double)vblank;

        phases[n++] = phase < 1e-6 ? 0 : phase;
    }
    return n;
}

/* ---- The GIF packets of the last vblank --------------------------------------------------------
   Each as a record: its path (bits 0-7), 1 << 8 when VU1's 3D programs sent it, its size, and for
   those the run that sent it (port_vif1_run_serial) and what that run's batch was (port_vif1_run_key). */
typedef struct {
    uint32_t what;
    uint32_t bytes;
    uint32_t serial;
    uint32_t program;  /* RECORD_EFFECTS: sent by VU1 program 0x700 */
    uint64_t key;
} Record;
#define RECORD_3D 0x100
#define RECORD_EFFECTS 1
#define RECORD_SHADOW 2 /* sent by VU1 program 0x5A0 (the shadows: Entity_DrawPoolModels) */

static uint8_t *kept;
static size_t kept_size, kept_room;
static int kept_whole; /* the paths were between packets when keeping began */

void port_interp_reset(int whole) {
    kept_size = 0;
    kept_whole = whole;
}

void port_interp_keep(int path, const void *data, size_t bytes) {
    Record r;

    if (!kept_whole) {
        return;
    }
    if (kept_size + sizeof(r) + bytes > kept_room) {
        size_t room = (kept_size + sizeof(r) + bytes) * 2;
        uint8_t *grown = realloc(kept, room);

        if (grown == NULL) {
            kept_whole = 0;
            return;
        }
        kept = grown;
        kept_room = room;
    }
    r.what = (uint32_t)path;
    if (path == 1 && !(port_vu1_run_pc >= 0x276 && port_vu1_run_pc < 0x280)) {
        r.what |= RECORD_3D; /* not program 0x276, which passes the game's own GIF packets on */
    }
    r.bytes = (uint32_t)bytes;
    r.serial = port_vif1_run_serial;
    r.program = path == 1 && port_vu1_run_pc >= 0x700 && port_vu1_run_pc < 0x7D2 ? RECORD_EFFECTS : 0;
    r.program |= path == 1 && port_vu1_run_pc >= 0x5A0 && port_vu1_run_pc < 0x642 ? RECORD_SHADOW : 0;
    r.key = port_vif1_run_key;
    memcpy(kept + kept_size, &r, sizeof(r));
    memcpy(kept + kept_size + sizeof(r), data, bytes);
    kept_size += sizeof(r) + bytes;
}

/* ---- Cameras ------------------------------------------------------------------------------------ */
typedef struct {
    double r[3][3]; /* world to view: rotation (row, column) */
    double t[3];    /* and translation */
    double eye[3];  /* the camera's place in the world */
} Camera;

typedef struct {
    double sx, sy, cx, cy, cz, az; /* view to screen: X = sx x / w + cx, Y = sy y / w + cy, Z = cz + az / w */
} Screen;

static void camera_from_fields(Camera *c, const float *view) {
    int row, col;

    /* the fields are the columns' x, y and z: rotation columns 0-2, then the translation */
    for (col = 0; col < 3; col++) {
        for (row = 0; row < 3; row++) {
            c->r[row][col] = view[col * 3 + row];
        }
    }
    for (row = 0; row < 3; row++) {
        c->t[row] = view[9 + row];
    }
    for (col = 0; col < 3; col++) {
        c->eye[col] = -(c->r[0][col] * c->t[0] + c->r[1][col] * c->t[1] + c->r[2][col] * c->t[2]);
    }
}

static void quat_from(const double m[3][3], double q[4]) {
    double tr = m[0][0] + m[1][1] + m[2][2];

    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25 * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        double s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25 * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        double s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25 * s;
    }
}

static void matrix_from(const double q[4], double m[3][3]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];

    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
}

/* The camera a fraction f of the way from a to b; 0 when the two are a cut apart */
static int camera_between(const Camera *a, const Camera *b, double f, Camera *out) {
    double qa[4], qb[4], q[4], dot = 0, angle, move = 0;
    int i, row;

    quat_from(a->r, qa);
    quat_from(b->r, qb);
    for (i = 0; i < 4; i++) {
        dot += qa[i] * qb[i];
    }
    if (dot < 0) {
        for (i = 0; i < 4; i++) {
            qb[i] = -qb[i];
        }
        dot = -dot;
    }
    dot = dot > 1 ? 1 : dot;
    angle = acos(dot); /* half the turn */
    for (i = 0; i < 3; i++) {
        move += (b->eye[i] - a->eye[i]) * (b->eye[i] - a->eye[i]);
    }
    if (2 * angle > INTERP_MAX_TURN || move > INTERP_MAX_MOVE * INTERP_MAX_MOVE) {
        return 0;
    }
    if (angle < 1e-6) {
        for (i = 0; i < 4; i++) {
            q[i] = qa[i] + (qb[i] - qa[i]) * f;
        }
    } else {
        double wa = sin((1 - f) * angle) / sin(angle), wb = sin(f * angle) / sin(angle);

        for (i = 0; i < 4; i++) {
            q[i] = qa[i] * wa + qb[i] * wb;
        }
    }
    {
        double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);

        for (i = 0; i < 4; i++) {
            q[i] /= n;
        }
    }
    matrix_from(q, out->r);
    for (i = 0; i < 3; i++) {
        out->eye[i] = a->eye[i] + (b->eye[i] - a->eye[i]) * f;
    }
    for (row = 0; row < 3; row++) {
        out->t[row] = -(out->r[row][0] * out->eye[0] + out->r[row][1] * out->eye[1] + out->r[row][2] * out->eye[2]);
    }
    return 1;
}

/* view space of camera `from` to view space of camera `to`: rotation and translation */
typedef struct {
    double r[3][3], t[3];
} Move;

static void move_between(const Camera *from, const Camera *to, Move *m) {
    int i, j, k;

    /* to.r * from.r^T (p - from.t) + to.t */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            m->r[i][j] = 0;
            for (k = 0; k < 3; k++) {
                m->r[i][j] += to->r[i][k] * from->r[j][k];
            }
        }
    }
    for (i = 0; i < 3; i++) {
        m->t[i] = to->t[i] - (m->r[i][0] * from->t[0] + m->r[i][1] * from->t[1] + m->r[i][2] * from->t[2]);
    }
}

/* ---- Walking the packets -------------------------------------------------------------------------
   One walk reads the kept packets (SCAN), another sends a changed copy of them to the GS (REPLAY). */
enum { SCAN, REPLAY };
/* WORLD: the world (projection 0, modes 0 and 1); EYE: models before the eye (mode 2), in the camera's
   space; ARM: the arm and the held item, in the world but moving only as they move */
/* REFLECT: the reflection's view of the world (projection 1, mode 1, into a 256x256 target before the
   world), by the same camera as the world's (its own projection to the target) */
enum { SECTION_NONE, SECTION_WORLD, SECTION_OTHER, SECTION_EYE, SECTION_ARM, SECTION_REFLECT, SECTIONS };

/* ---- What moves by itself ------------------------------------------------------------------------
   Each VU1 run of a 3D program draws one batch of one model part. A run is known again in the next
   frame by its batch (the hash of what VIF1 unpacked for it: the part's vertices as they are in the
   model, before any matrix) and, among the runs of the frame before with the same batch and as many
   vertices, as the one whose middle is nearest, it being the nearest to that one in turn (the map's
   pieces repeat: the same batch is drawn in many places, and one that came into view is not taken for
   the one beside it). Its vertices, in the order the GS gets them, are kept for each frame as places
   in the world (before the eye, in the camera's space).
   - A paired run that moved (a vertex of it by more than a 500th of its distance from the eye) is drawn
     half way between its two places, seen from the camera half way: the enemies, the arm and the held
     item, the effects and the moving objects move at 60 too.
   - One that did not move is moved with the camera alone, as before; so is one with no pair (a model
     that came or went, a batch the CPU made this frame) in the world. Before the eye and in the arm,
     a run with no pair is left as it is.
   - Not paired: runs more than INTERP_MAX_MOVE apart (two things that share a batch), and runs whose
     half-way place comes much nearer the eye than both ends (the arm's swing turns round the shoulder,
     past the eye, in one frame: the straight line between cuts the corner).
   - A vertex's depth comes from Q (1/w), or from Z when its Q is not 1/w; the sphere-mapped second pass
     of program 0x280 (Q 1.0) goes over the first pass's points, and takes their depth (the arm's Z is
     pulled near, so its Z gives the wrong depth).
   - Vertices with ADC may have been clipped in one of the frames (object_between). */
/* Texture coordinates made from the screen: Q 1, and S, T near one projective map H of X, Y over the
   vertices without ADC, S = (h0 x + h1 y + h2) / (h6 x + h7 y + 1), T = (h3 x + h4 y + h5) / (the
   same) (least squares of the linear form; the residue within a 20th of their spread: a pool's surface,
   its reflection's target read where the pool is on the screen, with the ripple's small offsets). Such
   S and T go with the vertex's place on the screen, not with the surface: in a picture between
   S + H(x', y') - H(x, y). state 0 collecting, 1 a map, -1 not, 2 an item without a texture. */
typedef struct {
    int state, n;
    double ata[8][8], atb[8], btb; /* the normal equations of the linear form */
    double sum[4];                  /* s, s s, t, t t */
    double h[8];
} StPlane;

typedef struct {
    uint64_t key;
    uint32_t first;    /* its first vertex in pos */
    uint32_t count;    /* its vertices */
    int section;
    int placed;        /* it has a vertex with a place */
    double middle[3];  /* the mean of its vertices' places (those without ADC) */
    uint32_t prim;     /* PRIM at its first vertex */
    uint64_t tex;      /* and TEX0's texture and CLUT */
    int32_t pair;      /* the run of the frame before that is this one (objects_pair), -1: none */
    int sky;           /* of the sky (PORT_FRAME_INTERP_SKY: a map record that follows the player) */
    int native;        /* the PS2 path's packets of a native draw, which the GS leaves out (Walk's alt) */
    int shadow;        /* sent by program 0x5A0 (RECORD_SHADOW) */
} Run;

#define VERTEX_SURE 1  /* a vertex without ADC: never one the VU1 program clipped */
#define VERTEX_MAYBE 2 /* with ADC: maybe one the VU1 program clipped, its place past the guard band
                          wrapped round (object_between). It is kept out of its run's middle and of how
                          far the run moved. */

typedef struct {
    Run *runs;
    size_t nruns, runs_room;
    double (*pos)[3];
    uint8_t *valid;
    size_t npos, pos_room;
    uint32_t *by_key; /* the runs' numbers, in the order of their batches */
    size_t by_key_room;
    double sky_move[3]; /* the sky's move from the frame before (objects_sky_move), when sky_ok */
    int sky_ok;
} Objects;

static void objects_reset(Objects *o) {
    o->nruns = 0;
    o->npos = 0;
    o->sky_ok = 0;
}

static void objects_add_run(Objects *o, uint64_t key, int section, int sky, int native) {
    Run *r;

    if (o->nruns == o->runs_room) {
        o->runs_room = o->runs_room ? o->runs_room * 2 : 4096;
        o->runs = realloc(o->runs, o->runs_room * sizeof(*o->runs));
        if (o->runs == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    r = &o->runs[o->nruns++];
    r->key = key;
    r->first = (uint32_t)o->npos;
    r->count = 0;
    r->section = section;
    r->sky = sky && section == SECTION_WORLD;
    r->native = native;
    r->shadow = 0;
    r->prim = 0;
    r->tex = 0;
    r->pair = -1;
}

/* H at x, y (StPlane) */
static void st_map(const StPlane *r, double x, double y, double *s, double *t) {
    const double *h = r->h;
    double d = h[6] * x + h[7] * y + 1.0;

    *s = (h[0] * x + h[1] * y + h[2]) / d;
    *t = (h[3] * x + h[4] * y + h[5]) / d;
}

/* A vertex for a StPlane (SCAN): x, y in pixels, its S, T, Q */
static void st_plane_vertex(StPlane *r, double x, double y, const float stq[3]) {
    double u = stq[0], v = stq[1];
    double rows[2][8] = {{x, y, 1, 0, 0, 0, -u * x, -u * y}, {0, 0, 0, x, y, 1, -v * x, -v * y}};
    double b[2] = {u, v};
    int k, i, j;

    if (r->state != 0) {
        return;
    }
    if (stq[2] != 1.0f || !isfinite(u) || !isfinite(v)) {
        r->state = -1;
        return;
    }
    r->n++;
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 8; i++) {
            for (j = 0; j < 8; j++) {
                r->ata[i][j] += rows[k][i] * rows[k][j];
            }
            r->atb[i] += rows[k][i] * b[k];
        }
        r->btb += b[k] * b[k];
    }
    r->sum[0] += u;
    r->sum[1] += u * u;
    r->sum[2] += v;
    r->sum[3] += v * v;
}

/* The fit, once (the 8 x 8 normal equations by Gaussian elimination): state 1 when it holds */
static void st_finish(StPlane *r) {
    double a[8][9], fitted = 0, spread;
    int i, j, k;

    if (r->state != 0) {
        return;
    }
    r->state = -1;
    if (r->n < 8) {
        return;
    }
    for (i = 0; i < 8; i++) {
        memcpy(a[i], r->ata[i], sizeof(r->ata[i]));
        a[i][8] = r->atb[i];
    }
    for (i = 0; i < 8; i++) {
        int best = i;

        for (j = i + 1; j < 8; j++) {
            if (fabs(a[j][i]) > fabs(a[best][i])) {
                best = j;
            }
        }
        if (!(fabs(a[best][i]) > 1e-12)) {
            return;
        }
        if (best != i) {
            for (k = 0; k < 9; k++) {
                double tmp = a[i][k];

                a[i][k] = a[best][k];
                a[best][k] = tmp;
            }
        }
        for (j = 0; j < 8; j++) {
            if (j != i) {
                double f = a[j][i] / a[i][i];

                for (k = i; k < 9; k++) {
                    a[j][k] -= f * a[i][k];
                }
            }
        }
    }
    for (i = 0; i < 8; i++) {
        r->h[i] = a[i][8] / a[i][i];
        fitted += r->h[i] * r->atb[i];
    }
    /* the residue's sum of squares (b.b - h . A^T b), against the values' spread about their means */
    spread = (r->sum[1] - r->sum[0] * r->sum[0] / r->n) + (r->sum[3] - r->sum[2] * r->sum[2] / r->n);
    if (!(spread > 1e-12) || r->btb - fitted > spread * 0.0025 + 1e-12) {
        return;
    }
    r->state = 1;
}

static int st_screen(StPlane *r) {
    st_finish(r);
    return r->state == 1;
}


static void objects_add_vertex(Objects *o, const double place[3], int valid) {
    if (o->npos == o->pos_room) {
        o->pos_room = o->pos_room ? o->pos_room * 2 : 65536;
        o->pos = realloc(o->pos, o->pos_room * sizeof(*o->pos));
        o->valid = realloc(o->valid, o->pos_room);
        if (o->pos == NULL || o->valid == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    memcpy(o->pos[o->npos], place, sizeof(o->pos[0]));
    o->valid[o->npos] = (uint8_t)valid;
    o->npos++;
    o->runs[o->nruns - 1].count++;
}

/* After the walk: each run's middle, and the runs in the order of their batches */
static const Objects *sorting;

static int by_key(const void *a, const void *b) {
    uint64_t x = sorting->runs[*(const uint32_t *)a].key, y = sorting->runs[*(const uint32_t *)b].key;

    return x < y ? -1 : x > y ? 1 : (int)(*(const uint32_t *)a - *(const uint32_t *)b);
}

static void objects_finish(Objects *o) {
    size_t i, k;

    for (i = 0; i < o->nruns; i++) {
        Run *r = &o->runs[i];
        double sum[3] = {0, 0, 0};
        uint32_t n = 0;

        for (k = r->first; k < r->first + r->count; k++) {
            if (o->valid[k] == VERTEX_SURE) {
                sum[0] += o->pos[k][0];
                sum[1] += o->pos[k][1];
                sum[2] += o->pos[k][2];
                n++;
            }
        }
        r->placed = n != 0;
        for (k = 0; k < 3; k++) {
            r->middle[k] = n ? sum[k] / n : 0;
        }
    }
    if (o->by_key_room < o->nruns) {
        o->by_key_room = o->nruns * 2;
        free(o->by_key);
        o->by_key = malloc(o->by_key_room * sizeof(*o->by_key));
        if (o->by_key == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    for (i = 0; i < o->nruns; i++) {
        o->by_key[i] = (uint32_t)i;
    }
    sorting = o;
    qsort(o->by_key, o->nruns, sizeof(*o->by_key), by_key);
}

/* The run of the frame before that is this one (cur, of this frame), or NULL */
static const Run *objects_find(const Objects *o, const Run *cur) {
    size_t lo = 0, hi, i;
    const Run *best = NULL;
    double best_d2 = INTERP_MAX_MOVE * INTERP_MAX_MOVE;

    if (o == NULL || !cur->placed) {
        return NULL;
    }
    hi = o->nruns;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;

        if (o->runs[o->by_key[mid]].key < cur->key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (i = lo; i < o->nruns && o->runs[o->by_key[i]].key == cur->key; i++) {
        const Run *r = &o->runs[o->by_key[i]];
        double d2 = 0;
        int k;

        if (r->count != cur->count || !r->placed || r->section != cur->section || r->native) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            d2 += (r->middle[k] - cur->middle[k]) * (r->middle[k] - cur->middle[k]);
        }
        if (d2 < best_d2) {
            best_d2 = d2;
            best = r;
        }
    }
    return best;
}

/* ---- What the frame writes and reads ------------------------------------------------------------
   In the order of the packets, the VRAM pages each register write says will be read (a texture: twice
   its first level when it has mipmaps; a CLUT) or written (a draw's FRAME and ZBUF, to its scissor's
   bottom; an upload or a local copy, to the rows it reaches): worked out generously, from the first
   block. A 2D item drawn differently into a buffer the frame reads before it draws it needs the frame
   drawn again exact after the pictures between (flat_pair). */
enum { EV_UPLOAD, EV_READ, EV_WRITE };

typedef struct {
    uint32_t first, last; /* pages (8 KB) */
    uint32_t kind;
} Event;

typedef struct {
    Event *ev;
    size_t nev, ev_room;
    /* the copies of the frame drawn into another buffer (GsDraw_CopyFrame: the haze, the wobble, the
       screen's picture on a model), by their events */
    uint32_t *copies;
    size_t ncopies, copies_room;
} Uploads;

static void uploads_reset(Uploads *u) {
    u->nev = 0;
    u->ncopies = 0;
}

/* The pages a buffer covers: from block bp, bw * 64 pixels wide, rows rows of psm */
static void pages_of(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t rows, uint32_t *first, uint32_t *last) {
    uint32_t bits = psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A ? 16
                    : psm == 0x13                                             ? 8
                    : psm == 0x14                                             ? 4
                                                                              : 32;
    uint64_t bytes = (uint64_t)(bw ? bw : 1) * 64 * rows * bits / 8;

    *first = bp / 32;
    *last = (uint32_t)((bp * 256ull + (bytes ? bytes : 1) - 1) / 8192) + (bp % 32 ? 1 : 0);
}

static void uploads_event(Uploads *u, uint32_t kind, uint32_t first, uint32_t last) {
    Event *e;

    if (u == NULL) {
        return;
    }
    if (u->nev == u->ev_room) {
        u->ev_room = u->ev_room ? u->ev_room * 2 : 1024;
        u->ev = realloc(u->ev, u->ev_room * sizeof(*u->ev));
        if (u->ev == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    e = &u->ev[u->nev++];
    e->first = first;
    e->last = last;
    e->kind = kind;
}

/* ---- The 2D: what the CPU draws anew each frame ---------------------------------------------------
   Everything that is not a run of VU1's 3D programs (the HUD, the screen passes and fades, the
   scrolling textures' copies, effects the CPU makes) is sent by the game as GIF packets of its own.
   Each GIF tag with vertices is an item; a frame's items are paired with the frame before's in their
   order, by what they draw with (the primitive, the texture and CLUT, the buffer drawn into, the vertex
   count: the buffer drawn and the one shown, which swap each frame, by their part). A pair is taken
   when it is a likely one: no vertex more than 128 pixels from its place, and the texture coordinates
   the same, or all moved by one small step (a scrolling texture: within a quarter of the item's own
   width of texture, after the texture's wrap), or changed only where the vertices moved (a gauge that
   grows). A glyph changing in place, or anything else, is drawn as the frame has it. A picture between
   draws a paired item with its places, colours (fades) and texture coordinates where they are at that
   time. Images the CPU makes anew (the orb's swirl, a flipbook's next picture) cannot be: the texture
   itself changed. */
typedef struct {
    uint32_t prim;     /* PRIM (type and switches) */
    uint64_t tex;      /* TEX0's texture and CLUT */
    uint32_t fbp;      /* FRAME's buffer */
    uint32_t first;    /* its first vertex in verts */
    uint32_t count;
    int32_t pair;      /* the item of the frame before (-1: none, or nothing to do) */
    float wrap_s, wrap_t; /* added to the frame before's texture coordinates (a scroll past the wrap) */
    /* A mesh of the world the CPU projected itself (item_mesh): triangles whose Z is not one, their
       texture coordinates made from the screen (st). Such an item is moved with the camera as
       the world is, not t of the way from the frame before's: the pools' reflecting surfaces (PATH2,
       Q 1, S and T from the reflection's view) are drawn over their basin with a Z test, and paired
       2D-wise their Z stayed the frame's while the basin's moved: the surface hidden, the basin's dark
       floor showing, while the camera turned (October 2026). */
    uint32_t zlo, zhi;
    StPlane st;
} Item;

typedef struct {
    int32_t x, y;      /* (1/16 pixel) */
    uint8_t rgba[4];
    float s, t;        /* ST, or UV (in texels) */
    uint8_t has_rgba, coords; /* coords: 0 none, 1 ST, 2 UV */
} FlatVertex;

typedef struct {
    Item *items;
    size_t nitems, items_room;
    FlatVertex *verts;
    size_t nverts, verts_room;
    uint32_t draw_fbp, shown_fbp; /* the frame's buffers */
    int restore;                   /* a paired item draws into a buffer read before it is drawn in the frame */
    /* by copy of the frame (Uploads): COPY_BETWEEN, made again in the pictures between, from their own
       frame: a copy the frame itself reads after it, and not before (the haze, the wobble). COPY_RESTORE,
       made again from the game's frame by the exact restore after them (port_interp_end_vblank): a copy the
       frame does not read, over pages it uploaded or drew into before the copy. The pictures between send
       those uploads again and not the copy, so they would leave the textures where the game's frame left
       its picture: the map change copies its last frame to 0x2BC0 (MapChange_CaptureFrame) for the frames
       after to fade out, and at 60-240 they faded out the textures uploaded there (the wand's passage,
       October 2026). 0: neither (nothing written there before it, or the frame drawn into after it). */
    uint8_t *copy_ok;
    size_t ncopies, copy_room;
    int restore_copy; /* a copy is COPY_RESTORE */
} Flat;

static void flat_reset(Flat *f) {
    f->nitems = 0;
    f->nverts = 0;
    f->restore = 0;
    f->restore_copy = 0;
}

static Item *flat_add_item(Flat *f, uint32_t prim, uint64_t tex0, uint32_t fbp) {
    Item *it;

    if (f->nitems == f->items_room) {
        f->items_room = f->items_room ? f->items_room * 2 : 1024;
        f->items = realloc(f->items, f->items_room * sizeof(*f->items));
        if (f->items == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    it = &f->items[f->nitems++];
    it->prim = prim;
    /* TBP0, TBW, PSM, TW, TH, TCC, TFX; CBP, CPSM, CSM, CSA (not CLD) */
    it->tex = (prim & 0x10) ? tex0 & ~((uint64_t)7 << 61) : 0;
    it->fbp = fbp;
    it->first = (uint32_t)f->nverts;
    it->count = 0;
    it->pair = -1;
    it->wrap_s = it->wrap_t = 0;
    it->zlo = 0xFFFFFFFFu;
    it->zhi = 0;
    memset(&it->st, 0, sizeof(it->st));
    it->st.state = (prim & 0x10) ? 0 : 2; /* (2: no texture) */
    return it;
}

static FlatVertex *flat_add_vertex(Flat *f) {
    if (f->nverts == f->verts_room) {
        f->verts_room = f->verts_room ? f->verts_room * 2 : 8192;
        f->verts = realloc(f->verts, f->verts_room * sizeof(*f->verts));
        if (f->verts == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    f->items[f->nitems - 1].count++;
    return &f->verts[f->nverts++];
}

/* What an item draws with, the swapping buffers by their part */
static int items_alike(const Flat *fa, const Item *a, const Flat *fb, const Item *b) {
    uint32_t pa = a->fbp == fa->draw_fbp ? 0x200 : a->fbp == fa->shown_fbp ? 0x201 : a->fbp;
    uint32_t pb = b->fbp == fb->draw_fbp ? 0x200 : b->fbp == fb->shown_fbp ? 0x201 : b->fbp;
    uint64_t ta = a->tex, tb = b->tex;
    uint32_t ba = (uint32_t)(ta & 0x3FFF), bb = (uint32_t)(tb & 0x3FFF);

    if (a->prim & 0x10) {
        ba = ba == fa->draw_fbp * 32 ? 0x4000 : ba == fa->shown_fbp * 32 ? 0x4001 : ba;
    }
    if (b->prim & 0x10) {
        bb = bb == fb->draw_fbp * 32 ? 0x4000 : bb == fb->shown_fbp * 32 ? 0x4001 : bb;
    }
    return a->prim == b->prim && a->count == b->count && pa == pb && ba == bb &&
           (ta & ~(uint64_t)0x3FFF) == (tb & ~(uint64_t)0x3FFF);
}

/* An item of this frame (b) and one of the frame before (a) are a likely pair, and something of it
   changed: 1, with the wrap to add to a's texture coordinates */
static int items_pair(const Flat *fa, const Item *a, const Flat *fb, const Item *b, float *wrap_s, float *wrap_t) {
    uint32_t k;
    int changed = 0, uniform = 1, moved_only = 1;
    float ds0 = 0, dt0 = 0, smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f, dsmost = 0, dtmost = 0;
    /* the texture's size: 1.0 in ST, its texels in UV */
    float tw = (float)(1u << ((b->tex >> 26) & 0xF)), th = (float)(1u << ((b->tex >> 30) & 0xF));

    *wrap_s = *wrap_t = 0;
    for (k = 0; k < b->count; k++) {
        const FlatVertex *va = &fa->verts[a->first + k], *vb = &fb->verts[b->first + k];
        int32_t dx = vb->x - va->x, dy = vb->y - va->y;

        if (dx > 2048 || dx < -2048 || dy > 2048 || dy < -2048) {
            return 0;
        }
        if (va->coords != vb->coords || va->has_rgba != vb->has_rgba) {
            return 0;
        }
        changed |= dx != 0 || dy != 0 || (vb->has_rgba && memcmp(va->rgba, vb->rgba, 4) != 0);
        if (vb->coords) {
            float ds = vb->s - va->s, dt = vb->t - va->t;
            float eps = vb->coords == 2 ? 0.07f : 1e-4f;

            if (k == 0) {
                ds0 = ds;
                dt0 = dt;
            } else if (fabsf(ds - ds0) > eps || fabsf(dt - dt0) > eps) {
                uniform = 0;
            }
            if ((fabsf(ds) > eps || fabsf(dt) > eps) && dx == 0 && dy == 0) {
                moved_only = 0;
            }
            dsmost = fabsf(ds) > dsmost ? fabsf(ds) : dsmost;
            dtmost = fabsf(dt) > dtmost ? fabsf(dt) : dtmost;
            smin = vb->s < smin ? vb->s : smin;
            smax = vb->s > smax ? vb->s : smax;
            tmin = vb->t < tmin ? vb->t : tmin;
            tmax = vb->t > tmax ? vb->t : tmax;
        }
    }
    if (b->count > 0 && fb->verts[b->first].coords) {
        float eps = fb->verts[b->first].coords == 2 ? 0.07f : 1e-4f;
        float size_s = fb->verts[b->first].coords == 2 ? tw : 1.0f, size_t_ = fb->verts[b->first].coords == 2 ? th : 1.0f;

        if (uniform && (fabsf(ds0) > eps || fabsf(dt0) > eps)) {
            /* all moved by one step: a scroll, the shortest way round the texture's wrap */
            float s = ds0 - size_s * floorf(ds0 / size_s + 0.5f), t = dt0 - size_t_ * floorf(dt0 / size_t_ + 0.5f);

            if (fabsf(s) > 0.25f * (smax - smin) + eps || fabsf(t) > 0.25f * (tmax - tmin) + eps) {
                return 0; /* (a glyph, or a picture, changed) */
            }
            *wrap_s = ds0 - s;
            *wrap_t = dt0 - t;
            changed = 1;
        } else if (!uniform) {
            /* changed only where the vertices moved (a gauge), or, not a sprite, each vertex's by a little
               of the item's width of texture (a flame's or the water's surface flowing); not a sprite's
               texture changed in place (a glyph) */
            if (!moved_only && ((b->prim & 7) == 6 || b->count < 3 || dsmost > 0.25f * (smax - smin) + eps ||
                                dtmost > 0.25f * (tmax - tmin) + eps)) {
                return 0;
            }
            changed = 1;
        }
    }
    return changed;
}

/* The frame reads the buffer at fbp before it draws into it, or uploads into it */
static int read_first(const Uploads *u, uint32_t fbp) {
    uint32_t first, last;
    size_t e;

    pages_of(fbp * 32, 1, 0, 1, &first, &last);
    for (e = 0; e < u->nev; e++) {
        const Event *ev = &u->ev[e];

        if (ev->kind == EV_READ && ev->first <= first && ev->last >= first) {
            return 1;
        }
        if (ev->kind != EV_READ && ev->first == first) {
            return 0; /* (drawn, or uploaded, before it is read) */
        }
    }
    return 0;
}

enum { COPY_BETWEEN = 1, COPY_RESTORE = 2 };

/* Which copies of the frame the pictures between make again, and which the exact restore (Flat's copy_ok) */
static void flat_copies(Flat *f, const Uploads *u) {
    uint32_t frame_first, frame_last;
    size_t c, e;

    if (f->copy_room < u->ncopies) {
        f->copy_room = u->ncopies * 2;
        f->copy_ok = realloc(f->copy_ok, f->copy_room);
        if (f->copy_ok == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    f->ncopies = u->ncopies;
    pages_of(f->draw_fbp * 32, 1, 0, 1, &frame_first, &frame_last);
    for (c = 0; c < u->ncopies; c++) {
        const Event *copy = &u->ev[u->copies[c]];
        int read_after = 0, read_before = 0, written_before = 0, frame_after = 0;
        /* (pages of the copy's that the frame has uploaded or drawn into so far: a read of those before
           the copy reads what the frame put there, not the copy of the frame before. The water's wobble
           copies the frame over VRAM the frame's textures came through earlier in it, read as textures
           there: counted as reads of the copy, the copy was not made again, and the pictures between
           drew the wobble's strips from those textures: noise and bands of light over the screen.) */
        uint8_t written[512];

        memset(written, 0, sizeof(written));
        for (e = 0; e < u->nev; e++) {
            const Event *ev = &u->ev[e];
            uint32_t p, lo, hi;

            if (e > u->copies[c] && ev->kind == EV_WRITE && ev->first == frame_first) {
                frame_after = 1; /* (the frame drawn into after the copy: its end is not what was copied) */
            }
            if (ev->first > copy->last || ev->last < copy->first) {
                continue;
            }
            lo = ev->first > copy->first ? ev->first : copy->first;
            hi = ev->last < copy->last ? ev->last : copy->last;
            if (e >= u->copies[c]) {
                read_after |= ev->kind == EV_READ;
                continue;
            }
            for (p = lo; p <= hi; p++) {
                if (ev->kind != EV_READ) {
                    written[p % 512] = 1;
                    written_before = 1;
                } else if (!written[p % 512]) {
                    read_before = 1;
                }
            }
        }
        f->copy_ok[c] = read_after && !read_before ? COPY_BETWEEN
                        : !read_after && written_before && !frame_after ? COPY_RESTORE : 0;
        f->restore_copy |= f->copy_ok[c] == COPY_RESTORE;
    }
}

/* Pairs this frame's items (f) with the frame before's (o): in order, each with the first alike among
   the next few of the frame before not taken yet */
static void flat_pair(Flat *f, const Flat *o, const Uploads *u) {
    size_t i, at = 0;

    for (i = 0; i < f->nitems; i++) {
        Item *b = &f->items[i];
        size_t k;

        b->pair = -1;
        if (o == NULL) {
            continue;
        }
        for (k = at; k < o->nitems && k < at + 48; k++) {
            if (items_alike(o, &o->items[k], f, b)) {
                if (items_pair(o, &o->items[k], f, b, &b->wrap_s, &b->wrap_t)) {
                    b->pair = (int32_t)k;
                }
                at = k + 1;
                break;
            }
        }
        if (b->pair >= 0 && b->fbp != f->draw_fbp && u != NULL && read_first(u, b->fbp)) {
            /* drawn into another buffer (a scrolling texture's) that the frame reads before it draws into
               it: the pictures between leave it as the game's frame has it (port_interp_end_vblank) */
            f->restore = 1;
        }
    }
}

/* Each run of this frame (o) with the one of the frame before (prev) that is it: by its batch, the nearest
   with the same, it the nearest to that one in turn (objects_find); then, among the runs neither pairs
   that way (what the CPU makes anew each frame: an effect's sprites, a model it moves vertex by vertex,
   whose batch is new every frame), in their order, each with the first of the frame before's within
   the next few that is drawn with the same primitive and texture, as many vertices, in the same
   section, its middle within INTERP_MAX_MOVE. run_motion then takes the pair only if no vertex moved
   more than that, and the place between does not cut past the eye. */
static uint64_t stat_runs, stat_runs_key, stat_runs_order;

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* The sky's move from the frame before (o->sky_move): the median, each way, of its paired runs' mean
   moves (their vertices without ADC in both frames). The sky (the map records that follow the player:
   map 000's dome, in strips a few thousand to twenty thousand away) goes with the player, and each
   picture between moves all of it by this one move (object_between), paired or not, so its strips stay
   together. Deciding it run by run, by whether each strip moved with the camera, came apart whenever
   the strips disagreed: the eye rises and falls with the step while the dome goes along the ground,
   and the places of the far strips are less sure, so some passed and others did not when the player
   set off, stopped, ran or turned while walking: thin bright and dark lines along the strips' edges,
   for a frame. (One place's error, a 10,000th of its distance, is averaged out over each strip's
   vertices and then left out by the median.) */
static void objects_sky_move(Objects *o, const Objects *prev) {
    static double *moves;
    static size_t room;
    size_t i, n = 0;
    int c;

    o->sky_ok = 0;
    for (i = 0; i < o->nruns; i++) {
        const Run *r = &o->runs[i], *a;
        double sum[3] = {0, 0, 0};
        uint32_t k, count = 0;

        if (!r->sky || r->pair < 0) {
            continue;
        }
        a = &prev->runs[r->pair];
        for (k = 0; k < r->count && k < a->count; k++) {
            if (o->valid[r->first + k] != VERTEX_SURE || prev->valid[a->first + k] != VERTEX_SURE) {
                continue;
            }
            for (c = 0; c < 3; c++) {
                sum[c] += o->pos[r->first + k][c] - prev->pos[a->first + k][c];
            }
            count++;
        }
        if (count == 0) {
            continue;
        }
        if (room < (n + 1) * 3) {
            room = room ? room * 2 : 3 * 256;
            moves = realloc(moves, room * sizeof(*moves));
            if (moves == NULL) {
                PORT_FATAL("out of memory");
            }
        }
        for (c = 0; c < 3; c++) {
            moves[n * 3 + c] = sum[c] / count;
        }
        n++;
    }
    if (n == 0) {
        return;
    }
    for (c = 0; c < 3; c++) {
        double *column = malloc(n * sizeof(*column));

        if (column == NULL) {
            PORT_FATAL("out of memory");
        }
        for (i = 0; i < n; i++) {
            column[i] = moves[i * 3 + c];
        }
        qsort(column, n, sizeof(*column), compare_double);
        o->sky_move[c] = n % 2 ? column[n / 2] : (column[n / 2 - 1] + column[n / 2]) / 2;
        free(column);
    }
    o->sky_ok = o->sky_move[0] * o->sky_move[0] + o->sky_move[1] * o->sky_move[1] + o->sky_move[2] * o->sky_move[2] <
                INTERP_MAX_MOVE * INTERP_MAX_MOVE;
}

/* (objects_pair's second way) a run of the frame before that may be r: drawn alike, in the same section */
static int run_alike(const Run *a, const Run *r) {
    return a->placed && !a->native && !r->native && a->section == r->section && a->count == r->count && a->prim == r->prim &&
           a->tex == r->tex;
}

static double middle_d2(const Run *a, const Run *r) {
    double d2 = 0;
    int c;

    for (c = 0; c < 3; c++) {
        d2 += (a->middle[c] - r->middle[c]) * (a->middle[c] - r->middle[c]);
    }
    return d2;
}

static void objects_pair(Objects *o, const Objects *prev, const double eye[3]) {
    size_t i, at = 0, k;
    uint8_t *taken;

    for (i = 0; i < o->nruns; i++) {
        o->runs[i].pair = -1;
    }
    if (prev == NULL || prev->nruns == 0) {
        return;
    }
    taken = calloc(prev->nruns, 1);
    if (taken == NULL) {
        PORT_FATAL("out of memory");
    }
    for (i = 0; i < o->nruns; i++) {
        Run *r = &o->runs[i];
        const Run *a;

        if (r->native) {
            continue; /* (native draw pairs its draws by their identity: gsn_native_draw.inc) */
        }
        a = objects_find(prev, r);
        stat_runs += r->placed;
        if (a != NULL && objects_find(o, a) == r) {
            r->pair = (int32_t)(a - prev->runs);
            taken[r->pair] = 1;
            stat_runs_key++;
        }
    }
    for (i = 0; i < o->nruns; i++) {
        Run *r = &o->runs[i];

        if (r->pair >= 0 || !r->placed || r->native || r->section == SECTION_OTHER || r->section == SECTION_REFLECT) {
            continue;
        }
        /* the nearest look-alike within the window, and this run the nearest to it in turn among this
           frame's look-alikes around it, paired or not. The near cut's pieces of a map record
           (ModelScreen_CutClippedTriangles: 16 smaller triangles for each with a corner out of the GS's
           range, made anew each frame, so their batches are new and they pair here; many alike side by
           side) were each taken for the first look-alike, its neighbour a hundred or more units away
           when one of them came or went, and the pictures between drew them flying in from the screen's
           edges: the dark shards in the doorway to the shop on map 000, hosted and modern alike (2026-10) */
        {
            size_t best = SIZE_MAX, first = SIZE_MAX, j;
            double best_d2 = INTERP_MAX_MOVE * INTERP_MAX_MOVE;

            for (k = at; k < prev->nruns && k < at + 32; k++) {
                const Run *a = &prev->runs[k];
                double d2;

                if (taken[k] || !run_alike(a, r)) {
                    continue;
                }
                if (first == SIZE_MAX) {
                    first = k;
                }
                d2 = middle_d2(a, r);
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = k;
                }
            }
            if (best == SIZE_MAX) {
                continue;
            }
            if (r->section == SECTION_WORLD) {
                /* in the world, not more than a quarter of its distance from the eye: the cut's pieces are a
                   hundred or more apart at two hundred from the eye, and a pair that far is a neighbour whose
                   own pair is gone (no look-alike nearer to it is left to tell) */
                double e2 = 0;
                int c;

                for (c = 0; c < 3; c++) {
                    e2 += (r->middle[c] - eye[c]) * (r->middle[c] - eye[c]);
                }
                if (best_d2 > e2 / 16) {
                    continue;
                }
            }
            for (j = i > 32 ? i - 32 : 0; j < o->nruns && j < i + 33; j++) {
                const Run *s = &o->runs[j];

                if (j != i && s->placed && run_alike(&prev->runs[best], s) && middle_d2(&prev->runs[best], s) < best_d2) {
                    break;
                }
            }
            if (j < o->nruns && j < i + 33) {
                continue; /* (another run of this frame is nearer to it, paired or not: it is that one's) */
            }
            r->pair = (int32_t)best;
            taken[best] = 1;
            if (best == first) {
                at = best + 1;
            }
            stat_runs_order++;
        }
    }
    free(taken);
    objects_sky_move(o, prev);
}

typedef struct {
    uint32_t left, flg, nreg, reg, dwords;
    uint64_t regs;
    uint32_t prim;
    uint8_t *st;      /* PACKED: the ST qword before the next vertex, in this record */
    uint8_t *rgbaq;   /* PACKED: the RGBAQ qword before the next vertex, in this record */
    uint8_t *uv;      /* PACKED: the UV qword before the next vertex, in this record */
    int adc_left;     /* vertices still to drop after one the new camera cannot see */
    uint32_t win[2];  /* GPU: the last two kicks of this path with a place (GSN_IM_NONE: none) */
    int item_open;    /* this tag's 2D vertices are an item (the first has come) */
    int dropping;     /* REPLAY: this IMAGE tag's data is an upload left out */
} GifWalk;

typedef struct {
    int mode;
    GifWalk paths[4];
    /* the marks */
    float fields[19];
    int section;
    int world_seen;   /* a world mark has been seen */
    int sky;          /* the runs from here are the sky's (PORT_FRAME_INTERP_SKY) */
    int alt;          /* the runs from here are the PS2 path's packets of native draws, which the GS leaves out
                         (--native-draw-compare native: between RD_ND_ALT_BEGIN and _END); not paired */
    Camera camera;    /* SCAN: the first world mark's camera */
    Screen screen;
    Screen screen_reflect; /* the reflection's projection (SECTION_REFLECT) */
    float reflect_view[12];
    uint32_t reflect_fbp;  /* the reflection's target */
    int have_reflect;      /* there is one, by the world's camera */
    uint32_t draw_fbp; /* SCAN: FRAME_1's FBP at the first world mark: the buffer drawn */
    /* the GS registers as the game wrote them */
    uint64_t frame[2], tex0[2], bitbltbuf, prmodecont, prmode;
    int frame_set[2], tex0_set[2], bitbltbuf_set;
    uint64_t trxpos, trxreg, scissor[2], zbuf[2], tex1[2];
    /* the pages read and written (SCAN), and the uploads */
    Uploads *uploads;    /* SCAN: this frame's events */
    uint32_t upload;     /* the next upload's number in the frame */
    uint32_t copy;       /* the next copy of the frame's */
    const Flat *copies;  /* REPLAY: the frame's, for its copies (NULL: none made) */
    int ask_uploads;     /* REPLAY: the native GS has the frame's uploads: ask for them again */
    int skip_image;      /* REPLAY: the next IMAGE data is an upload asked for again: left out */
    uint64_t upload_bytes_left_out;
    /* the 2D (Flat): SCAN keeps this frame's items; REPLAY moves the paired ones */
    Flat *flat;          /* SCAN: this frame's; REPLAY: the pending frame's (NULL: none moved) */
    const Flat *flat_prev; /* REPLAY: the frame's before it */
    long item;           /* the item of the last 2D vertex, -1 before the first */
    uint32_t item_vertex; /* the next vertex's number in it */
    uint64_t items_moved;
    /* the runs: SCAN adds them to objects; both walks number them the same way */
    Objects *objects;    /* SCAN: this frame's (or NULL); REPLAY: the pending frame's */
    const Objects *prev; /* REPLAY: the frame's before it, or NULL */
    uint32_t serial;     /* the run of the last 3D record */
    long run;            /* its number in this walk, -1 before the first */
    uint32_t run_vertex; /* the next vertex's number in the run */
    const Run *run_prev; /* REPLAY: the same run in the frame before, when there is one */
    int run_moving;      /* REPLAY: it moved */
    /* the run's points so far with a Q that is 1/w: X and Y, Z, and the depth (vertex_view) */
#define SEEN 128
    uint64_t seen_xy[SEEN];
    double seen_z[SEEN], seen_depth[SEEN];
    int nseen;
    int effects;         /* the record is program 0x700's (RECORD_EFFECTS): its Q is not 1/w (vertex_view) */
    double run_most;     /* REPLAY: the most a vertex of it without ADC moved */
    double eye_move[3];  /* REPLAY: how far the camera moved from the frame before to this one */
    /* REPLAY */
    uint32_t shown_fbp; /* the buffer the replay draws into, in place of draw_fbp */
    double t;           /* how far between the frame shown (0) and the one drawn (1) the picture is */
    int exact;          /* the frame as it was, for the buffers other than the frame (port_interp_end_vblank) */
    int prev_frame_kept; /* the native GS keeps the frame shown before the pictures between (take_snapshot) */
    int record_reads_prev; /* REPLAY: this record reads the frame shown as a texture (the native GS: read the copy) */
    int native_reads_shown; /* SCAN: a native draw's screen pass reads the frame shown (RD_ND_READS_SHOWN) */
    Move move;          /* the frame's view to the camera between */
    Camera mid;         /* the camera between */
    uint64_t moved, dropped, objects_moved[SECTIONS]; /* vertices moved, dropped (ADC), moved as their model moved (by section) */
    /* REPLAY on the GPU ("On the GPU"): the vertices are left where the game put them, and each vertex kick
       (in the GS's order) gets how the pictures between move it */
    int gpu;
    uint32_t kick;       /* the vertex kicks so far (XYZ2, XYZF2, XYZ3, XYZF3: PACKED, A+D, REGLIST) */
} Walk;

/* A primitive draws into the frame: its context's FRAME is the buffer drawn (PRIM's CTXT, or PRMODE's
   when PRMODECONT says so). Only those vertices are moved: whatever else the world draws into (none
   seen so far) is drawn again as it was. */
static int draws_into(const Walk *w, uint32_t prim, uint32_t fbp) {
    int n = (w->prmodecont & 1) ? (int)((prim >> 9) & 1) : (int)((w->prmode >> 9) & 1);

    return w->frame_set[n] && (w->frame[n] & 0x1FF) == fbp;
}

static int draws_frame(const Walk *w, uint32_t prim) {
    return draws_into(w, prim, w->draw_fbp);
}

/* The exact restore (port_interp_end_vblank) is for the buffers other than the frame: what it would draw
   into the frame goes into the buffer shown, which the next frame clears, and is never shown (the vblanks
   until the next frame show its own buffer, untouched). So nothing is drawn into the frame: a vertex kick
   of a primitive into it becomes ADC (no drawing kick), 3D and 2D alike, and a native draw into it is left
   out (exact_frame). Its 3D was meant to be left out before, but the bit set was the qword's 127th, not
   ADC (bit 111, 47 of the upper half): every restore drew the whole frame again for two 128 x 128 scrolling
   textures, with native draw the world too (its draws are not VU1's vertices), and the MP drain's and the
   water's wobble read the copy of the frame the restore does not make, from VRAM (1024 x 512 decoded, the
   frame written back): 15-60 ms a game frame at 240 (the Queen's Palace with its purple wobble on: the game
   at 14 frames a second). KF4_INTERP_RESTORE_FRAME=1: the restore draws into the frame as before. */
static int exact_frame_drawn(void) {
    static int drawn = -1;

    if (drawn < 0) {
        const char *v = getenv("KF4_INTERP_RESTORE_FRAME");
        drawn = v != NULL && atoi(v) == 1;
    }
    return drawn;
}

static void exact_kick(const Walk *w, const GifWalk *p, uint8_t *q) {
    if (!exact_frame_drawn() && draws_frame(w, p->prim)) {
        q[13] |= 0x80; /* (ADC: the qword's bit 111) */
    }
}

/* (the exact restore) A native draw goes into the frame: the game sets both contexts' FRAME alike
   (GsDraw_SetDrawTarget), the reflection's view too */
static int exact_frame(const Walk *w) {
    return !exact_frame_drawn() && w->frame_set[0] && (w->frame[0] & 0x1FF) == w->draw_fbp && (!w->frame_set[1] || (w->frame[1] & 0x1FF) == w->draw_fbp);
}

static void mark(Walk *w, uint64_t value) {
    uint32_t index = (uint32_t)(value >> 32) & 0xFF;
    uint32_t bits = (uint32_t)value;


    if (index >= PORT_FRAME_INTERP_VIEW && index < PORT_FRAME_INTERP_SCREEN + 6) {
        memcpy(&w->fields[index], &bits, 4);
        return;
    }
    if (index == PORT_FRAME_INTERP_SKY) {
        w->sky = bits != 0;
        return;
    }
    if (index != PORT_FRAME_INTERP_HEADER) {
        return;
    }
    if (bits == PORT_FRAME_INTERP_END) {
        w->section = SECTION_NONE;
        w->sky = 0;
        return;
    }
    if (bits == PORT_FRAME_INTERP_ARM) {
        w->section = w->world_seen ? SECTION_ARM : SECTION_NONE;
        return;
    }
    if ((bits & 0xFF) != 0) {
        /* the 256x256 view of the reflection (mode 1, the mirror image), or another projection */
        w->section = (bits >> 8) == 1 ? SECTION_REFLECT : SECTION_OTHER;
        if (w->section == SECTION_REFLECT && w->mode == SCAN && !w->have_reflect) {
            const float *s = &w->fields[PORT_FRAME_INTERP_SCREEN];

            memcpy(w->reflect_view, &w->fields[PORT_FRAME_INTERP_VIEW], sizeof(w->reflect_view));
            w->screen_reflect.sx = s[0];
            w->screen_reflect.sy = s[1];
            w->screen_reflect.cx = s[2];
            w->screen_reflect.cy = s[3];
            w->screen_reflect.cz = s[4];
            w->screen_reflect.az = s[5];
            w->reflect_fbp = w->frame_set[0] ? (uint32_t)(w->frame[0] & 0x1FF) : 0xFFFFFFFFu;
            w->have_reflect = 1;
        }
        return;
    }
    if ((bits >> 8) == 2) {
        w->section = w->world_seen ? SECTION_EYE : SECTION_OTHER; /* models before the eye */
        return;
    }
    w->section = SECTION_WORLD;
    if (w->mode == SCAN && !w->world_seen) {
        const float *s = &w->fields[PORT_FRAME_INTERP_SCREEN];

        camera_from_fields(&w->camera, &w->fields[PORT_FRAME_INTERP_VIEW]);
        if (w->have_reflect) {
            /* the reflection's view moves with the world's when it is the world's camera */
            int k;

            for (k = 0; k < 12; k++) {
                if (fabsf(w->reflect_view[k] - w->fields[PORT_FRAME_INTERP_VIEW + k]) > 1e-3f * (1.0f + fabsf(w->reflect_view[k]))) {
                    w->have_reflect = 0;
                }
            }
        }
        w->screen.sx = s[0];
        w->screen.sy = s[1];
        w->screen.cx = s[2];
        w->screen.cy = s[3];
        w->screen.cz = s[4];
        w->screen.az = s[5];
        w->draw_fbp = w->frame_set[0] ? (uint32_t)(w->frame[0] & 0x1FF) : 0xFFFFFFFFu;
    }
    w->world_seen = 1;
}

/* A vertex in the camera's space of its frame (v), from its screen place and depth: 0 if it has none
   (behind the camera, or no Q and no Z to tell) */
/* The vertex's Q is 1/w: textured by ST, and not 1.0, which is the Q of the sphere-mapped second pass
   of program 0x280 (its ST is a direction, not a place on the model: no depth in it) */
static int perspective_q(const GifWalk *p) {
    float sq[3];

    if (p->st == NULL || !(p->prim & 0x10) || (p->prim & 0x100)) {
        return 0;
    }
    memcpy(sq, p->st, 12);
    return sq[2] > 0 && isfinite(sq[2]) && sq[2] != 1.0f;
}

static int vertex_view(Walk *w, const GifWalk *p, const uint8_t *q, int xyzf, double v[3], double *depth_out,
                       double *z_out) {
    const Screen *s = &w->screen;
    uint64_t lo, hi;
    double X, Y, z, depth = 0;
    float sq[3];

    memcpy(&lo, q, 8);
    memcpy(&hi, q + 8, 8);
    X = (double)(lo & 0xFFFF) / 16.0;
    Y = (double)((lo >> 32) & 0xFFFF) / 16.0;
    z = xyzf ? (double)((hi >> 4) & 0xFFFFFF) : (double)(uint32_t)hi;
    if (perspective_q(p)) {
        memcpy(sq, p->st, 12);
        depth = 1.0 / sq[2];
        if (w->effects && z - s->cz > 0) {
            /* Program 0x700 (the fires, glows and magic of sfx.dat) divides by the w of a position whose own
               w is not 1 (16 for the fire of the tower): its Q is 1/w over that, its Z the world's. The
               depth is Q's, made the Z's size by the nearest power of two (Q's precision, Z's scale).
               Taken as 1/w, the fire was put 16 times as far: turning moved it as the room, but a step
               sideways hardly at all, and it stood still on the screen between the game's frames. */
            double ratio = depth / (s->az / (z - s->cz));

            if (ratio > 1.5 || ratio < 0.67) {
                depth /= pow(2.0, floor(log2(ratio) + 0.5));
            }
        }
        if (w->nseen < SEEN) {
            /* (for a second pass over the same points) */
            w->seen_xy[w->nseen] = lo & 0x0000FFFF0000FFFFull;
            w->seen_z[w->nseen] = z;
            w->seen_depth[w->nseen++] = depth;
        }
    } else {
        /* A point the run drew before with a Q that is 1/w (the second pass of program 0x280 goes over
           the first's points): that depth. The Z alone can be off: the arm's Z is pulled near. */
        int i;

        for (i = 0; i < w->nseen; i++) {
            if (w->seen_xy[i] == (lo & 0x0000FFFF0000FFFFull) && w->seen_z[i] == z) {
                depth = w->seen_depth[i];
                break;
            }
        }
    }
    if (depth == 0 && z - s->cz > 0) {
        depth = s->az / (z - s->cz);
    }
    if (!(depth > 0) || !isfinite(depth) || s->sx == 0 || s->sy == 0) {
        return 0;
    }
    v[0] = (X - s->cx) * depth / s->sx;
    v[1] = (Y - s->cy) * depth / s->sy;
    v[2] = depth;
    *depth_out = depth;
    *z_out = z;
    return 1;
}

/* The place a vertex is kept as: in the world, or before the eye in the camera's space */
static void view_to_place(const Walk *w, int section, const double v[3], double place[3]) {
    const Camera *c = &w->camera;
    int i;

    if (section == SECTION_EYE) {
        memcpy(place, v, sizeof(double) * 3);
        return;
    }
    for (i = 0; i < 3; i++) {
        place[i] = c->r[0][i] * (v[0] - c->t[0]) + c->r[1][i] * (v[1] - c->t[1]) + c->r[2][i] * (v[2] - c->t[2]);
    }
}

/* SCAN: a vertex of a 3D run, kept as its place */
static void keep_vertex(Walk *w, const GifWalk *p, const uint8_t *q, int xyzf) {
    double v[3], place[3] = {0, 0, 0}, depth, z;
    const Run *r = &w->objects->runs[w->objects->nruns - 1];
    int valid = r->section == SECTION_EYE || r->section == SECTION_ARM || r->section == SECTION_WORLD;

    uint64_t hi;

    /* A vertex with ADC (the first two of every strip, and those around one the VU1 program clipped):
       VERTEX_MAYBE */
    memcpy(&hi, q + 8, 8);
    valid = valid && vertex_view(w, p, q, xyzf, v, &depth, &z);
    if (valid) {
        view_to_place(w, r->section, v, place);
    }
    objects_add_vertex(w->objects, place, !valid ? 0 : ((hi >> 47) & 1) ? VERTEX_MAYBE : VERTEX_SURE);
}

/* REPLAY, at a run's start: whether it moved since the frame before, from its vertices without ADC:
   the most any of them moved (run_most), against a 500th of the middle's distance from the camera;
   or, in the world, all of them as far as the camera moved, and the same way: a model that goes with
   the camera. The sky's dome on map 000 is one: its strips, a few thousand to twenty thousand away,
   go with the player, and those nearer than a 500th of their move (6,000 at a walking step of 12)
   were moved with it while the others stayed in the world, half a step behind in the picture between:
   thin lines along the strips' shared edges, dark (a gap in the additive sky) walking forward and
   bright (two strips over each other) walking back. A model that goes with the camera is moved with
   its model whatever its distance, so the strips stay together. (A model that does not move taken
   for one that goes with the camera, far away while the player creeps, is drawn half way between its
   two places, which are the same: no harm.) */

/* A run in the world went with the camera: its vertices (n of them, the sum of their moves, the most
   any moved) all moved by the camera's move, within a tenth of it, or by its move along the ground (x
   and z), within a tenth of that. The dome goes with the player along the ground, while the eye also
   rises and falls with the step: from a stand, the first frames' steps (1.5 to 2.5 units) had the eye
   0.8 up or down that the dome did not follow, a third of the step, so only the far strips (whose
   places are less sure: the noise) passed as going with the camera and the near ones stayed in the
   world: thin bright and dark lines along their edges in the pictures between, for a frame, each time
   the player set off or stopped. */
/* REPLAY: the run is the sky's, and the sky's move is known (objects_sky_move) */
static int run_is_sky(const Walk *w) {
    const Objects *o = w->objects;

    return o != NULL && w->run >= 0 && (size_t)w->run < o->nruns && o->runs[w->run].sky && o->sky_ok;
}

static int goes_with_camera(const double eye_move[3], const double sum[3], uint32_t n, double most, double dist2) {
    /* (a vertex's place, from its screen place and Q, is off by up to about a 10,000th of its
       distance: the dome's far strips, 20,000 away, by 1 to 2) */
    double noise = sqrt(dist2) * 0.0001;
    int ground;

    for (ground = 0; ground < 2; ground++) {
        double off = 0, step = 0;
        int i;

        for (i = 0; i < 3; i++) {
            double e = ground && i == 1 ? 0 : eye_move[i];

            step += e * e;
            off += (sum[i] / n - e) * (sum[i] / n - e);
        }
        step = sqrt(step);
        if (step > 1.0 && sqrt(off) <= step * 0.1 + noise && most <= step * 1.1 + 2 * noise) {
            return 1;
        }
    }
    return 0;
}

static void run_motion(Walk *w) {
    const Objects *o = w->objects;
    const Run *cur, *prev = w->run_prev;
    double most = 0, dist2 = 0, sum[3] = {0, 0, 0};
    uint32_t k, n = 0;
    int i;

    w->run_moving = 0;
    w->run_most = 0;
    if (run_is_sky(w)) {
        w->run_moving = 1; /* (all of the sky by one move: object_between) */
        return;
    }
    if (prev == NULL) {
        return;
    }
    cur = &o->runs[w->run];
    for (k = 0; k < cur->count; k++) {
        const double *a = w->prev->pos[prev->first + k], *b = o->pos[cur->first + k];
        double d2 = 0;

        if (o->valid[cur->first + k] != VERTEX_SURE || w->prev->valid[prev->first + k] != VERTEX_SURE) {
            continue;
        }
        for (i = 0; i < 3; i++) {
            d2 += (b[i] - a[i]) * (b[i] - a[i]);
            sum[i] += b[i] - a[i];
        }
        n++;
        most = d2 > most ? d2 : most;
        {
            /* the place between (half way at 60), on the line between, must not come much nearer the eye
               than both ends: the arm's swing turns a long way round the shoulder, by the eye, in one frame */
            double mid[3], za, zb, zm;

            for (i = 0; i < 3; i++) {
                mid[i] = a[i] + (b[i] - a[i]) * w->t;
            }
            if (cur->section == SECTION_EYE) {
                za = a[2];
                zb = b[2];
                zm = mid[2];
            } else {
                za = w->camera.r[2][0] * a[0] + w->camera.r[2][1] * a[1] + w->camera.r[2][2] * a[2] + w->camera.t[2];
                zb = w->camera.r[2][0] * b[0] + w->camera.r[2][1] * b[1] + w->camera.r[2][2] * b[2] + w->camera.t[2];
                zm = w->mid.r[2][0] * mid[0] + w->mid.r[2][1] * mid[1] + w->mid.r[2][2] * mid[2] + w->mid.t[2];
            }
            if (zm < 0.75 * (za < zb ? za : zb)) {
                w->run_prev = NULL;
                return;
            }
        }
    }
    most = sqrt(most);
    if (most > INTERP_MAX_MOVE) {
        w->run_prev = NULL; /* two things that share a batch */
        return;
    }
    if (cur->section == SECTION_EYE) {
        dist2 = cur->middle[0] * cur->middle[0] + cur->middle[1] * cur->middle[1] + cur->middle[2] * cur->middle[2];
    } else {
        for (i = 0; i < 3; i++) {
            dist2 += (cur->middle[i] - w->camera.eye[i]) * (cur->middle[i] - w->camera.eye[i]);
        }
    }
    w->run_most = most;
    w->run_moving = most > sqrt(dist2) * 0.002;
    if (!w->run_moving && cur->section == SECTION_WORLD && n > 0) {
        w->run_moving = goes_with_camera(w->eye_move, sum, n, most, dist2);
    }
}

static double distance(const double *a, const double *b) {
    return sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

/* REPLAY: where a vertex of a moving model is half way, in the camera's space between (m); 0 when its
   run is not paired or did not move. A vertex with ADC in either frame may have been clipped in the
   other (its place there wrapped round) while the triangles of this frame use it: it is taken as it is
   only when it is as far from its run's middle in both frames, as a model's points are; otherwise it
   moves by half its run's middle's move. */
static int object_between(Walk *w, double m[3]) {
    const Objects *o = w->objects;
    const Run *cur, *prev = w->run_prev;
    const double *a, *b;
    double mid[3];
    uint32_t k = w->run_vertex;
    int i;

    if (run_is_sky(w) && w->run_moving) {
        cur = &o->runs[w->run];
        if (k >= cur->count || !o->valid[cur->first + k]) {
            return 0;
        }
        b = o->pos[cur->first + k];
        for (i = 0; i < 3; i++) {
            mid[i] = b[i] - o->sky_move[i] * (1.0 - w->t);
        }
        for (i = 0; i < 3; i++) {
            m[i] = w->mid.r[i][0] * mid[0] + w->mid.r[i][1] * mid[1] + w->mid.r[i][2] * mid[2] + w->mid.t[i];
        }
        return 1;
    }
    if (prev == NULL || !w->run_moving) {
        return 0;
    }
    cur = &o->runs[w->run];
    if (k >= cur->count || !o->valid[cur->first + k]) {
        return 0;
    }
    a = w->prev->pos[prev->first + k];
    b = o->pos[cur->first + k];
    if (w->prev->valid[prev->first + k] == VERTEX_SURE && o->valid[cur->first + k] == VERTEX_SURE) {
        for (i = 0; i < 3; i++) {
            mid[i] = a[i] + (b[i] - a[i]) * w->t;
        }
    } else {
        double ra = distance(a, prev->middle), rb = distance(b, cur->middle);
        double most = ra > rb ? ra : rb;

        if (w->prev->valid[prev->first + k] && fabs(ra - rb) <= most * 0.25 + 2.0) {
            for (i = 0; i < 3; i++) {
                mid[i] = a[i] + (b[i] - a[i]) * w->t;
            }
        } else {
            for (i = 0; i < 3; i++) {
                mid[i] = b[i] + (prev->middle[i] - cur->middle[i]) * (1.0 - w->t);
            }
        }
    }
    if (cur->section == SECTION_EYE) {
        memcpy(m, mid, sizeof(mid));
    } else {
        for (i = 0; i < 3; i++) {
            m[i] = w->mid.r[i][0] * mid[0] + w->mid.r[i][1] * mid[1] + w->mid.r[i][2] * mid[2] + w->mid.t[i];
        }
    }
    return 1;
}

/* A vertex of the world (PACKED XYZF2 or XYZ2, q its qword) seen from the camera between: moved with
   its model when it is paired, else with the camera (the world) or not at all (before the eye, the arm) */
static void move_vertex_in(Walk *w, GifWalk *p, uint8_t *q, int xyzf, int section, uint64_t lo, uint64_t hi, float *sq);

static void move_vertex(Walk *w, GifWalk *p, uint8_t *q, int xyzf, int section) {
    uint64_t lo, hi;
    float sq[3] = {0, 0, 0};

    memcpy(&lo, q, 8);
    memcpy(&hi, q + 8, 8);
    if (p->st != NULL && (p->prim & 0x10) && !(p->prim & 0x100)) {
        memcpy(sq, p->st, 12);
    }
    if (section == SECTION_REFLECT) {
        /* (by the reflection's projection, below too) */
        Screen world = w->screen;

        w->screen = w->screen_reflect;
        move_vertex_in(w, p, q, xyzf, section, lo, hi, sq);
        w->screen = world;
        return;
    }
    move_vertex_in(w, p, q, xyzf, section, lo, hi, sq);
}

static void move_vertex_in(Walk *w, GifWalk *p, uint8_t *q, int xyzf, int section, uint64_t lo, uint64_t hi, float *sq) {
    const Screen *s = &w->screen;
    double z, depth = 0, v[3], m[3], nx, ny, nz;
    int64_t ix, iy;
    int i, drop = 0;

    if (!vertex_view(w, p, q, xyzf, v, &depth, &z)) {
        return;
    }
    if (section != SECTION_REFLECT && object_between(w, m)) {
        w->objects_moved[section]++;
    } else if (section == SECTION_WORLD || section == SECTION_REFLECT) {
        for (i = 0; i < 3; i++) {
            m[i] = w->move.r[i][0] * v[0] + w->move.r[i][1] * v[1] + w->move.r[i][2] * v[2] + w->move.t[i];
        }
    } else {
        return;
    }
    if (!(m[2] > 0)) {
        drop = 1;
    } else {
        /* clipped as the VU1 programs clip: past the guard band, or Z below 0 (beyond the far plane) */
        nx = s->sx * m[0] / m[2] + s->cx;
        ny = s->sy * m[1] / m[2] + s->cy;
        /* Z is cz + k / w, k the projection's az for the world. Program 0x700's effects (the
           flames and the lights' glows, drawn with a GEQUAL test and no Z write) have a k of their
           own, about 16 times the world's (the glow on map 000 at w 4,700: Z 3,538 where the world
           there has 206), which pulls them in front. So k is taken from the vertex itself, (z - cz) w:
           moving such a vertex by the world's az alone moved its Z a 16th of the way, and the glows
           came through the walls before them in the pictures between (the lights flickered as the
           player walked toward them). For the world k is az, as before. */
        nz = s->cz + (z - s->cz) * depth / m[2];
        if (!(fabs(nx - 2048.0) <= 2047.0 && fabs(ny - 2048.0) <= 2047.0 && nz >= 0)) {
            drop = 1;
        } else {
            ix = (int64_t)floor(nx * 16.0 + 0.5);
            iy = (int64_t)floor(ny * 16.0 + 0.5);
            nz = floor(nz + 0.5);
            lo = (lo & ~(uint64_t)0xFFFF & ~((uint64_t)0xFFFF << 32)) | (uint64_t)ix | ((uint64_t)iy << 32);
            if (xyzf) {
                uint64_t z = nz < 0 ? 0 : nz > 16777215.0 ? 16777215u : (uint64_t)nz;

                hi = (hi & ~((uint64_t)0xFFFFFF << 4)) | (z << 4);
            } else {
                uint64_t z = nz < 0 ? 0 : nz > 4294967295.0 ? 4294967295u : (uint64_t)nz;

                hi = (hi & ~(uint64_t)0xFFFFFFFF) | z;
            }
            if (perspective_q(p)) {
                double f = depth / m[2];

                sq[0] = (float)(sq[0] * f);
                sq[1] = (float)(sq[1] * f);
                sq[2] = (float)(sq[2] * f); /* (1 / m[2], or for program 0x700 that over its scale: vertex_view) */
                memcpy(p->st, sq, 12);
            }
            w->moved++;
        }
    }
    if (drop) {
        if (!((hi >> 47) & 1)) {
            w->dropped++; /* (a vertex the game already dropped is not counted) */
        }
        p->adc_left = 3; /* this vertex and the two after it: every triangle with it */
    }
    if (p->adc_left > 0) {
        p->adc_left--;
        hi |= (uint64_t)1 << 47;
    }
    memcpy(q, &lo, 8);
    memcpy(q + 8, &hi, 8);
}

/* A+D, or a PACKED register with its own descriptor (TEX0): what the game wrote, and in REPLAY the
   copy changed. reg may be changed (to a register that does nothing, or one that does not draw). */
static void ad_write(Walk *w, GifWalk *p, uint32_t *reg, uint64_t *value) {
    uint32_t r = *reg;


    switch (r) {
    case GS_PRIM:
        p->prim = (uint32_t)(*value & 0x7FF);
        break;
    case GS_TEX0_1:
    case GS_TEX0_2: {
        int n = (int)(r - GS_TEX0_1);

        w->tex0[n] = *value;
        w->tex0_set[n] = 1;
        if (w->mode == REPLAY && w->prev_frame_kept && (*value & 0x3FFF) == w->shown_fbp * 32) {
            w->record_reads_prev = 1;
        }
        if (w->mode == SCAN && w->uploads != NULL) {
            /* the texture and the CLUT are read: the first level, and with mipmaps the levels after it (TEX1's
               MTBA: levels 1 to 3 packed after level 0, each a quarter of the one before, as the GS makes
               MIPTBP1; otherwise twice the first level). Not more: a read of pages the frame has not
               written before a copy of the frame over them says the frame reads the copy of the frame
               before (flat_copies). The twice took the death screen under water's 256 x 256 PSMT8 (10
               pages with its mipmap) for 16, past the textures uploaded there into the wobble's copy left
               from the frame before, so the copy was not made again and the pictures between drew the
               wobble's strips from the textures uploaded over it: noise and bands of light. */
            uint32_t first, last, levels = (uint32_t)((w->tex1[n] >> 2) & 7);
            uint64_t v = *value;
            uint32_t tbp = (uint32_t)(v & 0x3FFF), psm = (uint32_t)((v >> 20) & 0x3F);
            uint32_t tw = 1u << ((v >> 26) & 0xF), th = 1u << ((v >> 30) & 0xF);

            if (levels > 0 && levels <= 3 && ((w->tex1[n] >> 9) & 1)) {
                /* (a block of the format, in pixels: gsn.cpp's mip_addresses) */
                uint32_t bw = psm == 0x14 ? 32 : psm == 0x13 || psm == 0x02 || psm == 0x0A ? 16 : 8;
                uint32_t bh = psm == 0x13 || psm == 0x14 ? 16 : 8;
                uint32_t blocks = (tw / bw) * (th / bh), end = tbp + blocks, l;

                pages_of(tbp, (uint32_t)((v >> 14) & 0x3F), psm, th, &first, &last);
                for (l = 0; l < levels; l++) {
                    blocks /= 4;
                    end += blocks;
                }
                if (end > tbp + (tw / bw) * (th / bh) && (end - 1) / 32 > last) {
                    last = (end - 1) / 32;
                }
            } else {
                pages_of(tbp, (uint32_t)((v >> 14) & 0x3F), psm, (levels ? 2u : 1u) * th, &first, &last);
            }
            uploads_event(w->uploads, EV_READ, first, last);
            pages_of((uint32_t)((v >> 37) & 0x3FFF), 1, 0, 16, &first, &last);
            uploads_event(w->uploads, EV_READ, first, last);
        }
        if (w->mode == REPLAY && (*value & 0x3FFF) == w->draw_fbp * 32) {
            *value = (*value & ~(uint64_t)0x3FFF) | (w->shown_fbp * 32);
        }
        break;
    }
    case GS_PRMODECONT:
        w->prmodecont = *value;
        break;
    case 0x14: /* TEX1_1 */
    case 0x15:
        w->tex1[r - 0x14] = *value;
        break;
    case 0x40: /* SCISSOR_1 */
    case 0x41:
        w->scissor[r - 0x40] = *value;
        break;
    case 0x4E: /* ZBUF_1 */
    case 0x4F:
        w->zbuf[r - 0x4E] = *value;
        if (w->mode == SCAN && w->uploads != NULL && !((*value >> 32) & 1)) {
            /* (Z written: as wide as the FRAME of its context) */
            uint32_t first, last, n = r - 0x4E;
            uint32_t rows = w->scissor[n] ? (uint32_t)((w->scissor[n] >> 48) & 0x7FF) + 1 : 512;

            pages_of((uint32_t)(*value & 0x1FF) * 32, (uint32_t)((w->frame[n] >> 16) & 0x3F),
                     0x30 | (uint32_t)((*value >> 24) & 0xF), rows, &first, &last);
            uploads_event(w->uploads, EV_WRITE, first, last);
        }
        break;
    case 0x51: /* TRXPOS */
        w->trxpos = *value;
        break;
    case 0x52: /* TRXREG */
        w->trxreg = *value;
        break;
    case GS_PRMODE:
        w->prmode = *value;
        break;
    case GS_FRAME_1:
    case GS_FRAME_2: {
        int n = (int)(r - GS_FRAME_1);

        w->frame[n] = *value;
        w->frame_set[n] = 1;
        if (w->mode == SCAN && w->uploads != NULL) {
            /* drawn into, to the scissor's bottom (or 512 rows) */
            uint32_t first, last, rows = w->scissor[n] ? (uint32_t)((w->scissor[n] >> 48) & 0x7FF) + 1 : 512;

            pages_of((uint32_t)(*value & 0x1FF) * 32, (uint32_t)((*value >> 16) & 0x3F), (uint32_t)((*value >> 24) & 0x3F),
                     rows, &first, &last);
            uploads_event(w->uploads, EV_WRITE, first, last);
        }
        if (w->mode == REPLAY) {
            if ((*value & 0x1FF) == w->draw_fbp) {
                *value = (*value & ~(uint64_t)0x1FF) | w->shown_fbp;
            }
        }
        break;
    }
    case GS_BITBLTBUF:
        w->bitbltbuf = *value;
        w->bitbltbuf_set = 1;
        if (w->mode == REPLAY) {
            if ((*value & 0x3FFF) == w->draw_fbp * 32 && !w->exact) {
                /* (a copy of the frame reads the picture between's; in the exact restore, the only one made is a
                   COPY_RESTORE, from the game's frame) */
                *value = (*value & ~(uint64_t)0x3FFF) | (w->shown_fbp * 32);
            }
            if (((*value >> 32) & 0x3FFF) == w->draw_fbp * 32) {
                *value = (*value & ~((uint64_t)0x3FFF << 32)) | ((uint64_t)(w->shown_fbp * 32) << 32);
            }
        }
        break;
    case GS_TRXDIR: {
        uint32_t dbp = (uint32_t)(w->bitbltbuf >> 32) & 0x3FFF;
        uint32_t dir = (uint32_t)(*value & 3);

        int frame_copy = dir == 2 && dbp != w->draw_fbp * 32 && (uint32_t)(w->bitbltbuf & 0x3FFF) == w->draw_fbp * 32;

        if (w->mode == SCAN && w->uploads != NULL && (dir == 0 || dir == 2)) {
            /* written: from the destination's first block, to the rows the transfer reaches */
            uint32_t first, last;
            Uploads *u = w->uploads;

            pages_of(dbp, (uint32_t)(w->bitbltbuf >> 48) & 0x3F, (uint32_t)(w->bitbltbuf >> 56) & 0x3F,
                     (uint32_t)((w->trxpos >> 48) & 0x7FF) + (uint32_t)((w->trxreg >> 32) & 0xFFF), &first, &last);
            uploads_event(u, dir == 0 ? EV_UPLOAD : EV_WRITE, first, last);
            if (frame_copy) {
                if (u->ncopies == u->copies_room) {
                    u->copies_room = u->copies_room ? u->copies_room * 2 : 16;
                    u->copies = realloc(u->copies, u->copies_room * sizeof(*u->copies));
                    if (u->copies == NULL) {
                        PORT_FATAL("out of memory");
                    }
                }
                u->copies[u->ncopies++] = (uint32_t)(u->nev - 1);
            }
        }
        if (w->mode == REPLAY && frame_copy && w->copies != NULL && w->copy < w->copies->ncopies &&
            w->copies->copy_ok[w->copy] == (w->exact ? COPY_RESTORE : COPY_BETWEEN)) {
            /* a copy of the frame the frame reads: made from the picture between (BITBLTBUF's source is the
               buffer shown); or in the exact restore, one it does not read made again from the game's frame
               (BITBLTBUF's source left as the game's: the frame is not drawn after it) */
        } else if (w->mode == REPLAY) {
            /* (bitbltbuf is the game's: a transfer into the buffer drawn goes into the one shown) */
            if (dir == 1 || (dir == 2 && dbp != w->draw_fbp * 32)) {
                *reg = PORT_GS_REG_FRAME_INTERP;
                *value = (uint64_t)0xFF << 32;
            } else if (dir == 0 && w->ask_uploads && dbp != w->draw_fbp * 32) {
                /* the native GS asked for the frame's upload again (it kept its pages, or its bytes):
                   the data left out (walk_record) */
                *reg = PORT_GS_REG_REPLAY_UPLOAD;
                *value = (uint64_t)w->upload | (uint64_t)(uint32_t)(w->bitbltbuf >> 32) << 32;
                w->skip_image = 1;
            }
        }
        if (dir == 0) {
            w->upload++;
        }
        w->copy += frame_copy;
        break;
    }
    case GS_SIGNAL:
    case GS_FINISH:
    case GS_LABEL:
        if (w->mode == REPLAY) {
            *reg = PORT_GS_REG_FRAME_INTERP;
            *value = (uint64_t)0xFF << 32;
        }
        break;
    case PORT_GS_REG_FRAME_INTERP:
        mark(w, *value);
        break;
    }
}

/* SCAN: a 2D vertex kept for the pairs (its place, and its colour and texture coordinates when they are
   in this tag's qwords, where a picture between can change them) */
static void keep_flat_vertex(Walk *w, GifWalk *p, const uint8_t *q, int xyzf) {
    FlatVertex *v = flat_add_vertex(w->flat);
    Item *it = &w->flat->items[w->flat->nitems - 1];
    uint64_t lo, hi;

    memcpy(&lo, q, 8);
    memcpy(&hi, q + 8, 8);
    if (!((hi >> 47) & 1)) {
        uint32_t z = xyzf ? (uint32_t)((hi >> 4) & 0xFFFFFF) : (uint32_t)hi;

        it->zlo = z < it->zlo ? z : it->zlo;
        it->zhi = z > it->zhi ? z : it->zhi;
        if (it->st.state >= 0 && it->st.state != 2) {
            if ((p->prim & 0x100) || p->st == NULL) {
                it->st.state = -1;
            } else {
                float stq[3];

                memcpy(stq, p->st, 12);
                st_plane_vertex(&it->st, (double)(lo & 0xFFFF) / 16.0, (double)((lo >> 32) & 0xFFFF) / 16.0, stq);
            }
        }
    }
    v->x = (int32_t)(lo & 0xFFFF);
    v->y = (int32_t)((lo >> 32) & 0xFFFF);
    v->has_rgba = p->rgbaq != NULL;
    if (p->rgbaq != NULL) {
        uint32_t c[4];

        memcpy(c, p->rgbaq, 16);
        v->rgba[0] = (uint8_t)c[0];
        v->rgba[1] = (uint8_t)c[1];
        v->rgba[2] = (uint8_t)c[2];
        v->rgba[3] = (uint8_t)c[3];
    }
    v->coords = 0;
    v->s = v->t = 0;
    if ((p->prim & 0x10) && !(p->prim & 0x100) && p->st != NULL) {
        float st[2];

        memcpy(st, p->st, 8);
        v->coords = 1;
        v->s = st[0];
        v->t = st[1];
    } else if ((p->prim & 0x10) && (p->prim & 0x100) && p->uv != NULL) {
        uint32_t uv[2];

        memcpy(uv, p->uv, 8);
        v->coords = 2;
        v->s = (float)(uv[0] & 0x3FFF) / 16.0f;
        v->t = (float)(uv[1] & 0x3FFF) / 16.0f;
    }
}

/* A mesh of the world the CPU projected (Item's zlo, zhi, st): triangles, Z varying, S and T made from
   the screen; while the world has a camera */
static int item_mesh(const Walk *w, const Item *b) {
    int tri = (b->prim & 7) >= 3 && (b->prim & 7) <= 5;

    return tri && b->zhi > b->zlo + 1 && b->zlo != 0xFFFFFFFFu && st_screen((StPlane *)&b->st) &&
           w->screen.sx != 0 && w->screen.sy != 0 && b->fbp == w->draw_fbp;
}

/* REPLAY, a vertex of an item_mesh: moved with the camera as the world is (move_vertex_in's way), S and T
   with its place on the screen */
static void move_mesh_vertex(Walk *w, GifWalk *p, uint8_t *q, int xyzf, const Item *b) {
    const Screen *s = &w->screen;
    double z, depth = 0, v[3], m[3], nx, ny, nz;
    uint64_t lo, hi;
    int i;

    memcpy(&lo, q, 8);
    memcpy(&hi, q + 8, 8);
    if (!vertex_view(w, p, q, xyzf, v, &depth, &z)) {
        return;
    }
    for (i = 0; i < 3; i++) {
        m[i] = w->move.r[i][0] * v[0] + w->move.r[i][1] * v[1] + w->move.r[i][2] * v[2] + w->move.t[i];
    }
    if (!(m[2] > 0)) {
        p->adc_left = 3;
        return;
    }
    nx = s->sx * m[0] / m[2] + s->cx;
    ny = s->sy * m[1] / m[2] + s->cy;
    nz = s->cz + (z - s->cz) * depth / m[2];
    if (!(fabs(nx - 2048.0) <= 2047.0 && fabs(ny - 2048.0) <= 2047.0 && nz >= 0)) {
        p->adc_left = 3;
        return;
    }
    {
        int64_t ix = (int64_t)floor(nx * 16.0 + 0.5), iy = (int64_t)floor(ny * 16.0 + 0.5);

        if (b->st.state == 1 && p->st != NULL) {
            float st[2];
            double s0, t0, s1, t1;

            st_map(&b->st, (double)(lo & 0xFFFF) / 16.0, (double)((lo >> 32) & 0xFFFF) / 16.0, &s0, &t0);
            st_map(&b->st, (double)ix / 16.0, (double)iy / 16.0, &s1, &t1);
            memcpy(st, p->st, 8);
            st[0] = (float)(st[0] + (s1 - s0));
            st[1] = (float)(st[1] + (t1 - t0));
            memcpy(p->st, st, 8);
        }
        nz = floor(nz + 0.5);
        lo = (lo & ~(uint64_t)0xFFFF & ~((uint64_t)0xFFFF << 32)) | (uint64_t)ix | ((uint64_t)iy << 32);
        if (xyzf) {
            uint64_t zz = nz > 16777215.0 ? 16777215u : (uint64_t)nz;

            hi = (hi & ~((uint64_t)0xFFFFFF << 4)) | (zz << 4);
        } else {
            uint64_t zz = nz > 4294967295.0 ? 4294967295u : (uint64_t)nz;

            hi = (hi & ~(uint64_t)0xFFFFFFFF) | zz;
        }
        memcpy(q, &lo, 8);
        memcpy(q + 8, &hi, 8);
        w->moved++;
    }
}

/* A paired 2D vertex's alpha at the picture's time */
static double vb_alpha_between(const Walk *w, const Item *b) {
    const Item *a = &w->flat_prev->items[b->pair];
    double va = w->flat_prev->verts[a->first + w->item_vertex].rgba[3], vb = w->flat->verts[b->first + w->item_vertex].rgba[3];

    return vb + (va - vb) * (1.0 - w->t);
}

/* Where a UV between two frames is counted from: this frame's, or, after a scroll back past the
   texture's wrap (a negative wrap: the frame before's coordinates less the texture's size), this frame's
   plus that size, the same texels a texture that repeats. UV is unsigned in the GS (14 bits, 1/16 texel):
   counted from this frame's, a sprite's first corner at 0 was between -0.25 and 0 in the pictures between
   and went in as 1023.9, the texture squeezed a thousand times over: map 000's sky (its 128 x 128 cloud
   targets drawn by scrolling sprites, MapScroll_UpdateDraw) a picture of grey streaks for a frame's
   pictures between each time a cloud layer's scroll came round (every 512 frames, about 17 s). */
static float uv_base(float v, float wrap) {
    return wrap < 0 ? v - wrap : v;
}

/* REPLAY: a paired 2D vertex where it is at the picture's time (t of the way from the frame before's) */
static void move_flat_vertex(Walk *w, GifWalk *p, uint8_t *q) {
    const Item *b = &w->flat->items[w->item];
    const Item *a;
    const FlatVertex *va, *vb;
    double u = 1.0 - w->t;
    uint64_t lo;

    if (w->item_vertex >= b->count) {
        return;
    }
    if (w->prev_frame_kept && (b->prim & 0x10) && b->fbp == w->flat->draw_fbp &&
        (uint32_t)(b->tex & 0x3FFF) == w->flat->shown_fbp * 32 && p->rgbaq != NULL) {
        /* The frame shown before blended in (Game_BlendPreviousFrame: ALPHA 0x44, As of 0x80 from it):
           P = a Prev + (1 - a) Drawn, which decays the frames before by a each frame. A picture between at
           t blends in the frame shown before it (the native GS's copy of it, take_snapshot) by a^t: from
           the frame shown at t 0 to the game's own blend at t 1, with no jump at either; the trail as long
           in time as the game's. (Blending each picture with the one before instead, by the weight that
           gives the same lag, moved evenly between the frames but jumped at each: the game's frame is
           blended with the frame before, not with the last picture between.) */
        uint32_t c[4];
        double a, base = b->pair >= 0 && w->flat_prev != NULL
                             ? vb_alpha_between(w, b)
                             : (double)w->flat->verts[b->first + w->item_vertex].rgba[3];

        memcpy(c, p->rgbaq, 16);
        a = base / 128.0;
        a = a <= 0 ? 0 : a >= 1 ? 1 : pow(a, w->t);
        c[3] = (uint32_t)floor(a * 128.0 + 0.5);
        memcpy(p->rgbaq, c, 16);
        w->items_moved++;
        return;
    }
    if (b->pair < 0 || w->flat_prev == NULL) {
        return;
    }
    a = &w->flat_prev->items[b->pair];
    va = &w->flat_prev->verts[a->first + w->item_vertex];
    vb = &w->flat->verts[b->first + w->item_vertex];
    memcpy(&lo, q, 8);
    lo = (lo & ~(uint64_t)0xFFFF & ~((uint64_t)0xFFFF << 32)) |
         (uint64_t)(uint32_t)(vb->x + (int32_t)floor((va->x - vb->x) * u + 0.5)) |
         ((uint64_t)(uint32_t)(vb->y + (int32_t)floor((va->y - vb->y) * u + 0.5)) << 32);
    memcpy(q, &lo, 8);
    if (p->rgbaq != NULL && va->has_rgba && vb->has_rgba) {
        uint32_t c[4];
        int k;

        memcpy(c, p->rgbaq, 16);
        for (k = 0; k < 4; k++) {
            c[k] = (uint32_t)(vb->rgba[k] + (int)floor((va->rgba[k] - vb->rgba[k]) * u + 0.5));
        }
        memcpy(p->rgbaq, c, 16);
    }
    if (vb->coords == 1 && p->st != NULL) {
        float st[2];

        st[0] = (float)(vb->s + (va->s + b->wrap_s - vb->s) * u);
        st[1] = (float)(vb->t + (va->t + b->wrap_t - vb->t) * u);
        memcpy(p->st, st, 8);
    } else if (vb->coords == 2 && p->uv != NULL) {
        uint32_t uv[2];
        /* (a picture between counted from past the wrap: uv_base) */
        double bs = u > 0 ? uv_base(vb->s, b->wrap_s) : vb->s, bt = u > 0 ? uv_base(vb->t, b->wrap_t) : vb->t;
        double s = bs + (va->s + b->wrap_s - vb->s) * u, t = bt + (va->t + b->wrap_t - vb->t) * u;

        memcpy(uv, p->uv, 8);
        uv[0] = (uv[0] & ~0x3FFFu) | ((uint32_t)floor(s * 16.0 + 0.5) & 0x3FFF);
        uv[1] = (uv[1] & ~0x3FFFu) | ((uint32_t)floor(t * 16.0 + 0.5) & 0x3FFF);
        memcpy(p->uv, uv, 8);
    }
    w->items_moved++;
}

/* ---- On the GPU ---------------------------------------------------------------------------------
   With the native GS alone, the pictures between are drawn on the GPU (port/gs/native/gsn.cpp, "Pictures
   between on the GPU"): the first picture of a frame walks its packets as REPLAY does, but leaves every
   vertex where the game put it and notes, for each vertex kick, what moving it takes (a GsnInterpMove:
   its place in the frame's view, its model's two places, a 2D item's two places, colours and texture
   coordinates); the native GS draws the packets from that once and records what it did, and each
   picture after it hands over only its time, its cameras and the model parts that move with their model
   then (gpu_picture), and gs_replay.vert moves the vertices as move_vertex and move_flat_vertex do here.
   Not done on the GPU (the CPU's walk as before): with paraLLEl-GS (--renderer both), KF4_INTERP_STILL,
   the frame drawn again exact for the buffers it changed (port_interp_end_vblank), and with
   KF4_INTERP_CPU set. */
static GsnInterpMove *gpu_moves; /* the frame's, by kick (the native GS frees the array it is handed) */
static size_t gpu_moves_room;
static uint8_t *gpu_run_ok;      /* by run: paired, moved, not apart (the pictures' bits: gpu_picture) */
static size_t gpu_run_room;

static GsnInterpMove *gpu_move(Walk *w) {
    if (w->kick >= gpu_moves_room) {
        size_t room = gpu_moves_room ? gpu_moves_room * 2 : 65536;
        GsnInterpMove *grown = realloc(gpu_moves, room * sizeof(*gpu_moves));

        if (grown == NULL) {
            PORT_FATAL("out of memory");
        }
        gpu_moves = grown;
        gpu_moves_room = room;
    }
    memset(&gpu_moves[w->kick], 0, sizeof(*gpu_moves));
    return &gpu_moves[w->kick];
}

static uint32_t float_bits(double v) {
    float f = (float)v;
    uint32_t b;

    memcpy(&b, &f, 4);
    return b;
}

/* At a run's start: run_motion's decision but for the place between (which depends on the picture's
   time: gpu_zm_ok): whether the run moved, paired with one not apart */
static int gpu_run_static(Walk *w) {
    const Objects *o = w->objects;
    const Run *cur, *prev = w->run_prev;
    double most = 0, dist2 = 0, sum[3] = {0, 0, 0};
    uint32_t k, n = 0;
    int i;

    if (run_is_sky(w)) {
        return 1; /* (all of the sky by one move: gpu_vertex) */
    }
    if (prev == NULL) {
        return 0;
    }
    cur = &o->runs[w->run];
    for (k = 0; k < cur->count; k++) {
        const double *a = w->prev->pos[prev->first + k], *b = o->pos[cur->first + k];
        double d2 = 0;

        if (o->valid[cur->first + k] != VERTEX_SURE || w->prev->valid[prev->first + k] != VERTEX_SURE) {
            continue;
        }
        for (i = 0; i < 3; i++) {
            d2 += (b[i] - a[i]) * (b[i] - a[i]);
            sum[i] += b[i] - a[i];
        }
        n++;
        most = d2 > most ? d2 : most;
    }
    most = sqrt(most);
    if (most > INTERP_MAX_MOVE) {
        return 0;
    }
    if (cur->section == SECTION_EYE) {
        dist2 = cur->middle[0] * cur->middle[0] + cur->middle[1] * cur->middle[1] + cur->middle[2] * cur->middle[2];
    } else {
        for (i = 0; i < 3; i++) {
            dist2 += (cur->middle[i] - w->camera.eye[i]) * (cur->middle[i] - w->camera.eye[i]);
        }
    }
    if (most > sqrt(dist2) * 0.002) {
        return 1;
    }
    return cur->section == SECTION_WORLD && n > 0 && goes_with_camera(w->eye_move, sum, n, most, dist2);
}

/* run_motion's place between at the picture's time: it must not come much nearer the eye than both ends */
static int gpu_zm_ok(const Objects *o, const Objects *before, const Run *cur, const Camera *camera, const Camera *mid,
                     double t) {
    const Run *prev = &before->runs[cur->pair];
    uint32_t k;
    int i;

    for (k = 0; k < cur->count; k++) {
        const double *a = before->pos[prev->first + k], *b = o->pos[cur->first + k];
        double m[3], za, zb, zm;

        if (o->valid[cur->first + k] != VERTEX_SURE || before->valid[prev->first + k] != VERTEX_SURE) {
            continue;
        }
        for (i = 0; i < 3; i++) {
            m[i] = a[i] + (b[i] - a[i]) * t;
        }
        if (cur->section == SECTION_EYE) {
            za = a[2];
            zb = b[2];
            zm = m[2];
        } else {
            za = camera->r[2][0] * a[0] + camera->r[2][1] * a[1] + camera->r[2][2] * a[2] + camera->t[2];
            zb = camera->r[2][0] * b[0] + camera->r[2][1] * b[1] + camera->r[2][2] * b[2] + camera->t[2];
            zm = mid->r[2][0] * m[0] + mid->r[2][1] * m[1] + mid->r[2][2] * m[2] + mid->t[2];
        }
        if (zm < 0.75 * (za < zb ? za : zb)) {
            return 0;
        }
    }
    return 1;
}

/* GSN_IM_SCREEN_ST's map in a GsnInterpMove: h0..h5 in w[8..13], h6 in w[15], h7 in w[1] */
static void gpu_st_map(GsnInterpMove *e, const StPlane *st) {
    int i;

    for (i = 0; i < 6; i++) {
        e->w[8 + i] = float_bits(st->h[i]);
    }
    e->w[15] = float_bits(st->h[6]);
    e->w[1] = float_bits(st->h[7]);
}

/* A vertex of the world (move_vertex's) as a GsnInterpMove: its place in the frame's view, depth and Z,
   and when its run is paired its model's two places (object_between's) */
static void gpu_vertex(Walk *w, GifWalk *p, const uint8_t *q, int xyzf, int section) {
    GsnInterpMove *e = gpu_move(w);
    Screen world = w->screen;
    double v[3], depth, z;
    int ok;

    if (section == SECTION_REFLECT) {
        w->screen = w->screen_reflect;
    }
    ok = vertex_view(w, p, q, xyzf, v, &depth, &z);
    if (!ok) {
        w->screen = world;
        return; /* (as it is: move_vertex leaves it) */
    }
    e->w[0] = GSN_IM_3D | GSN_IM_VIEW | (perspective_q(p) ? GSN_IM_SCALE_ST : 0) | (xyzf ? GSN_IM_XYZF : 0) |
              (section == SECTION_REFLECT ? GSN_IM_REFLECT : 0) |
              (section == SECTION_WORLD || section == SECTION_REFLECT ? GSN_IM_WORLD : 0) |
              (section == SECTION_EYE ? GSN_IM_EYE : 0);
    e->w[1] = (uint32_t)w->run;
    e->w[4] = float_bits(v[0]);
    e->w[5] = float_bits(v[1]);
    e->w[6] = float_bits(v[2]);
    e->w[7] = float_bits(depth);
    e->w[14] = float_bits(z - w->screen.cz);
    w->screen = world;
    if (section == SECTION_WORLD && run_is_sky(w) && (size_t)w->run < gpu_run_room && gpu_run_ok[w->run]) {
        /* object_between's sky: b less the sky's move (1 - t) */
        const Objects *o = w->objects;
        const Run *cur = &o->runs[w->run];
        uint32_t k = w->run_vertex;
        int i;

        if (k < cur->count && o->valid[cur->first + k]) {
            for (i = 0; i < 3; i++) {
                e->w[8 + i] = float_bits(-o->sky_move[i]);
                e->w[11 + i] = float_bits(o->sky_move[i]);
            }
            e->w[0] |= GSN_IM_OBJ;
        }
    } else if (section != SECTION_REFLECT && w->run_prev != NULL && (size_t)w->run < w->objects->nruns &&
        gpu_run_ok[w->run]) {
        /* object_between's two places, as a start and a move */
        const Objects *o = w->objects;
        const Run *cur = &o->runs[w->run], *prev = w->run_prev;
        uint32_t k = w->run_vertex;

        if (k < cur->count && o->valid[cur->first + k]) {
            const double *a = w->prev->pos[prev->first + k], *b = o->pos[cur->first + k];
            int i, straight = w->prev->valid[prev->first + k] == VERTEX_SURE && o->valid[cur->first + k] == VERTEX_SURE;

            if (!straight) {
                double ra = distance(a, prev->middle), rb = distance(b, cur->middle);
                double most = ra > rb ? ra : rb;

                straight = w->prev->valid[prev->first + k] && fabs(ra - rb) <= most * 0.25 + 2.0;
            }
            for (i = 0; i < 3; i++) {
                /* (from where it is in this frame, b: the place at t 0 less b, and the move) */
                if (straight) {
                    e->w[8 + i] = float_bits(a[i] - b[i]);
                    e->w[11 + i] = float_bits(b[i] - a[i]);
                } else {
                    /* b + (prev middle - cur middle)(1 - t) */
                    e->w[8 + i] = float_bits(prev->middle[i] - cur->middle[i]);
                    e->w[11 + i] = float_bits(cur->middle[i] - prev->middle[i]);
                }
            }
            e->w[0] |= GSN_IM_OBJ;
        }
    }
    if (e->w[0] & (GSN_IM_WORLD | GSN_IM_OBJ)) {
        /* with a place: the ADC of a vertex dropped covers the next two of the path with one */
        e->w[2] = p->win[0];
        e->w[3] = p->win[1];
        p->win[1] = p->win[0];
        p->win[0] = w->kick;
    }
}

/* A 2D vertex (move_flat_vertex's) as a GsnInterpMove: its item's two places, colours and texture
   coordinates, or the previous frame's alpha */
static void gpu_flat_vertex(Walk *w, GifWalk *p) {
    const Item *b = &w->flat->items[w->item];
    const Item *a;
    const FlatVertex *va, *vb;
    GsnInterpMove *e = gpu_move(w);

    if (w->item_vertex >= b->count) {
        return;
    }
    vb = &w->flat->verts[b->first + w->item_vertex];
    if (w->prev_frame_kept && (b->prim & 0x10) && b->fbp == w->flat->draw_fbp &&
        (uint32_t)(b->tex & 0x3FFF) == w->flat->shown_fbp * 32 && p->rgbaq != NULL) {
        int paired = b->pair >= 0 && w->flat_prev != NULL;

        e->w[0] = GSN_IM_FLAT | GSN_IM_PREV | (paired ? GSN_IM_PAIRED : 0);
        va = paired ? &w->flat_prev->verts[w->flat_prev->items[b->pair].first + w->item_vertex] : vb;
        e->w[1] = (uint32_t)va->rgba[3] << 24;
        e->w[2] = (uint32_t)vb->rgba[3] << 24;
        return;
    }
    if (b->pair < 0 || w->flat_prev == NULL) {
        return;
    }
    a = &w->flat_prev->items[b->pair];
    va = &w->flat_prev->verts[a->first + w->item_vertex];
    e->w[0] = GSN_IM_FLAT | GSN_IM_XY;
    e->w[3] = ((uint32_t)va->x & 0xFFFF) | (uint32_t)va->y << 16;
    e->w[4] = ((uint32_t)vb->x & 0xFFFF) | (uint32_t)vb->y << 16;
    if (p->rgbaq != NULL && va->has_rgba && vb->has_rgba) {
        e->w[0] |= GSN_IM_RGBA;
        memcpy(&e->w[1], va->rgba, 4);
        memcpy(&e->w[2], vb->rgba, 4);
    }
    if ((vb->coords == 1 && p->st != NULL) || (vb->coords == 2 && p->uv != NULL)) {
        /* (move_flat_vertex: vb + (va + wrap - vb) u, the sum and difference in floats) */
        float ds = va->s + b->wrap_s - vb->s, dt = va->t + b->wrap_t - vb->t;
        /* (UV counted from past a wrap back: uv_base; every picture here is between, t below 1) */
        float bs = vb->coords == 2 ? uv_base(vb->s, b->wrap_s) : vb->s, bt = vb->coords == 2 ? uv_base(vb->t, b->wrap_t) : vb->t;

        e->w[0] |= vb->coords == 1 ? GSN_IM_ST : GSN_IM_UV;
        memcpy(&e->w[5], &bs, 4);
        memcpy(&e->w[6], &bt, 4);
        memcpy(&e->w[7], &ds, 4);
        memcpy(&e->w[8], &dt, 4);
    }
}

/* An item_mesh's vertex as a GsnInterpMove: a vertex of the world, with the camera (gpu_vertex's), S and T
   made from the screen with its place */
static void gpu_mesh_vertex(Walk *w, GifWalk *p, const uint8_t *q, int xyzf, const Item *b) {
    GsnInterpMove *e = gpu_move(w);
    double v[3], depth, z;

    if (!vertex_view(w, p, q, xyzf, v, &depth, &z)) {
        return;
    }
    e->w[0] = GSN_IM_3D | GSN_IM_VIEW | GSN_IM_WORLD | (xyzf ? GSN_IM_XYZF : 0) |
              (b->st.state == 1 ? GSN_IM_SCREEN_ST : 0);
    e->w[1] = GSN_IM_NONE;
    e->w[4] = float_bits(v[0]);
    e->w[5] = float_bits(v[1]);
    e->w[6] = float_bits(v[2]);
    e->w[7] = float_bits(depth);
    e->w[14] = float_bits(z - w->screen.cz);
    if (b->st.state == 1) {
        gpu_st_map(e, &b->st);
    }
    e->w[2] = p->win[0];
    e->w[3] = p->win[1];
    p->win[1] = p->win[0];
    p->win[0] = w->kick;
}

/* One record: in REPLAY, data is the copy to send, and is changed (uploads left out are taken out of
   it). Returns the bytes left. */
static size_t walk_record(Walk *w, const Record *r, uint8_t *data) {
    int path = (int)(r->what & 0xFF), is_3d = (r->what & RECORD_3D) != 0;

    w->effects = (r->program & RECORD_EFFECTS) != 0;
    GifWalk *p = &w->paths[path];
    size_t count = r->bytes / 16, i = 0, out = 0;

    p->st = NULL;
    p->rgbaq = NULL;
    p->uv = NULL;
    if (is_3d && (w->run < 0 || r->serial != w->serial)) {
        /* a new VU1 run */
        w->serial = r->serial;
        w->run++;
        w->run_vertex = 0;
        w->nseen = 0;
        if (w->mode == SCAN && w->objects != NULL) {
            objects_add_run(w->objects, r->key, w->section, w->sky, w->alt);
            w->objects->runs[w->objects->nruns - 1].shadow = (r->program & RECORD_SHADOW) != 0;
        } else if (w->mode == REPLAY) {
            const Objects *o = w->objects;

            /* (paired once for the frame: objects_pair) */
            w->run_prev = o != NULL && w->prev != NULL && (size_t)w->run < o->nruns && o->runs[w->run].pair >= 0
                              ? &w->prev->runs[o->runs[w->run].pair]
                              : NULL;
            if (w->gpu) {
                /* (the place between at each picture's time: gpu_picture) */
                if ((size_t)w->run < gpu_run_room) {
                    gpu_run_ok[w->run] = (uint8_t)gpu_run_static(w);
                }
            } else {
                run_motion(w);
            }
        }
    }
    while (i < count) {
        uint8_t *q = data + i * 16;
        size_t n = 1; /* qwords looked at */
        int keep = 1;

        if (p->left == 0) {
            uint64_t tag, regs;
            uint32_t nloop;

            memcpy(&tag, q, 8);
            memcpy(&regs, q + 8, 8);
            nloop = (uint32_t)(tag & 0x7FFF);
            p->flg = (uint32_t)((tag >> 58) & 3);
            p->nreg = (uint32_t)(tag >> 60);
            if (p->nreg == 0) {
                p->nreg = 16;
            }
            p->regs = regs;
            p->reg = 0;
            p->item_open = 0;
            p->dropping = 0;
            if ((tag >> 46) & 1) {
                p->prim = (uint32_t)(tag >> 47) & 0x7FF; /* PRE */
            }
            if (p->flg == 0) {
                p->left = nloop * p->nreg;
            } else if (p->flg == 1) {
                p->left = (nloop * p->nreg + 1) / 2;
                p->dwords = nloop * p->nreg;
            } else {
                p->left = nloop;
                if (w->mode == REPLAY && w->skip_image && nloop != 0) {
                    /* an upload left out: its data, behind a tag with none (which keeps its EOP) */
                    w->skip_image = 0;
                    p->dropping = 1;
                    tag &= ~(uint64_t)0x7FFF;
                    memcpy(q, &tag, 8);
                    w->upload_bytes_left_out += (uint64_t)nloop * 16;
                }
            }
        } else if (p->flg == 0) {
            uint32_t d = (uint32_t)(p->regs >> (p->reg * 4)) & 0xF;
            uint64_t v[2];

            memcpy(v, q, 16);
            switch (d) {
            case 0x0:
                p->prim = (uint32_t)(v[0] & 0x7FF);
                break;
            case 0x1:
                p->rgbaq = q;
                break;
            case 0x2:
                p->st = q;
                break;
            case 0x3:
                p->uv = q;
                break;
            case 0x4:
            case 0x5:
                if (is_3d) {
                    int section = w->section;
                    int tri = (p->prim & 7) >= 3 && (p->prim & 7) <= 5;
                    int moves = ((section == SECTION_WORLD || section == SECTION_EYE || section == SECTION_ARM) && tri &&
                                 draws_frame(w, p->prim)) ||
                                (section == SECTION_REFLECT && tri && w->have_reflect && draws_into(w, p->prim, w->reflect_fbp));

                    if (w->mode == SCAN && w->objects != NULL) {
                        Run *run = &w->objects->runs[w->objects->nruns - 1];

                        if (run->count == 0) {
                            /* what it is drawn with */
                            int c = (w->prmodecont & 1) ? (int)((p->prim >> 9) & 1) : (int)((w->prmode >> 9) & 1);

                            run->prim = p->prim;
                            run->tex = (p->prim & 0x10) ? w->tex0[c] & ~((uint64_t)7 << 61) : 0;
                        }
                        if (moves) {
                            keep_vertex(w, p, q, d == 0x4);
                        } else {
                            static const double none[3] = {0, 0, 0};

                            objects_add_vertex(w->objects, none, 0);
                        }
                    } else if (w->mode == REPLAY && w->exact) {
                        /* (drawing the frame again for the other buffers: nothing into the frame is drawn,
                           every vertex with ADC: exact_kick) */
                        exact_kick(w, p, q);
                    } else if (w->mode == REPLAY && moves && w->gpu) {
                        gpu_vertex(w, p, q, d == 0x4, section);
                    } else if (w->mode == REPLAY && moves) {
                        move_vertex(w, p, q, d == 0x4, section);
                    } else if (w->gpu) {
                        gpu_move(w);
                    }
                    w->run_vertex++;
                } else if (w->flat != NULL && (p->prim & 7) != 7) {
                    /* a 2D vertex: the first of its tag begins an item */
                    if (!p->item_open) {
                        int c = (w->prmodecont & 1) ? (int)((p->prim >> 9) & 1) : (int)((w->prmode >> 9) & 1);

                        p->item_open = 1;
                        w->item++;
                        w->item_vertex = 0;
                        if (w->mode == SCAN) {
                            flat_add_item(w->flat, (w->prmodecont & 1) ? p->prim : (p->prim & 7) | (uint32_t)(w->prmode & 0x7F8),
                                          w->tex0[c], (uint32_t)(w->frame[c] & 0x1FF));
                        }
                    }
                    if (w->mode == SCAN) {
                        keep_flat_vertex(w, p, q, d == 0x4);
                    } else if ((size_t)w->item < w->flat->nitems && w->item_vertex < w->flat->items[w->item].count &&
                               item_mesh(w, &w->flat->items[w->item])) {
                        if (w->gpu) {
                            gpu_mesh_vertex(w, p, q, d == 0x4, &w->flat->items[w->item]);
                        } else {
                            move_mesh_vertex(w, p, q, d == 0x4, &w->flat->items[w->item]);
                        }
                    } else if ((size_t)w->item < w->flat->nitems && w->gpu) {
                        gpu_flat_vertex(w, p);
                    } else if ((size_t)w->item < w->flat->nitems) {
                        move_flat_vertex(w, p, q);
                    } else if (w->gpu) {
                        gpu_move(w);
                    }
                    w->item_vertex++;
                } else if (w->mode == REPLAY && w->exact) {
                    exact_kick(w, p, q); /* (the 2D into the frame: not drawn either) */
                } else if (w->gpu) {
                    gpu_move(w);
                }
                p->st = NULL;
                p->rgbaq = NULL;
                p->uv = NULL;
                w->kick++; /* (the GS's vertex kicks, all of them: gsn.cpp numbers them alike) */
                break;
            case 0xC:
            case 0xD:
                /* (XYZF3, XYZ3: kicks too, never moved) */
                if (w->gpu) {
                    gpu_move(w);
                    p->st = NULL;
                    p->rgbaq = NULL;
                    p->uv = NULL;
                }
                w->kick++;
                break;
            case 0x6:
            case 0x7: {
                uint32_t reg = d;

                ad_write(w, p, &reg, &v[0]);
                memcpy(q, &v[0], 8);
                break;
            }
            case 0xE: {
                uint32_t reg = (uint32_t)(v[1] & 0xFF);

                if (reg == PORT_GS_REG_NATIVE_DRAW) {
                    /* native draw's marks: the PS2 path's packets of the native draws before them follow, for
                       the native GS to leave out when it draws the native ones (gsn_set_native_draw). Their
                       runs are not paired: the native draws are, by their identity (gsn_native_draw.inc) */
                    uint32_t command = RD_ND_COMMAND(v[0]);

                    if (command == RD_ND_ALT_BEGIN) {
                        w->alt = port_options.renderer == PORT_RENDERER_NATIVE &&
                                 port_options.native_draw_compare != PORT_ND_COMPARE_PS2;
                    } else if (command == RD_ND_ALT_END) {
                        w->alt = 0;
                    } else if (command == RD_ND_READS_SHOWN) {
                        w->native_reads_shown = 1; /* (native draw's screen pass, gsn_nd_pass.inc: no packet says so) */
                    } else if (command == RD_ND_DRAW && w->mode == REPLAY && w->exact && exact_frame(w)) {
                        /* (the exact restore: a native draw into the frame is left out, as the PS2 path's
                           vertices are: exact_kick) */
                        reg = PORT_GS_REG_FRAME_INTERP;
                        v[0] = (uint64_t)0xFF << 32;
                    }
                }

                if ((reg == 0x4 || reg == 0x5) && w->mode == REPLAY && w->exact && draws_frame(w, p->prim) && !exact_frame_drawn()) {
                    reg += 8; /* (XYZF3, XYZ3: exact_kick) */
                }
                if (reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD) {
                    if (w->gpu) {
                        gpu_move(w);
                    }
                    w->kick++; /* (a vertex kick by A+D: never moved) */
                }
                ad_write(w, p, &reg, &v[0]);
                v[1] = (v[1] & ~(uint64_t)0xFF) | reg;
                memcpy(q, v, 16);
                break;
            }
            }
            p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            p->left--;
        } else if (p->flg == 1) {
            int half;

            for (half = 0; half < 2 && p->dwords != 0; half++, p->dwords--) {
                uint32_t d = (uint32_t)(p->regs >> (p->reg * 4)) & 0xF;
                uint64_t v;

                memcpy(&v, q + half * 8, 8);
                if (d == 0x0 || d == 0x6 || d == 0x7) {
                    uint32_t reg = d;

                    ad_write(w, p, &reg, &v);
                    memcpy(q + half * 8, &v, 8);
                } else if (d == 0x4 || d == 0x5 || d == 0xC || d == 0xD) {
                    if (w->gpu) {
                        gpu_move(w);
                    }
                    w->kick++; /* (a vertex kick by REGLIST: never moved) */
                }
                p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            }
            p->left--;
        } else {
            n = count - i < p->left ? count - i : p->left;
            p->left -= (uint32_t)n;
            keep = !p->dropping;
        }
        if (keep) {
            if (out != i) {
                memmove(data + out * 16, data + i * 16, n * 16);
            }
            out += n;
        }
        i += n;
    }
    return out * 16;
}

/* ---- At a vblank ----------------------------------------------------------------------------
   The display registers of vblank n are those the vblank found, before the game swapped at it: the
   chain drawn after the game's swap at vblank 2k - 1 is kept by vblank 2k (where the frame it swapped
   to is first shown) as the pending frame (age 0), and stays pending through vblank 2k + 1 (age 1)
   when nothing else has been sent since and the same buffer is still shown. The pictures between are
   drawn from it: at vblank 2k after the vblank's own picture (the frame before, as the game has it),
   at 2k + 1 from its start; a picture at phase f of the vblank shows the time (age + f) / 2 of the way
   from the frame shown to the one drawn. */
typedef struct {
    uint8_t *data;
    size_t size;
    int age;          /* vblanks since it was kept */
    int cut;          /* the cameras are a cut apart: no pictures between */
    Walk scan;        /* what the walk found in it */
    uint32_t shown;   /* the buffer shown at the vblank that kept it */
    Camera before;    /* the camera of the frame shown then */
    int have_before;
    const Objects *objects, *objects_before; /* its runs, and the frame's before (NULL: none) */
    Flat *flat;                    /* its 2D items, paired with the frame before's */
    const Flat *flat_before;       /* (NULL: none) */
    int dirty;                     /* a picture between changed a buffer the next frame reads first */
    int reads_prev;                /* the frame blends in the frame shown (Game_BlendPreviousFrame) */
    int snapshot_sent;             /* and the native GS was asked to keep the frame shown */
    int gpu;                       /* its pictures between on the GPU ("On the GPU"): 1 handed over, -1 not */
    uint64_t gpu_serial;           /* (the frame's, for the native GS) */
} Pending;

/* The runs of the last frame drawn and of the one before, and one to walk the next into */
static Objects objects_pool[3];
static Objects *objects_new = &objects_pool[0], *objects_last = &objects_pool[1], *objects_before = &objects_pool[2];
static int objects_last_set, objects_before_set;
/* the same for the 2D items; and the uploads of the frame walked last, and of the pending one */
static Flat flat_pool[3];
static Flat *flat_new = &flat_pool[0], *flat_last = &flat_pool[1], *flat_before = &flat_pool[2];
static Uploads uploads_new_frame;
static Uploads *uploads_new = &uploads_new_frame;

static Pending pending; /* a frame drawn and not yet shown (size 0: none) */
static size_t pending_room;
static Camera last_camera; /* the camera of the last frame drawn */
static int last_camera_set;
static uint8_t *copy;
static size_t copy_room;
static uint64_t stat_replays, stat_cuts, stat_moved, stat_dropped, stat_objects[SECTIONS], stat_items, stat_left_out, stat_restores;
static uint64_t stat_ticks; /* the replays' walks, on this thread */
static uint64_t stat_gpu_frames, stat_gpu_pictures, stat_gpu_ticks; /* on the GPU: frames handed over, pictures */
static uint64_t stat_sent;  /* the bytes they sent */

void port_interp_forget(void) {
    last_camera_set = 0;
    objects_last_set = objects_before_set = 0;
    pending.size = 0;
    pending.have_before = 0;
    port_interp_reset(0);
}

static uint32_t gpu_kicks; /* the vertex kicks of the frame walked last on the GPU's way (replay_pending) */

/* KF4_INTERP_STILL=1, a check: the picture between is drawn with the camera of the frame drawn, so it
   must be that frame (the next vblank's picture) */
static int interp_still(void) {
    static int still = -1;

    if (still < 0) {
        still = getenv("KF4_INTERP_STILL") != NULL;
    }
    return still;
}

static void walk_all(Walk *w, uint8_t *data, size_t size, int mode, Objects *objects, Flat *flat, Uploads *uploads) {
    size_t at;

    memset(w, 0, sizeof(*w));
    w->mode = mode;
    w->prmodecont = 1;
    w->run = -1;
    w->item = -1;
    w->objects = objects;
    w->flat = flat;
    w->uploads = uploads;
    if (objects != NULL) {
        objects_reset(objects);
    }
    if (flat != NULL) {
        flat_reset(flat);
    }
    if (uploads != NULL) {
        uploads_reset(uploads);
    }
    for (at = 0; at < size;) {
        Record r;

        memcpy(&r, data + at, sizeof(r));
        walk_record(w, &r, data + at + sizeof(r));
        at += sizeof(r) + r.bytes;
    }
    if (objects != NULL) {
        objects_finish(objects);
    }
}

/* The pending frame again, into the buffer shown, seen from the camera between (t of the way); with t 1
   and exact, the game's frame as it was (port_interp_end_vblank) */
static void replay_pending(const Camera *between, double t, int exact, void (*send)(int path, const void *data, size_t bytes),
                           int gpu) {
    static Walk w; /* (large: kept off the stack) */
    const Walk *scan = &pending.scan;
    uint64_t fix[2 * 9];
    uint64_t start = port_options.stats ? port_ticks() : 0;
    size_t at;
    int n = 0, i;

    memset(&w, 0, sizeof(w));
    w.mode = REPLAY;
    w.prmodecont = 1;
    w.run = -1;
    w.t = interp_still() ? 1.0 : t;
    w.exact = exact;
    w.draw_fbp = scan->draw_fbp;
    w.shown_fbp = pending.shown;
    w.screen = scan->screen;
    w.screen_reflect = scan->screen_reflect;
    w.reflect_fbp = scan->reflect_fbp;
    w.have_reflect = scan->have_reflect && !exact;
    w.camera = scan->camera;
    w.mid = interp_still() || exact ? scan->camera : *between;
    w.objects = interp_still() || exact ? NULL : (Objects *)pending.objects;
    w.prev = pending.objects_before;
    w.item = -1;
    w.flat = interp_still() || exact || pending.flat_before == NULL ? NULL : pending.flat;
    w.copies = pending.flat;
    w.flat_prev = pending.flat_before;
    w.ask_uploads = port_gs_replay_uploads();
    w.gpu = gpu;
    for (i = 0; i < 4; i++) {
        w.paths[i].win[0] = w.paths[i].win[1] = GSN_IM_NONE;
    }
    {
        /* the native GS's marks around the picture between (it records the game's uploads, not these) */
        uint64_t mark_begin[4] = {1 | ((uint64_t)1 << 15) | ((uint64_t)1 << 60), 0xE, (uint64_t)0xF0 << 32,
                                  PORT_GS_REG_FRAME_INTERP};

        send(3, mark_begin, sizeof(mark_begin));
        if (pending.reads_prev && w.ask_uploads && !exact) {
            /* the frame shown, kept before the first picture between draws over it (the later ones read
               the copy too) */
            if (!pending.snapshot_sent) {
                uint64_t mark_keep[4] = {1 | ((uint64_t)1 << 15) | ((uint64_t)1 << 60), 0xE,
                                         ((uint64_t)0xF2 << 32) | pending.shown, PORT_GS_REG_FRAME_INTERP};

                send(3, mark_keep, sizeof(mark_keep));
                pending.snapshot_sent = 1;
            }
            w.prev_frame_kept = 1;
        }
    }
    for (i = 0; i < 3; i++) {
        w.eye_move[i] = scan->camera.eye[i] - pending.before.eye[i];
    }
    move_between(&scan->camera, &w.mid, &w.move);
    for (at = 0; at < pending.size;) {
        Record r;

        memcpy(&r, pending.data + at, sizeof(r));
        {
            /* a record that is all an upload's data: sent as it is, or left out when the native GS has
               the upload (no copy, no walk) */
            GifWalk *pw = &w.paths[r.what & 0xFF];
            size_t qwords = r.bytes / 16;

            if (pw->flg == 2 && pw->left >= qwords && qwords > 0 && !(r.what & RECORD_3D)) {
                pw->left -= (uint32_t)qwords;
                if (!pw->dropping) {
                    send((int)(r.what & 0xFF), pending.data + at + sizeof(r), r.bytes);
                    stat_sent += r.bytes;
                }
                at += sizeof(r) + r.bytes;
                continue;
            }
        }
        if (copy_room < r.bytes) {
            free(copy);
            copy_room = r.bytes * 2;
            copy = malloc(copy_room);
            if (copy == NULL) {
                PORT_FATAL("out of memory");
            }
        }
        memcpy(copy, pending.data + at + sizeof(r), r.bytes);
        {
            size_t bytes;

            w.record_reads_prev = 0;
            bytes = walk_record(&w, &r, copy);
            if (bytes != 0) {
                /* a record that reads the frame shown before (Game_BlendPreviousFrame's packet is one of
                   its own): between marks that have the native GS read its copy (take_snapshot) */
                uint64_t mark[4] = {1 | ((uint64_t)1 << 15) | ((uint64_t)1 << 60), 0xE, (uint64_t)0xF4 << 32,
                                    PORT_GS_REG_FRAME_INTERP};

                if (w.record_reads_prev) {
                    send(3, mark, sizeof(mark));
                }
                send((int)(r.what & 0xFF), copy, bytes);
                stat_sent += bytes;
                if (w.record_reads_prev) {
                    mark[2] = (uint64_t)0xF5 << 32;
                    send(3, mark, sizeof(mark));
                }
            }
        }
        at += sizeof(r) + r.bytes;
    }
    /* the registers the replay changed, back to what the game left in them (by PATH3: every path is
       between packets) */
    for (i = 0; i < 2; i++) {
        if (scan->frame_set[i]) {
            n++;
            fix[2 * n] = scan->frame[i];
            fix[2 * n + 1] = (uint64_t)(GS_FRAME_1 + i);
        }
        if (scan->tex0_set[i]) {
            n++;
            fix[2 * n] = scan->tex0[i] & ~((uint64_t)7 << 61); /* CLD 0: no CLUT load */
            fix[2 * n + 1] = (uint64_t)(GS_TEX0_1 + i);
        }
    }
    if (scan->bitbltbuf_set) {
        n++;
        fix[2 * n] = scan->bitbltbuf;
        fix[2 * n + 1] = GS_BITBLTBUF;
    }
    n++;
    fix[2 * n] = (uint64_t)0xF1 << 32; /* the end of the picture between */
    fix[2 * n + 1] = PORT_GS_REG_FRAME_INTERP;
    fix[0] = (uint64_t)n | ((uint64_t)1 << 15) | ((uint64_t)1 << 60); /* NLOOP n, EOP, PACKED, NREG 1 */
    fix[1] = 0xE;                                                       /* A+D */
    send(3, fix, (size_t)(n + 1) * 16);
    if (gpu) {
        gpu_kicks = w.kick;
        return;
    }
    stat_replays += !exact;
    stat_restores += exact;
    stat_items += w.items_moved;
    stat_left_out += w.upload_bytes_left_out;
    stat_moved += w.moved;
    stat_dropped += w.dropped;
    for (i = 0; i < SECTIONS; i++) {
        stat_objects[i] += w.objects_moved[i];
    }
    if (port_options.stats) {
        stat_ticks += port_ticks() - start;
    }
}

void port_interp_vsync(const uint64_t *privileged) {
    static Walk w; /* (large: kept off the stack) */
    /* the buffer shown: DISPFB2's, or DISPFB1's when only read circuit 1 is on */
    uint64_t dispfb = (privileged[0] & 2) ? privileged[0x90 / 8] : privileged[0x70 / 8];
    uint32_t shown = (uint32_t)(dispfb & 0x1FF);

    if (pending.size != 0) {
        /* the vblank after the one that kept it: nothing sent since, the same buffer shown */
        if (pending.age == 0 && kept_size == 0 && kept_whole && shown == pending.shown) {
            pending.age = 1;
        } else {
            pending.size = 0;
        }
    }
    if (!kept_whole || kept_size == 0) {
        return;
    }
    walk_all(&w, kept, kept_size, SCAN, objects_new, flat_new, uploads_new);
    if (!w.world_seen) {
        return;
    }
    {
        /* this frame's runs and items become the last frame's */
        Objects *free_one = objects_before;
        Flat *free_flat = flat_before;

        objects_before = objects_last;
        objects_before_set = objects_last_set;
        objects_last = objects_new;
        objects_last_set = 1;
        objects_new = free_one;
        flat_before = flat_last;
        flat_last = flat_new;
        flat_new = free_flat;
        flat_last->draw_fbp = w.draw_fbp;
        flat_last->shown_fbp = shown;
    }
    if (w.draw_fbp != 0xFFFFFFFFu && w.draw_fbp != shown && w.screen.sx != 0 && w.screen.sy != 0) {
        /* kept for the pictures between: the buffers swap, so the packets' becomes the pending one's */
        uint8_t *data = pending.data;
        size_t room = pending_room;

        pending.data = kept;
        pending_room = kept_room;
        pending.size = kept_size;
        pending.age = 0;
        kept = data;
        kept_room = room;
        kept_size = 0;
        pending.scan = w;
        pending.shown = shown;
        pending.before = last_camera;
        pending.have_before = last_camera_set;
        pending.objects = objects_last;
        pending.objects_before = objects_before_set ? objects_before : NULL;
        pending.flat = flat_last;
        pending.flat_before = objects_before_set ? flat_before : NULL;
        pending.dirty = 0;
        objects_pair(objects_last, pending.objects_before ? objects_before : NULL, w.camera.eye);
        pending.reads_prev = 0;
        pending.snapshot_sent = 0;
        pending.gpu = 0;
        {
            size_t i;

            for (i = 0; i < flat_last->nitems; i++) {
                const Item *it = &flat_last->items[i];

                pending.reads_prev |= (it->prim & 0x10) && it->fbp == w.draw_fbp && (uint32_t)(it->tex & 0x3FFF) == shown * 32;
            }
            pending.reads_prev |= w.native_reads_shown; /* (native draw) */
        }
        flat_pair(flat_last, pending.flat_before, uploads_new);
        flat_copies(flat_last, uploads_new);
        if (w.have_reflect && read_first(uploads_new, w.reflect_fbp)) {
            flat_last->restore = 1; /* (the reflection drawn from the camera between: as for the 2D) */
        }
        {
            Camera between;

            pending.cut = pending.have_before && !camera_between(&pending.before, &w.camera, 0.5, &between);
            stat_cuts += pending.cut;
        }
    }
    last_camera = w.camera;
    last_camera_set = 1;
}

int port_interp_active(void) {
    return pending.size != 0 && pending.have_before && !pending.cut;
}

/* The pictures between on the GPU ("On the GPU"): with the native GS alone (it asks for the uploads
   again), not KF4_INTERP_STILL's check, and not KF4_INTERP_CPU */
static int gpu_wanted(void) {
    static int cpu = -1;

    if (cpu < 0) {
        cpu = getenv("KF4_INTERP_CPU") != NULL;
    }
    return !cpu && !interp_still() && port_gs_interp_gpu();
}

int port_interp_on_gpu(void) {
    return gpu_wanted();
}

/* The packets of a REPLAY on the GPU's way, gathered for the native GS: a uint32_t path and size, then
   the bytes */
static uint8_t *gpu_stream;
static size_t gpu_stream_size, gpu_stream_room;

static void gpu_send(int path, const void *data, size_t bytes) {
    uint32_t head[2] = {(uint32_t)path, (uint32_t)bytes};

    if (gpu_stream_size + 8 + bytes > gpu_stream_room) {
        size_t room = (gpu_stream_size + 8 + bytes) * 2;
        uint8_t *grown = realloc(gpu_stream, room);

        if (grown == NULL) {
            PORT_FATAL("out of memory");
        }
        gpu_stream = grown;
        gpu_stream_room = room;
    }
    memcpy(gpu_stream + gpu_stream_size, head, 8);
    memcpy(gpu_stream + gpu_stream_size + 8, data, bytes);
    gpu_stream_size += 8 + bytes;
    stat_sent += bytes;
}

/* The frame for the GPU: walked once (replay_pending's way, the vertices left as they are and their
   moves noted), handed over with its packets. 0 if it cannot be. */
static int gpu_frame(const Camera *between, double t, void (*send)(int path, const void *data, size_t bytes)) {
    static uint64_t serial;
    const Objects *o = pending.objects;
    uint64_t start = port_options.stats ? port_ticks() : 0;
    GsnInterpFrame *f;

    if (o != NULL && gpu_run_room < o->nruns) {
        free(gpu_run_ok);
        gpu_run_room = o->nruns * 2;
        gpu_run_ok = malloc(gpu_run_room);
        if (gpu_run_ok == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    if (o != NULL && o->nruns) {
        memset(gpu_run_ok, 0, o->nruns);
    }
    if (pending.reads_prev && port_gs_replay_uploads() && !pending.snapshot_sent) {
        /* the frame shown, kept before the first picture between draws over it (as replay_pending) */
        uint64_t mark_keep[4] = {1 | ((uint64_t)1 << 15) | ((uint64_t)1 << 60), 0xE,
                                 ((uint64_t)0xF2 << 32) | pending.shown, PORT_GS_REG_FRAME_INTERP};

        send(3, mark_keep, sizeof(mark_keep));
        pending.snapshot_sent = 1;
    }
    gpu_stream_size = 0;
    gpu_kicks = 0;
    replay_pending(between, t, 0, gpu_send, 1);
    f = calloc(1, sizeof(*f));
    if (f == NULL) {
        PORT_FATAL("out of memory");
    }
    f->serial = ++serial;
    f->screen[0][0] = (float)pending.scan.screen.sx;
    f->screen[0][1] = (float)pending.scan.screen.sy;
    f->screen[0][2] = (float)pending.scan.screen.cx;
    f->screen[0][3] = (float)pending.scan.screen.cy;
    f->screen[0][4] = (float)pending.scan.screen.cz;
    f->screen[1][0] = (float)pending.scan.screen_reflect.sx;
    f->screen[1][1] = (float)pending.scan.screen_reflect.sy;
    f->screen[1][2] = (float)pending.scan.screen_reflect.cx;
    f->screen[1][3] = (float)pending.scan.screen_reflect.cy;
    f->screen[1][4] = (float)pending.scan.screen_reflect.cz;
    {
        /* (the native GS bounds the copies its blending reads by where the vertices are at t 0 and 1) */
        Move move0;
        int i;

        move_between(&pending.scan.camera, &pending.before, &move0);
        for (i = 0; i < 3; i++) {
            f->move0[i * 4 + 0] = (float)(move0.r[i][0] - (i == 0));
            f->move0[i * 4 + 1] = (float)(move0.r[i][1] - (i == 1));
            f->move0[i * 4 + 2] = (float)(move0.r[i][2] - (i == 2));
            f->move0[i * 4 + 3] = (float)move0.t[i];
            f->mid0[i * 4 + 0] = (float)pending.before.r[i][0];
            f->mid0[i * 4 + 1] = (float)pending.before.r[i][1];
            f->mid0[i * 4 + 2] = (float)pending.before.r[i][2];
            f->mid0[i * 4 + 3] = (float)pending.before.t[i];
        }
    }
    f->kicks = gpu_kicks;
    f->runs = o != NULL ? (uint32_t)o->nruns : 0;
    /* (the camera did not move: the same fields the game wrote, so every camera between is this one) */
    f->still = memcmp(pending.before.r, pending.scan.camera.r, sizeof(pending.before.r)) == 0 &&
               memcmp(pending.before.t, pending.scan.camera.t, sizeof(pending.before.t)) == 0;
    /* (the native GS frees what it is handed: these start again) */
    f->moves = gpu_moves;
    f->stream = gpu_stream;
    f->stream_bytes = gpu_stream_size;
    gpu_moves = NULL;
    gpu_moves_room = 0;
    gpu_stream = NULL;
    gpu_stream_size = gpu_stream_room = 0;
    pending.gpu_serial = f->serial;
    port_gs_interp_frame(f);
    stat_gpu_frames++;
    if (port_options.stats) {
        stat_ticks += port_ticks() - start;
    }
    return 1;
}

/* A picture between on the GPU: its time, its cameras, and the runs that move with their model then */
static void gpu_picture(const Camera *between, double t) {
    static uint32_t *bits;
    static size_t bits_room;
    const Objects *o = pending.objects;
    const Camera *c = &pending.scan.camera;
    uint64_t start = port_options.stats ? port_ticks() : 0;
    GsnInterpPicture pic;
    Move move;
    size_t r, words = o != NULL ? (o->nruns + 31) / 32 : 0;
    int i;

    move_between(c, between, &move);
    for (i = 0; i < 3; i++) {
        /* (the rotation less the identity: the floats' precision is the move's) */
        pic.move[i * 4 + 0] = (float)(move.r[i][0] - (i == 0));
        pic.move[i * 4 + 1] = (float)(move.r[i][1] - (i == 1));
        pic.move[i * 4 + 2] = (float)(move.r[i][2] - (i == 2));
        pic.move[i * 4 + 3] = (float)move.t[i];
        pic.mid[i * 4 + 0] = (float)between->r[i][0];
        pic.mid[i * 4 + 1] = (float)between->r[i][1];
        pic.mid[i * 4 + 2] = (float)between->r[i][2];
        pic.mid[i * 4 + 3] = (float)between->t[i];
    }
    if (bits_room < words + 1) {
        free(bits);
        bits_room = words * 2 + 1;
        bits = malloc(bits_room * 4);
        if (bits == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    memset(bits, 0, (words + 1) * 4);
    for (r = 0; o != NULL && r < o->nruns && r < gpu_run_room; r++) {
        const Run *run = &o->runs[r];

        if (gpu_run_ok[r] && ((run->sky && o->sky_ok) || (run->pair >= 0 && pending.objects_before != NULL &&
                                                         gpu_zm_ok(o, pending.objects_before, run, c, between, t)))) {
            bits[r >> 5] |= 1u << (r & 31);
        }
    }
    pic.serial = pending.gpu_serial;
    pic.t = (float)t;
    pic.runs = o != NULL ? (uint32_t)o->nruns : 0;
    pic.run_bits = bits;
    port_gs_interp_picture(&pic);
    stat_gpu_pictures++;
    stat_replays++;
    if (port_options.stats) {
        stat_gpu_ticks += port_ticks() - start;
    }
}

/* ---- A check: pictures between moved outside their frames (KF4_INTERP_OUTSIDE) -------------------
   For each run of the world placed in the pending frame, where its vertices are on the screen at the
   picture's time t (moved as the replay moves them: with their model, paired and moving, else with the
   camera), against where they were at t 0 (the frame shown: the pair's own places from the camera
   before, or this frame's places from it when not paired) and at t 1 (the frame drawn). A run whose box
   at t is outside both, by more than a margin, is logged (vblank, phase, the run, its pair, the boxes):
   a picture between drew something where neither frame had it. KF4_INTERP_OUTSIDE=2 also logs every
   shadow run (program 0x5A0). The log goes to stderr, or to the file KF4_INTERP_OUTSIDE_LOG names. */
typedef struct {
    double x0, y0, x1, y1;
    int n;
} OutBox;

static void outbox_add(OutBox *b, const Screen *s, const Camera *c, const double p[3]) {
    double v[3], x, y;
    int i;

    for (i = 0; i < 3; i++) {
        v[i] = c->r[i][0] * p[0] + c->r[i][1] * p[1] + c->r[i][2] * p[2] + c->t[i];
    }
    if (!(v[2] > 1e-6)) {
        return;
    }
    x = (s->sx * v[0] / v[2] + s->cx) - 2048.0;
    y = (s->sy * v[1] / v[2] + s->cy) - 2048.0;
    if (!(fabs(x) <= 2047.0 && fabs(y) <= 2047.0)) {
        return;
    }
    if (b->n == 0) {
        b->x0 = b->x1 = x;
        b->y0 = b->y1 = y;
    } else {
        b->x0 = x < b->x0 ? x : b->x0;
        b->x1 = x > b->x1 ? x : b->x1;
        b->y0 = y < b->y0 ? y : b->y0;
        b->y1 = y > b->y1 ? y : b->y1;
    }
    b->n++;
}

/* the screen seen, about the centre (X and Y less 2048, GS pixels), and a little more */
#define VIS_X 360.0
#define VIS_Y 260.0
static void outbox_clip(OutBox *b) {
    b->x0 = b->x0 < -VIS_X ? -VIS_X : b->x0 > VIS_X ? VIS_X : b->x0;
    b->x1 = b->x1 < -VIS_X ? -VIS_X : b->x1 > VIS_X ? VIS_X : b->x1;
    b->y0 = b->y0 < -VIS_Y ? -VIS_Y : b->y0 > VIS_Y ? VIS_Y : b->y0;
    b->y1 = b->y1 < -VIS_Y ? -VIS_Y : b->y1 > VIS_Y ? VIS_Y : b->y1;
}

static void outside_check(const Camera *between, double t, double phase, int gpu) {
    static int mode = -1;
    static FILE *log;
    static Walk w; /* (run_motion's, gpu_run_static's) */
    const Objects *o = pending.objects, *before = pending.objects_before;
    const Screen *s = &pending.scan.screen;
    size_t r;

    if (mode < 0) {
        const char *e = getenv("KF4_INTERP_OUTSIDE"), *f = getenv("KF4_INTERP_OUTSIDE_LOG");

        mode = e != NULL ? atoi(e) : 0;
        log = f != NULL ? fopen(f, "w") : NULL;
        if (log == NULL) {
            log = stderr;
        }
    }
    if (mode <= 0 || o == NULL) {
        return;
    }
    memset(&w, 0, sizeof(w));
    w.objects = (Objects *)o;
    w.prev = before;
    w.camera = pending.scan.camera;
    w.mid = *between;
    w.t = t;
    {
        int i;

        for (i = 0; i < 3; i++) {
            w.eye_move[i] = pending.scan.camera.eye[i] - pending.before.eye[i];
        }
    }
    for (r = 0; r < o->nruns; r++) {
        const Run *cur = &o->runs[r], *prev;
        OutBox b0 = {0}, b1 = {0}, bt = {0};
        double m = 4.0, d = 0;
        int moving, flag, i;
        uint32_t k;

        if (cur->section != SECTION_WORLD || !cur->placed || cur->native) {
            continue;
        }
        w.run = (int)r;
        w.run_prev = before != NULL && cur->pair >= 0 ? &before->runs[cur->pair] : NULL;
        if (gpu) {
            moving = gpu_run_static(&w) && ((cur->sky && o->sky_ok) ||
                                            (w.run_prev != NULL && gpu_zm_ok(o, before, cur, &w.camera, between, t)));
        } else {
            run_motion(&w);
            moving = w.run_moving && (w.run_prev != NULL || run_is_sky(&w));
        }
        prev = w.run_prev;
        for (k = 0; k < cur->count; k++) {
            const double *b = o->pos[cur->first + k];
            double p[3];

            if (!o->valid[cur->first + k]) {
                continue;
            }
            outbox_add(&b1, s, &pending.scan.camera, b);
            if (prev != NULL && before->valid[prev->first + k]) {
                outbox_add(&b0, s, &pending.before, before->pos[prev->first + k]);
            } else if (prev == NULL) {
                outbox_add(&b0, s, &pending.before, b);
            }
            if (moving && cur->sky && o->sky_ok) {
                for (i = 0; i < 3; i++) {
                    p[i] = b[i] - o->sky_move[i] * (1.0 - t);
                }
            } else if (moving && prev != NULL) {
                const double *a = before->pos[prev->first + k];
                int straight = before->valid[prev->first + k] == VERTEX_SURE && o->valid[cur->first + k] == VERTEX_SURE;

                if (!straight) {
                    double ra = distance(a, prev->middle), rb = distance(b, cur->middle);
                    double most = ra > rb ? ra : rb;

                    straight = before->valid[prev->first + k] && fabs(ra - rb) <= most * 0.25 + 2.0;
                }
                for (i = 0; i < 3; i++) {
                    p[i] = straight ? a[i] + (b[i] - a[i]) * t : b[i] + (prev->middle[i] - cur->middle[i]) * (1.0 - t);
                }
            } else {
                memcpy(p, b, sizeof(p));
            }
            outbox_add(&bt, s, between, p);
        }
        if (bt.n == 0 || bt.x1 < -VIS_X || bt.x0 > VIS_X || bt.y1 < -VIS_Y || bt.y0 > VIS_Y) {
            continue; /* (not on the screen) */
        }
        outbox_clip(&bt);
        outbox_clip(&b0);
        outbox_clip(&b1);
        flag = 0;
        if (b0.n == 0 && b1.n == 0) {
            flag = 1;
        } else {
            double x0 = b1.n ? b1.x0 : b0.x0, x1 = b1.n ? b1.x1 : b0.x1, y0 = b1.n ? b1.y0 : b0.y0, y1 = b1.n ? b1.y1 : b0.y1;

            if (b0.n) {
                x0 = b0.x0 < x0 ? b0.x0 : x0;
                x1 = b0.x1 > x1 ? b0.x1 : x1;
                y0 = b0.y0 < y0 ? b0.y0 : y0;
                y1 = b0.y1 > y1 ? b0.y1 : y1;
            }
            flag = bt.x0 < x0 - m || bt.x1 > x1 + m || bt.y0 < y0 - m || bt.y1 > y1 + m;
        }
        if (prev != NULL) {
            for (i = 0; i < 3; i++) {
                d += (prev->middle[i] - cur->middle[i]) * (prev->middle[i] - cur->middle[i]);
            }
            d = sqrt(d);
        }
        if (flag || (mode >= 2 && cur->shadow)) {
            fprintf(log,
                    "[outside] %s vblank %llu phase %.3f t %.3f run %zu key %016llx n %u%s%s pair %d (%.1f apart) %s "
                    "t: %.0f %.0f %.0f %.0f | t0: %.0f %.0f %.0f %.0f | t1: %.0f %.0f %.0f %.0f\n",
                    flag ? "OUT" : "ok", (unsigned long long)port_vblank_count(), phase, t, r, (unsigned long long)cur->key,
                    cur->count, cur->shadow ? " shadow" : "", cur->sky ? " sky" : "", cur->pair, d,
                    moving ? "moving" : "with-camera", bt.x0, bt.y0, bt.x1, bt.y1, b0.x0, b0.y0, b0.x1, b0.y1, b1.x0, b1.y0,
                    b1.x1, b1.y1);
        }
    }
    fflush(log);
}

int port_interp_picture(double phase, void (*send)(int path, const void *data, size_t bytes)) {
    Camera between;
    double t;

    if (!port_interp_active()) {
        return 0;
    }
    t = (pending.age + phase) * 0.5;
    if (t <= 0 || t >= 1) {
        return 0; /* the frame shown, as the game drew it */
    }
    if (!camera_between(&pending.before, &pending.scan.camera, t, &between)) {
        return 0;
    }
    if (pending.gpu == 0) {
        pending.gpu = gpu_wanted() && gpu_frame(&between, t, send) ? 1 : -1;
    }
    outside_check(&between, t, phase, pending.gpu > 0);
    if (pending.gpu > 0) {
        gpu_picture(&between, t);
    } else {
        replay_pending(&between, t, 0, send, 0);
    }
    pending.dirty |= pending.flat != NULL && ((pending.flat->restore && pending.flat_before != NULL) || pending.flat->restore_copy);
    return 1;
}

/* After the vblank's pictures between: a buffer other than the frame that a picture between drew
   differently (a scrolling texture's) and that the game's next frame reads before it draws it, is
   drawn again as the game's frame left it: the frame once more, exact, into the buffer shown, which
   the next frame clears. (None of the maps seen so far needs it: they draw those buffers before they
   read them.) Also after a frame with a COPY_RESTORE copy (the map change's last frame): the uploads go
   again in their order, then the copy, from the game's frame. */
void port_interp_end_vblank(void (*send)(int path, const void *data, size_t bytes)) {
    if (pending.size != 0 && pending.age == 1 && pending.dirty) {
        replay_pending(&pending.scan.camera, 1.0, 1, send, 0);
        pending.dirty = 0;
    }
}

void port_interp_print_stats(void) {
    fprintf(stderr,
            "[stats] frame rate %d: %llu pictures between drawn (%.3f s walking the packets; %.1f MB of uploads left "
            "out), %llu cuts not, %llu frames drawn again for buffers they changed; %llu vertices moved (with their "
            "model: %llu in the world, %llu of the arm, %llu before the eye), %llu dropped; %llu 2D vertices moved\n",
            port_frame_rate_hz(), (unsigned long long)stat_replays, (double)stat_ticks / port_ticks_per_second(),
            (double)stat_left_out / 1048576.0, (unsigned long long)stat_cuts, (unsigned long long)stat_restores,
            (unsigned long long)stat_moved, (unsigned long long)stat_objects[SECTION_WORLD],
            (unsigned long long)stat_objects[SECTION_ARM], (unsigned long long)stat_objects[SECTION_EYE],
            (unsigned long long)stat_dropped, (unsigned long long)stat_items);
    fprintf(stderr, "[stats] frame rate: the pictures between sent %.1f MB of packets; of %llu model parts placed, %llu "
            "paired by their batch and %llu by their order\n", (double)stat_sent / 1048576.0, (unsigned long long)stat_runs,
            (unsigned long long)stat_runs_key, (unsigned long long)stat_runs_order);
    if (unlocked_on) {
        fprintf(stderr, "[stats] frame rate unlocked: %.0f a second at the end (between %.0f and %.0f), %llu times down "
                "as the renderer fell behind, %llu up\n", unlocked.rate_hz, unlocked.floor_hz, unlocked.ceiling_hz,
                (unsigned long long)unlocked.falls, (unsigned long long)unlocked.rises);
    }
    if (stat_gpu_frames) {
        fprintf(stderr, "[stats] frame rate on the GPU: %llu frames handed over, %llu pictures between (%.3f s on this thread, "
                "%.1f us a picture)\n", (unsigned long long)stat_gpu_frames, (unsigned long long)stat_gpu_pictures,
                (double)stat_gpu_ticks / port_ticks_per_second(),
                stat_gpu_pictures ? (double)stat_gpu_ticks / port_ticks_per_second() * 1e6 / (double)stat_gpu_pictures : 0.0);
    }
}
