/* nd_water.c: PC-only. The reflection's water and the wave UVs, natively (--native-draw water; P6 of
   docs/port/NATIVE_DRAW_TASKS.md, NATIVE_DRAW.md 1.2.7 and 1.2.8).

   The rippled water (P6-1). On the maps with a reflection (record flag 0x200: 008, 009, 014) the world is
   first drawn mirrored into the 256x256 CT16S target at 0x2BC0 (GameLoop_DrawFrame's mode 2: those draws
   are the map's, objects', entities' ..., each of its own kind; the target stays the GS's, hosted). Then the
   real frame, where Map_DrawRecords and Map_DrawLateRecords draw the water record twice: first
   Wobble_DrawRippledModel (gfx/wobble.c), which projects each vertex of the record on the EE (VU0's
   arithmetic) and again moved up or down by one of 32 sines (wobble_sines, a phase that steps each frame)
   picked by the vertex's x and z, and sends a PATH2 strip of each block: the vertex where it is, with
   texture coordinates the moved one's place on the screen (Q 1), the reflection's target as the texture
   (bilinear, clamped, ALPHA 0x44) and the map's tint as the colour; then the record itself
   (draw_parts, additive, as a map record).

   Natively the strip is a draw of its own with an identity (RD_KIND_WATER: the model, the part) and
   program RD_PROGRAM_RIPPLE: the renderer makes each corner on the GPU from the part's mesh (the PC renderer:
   port/render/ripple.hpp) (the record's own blocks, cached: only the positions and the
   data's ADC are read), the part's matrix and the 32 sines, in the EE's arithmetic, so the game's frames are
   the PS2 path's bit for bit; and in a picture between the place and the texture coordinates are made by the
   camera between, so the water's reflection moves with the camera (the PS2 path's strip keeps the frame's
   texture coordinates while its corners move: its reflection slides over the water). The GS registers the
   strip's first and last packets write (TEX0_1, TEX1_1, ALPHA_1, CLAMP_1, RGBAQ; CLAMP_1 again) are queued as
   they are. Wobble_DrawRippledModel still runs (the call site is its last line, where it appends its
   packets): VU0's registers and the packet buffer are left as the PS2 path leaves them.

   Nothing here changes what the game does: it reads the game's state and the packets the game built,
   writes only the packet chain, and with the switch off is never called. */
#include "common.h"
#include "model/model.h"
#include "gfx/gs.h"
#include "gfx/screen.h"
#include "map/map.h"
#include "port/render.h"
#include "port/vu_float.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Packet_BeginGsRegisters(void);
void Packet_AddGsRegister(int reg, s64 value);
void Packet_EndGsRegisters(void);
void Packet_AppendToChain(u32 *start, u32 *end);
int NativeDraw_PartMeshes(ModelPart *part, RdMesh *meshes); /* (native_draw.c) */
u32 NativeDraw_CurrentCamera(void);

extern f32 world_screen_matrix[16]; /* (what Wobble_DrawRippledModel multiplies by) */
extern int port_fpu_round_nearest;  /* (--fpu-round nearest: port/include/port/platform.h) */

#define ND_MAX_MESHES 2048 /* (native_draw.c's: ModelPart.data_count's 11 bits) */

/* Matrix_Multiply(dst, matrix, src) (math/vu0.c) without VU0's registers: each of src's columns by matrix, as
   MULAx ACC; MADDAy; MADDAz; MADDw in VU arithmetic (port/vu_float.h), the same bits */
static void vu_matrix_multiply(f32 *dst, const f32 *matrix, const f32 *src) {
    f32 out[16];
    int col;
    int row;

    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            f32 acc = Vu_FMul(matrix[row], src[col * 4 + 0]);

            acc = Vu_FAdd(acc, Vu_FMul(matrix[4 + row], src[col * 4 + 1]));
            acc = Vu_FAdd(acc, Vu_FMul(matrix[8 + row], src[col * 4 + 2]));
            out[col * 4 + row] = Vu_FAdd(acc, Vu_FMul(matrix[12 + row], src[col * 4 + 3]));
        }
    }
    memcpy(dst, out, sizeof(out));
}

/* ---- KF4_ND_WATER_CHECK=1 (debug only): the game's own strips against the native draw's maths ----------
   The water draw (RD_PROGRAM_RIPPLE) makes each vertex's place, ADC, S and T from the part's mesh;
   this makes them the same way here, from the block, and compares them with what Wobble_DrawRippledModel
   wrote into its packets, word for word: a difference is printed (the first few) and counted. */
static void check_place(const f32 *vm, f32 x, f32 y, f32 z, s32 out[3]) {
    f32 p[4] = {x, y, z, 1.0f};
    u32 q;
    f32 c[4];
    int row;

    for (row = 0; row < 4; row++) {
        f32 acc = Vu_FMul(vm[row], p[0]);

        acc = Vu_FAdd(acc, Vu_FMul(vm[4 + row], p[1]));
        acc = Vu_FAdd(acc, Vu_FMul(vm[8 + row], p[2]));
        c[row] = Vu_FAdd(acc, Vu_FMul(vm[12 + row], p[3]));
    }
    q = vu_float_div(0x3F800000u, vu_float_bits(c[3]));
    for (row = 0; row < 3; row++) {
        out[row] = vu_float_ftoi(vu_float_mul(vu_float_bits(c[row]), q), 4);
    }
}

static void check_strips(ModelInstance *model, const u32 *packet_start, const f32 *sines) {
    static int checked, differed, next_report;
    const u32 *packet = packet_start + 9 * 4; /* (the first strip's packet) */
    ModelPart *part = model->parts;
    int i;
    int j;

    for (i = 0; i < model->part_count; i++, part++) {
        f32 m[16];
        f32 vm[16];
        ModelPartData *data = EE_PTR(ModelPartData *, part->data_addr);

        vu_matrix_multiply(m, (const f32 *)model->model_matrix, (const f32 *)part);
        vu_matrix_multiply(vm, world_screen_matrix, m);
        for (j = 0; j < part->data_count; j++) {
            int n = data->vertex_count & 0x7FFF;
            const ModelPartVertex *vertex = data->vertices;
            const u32 *out = packet + 12;
            u32 window = 0;
            int k;

            for (k = 0; k < n; k++, vertex++, out += 8) {
                s32 p[3];
                s32 w[3];
                f32 lift;
                f32 y;
                f32 st[2];
                u32 flags;
                int index = vertex->x + vertex->z;
                int bad;

                check_place(vm, vertex->x, vertex->y, vertex->z, p);
                p[2] >>= 4;
                window |= (u32)p[2] > 0xFFFF0 || ((p[0] | p[1]) & 0xFFFF0000);
                flags = (window != 0 ? 0x8000u : 0) | vertex->flags;
                window = (window << 1) & 7;
                index = (index * index * index) % 32;
                if (index < 0) {
                    index = -index;
                }
                lift = sines[index] * 5.0f;
                y = vertex->y + lift;
                check_place(vm, vertex->x, y, vertex->z, w);
                st[0] = (f32)(w[0] - 0x6C00) / 10240.0f;
                st[1] = (f32)(w[1] - 0x7200) / 7168.0f;
                bad = memcmp(st, out, 8) != 0 || (s32)out[4] != p[0] || (s32)out[5] != p[1] || (s32)out[6] != p[2] ||
                      ((out[7] ^ flags) & 0x8000) != 0;
                checked++;
                if (bad && differed++ < 20) {
                    f32 gst[2];

                    memcpy(gst, out, 8);
                    fprintf(stderr, "[nd water check] model %#x part %d block %d vertex %d: game %d %d %d %#x %.9g %.9g, here %d %d %d %#x %.9g %.9g\n",
                            (u32)EE_WORD(model), i, j, k, (s32)out[4], (s32)out[5], (s32)out[6], out[7], gst[0], gst[1], p[0], p[1],
                            p[2], flags, st[0], st[1]);
                }
            }
            packet += (n * 2 + 3) * 4;
            data = (ModelPartData *)((char *)data + (data->length + 1) * 0x10);
        }
    }
    if (checked >= next_report) {
        fprintf(stderr, "[nd water check] %d vertices checked, %d differ\n", checked, differed);
        next_report = checked + 100000;
    }
}

static u64 word64(const u32 *at) {
    u64 v;

    memcpy(&v, at, 8);
    return v;
}

/* Wobble_DrawRippledModel's packets, from packet_start to packet_end (the tail it would append), natively:
   its first packet's A+D writes (TEX0_1, TEX1_1, ALPHA_1, CLAMP_1) and RGBAQ (the tint, PACKED, Q 1 as the
   tag leaves it) and its last packet's (CLAMP_1) queued as GS registers, and between them a draw of
   RD_PROGRAM_RIPPLE for each part. sines: wobble_sines (32). 1 if done (the packets themselves are not
   appended); 0 if not, and nothing was queued: the caller appends them. */
int NativeDraw_RippledModel(ModelInstance *model, u32 *packet_start, u32 *packet_end, const f32 *sines) {
    static RdMesh meshes[ND_MAX_MESHES];
    const u32 *end = packet_end - 16; /* (the last packet: a CNT of 3 qwords, DIRECT 2, one A+D write) */
    ModelPart *part;
    RdDraw d;
    u32 rgba;
    int i;

    /* the packets as Wobble_DrawRippledModel builds them (anything else is left to it) */
    if (packet_start[0] != 0x10000008 || packet_start[11] != 0x51000006 || word64(packet_start + 12) != 0x5000000000008001ull ||
        word64(packet_start + 14) != 0x1EEEEull || end[0] != 0x10000003 || end[7] != 0x51000002 ||
        word64(end + 8) != 0x1000000000008001ull || word64(end + 10) != 0xEull) {
        return 0;
    }
    if (getenv("KF4_ND_WATER_CHECK")) {
        check_strips(model, packet_start, sines);
    }
    /* every part's blocks first: drawn one way or the other, whole */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        if (part->data_count > ND_MAX_MESHES || !NativeDraw_PartMeshes(part, meshes)) {
            return 0;
        }
    }
    Packet_BeginGsRegisters();
    for (i = 0; i < 4; i++) {
        Packet_AddGsRegister((int)(word64(packet_start + 18 + i * 4) & 0xFF), (s64)word64(packet_start + 16 + i * 4));
    }
    rgba = (packet_start[32] & 0xFF) | (packet_start[33] & 0xFF) << 8 | (packet_start[34] & 0xFF) << 16 | (packet_start[35] & 0xFF) << 24;
    Packet_AddGsRegister(GS_REG_RGBAQ, (s64)((u64)rgba | (u64)0x3F800000u << 32));
    Packet_EndGsRegisters();
    memset(&d, 0, sizeof(d));
    d.kind = RD_KIND_WATER;
    d.id[0] = (u32)EE_WORD(model);
    d.program = RD_PROGRAM_RIPPLE;
    d.flags = port_fpu_round_nearest ? RD_DRAW_ROUND_NEAREST : 0;
    d.camera = NativeDraw_CurrentCamera();
    memcpy(d.point_pos, sines, 16 * sizeof(f32));
    memcpy(d.point_col, sines + 16, 16 * sizeof(f32));
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        u64 value;

        NativeDraw_PartMeshes(part, meshes); /* (found again in the cache) */
        vu_matrix_multiply(&d.model[0][0], (const f32 *)model->model_matrix, (const f32 *)part);
        d.id[1] = (u32)i;
        value = rd_draw(&d, meshes, part->data_count);
        if (value != 0) {
            Packet_BeginGsRegisters();
            Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)value);
            Packet_EndGsRegisters();
        }
    }
    Packet_BeginGsRegisters();
    Packet_AddGsRegister((int)(word64(end + 14) & 0xFF), (s64)word64(end + 12));
    Packet_EndGsRegisters();
    return 1;
}

/* ---- The wave UVs on the GPU (P6-2, --native-wave-uvs) ------------------------------------------------
   A map record with flag 0x100 (maps 027, 029, 034, 039, 040, 042, 114) has a table of its vertices' own
   texture coordinates and waves (ModelWaveUv, Wobble_BuildWaveUvs), and func_00112BD0 has
   Wobble_ApplyWaveUvs write each vertex's coordinates as its own plus its wave's offset (one of 16
   oscillators, Wobble_StepOscillators) every frame it puts the record in the draw list. The game keeps
   doing so (the PS2 path and the CPU's near cut read them). Natively (PORT_WAVE_UVS_GPU, the default) the
   record's meshes are made from copies of its blocks that hold the vertices' own coordinates and, in the
   ST's w (which program 0x000 does not read), the vertex's wave: the same bytes every frame, so a block is
   parsed and put on the GPU once instead of every frame (P0-3: up to 201 blocks a frame on 042). The draw
   carries the 16 offsets of the frame (RD_DRAW_WAVE_UVS, in RdDraw.parent), which the renderer's shader
   adds as Wobble_ApplyWaveUvs adds them (16 bits). The offsets are those Wobble_ApplyWaveUvs used last for
   the model (NativeDraw_WaveUvsApplied keeps them: every record in the draw list had it this frame), so the
   picture is the frame the game drew, as PORT_WAVE_UVS_CPU's is.

   PORT_WAVE_UVS_LATE draws the offsets the oscillators will have after their next step instead: the PS2
   path's picture as its DMA mostly reads these blocks, after the next frame's update has written them
   (P2-6: Packet_SendChain returns, the game goes on to its next update, and the DMA thread reads the
   REF'd blocks meanwhile). Which of the two the native default is, is the owner's call. */

/* Who last wrote a model's blocks' coordinates: records can share their model's data (the same model of the
   map file, set up for each), each with its own table (Wobble_BuildWaveUvs picks the waves by the
   record's place), so the blocks hold what the last of them to be applied wrote, and every record drawn
   from them shows that (the PS2 path's too). A snapshot is kept by the data (its first part's first
   block's address): the writer's table and the offsets it was applied with, now (this frame's) and late
   (the next step's). */
typedef struct {
    u32 data;
    s32 part_count;
    const ModelWaveUv *table;
    u32 now[16];
    u32 late[16];
    u32 frame; /* the frame it was applied for (wave_frame's, NativeDraw_WaveFrame) */
} WaveSnapshot;

#define WAVE_SNAPSHOTS 64
static WaveSnapshot wave_snapshots[WAVE_SNAPSHOTS];
static int wave_next;

/* The frames drawn (NativeDraw_BeginFrame, at the start of GameLoop_DrawFrame). A snapshot is applied in the
   update before the frame it is drawn in (func_00112BD0), so it is for the frame wave_frame + 1. One kept
   from an earlier frame is not used (NativeDraw_WaveOffsets): after a map change a model of the new map at
   the same address with the same part count would find the old map's table and offsets; the record is
   drawn from the coordinates the game wrote instead (as --native-wave-uvs cpu). */
static u32 wave_frame;

void NativeDraw_WaveFrame(void) {
    wave_frame++;
}

/* a model's snapshot, or NULL */
static WaveSnapshot *wave_snapshot(const ModelInstance *model) {
    int i;

    if (model->parts == NULL || model->part_count == 0) {
        return NULL;
    }
    for (i = 0; i < WAVE_SNAPSHOTS; i++) {
        if (wave_snapshots[i].data == (u32)model->parts[0].data_addr && wave_snapshots[i].part_count == model->part_count) {
            return &wave_snapshots[i];
        }
    }
    return NULL;
}

/* Wobble_ApplyWaveUvs(model)'s call site (behind port_native_draw(PORT_ND_MAP)): the 16 oscillators as it
   reads them, kept for the model's draws. Runs on the game's thread: the next step is worked out as
   Wobble_StepOscillators would, in the same floats. */
void NativeDraw_WaveUvsApplied(const ModelInstance *model, const WaveOscillator *oscillators) {
    WaveSnapshot *s = wave_snapshot(model);
    int i;

    if (model->parts == NULL || model->part_count == 0) {
        return;
    }
    if (s == NULL) {
        s = &wave_snapshots[wave_next];
        wave_next = (wave_next + 1) % WAVE_SNAPSHOTS;
        s->data = (u32)model->parts[0].data_addr;
        s->part_count = model->part_count;
    }
    s->table = model->wave_uvs;
    s->frame = wave_frame + 1;
    for (i = 0; i < 16; i++) {
        const WaveOscillator *o = &oscillators[i];
        float step = 0.06981317f;
        float half_turn = 3.1415927f;
        float two_pi = 6.2831855f;
        float phase = o->phase;
        s16 x;
        s16 y;

        s->now[i] = (u32)(u16)o->x | (u32)(u16)o->y << 16;
        phase += step;
        if (half_turn <= phase) {
            phase -= two_pi;
        }
        x = o->amp_x * sinf(phase) * 4096.0f;
        y = o->amp_y * sinf(phase) * 4096.0f;
        s->late[i] = (u32)(u16)x | (u32)(u16)y << 16;
    }
}

/* whether a block's vertices are one UNPACK V4-16, signed, of 4 qwords a vertex (the VIF code before them,
   at 0x8C), and lie within the block */
static int wave_block_ok(const ModelPartData *data) {
    u32 qwc = *(const u32 *)data & 0xFFFF;
    u32 n = data->vertex_count & 0x7FFF;
    u32 code = *(const u32 *)((const char *)data + 0x8C);

    return ((code >> 24) & 0xEF) == 0x6D && !((code >> 14) & 1) && ((code >> 16) & 0xFF) == ((n * 4) & 0xFF) &&
           0x80 + n * 0x20 <= qwc * 16;
}

/* --native-wave-uvs, its default (PORT_WAVE_UVS_PS2) made one of the others: the PS2 path's picture, which is
   late (its DMA's race) unless the test wait (NativeDraw_TestDmaWait) takes the race away, and then the frame
   the game drew (gpu) */
int NativeDraw_TestDmaWait(void);

static int wave_uvs_mode(void) {
    int mode = port_native_wave_uvs();

    if (mode == PORT_WAVE_UVS_PS2) {
        mode = NativeDraw_TestDmaWait() ? PORT_WAVE_UVS_GPU : PORT_WAVE_UVS_LATE;
    }
    return mode;
}

/* The offsets a map record's model is drawn with on the GPU (16, RdDraw.parent's), or NULL: drawn from the
   coordinates the game wrote (--native-wave-uvs cpu, not a wave record, no offsets kept, a block not of
   the shape the copies take) */
static const MapDrawRecord *wave_record; /* (the last record asked about: KF4_ND_WATER_CHECK) */

const u32 *NativeDraw_WaveOffsets(const ModelInstance *model, const MapDrawRecord *record) {
    const ModelPart *part;
    int i;
    int j;

    int mode = wave_uvs_mode();

    wave_record = record;
    if (mode == PORT_WAVE_UVS_CPU || !(record->flags & 0x100) || model->wave_uvs == NULL) {
        return NULL;
    }
    WaveSnapshot *s = wave_snapshot(model);

    if (s == NULL || s->table == NULL || s->frame != wave_frame) {
        return NULL;
    }
    part = model->parts;
    for (j = 0; j < model->part_count; j++, part++) {
        const ModelPartData *data = EE_PTR(const ModelPartData *, part->data_addr);
        int k;

        for (k = 0; k < part->data_count; k++) {
            if (!wave_block_ok(data)) {
                return NULL;
            }
            data = (const ModelPartData *)((const char *)data + (data->length + 1) * 0x10);
        }
    }
    return mode == PORT_WAVE_UVS_LATE ? s->late : s->now;
}

/* The meshes of part part_index of a wave record's model (as NativeDraw_PartMeshes), each from a copy of
   its block with the vertices' own coordinates (model->wave_uvs, in the order Wobble_BuildWaveUvs made
   them: parts, blocks, vertices) and their waves in the ST's w; 0 if one cannot be drawn natively */
int NativeDraw_WaveMeshes(const ModelInstance *model, int part_index, const ModelPart *part, RdMesh *meshes) {
    static u8 copy[0x10000 * 16];
    const u32 *wave_check = getenv("KF4_ND_WATER_CHECK") ? NativeDraw_WaveOffsets(model, wave_record) : NULL;
    WaveSnapshot *s = wave_snapshot(model);
    const ModelWaveUv *wave = s ? s->table : NULL;
    const ModelPart *before = model->parts;
    const ModelPartData *data;
    int i;
    int j;

    if (wave == NULL) {
        return 0;
    }
    for (i = 0; i < part_index; i++, before++) {
        data = EE_PTR(const ModelPartData *, before->data_addr);
        for (j = 0; j < before->data_count; j++) {
            wave += data->vertex_count & 0x7FFF;
            data = (const ModelPartData *)((const char *)data + (data->length + 1) * 0x10);
        }
    }
    data = EE_PTR(const ModelPartData *, part->data_addr);
    for (j = 0; j < part->data_count; j++) {
        u32 qwc = *(const u32 *)data & 0xFFFF;
        u32 n = data->vertex_count & 0x7FFF;
        u32 k;

        memcpy(copy, (const u8 *)data + 0x10, qwc * 16);
        for (k = 0; k < n; k++, wave++) {
            u8 *v = copy + 0x80 + k * 0x20;
            u16 w = (u16)wave->wave;

            if (wave_check) {
                /* (KF4_ND_WATER_CHECK: the coordinates the game wrote against the copy's and the offsets) */
                const u32 *off = wave_check;
                u16 gu;
                u16 gv;
                static int differed;

                memcpy(&gu, v + 0x10, 2);
                memcpy(&gv, v + 0x12, 2);
                if ((u16)(wave->tex_u + (u16)off[w & 15]) != gu || (u16)(wave->tex_v + (u16)(off[w & 15] >> 16)) != gv) {
                    if (differed++ < 20) {
                        fprintf(stderr, "[nd wave check] model %#x part %d block %d vertex %u: game %#x %#x, own %#x %#x wave %u offset %#x\n",
                                (u32)EE_WORD(model), part_index, j, k, gu, gv, wave->tex_u, wave->tex_v, w, off[w & 15]);
                    }
                }
            }

            memcpy(v + 0x10, &wave->tex_u, 2);
            memcpy(v + 0x12, &wave->tex_v, 2);
            memcpy(v + 0x16, &w, 2);
        }
        meshes[j] = rd_mesh_from_block(copy, qwc, (u32)EE_WORD(data), RD_GEN_HASH, RD_VF_PACKED16);
        if (meshes[j] == 0) {
            return 0;
        }
        data = (const ModelPartData *)((const char *)data + (qwc + 1) * 16);
    }
    return part->data_count == 0 || rd_mesh_program(meshes[0]) == 0;
}
