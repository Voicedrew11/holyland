/* native_draw.c: PC-only. The game's side of native draw (--native-draw, port_native_draw();
   docs/port/NATIVE_DRAW.md): the adapter between the game's draw code and the renderer core
   (port/include/port/render.h).

   A patched draw function calls in here where it would have queued a model's packets for VU1. This
   knows the game's structs (ModelInstance, ModelPart, the light and fog blocks, the camera's matrices)
   and turns them into what the core takes: a mesh per data block, the camera, and a draw per model part
   that carries what the packets would have put in VU1's fixed block (vu1_hle.c is what VU1 makes of it).
   It queues, in the packet chain, the GS registers the PS2 path queues itself (ZBUF, TEST) and, for each
   part, a write to PORT_GS_REG_NATIVE_DRAW naming the draw: at the very place of the packets it
   replaces, so that the renderer draws it in the same order, with the same GS state.

   Nothing here changes what the game does: it reads the game's state, writes only the packet chain (and
   the scratchpad matrices the PS2 path writes too), and with the switch off is never called. */
#include "common.h"
#include "model/model.h"
#include "map/map.h"
#include "entity/entity.h"
#include "gfx/gs.h"
#include "port/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Packet_BeginGsRegisters(void);
void Packet_AddGsRegister(int reg, s64 value);
void Packet_EndGsRegisters(void);
void Matrix_Multiply(void *dst, void *matrix, void *src);
u32 *Packet_Alloc(int quadwords);
void Packet_AppendToChain(u32 *start, u32 *end);
void ModelMatrix_BuildDrawMatrices(ModelInstance *, sptr, sptr, void *, void *);
void Model_DrawPosed(ModelInstance *model);

extern f32 world_screen_matrix[16]; /* VU1 0x00: what GsDraw_QueueVu1Matrix sent last */
extern f32 world_view_matrix[16];
extern f32 view_screen_matrix[16];

/* a part's data blocks at most: ModelPart.data_count is 11 bits. (It was 64, which sent every map record of
   more blocks the PS2 way without a word, the lava and most big floors among them: the stats' "refused"
   counts blocks that do not parse, not this.) */
#define ND_MAX_MESHES 2048

/* (nd_water.c: a wave record's meshes and offsets, the wave UVs on the GPU, P6-2) */
const u32 *NativeDraw_WaveOffsets(const ModelInstance *model, const MapDrawRecord *record);
int NativeDraw_WaveMeshes(const ModelInstance *model, int part_index, const ModelPart *part, RdMesh *meshes);

/* At the start of GameLoop_DrawFrame (FrameInterp_BeginFrame): the side buffer's next frame */
static void queue_mark(u32 command, u64 bits) {
    Packet_BeginGsRegisters();
    Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)(((u64)command << 56) | bits));
    Packet_EndGsRegisters();
}

void NativeDraw_WaveFrame(void); /* (nd_water.c: the wave snapshots' frame) */
void NativeDraw_SkyFrame(void);  /* (nd_sky.c: no map record drawn yet) */

void NativeDraw_BeginFrame(void) {
    NativeDraw_WaveFrame();
    NativeDraw_SkyFrame();
    if (port_native_draw(PORT_ND_ALL)) {
        u64 mark = rd_begin_frame();

        queue_mark((u32)(mark >> 56), mark & 0xFFFFFFFFFFFFFFull);
    }
}

/* GameLoop_SetCameraMatrices' last arguments (NativeDraw_MarkCamera) */
static int camera_projection, camera_mode;

/* After GameLoop_SetCameraMatrices(projection, mode) (from FrameInterp_MarkCamera, which it calls): what
   the matrices it built are. Camera mode 1 is the mirror image, in both of GameLoop_DrawFrame's mirror
   modes (map_draw_state.mode 1, the mirror drawn into the frame: map 041; mode 2, the reflection: maps
   008, 009 and 014): world_screen_matrix has the reflection about mirror_height in it, world_view_matrix
   does not. Projection 1 is the reflection's (sceVu0ViewScreenMatrix with ax 0.4, ay 0.571, drawn into
   the 256x256 CT16S target at 0x2BC0). The draws themselves need nothing more (the GS state, the target
   included, comes from the chain; no culling, so the flipped winding does not matter): the pictures
   between do (P1-3). */
void NativeDraw_MarkCamera(int projection, int mode) {
    camera_projection = projection;
    camera_mode = mode;
}

/* the camera the world is drawn by now (GameLoop_SetCameraMatrices' last) */
static u32 current_camera(void) {
    RdCamera c;

    memset(&c, 0, sizeof(c));
    memcpy(c.world_screen, world_screen_matrix, sizeof(c.world_screen));
    memcpy(c.view, world_view_matrix, sizeof(c.view));
    memcpy(c.screen, view_screen_matrix, sizeof(c.screen));
    c.mode = (u32)camera_mode;
    c.projection = (u32)camera_projection;
    return rd_camera(&c);
}

/* the meshes of a part's data blocks (ModelPart.data_addr, data_count), as Model_DrawPosed* sends them:
   a DMA REF of each block's qwords after its first; 0 if one cannot be drawn natively */
static int part_meshes(ModelPart *part, RdMesh *meshes) {
    ModelPartData *data = EE_PTR(ModelPartData *, part->data_addr);
    int j;

    if (part->data_count > ND_MAX_MESHES) {
        return 0;
    }
    for (j = 0; j < part->data_count; j++) {
        u32 qwc = *(u32 *)data & 0xFFFF;

        meshes[j] = rd_mesh_from_block((const u8 *)data + 0x10, qwc, (u32)EE_WORD(data), RD_GEN_HASH, RD_VF_PACKED16);
        if (meshes[j] == 0) {
            return 0;
        }
        data = (ModelPartData *)((char *)data + (qwc + 1) * 16);
    }
    /* (the part's first block starts program 0x000 and its set-up, as the PS2 path's do: else its
       vertex matrix would be the part's before, which a draw of its own cannot say) */
    return part->data_count == 0 || rd_mesh_program(meshes[0]) == 0;
}

/* VU1's fixed block as Model_DrawPosed and Model_DrawPosedDepthByPart queue it ahead of the parts (their
   first packet: the light matrices at 0x0C-0x13, the fog at 0x3C, the FOGCOL packet at 0x3D, the point
   lights at 0x40-0x48, and with model flag 0x800 the colours at 0x64-0x66), without the parts: VU1 runs
   nothing for it. So a model drawn natively leaves VU1's memory as the PS2 path's would, for what runs on
   VU1 after it and reads what it left: the CPU's near cut (ModelScreen_CutClippedTriangles, after each
   record of Map_DrawRecords and Map_DrawLateRecords; it stays the PS2 path, --clip ps2) sends only its part
   matrix and starts program 0x000's set-up, which reads the rest from here. Without this its pieces were
   lit and fogged by whatever model VU1 drew last (a close wall on map 021 bright, up to 83 levels). */
static void queue_fixed_block(ModelInstance *model, const void *matrix_a, const void *matrix_b) {
    u32 *packet_start;
    u32 *packet;
    u32 *light_out;
    u32 *light_out_b;
    int i;

    packet_start = packet = Packet_Alloc((model->model_flags & 0x800) ? 0x21 : 0x1C);
    *packet = 0x10000009;
    packet += 7;
    *packet++ = 0x6C08000C;
    memcpy(packet, matrix_a, 0x40);
    packet += 0x10;
    memcpy(packet, matrix_b, 0x40);
    packet += 0x10;
    *packet = 0x10000002;
    packet += 7;
    *packet++ = 0x6C01003C;
    memcpy(packet, model->fog_block, 16);
    packet += 4;
    *packet = 0x10000003;
    packet += 7;
    *packet++ = 0x6C02003D;
    *(u64 *)packet = 0x1000000000008001ull;
    packet += 2;
    *(u64 *)packet = 0xE;
    packet += 2;
    *(u64 *)packet = ((MapFogBlockValues *)model->fog_block)->unk10;
    packet += 2;
    *packet++ = 0x3D;
    *packet++ = 0;
    *packet = 0x1000000A;
    packet += 7;
    *packet = 0x6C090040;
    packet += 4;
    *packet++ = model->hit_count;
    light_out = packet;
    light_out_b = packet + 0x10;
    for (i = 0; i < model->hit_count; i++) {
        memcpy(light_out, &linked_point_pool[model->hit_handles[i]].position, 16);
        memcpy(light_out_b, &linked_point_pool[model->hit_handles[i]].target, 16);
        light_out_b += 4;
        ((f32 *)light_out)[3] = linked_point_pool[model->hit_handles[i]].progress_scale;
        light_out += 4;
    }
    packet += 0x20;
    if (model->model_flags & 0x800) {
        *packet++ = 0x10000004;
        memset(packet, 0, 6 * 4);
        packet += 6;
        *packet++ = 0x6C030064;
        memcpy(packet, &model->colour_red, 16);
        packet += 4;
        packet[3] = model->found_record_id;
        memcpy(packet + 4, model->found_record, 16);
    }
    packet = Packet_Alloc(1);
    Packet_AppendToChain(packet_start, packet);
}

/* The two ways model/model.c queues a posed model's parts that are drawn natively so far:
   Model_DrawPosed (the map records' draw_parts, map objects ...) and Model_DrawPosedDepthByPart (items,
   gold: parts scaled to nothing left out, each part's ZBUF and TEST queued before it). */
enum { FORM_POSED, FORM_DEPTH_BY_PART };

/* What the form's function queues, drawn natively: the same parts left out, the same GS registers, the
   same VU1 fixed block. kind and owner make the draws' identity. 1 if drawn; 0 if the model has a block
   the core cannot draw (another VU1 program, a block that is not self-contained ...), and nothing was
   queued: the caller draws it the PS2 way. flags: RD_DRAW_* the draws carry besides (the modern sky's
   RD_DRAW_FAR_Z). */
static int posed_model(ModelInstance *model, int form, int kind, const void *owner, u32 flags) {
    static RdMesh meshes[ND_MAX_MESHES];
    f32 (*matrix_a)[4] = MODEL_DRAW_MATRIX_A;
    f32 (*matrix_b)[4] = MODEL_DRAW_MATRIX_B;
    ModelPart *part;
    RdDraw d;
    u32 camera;
    const u32 *wave;
    int i;

    if (model->parts == NULL || model->part_count == 0) {
        return 1; /* (the PS2 path queues nothing either) */
    }
    /* a wave record's meshes keep the vertices' own coordinates, its offsets go with the draws (nd_water.c) */
    wave = kind == RD_KIND_MAP_RECORD ? NativeDraw_WaveOffsets(model, owner) : NULL;
    /* every part's blocks first: a model is drawn one way or the other, whole */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        if (!(wave ? NativeDraw_WaveMeshes(model, i, part, meshes) : part_meshes(part, meshes))) {
            return 0;
        }
    }
    ModelMatrix_BuildDrawMatrices(model, model->light_block, model->light_block + 0x40, matrix_a, matrix_b);
    queue_fixed_block(model, matrix_a, matrix_b);
    camera = current_camera();
    memset(&d, 0, sizeof(d));
    d.kind = (u32)kind;
    d.id[0] = (u32)EE_WORD(owner);
    d.program = 0x000;
    d.camera = camera;
    d.flags = flags;
    if (wave) {
        d.flags |= RD_DRAW_WAVE_UVS;
        memcpy(d.parent, wave, sizeof(d.parent));
    }
    memcpy(d.light_dir, matrix_a, sizeof(d.light_dir));
    memcpy(d.light_col, matrix_b, sizeof(d.light_col));
    memcpy(d.fog, model->fog_block, sizeof(d.fog));
    d.fogcol = (u64)((MapFogBlockValues *)model->fog_block)->unk10;
    d.point_count = model->hit_count;
    for (i = 0; i < model->hit_count && i < 4; i++) {
        memcpy(d.point_pos[i], &linked_point_pool[model->hit_handles[i]].position, 16);
        d.point_pos[i][3] = linked_point_pool[model->hit_handles[i]].progress_scale;
        memcpy(d.point_col[i], &linked_point_pool[model->hit_handles[i]].target, 16);
    }
    if (model->model_flags & 0x800) {
        /* (VU1 0x64-0x66: read by program 0x700 only; carried for the effects, P4) */
        d.colour[0][0] = model->colour_red;
        d.colour[0][1] = model->colour_green;
        d.colour[0][2] = model->colour_blue;
        memcpy(&d.colour[0][3], &model->found_record_id, 4);
        memcpy(d.colour[1], &model->found_record, 16);
    }
    if (kind == RD_KIND_MAP_RECORD && rd_dump_enabled()) {
        /* KF4_ND_DUMP (debug only, P0-3): the record's flags, the camera's view and the model's place, before its parts' D lines */
        char line[512];
        int n = snprintf(line, sizeof(line), "R %#x %#x", (u32)EE_WORD(owner), (u32)((const MapDrawRecord *)owner)->flags);

        for (i = 0; i < 16; i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %.9g", world_view_matrix[i]);
        }
        for (i = 0; i < 3; i++) { /* the model's place (the sky's: the player's feet) */
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %.9g", ((f32 *)model->model_matrix)[12 + i]);
        }
        rd_dump_note(line);
    }
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        f32 matrix[4][4] __attribute__((aligned(16)));
        u64 value;

        if (MODEL_PART_FLAG_WORD(part) & MODEL_PART_BIT40) {
            continue;
        }
        if (form == FORM_DEPTH_BY_PART && part->scale[0] == 0.0f && part->scale[1] == 0.0f && part->scale[2] == 0.0f) {
            continue;
        }
        if (wave) {
            NativeDraw_WaveMeshes(model, i, part, meshes); /* (found again in the cache) */
        } else {
            part_meshes(part, meshes); /* (found again in the cache) */
        }
        Matrix_Multiply(matrix, model->model_matrix, part);
        memcpy(d.model, matrix, sizeof(d.model));
        d.id[1] = (u32)i;
        value = rd_draw(&d, meshes, part->data_count);
        Packet_BeginGsRegisters();
        if (form == FORM_DEPTH_BY_PART) {
            if (MODEL_PART_FLAG_WORD(part) & MODEL_PART_BIT41) {
                Packet_AddGsRegister(GS_REG_ZBUF_1, 0xA000118);
                Packet_AddGsRegister(GS_REG_TEST_1, 0x5000D);
            } else {
                Packet_AddGsRegister(GS_REG_ZBUF_1, 0x10A000118LL);
                Packet_AddGsRegister(GS_REG_TEST_1, 0x5000D);
            }
        }
        if (value != 0) {
            Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)value);
        }
        Packet_EndGsRegisters();
    }
    return 1;
}

/* A model given back to the caller, to draw the PS2 way: counted (--stats, "models declined"), so that "0
   refused" with 0 declined means every model of the kinds asked for was drawn natively. With KF4_ND_LOG
   set, the first few are named. */
static int declined(ModelInstance *model, int kind, const char *why) {
    static int logged;

    rd_stats.declined++;
    if (logged < 32 && getenv("KF4_ND_LOG")) {
        logged++;
        fprintf(stderr, "[native draw] kind %d model %#x drawn the PS2 way: %s\n", kind, (u32)EE_WORD(model), why);
    }
    return 0;
}

/* Model_DrawPosedDepthByPart(model), natively (posed_model) */
int NativeDraw_PosedDepthByPart(ModelInstance *model, int kind, const void *owner) {
    if (!posed_model(model, FORM_DEPTH_BY_PART, kind, owner, 0)) {
        return declined(model, kind, "a block the core cannot draw");
    }
    return 1;
}

int NativeDraw_PosedWithFlags(ModelInstance *model, int kind, const void *owner, u32 flags);

/* Model_DrawPosed(model), natively (posed_model); a model whose draw_parts is something else is not */
int NativeDraw_Posed(ModelInstance *model, int kind, const void *owner) {
    return NativeDraw_PosedWithFlags(model, kind, owner, 0);
}

/* NativeDraw_Posed with its draws carrying flags (RD_DRAW_*) besides: nd_sky.c's modern sky */
int NativeDraw_PosedWithFlags(ModelInstance *model, int kind, const void *owner, u32 flags) {
    if (model->draw_parts != Model_DrawPosed) {
        return declined(model, kind, "draw_parts is not Model_DrawPosed");
    }
    if (!posed_model(model, FORM_POSED, kind, owner, flags)) {
        return declined(model, kind, "a block the core cannot draw");
    }
    return 1;
}

/* ---- For the other adapter files (nd_*.c): the helpers above ---------------------------------- */
int NativeDraw_PartMeshes(ModelPart *part, RdMesh *meshes) {
    return part_meshes(part, meshes);
}

u32 NativeDraw_CurrentCamera(void) {
    return current_camera();
}

void NativeDraw_QueueMark(u32 command, u64 bits) {
    queue_mark(command, bits);
}

void NativeDraw_QueueFixedBlock(ModelInstance *model, const void *matrix_a, const void *matrix_b) {
    queue_fixed_block(model, matrix_a, matrix_b);
}

/* KF4_ND_DMA_WAIT=1, a test switch (F-013): the game loop waits for the chain it has just sent
   (Packet_WaitForSend, which it would do at the end of the frame anyway) before the frame's update.
   The port's DMA thread reads the chain's REF'd blocks after Packet_SendChain returns, while the game
   goes on to the next update, which rewrites some of them in place (func_00112BD0's wave UVs, flag
   0x100; MapObject_UpdateAll's fades). So the PS2 path can draw a block as the next update left it,
   a frame late, and how much of the chain is read late changes run to run (P2-6). Native draws take
   the block at the draw call. With the wait the PS2 path reads every block before the update too,
   and the two can be compared pixel for pixel on maps with such blocks. Off (the default) the game
   is unchanged; the comparisons set it in both runs (the environment reaches both). P6-2's
   --test-dma-wait (port_test_dma_wait: Packet_SendChain waits, gfx.patch) is the same wait; either
   switch turns on both. */
int NativeDraw_TestDmaWait(void) {
    static int wait = -1;

    if (wait < 0) {
        const char *v = getenv("KF4_ND_DMA_WAIT");

        wait = v != NULL && v[0] != '\0' && v[0] != '0';
    }
    return wait || port_test_dma_wait();
}
