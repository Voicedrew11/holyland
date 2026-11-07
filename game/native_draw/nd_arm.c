/* nd_arm.c: PC-only. Native draw's kind "arm" (--native-draw arm, PORT_ND_ARM; docs/port/NATIVE_DRAW.md
   1.2.4, NATIVE_DRAW_TASKS.md P2-4): the arm, the held weapon and the weapon's trail, drawn by
   Player_DrawArm (player/held_weapon.c) through the renderer core instead of VU1's packets.

   The arm and the held weapon are skinned models (Model_SetUpSkinned: draw_parts is Model_DrawSkinned,
   VU1 program 0x280). They are placed in the world a few units before the eye and drawn by the world's
   camera, lit by held_item_light_blocks, after the depth clear (GsDraw_ClearDepth, a GS sprite that
   stays as it is). NativeDraw_Arm does what Model_DrawSkinned queues, with each part's data blocks
   drawn natively (render.h: meshes of RD_VF_FLOAT_SKIN, a draw of program 0x280 per part, carrying VU1's
   fixed block by value). The GS registers around the draws (ALPHA, TEST, the textures, weapon 0x2E's
   additive pass) are the call site's own and stay in the chain.

   The trail is a PATH2 strip (RGBAQ, XYZ2) the game builds on the CPU from the blade's points, projected
   when WeaponTrail_Update ran: NativeDraw_ArmTrail hands it to the core as a strip of its own
   (RD_VF_GS_STRIP, program RD_PROGRAM_GS_STRIP) with the arm's identity, and the renderer writes the
   same registers as the packet would.

   VU1 still gets what Model_DrawSkinned sends but the data blocks (the fixed block and each part's
   matrices: VU1 runs nothing for them), so VU1's memory is left as the PS2 path leaves it for whatever
   reads it next.

   Nothing here changes what the game does: it reads the game's state and writes only the packet chain
   and the scratchpad matrices (and VU0's registers) that Model_DrawSkinned writes too. */
#include "common.h"
#include "model/model.h"
#include "map/map.h"
#include "entity/entity.h"
#include "gfx/gs.h"
#include "math/vu0.h"
#include "libvu0.h"
#include "port/render.h"

#include <string.h>

void Packet_BeginGsRegisters(void);
void Packet_AddGsRegister(int reg, s64 value);
void Packet_EndGsRegisters(void);
u32 *Packet_Alloc(int quadwords);
void Packet_AppendToChain(u32 *start, u32 *end);
void Matrix_Multiply(void *dst, void *matrix, void *src);
void ModelMatrix_BuildDrawMatrices(ModelInstance *, sptr, sptr, void *, void *);
void Model_DrawSkinned(ModelInstance *model);
void Vector_Copy(void *, void *);
float asinf(float);
float atan2f(float, float);
float Angle_Wrap(float);

extern f32 identity_matrix[16];
extern f32 world_screen_matrix[16]; /* VU1 0x00: what GsDraw_QueueVu1Matrix sent last */
extern f32 world_view_matrix[16];
extern f32 view_screen_matrix[16];

/* a part's data blocks at most (ModelPart.data_count is 11 bits) */
#define ARM_MAX_MESHES 2048
/* the trail's mesh key: not an EE address (those are below 0x80000000) */
#define ARM_TRAIL_KEY 0xFFFF7A11u

static void queue_value(u64 value) {
    Packet_BeginGsRegisters();
    Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)value);
    Packet_EndGsRegisters();
}

/* (Model_DrawSkinned's own test) whether a part is drawn: switched on and not scaled to nothing */
static int part_drawn(ModelPart *part) {
    if (MODEL_PART_FLAG_WORD(part) & MODEL_PART_BIT40) {
        return 0;
    }
    return !(part->scale[0] == 0.0f && part->scale[1] == 0.0f && part->scale[2] == 0.0f);
}

/* the meshes of a part's data blocks, as Model_DrawSkinned sends them (a DMA REF of each block's qwords
   after its first); 0 if one cannot be drawn natively or the part's first does not start program 0x280 */
static int part_meshes(ModelPart *part, RdMesh *meshes) {
    ModelPartData *data = EE_PTR(ModelPartData *, part->data_addr);
    int j;

    if (part->data_count > ARM_MAX_MESHES) {
        return 0;
    }
    for (j = 0; j < part->data_count; j++) {
        u32 qwc = *(u32 *)data & 0xFFFF;

        meshes[j] = rd_mesh_from_block((const u8 *)data + 0x10, qwc, (u32)EE_WORD(data), RD_GEN_HASH, RD_VF_FLOAT_SKIN);
        if (meshes[j] == 0) {
            return 0;
        }
        data = (ModelPartData *)((char *)data + (qwc + 1) * 16);
    }
    return part->data_count == 0 || rd_mesh_program(meshes[0]) == 0x280;
}

/* (model.c's wrap_angle, the same code) an angle brought back into -pi..pi (for angles above -pi) */
static inline f32 wrap_angle(f32 angle) {
    angle += 3.1415927f;
    return angle - (f32)(s32)(angle / 6.2831855f) * 6.2831855f - 3.1415927f;
}

/* (model.c's COPY_MATRIX, the same code: through VU0's vf16-vf19, which it leaves as Model_DrawSkinned does) */
#define COPY_MATRIX(to, from)                                                                      \
    do {                                                                                           \
        Vu_Lqc2(16, (u8 *)(from) + 0x0);                                                           \
        Vu_Lqc2(17, (u8 *)(from) + 0x10);                                                          \
        Vu_Lqc2(18, (u8 *)(from) + 0x20);                                                          \
        Vu_Lqc2(19, (u8 *)(from) + 0x30);                                                          \
        Vu_Sqc2((u8 *)(to) + 0x0, 16);                                                             \
        Vu_Sqc2((u8 *)(to) + 0x10, 17);                                                            \
        Vu_Sqc2((u8 *)(to) + 0x20, 18);                                                            \
        Vu_Sqc2((u8 *)(to) + 0x30, 19);                                                            \
    } while (0)

/* Model_DrawSkinned (model/model.c) to the letter but for the data blocks: the turn and the matrices
   into the scratchpad, and its packets for VU1 without the parts' data references
   (VU1 0x0C-0x13, 0x3C-0x3E, 0x40-0x48 and 0x64-0x65; then each drawn part's matrices at 0x04, 0x24 and its
   parent's at 0x08, 0x28). Nothing is run on VU1: no MSCAL is sent. */
static void skinned_without_data(ModelInstance *model) {
    void *matrix_a = MODEL_TURN_MATRIX_A;
    void *matrix_b = MODEL_TURN_MATRIX_B;
    f32 (*part_matrices)[16] = MODEL_TURN_PART_MATRICES;
    f32 (*part_turns)[16] = MODEL_TURN_PART_TURNS;
    u32 *packet;
    u32 *packet_start;
    u32 *light_out;
    u32 *light_out_b;
    ModelPart *part;
    s32 i;
    s32 part_count;
    f32 pitch;
    f32 yaw;

    i = 0;
    pitch = -asinf(((f32 *)model->light_block)[4]);
    yaw = Angle_Wrap(atan2f(((f32 *)model->light_block)[8], ((f32 *)model->light_block)[0]) + 1.5707964f);
    sceVu0RotMatrixY(MODEL_TURN_SCRATCH, identity_matrix, yaw);
    sceVu0RotMatrixX(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, pitch);
    sceVu0RotMatrixX(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, wrap_angle(model->rotation.x));
    sceVu0RotMatrixY(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, wrap_angle(model->rotation.y));
    sceVu0RotMatrixZ(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, wrap_angle(model->rotation.z));
    ModelMatrix_BuildDrawMatrices(model, model->light_block, model->light_block + 0x40, MODEL_TURN_MATRIX_A, MODEL_TURN_MATRIX_B);
    part_count = model->part_count;
    part = model->parts;
    for (; i < part_count; i++, part++) {
        Matrix_Multiply(part_matrices[i], model->model_matrix, part);
        Matrix_Multiply(part_turns[i], MODEL_TURN_SCRATCH, part);
    }
    packet_start = packet = Packet_Alloc(0x20);
    *packet = 0x10000009;
    packet += 7;
    *packet++ = 0x6C08000C;
    COPY_MATRIX(packet, matrix_a);
    packet += 0x10;
    COPY_MATRIX(packet, matrix_b);
    packet += 0x10;
    *packet = 0x10000002;
    packet += 7;
    *packet++ = 0x6C01003C;
    *(Vector4 *)packet = *(Vector4 *)model->fog_block;
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
    *packet++ = 0x10000003;
    *packet++ = 0;
    *packet++ = 0;
    *packet++ = 0;
    *packet++ = 0;
    *packet++ = 0;
    *packet++ = 0;
    *packet++ = 0x6C020064;
    Vector_Copy(packet, model->unk160);
    Vector_Copy(packet + 4, &model->colour_red);
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        if (!part_drawn(part)) {
            continue;
        }
        packet = Packet_Alloc(2);
        packet[0] = 0x10000001;
        packet[7] = 0x13000000;
        packet = Packet_Alloc(0xC);
        *packet = 0x10000005;
        packet += 7;
        *packet++ = 0x6C040004;
        COPY_MATRIX(packet, part_matrices[i]);
        packet += 0x10;
        *packet++ = 0x10000005;
        *packet++ = 0;
        *packet++ = 0;
        *packet++ = 0;
        *packet++ = 0;
        *packet++ = 0;
        *packet++ = 0;
        *packet++ = 0x6C040024;
        COPY_MATRIX(packet, part_turns[i]);
        if (MODEL_PART_PARENT_ANY(part) != -1) { /* (always: the 7-bit field holds 0x7F for "no parent") */
            packet = Packet_Alloc(0xC);
            *packet = 0x10000005;
            packet += 7;
            *packet++ = 0x6C040008;
            COPY_MATRIX(packet, part_matrices[MODEL_PART_PARENT_ANY(part)]);
            packet += 0x10;
            *packet = 0x10000005;
            packet += 7;
            *packet++ = 0x6C040028;
            COPY_MATRIX(packet, part_turns[MODEL_PART_PARENT_ANY(part)]);
        }
        /* (here Model_DrawSkinned sends the part's data blocks: drawn natively instead) */
    }
    packet = Packet_Alloc(1);
    Packet_AppendToChain(packet_start, packet);
}

/* The camera the world is drawn by now (the arm has no projection of its own: the world's). (The mode
   and projection native_draw.c notes are the world's here: the arm is drawn after the world's camera is
   set again, mode 0, projection 0.) */
static u32 world_camera(void) {
    RdCamera c;

    memset(&c, 0, sizeof(c));
    memcpy(c.world_screen, world_screen_matrix, sizeof(c.world_screen));
    memcpy(c.view, world_view_matrix, sizeof(c.view));
    memcpy(c.screen, view_screen_matrix, sizeof(c.screen));
    return rd_camera(&c);
}

/* Player_DrawArm's model->draw_parts(model), natively: which is 0 for the arm, 1 for the held item (the
   draws' identity: kind RD_KIND_ARM, which, part). 1 if drawn; 0 if a block cannot be drawn natively or
   the model is not a skinned one, and nothing was queued: the caller draws it the PS2 way. A model goes
   one way or the other, whole. */
int NativeDraw_Arm(ModelInstance *model, int which) {
    static RdMesh meshes[ARM_MAX_MESHES];
    f32 (*part_matrices)[16] = MODEL_TURN_PART_MATRICES;
    f32 (*part_turns)[16] = MODEL_TURN_PART_TURNS;
    ModelPart *part;
    RdDraw d;
    int i;

    if (model->draw_parts != Model_DrawSkinned) {
        return 0;
    }
    /* every drawn part's blocks first */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        if (part_drawn(part) && !part_meshes(part, meshes)) {
            return 0;
        }
    }
    skinned_without_data(model);
    memset(&d, 0, sizeof(d));
    d.kind = RD_KIND_ARM;
    d.id[0] = (u32)which;
    d.program = 0x280;
    d.camera = world_camera();
    memcpy(d.light_dir, MODEL_TURN_MATRIX_A, sizeof(d.light_dir));
    memcpy(d.light_col, MODEL_TURN_MATRIX_B, sizeof(d.light_col));
    memcpy(d.fog, model->fog_block, sizeof(d.fog));
    d.fogcol = (u64)((MapFogBlockValues *)model->fog_block)->unk10;
    d.point_count = model->hit_count;
    for (i = 0; i < model->hit_count && i < 4; i++) {
        memcpy(d.point_pos[i], &linked_point_pool[model->hit_handles[i]].position, 16);
        d.point_pos[i][3] = linked_point_pool[model->hit_handles[i]].progress_scale;
        memcpy(d.point_col[i], &linked_point_pool[model->hit_handles[i]].target, 16);
    }
    memcpy(d.colour[0], model->unk160, 16);   /* VU1 0x64: the colour's bias */
    memcpy(d.colour[1], &model->colour_red, 16); /* 0x65: its scale (red, green, blue, alpha) */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        int parent = MODEL_PART_PARENT_ANY(part);
        u64 value;

        if (!part_drawn(part)) {
            continue;
        }
        part_meshes(part, meshes); /* (found again in the cache) */
        memcpy(d.model, part_matrices[i], sizeof(d.model));
        memcpy(d.parent, part_matrices[parent], sizeof(d.parent));
        memcpy(d.turn[0], part_turns[i], sizeof(d.turn[0]));
        memcpy(d.turn[1], part_turns[parent], sizeof(d.turn[1]));
        d.id[1] = (u32)i;
        value = rd_draw(&d, meshes, part->data_count);
        if (value != 0) {
            queue_value(value);
        }
    }
    return 1;
}

/* Player_DrawArm's trail: the packet it built from packet_start to packet_end (a DMA CNT tag, VIF1 DIRECT,
   then a PATH2 GIF packet of a PACKED triangle strip of RGBAQ and XYZ2, ADC on the points off the screen),
   appended to the chain, natively: the strip handed to the core as a mesh of RD_VF_GS_STRIP and a draw of
   program RD_PROGRAM_GS_STRIP (identity RD_KIND_ARM, 2), which the renderer writes as the GIF would.
   1 if done; 0 if not, and nothing was queued: the caller appends it. */
int NativeDraw_ArmTrail(u32 *packet_start, u32 *packet_end) {
    u32 qwords = packet_start[7] & 0xFFFF; /* (DIRECT's size: the GIF packet's qwords) */
    RdMesh mesh;
    RdDraw d;
    u64 value;

    if ((packet_start[7] & 0xFF000000u) != 0x51000000u || packet_start + 8 + qwords * 4 != packet_end) {
        return 0;
    }
    mesh = rd_mesh_from_block(packet_start + 8, qwords, ARM_TRAIL_KEY, RD_GEN_HASH, RD_VF_GS_STRIP);
    if (mesh == 0) {
        return 0;
    }
    memset(&d, 0, sizeof(d));
    d.kind = RD_KIND_ARM;
    d.id[0] = 2;
    d.program = RD_PROGRAM_GS_STRIP;
    d.camera = world_camera();
    value = rd_draw(&d, &mesh, 1);
    if (value == 0) {
        return 0;
    }
    queue_value(value);
    return 1;
}
