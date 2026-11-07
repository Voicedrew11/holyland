/* nd_entities.c: PC-only. Native draw (--native-draw entities; docs/port/NATIVE_DRAW.md, P2-3): the
   skinned models of the entities (enemies and NPCs), drawn at the game's level with VU1 program 0x280's
   maths (port/gs/pc/shaders/pcr_nd280.vert) instead of their packets.

   Entities draw through Model_DrawSkinned in two places, both patched in entity/ (entity.patch):
   - Entity_DrawAll calls it directly for an opaque entity;
   - a translucent one (alpha below 128, model flag 0x200, type 0x10) goes to the draw list, whose
     DrawList_Draw calls the model's draw (Entity_PrepareModelDraw: ZBUF, TEST, ALPHA, the textures) and
     then its draw_parts; Entity_TryActivate gives the entity's model a draw_parts of entity/'s that
     calls NativeDraw_Skinned, and Model_DrawSkinned when that declines. The Fire Lord's afterimages are
     copies of its model (FireLord_Think), draw_parts included, so they come here too, as models of their
     own (their identity is the copy's address).
   The draw list keeps its order: each model's draws are queued where its packets would have been.

   The tint (the hit flash's 255, 64, 64, the ailments' pulses, 128 otherwise) and the fades (the model's
   alpha) are VU1 0x65, the colour scale (RdDraw.colour[1]); hidden parts (MODEL_PART_BIT40) and parts
   scaled to nothing are left out as Model_DrawSkinned leaves them. Type 0x47 (the frame copied to 0x3540
   and mapped by screen UVs, Entity_PrepareScreenTexturedDraw) stays the PS2 path's: its caller does not
   come here.

   Nothing here changes what the game does: it reads the game's state, writes the packet chain and the
   scratchpad matrices Model_DrawSkinned writes too (the same values), and with the switch off is never
   called. */
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
void Matrix_Multiply(void *dst, void *matrix, void *src);
u32 *Packet_Alloc(int quadwords);
void Packet_AppendToChain(u32 *start, u32 *end);
void ModelMatrix_BuildDrawMatrices(ModelInstance *, sptr, sptr, void *, void *);
void Model_DrawSkinned(ModelInstance *model);
float Angle_Wrap(float);
float asinf(float);
float atan2f(float, float);

extern f32 identity_matrix[16];
extern f32 world_screen_matrix[16];
extern f32 world_view_matrix[16];
extern f32 view_screen_matrix[16];

/* a part's data blocks at most (ModelPart.data_count's 11 bits), as native_draw.c's */
#define ND_MAX_MESHES 2048

/* (model.c's wrap_angle: an angle brought back into -pi..pi, for angles above -pi) */
static inline f32 wrap_angle(f32 angle) {
    angle += 3.1415927f;
    return angle - (f32)(s32)(angle / 6.2831855f) * 6.2831855f - 3.1415927f;
}

/* Model_DrawSkinned's test for a part it sends */
static int part_drawn(ModelPart *part) {
    if (MODEL_PART_FLAG_WORD(part) & MODEL_PART_BIT40) {
        return 0;
    }
    return !(part->scale[0] == 0.0f && part->scale[1] == 0.0f && part->scale[2] == 0.0f);
}

/* the meshes of a skinned part's data blocks, as Model_DrawSkinned sends them (a DMA REF of each block's
   qwords after its first); 0 if one cannot be drawn natively or the part does not start program 0x280 */
static int skinned_part_meshes(ModelPart *part, RdMesh *meshes) {
    ModelPartData *data = EE_PTR(ModelPartData *, part->data_addr);
    int j;

    if (part->data_count > ND_MAX_MESHES) {
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

/* the camera the world is drawn by now. (native_draw.c's current_camera also fills the mirror's mode and
   projection; nothing reads them yet, and the pictures between of models are P2-5's.) */
static u32 skinned_camera(void) {
    RdCamera c;

    memset(&c, 0, sizeof(c));
    memcpy(c.world_screen, world_screen_matrix, sizeof(c.world_screen));
    memcpy(c.view, world_view_matrix, sizeof(c.view));
    memcpy(c.screen, view_screen_matrix, sizeof(c.screen));
    return rd_camera(&c);
}

/* What Model_DrawSkinned leaves in VU1's memory, without the parts' data (VU1 runs nothing for it): its
   first packet (the light matrices at 0x0C-0x13, the fog at 0x3C, the FOGCOL packet at 0x3D, the point
   lights at 0x40-0x48, the colour bias and scale at 0x64-0x65), then the last part it sends: its two
   matrices at 0x04 and 0x24 and its parent's at 0x08 and 0x28. So whatever runs on VU1 after a model drawn
   natively finds what the PS2 path would have left there (the CPU's near cut, also on program 0x280,
   reads the set-up's inputs from here; native_draw.c's queue_fixed_block is the same for posed models). */
static void queue_vu1_state(ModelInstance *model, int last) {
    f32 (*part_matrices)[16] = MODEL_TURN_PART_MATRICES;
    f32 (*part_turns)[16] = MODEL_TURN_PART_TURNS;
    u32 *packet_start;
    u32 *packet;
    u32 *light_out;
    u32 *light_out_b;
    int i;

    packet_start = packet = Packet_Alloc(0x20);
    *packet = 0x10000009;
    packet += 7;
    *packet++ = 0x6C08000C;
    memcpy(packet, MODEL_TURN_MATRIX_A, 0x40);
    packet += 0x10;
    memcpy(packet, MODEL_TURN_MATRIX_B, 0x40);
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
    *packet++ = 0x10000003;
    memset(packet, 0, 6 * 4);
    packet += 6;
    *packet++ = 0x6C020064;
    memcpy(packet, model->unk160, 16);
    memcpy(packet + 4, &model->colour_red, 16);
    if (last >= 0) {
        ModelPart *part = model->parts + last;
        int parent = MODEL_PART_PARENT_ANY(part);

        packet = Packet_Alloc(0x18);
        *packet = 0x10000005;
        packet += 7;
        *packet++ = 0x6C040004;
        memcpy(packet, part_matrices[last], 0x40);
        packet += 0x10;
        *packet = 0x10000005;
        packet += 7;
        *packet++ = 0x6C040024;
        memcpy(packet, part_turns[last], 0x40);
        packet += 0x10;
        *packet = 0x10000005;
        packet += 7;
        *packet++ = 0x6C040008;
        memcpy(packet, part_matrices[parent], 0x40);
        packet += 0x10;
        *packet = 0x10000005;
        packet += 7;
        *packet++ = 0x6C040028;
        memcpy(packet, part_turns[parent], 0x40);
    }
    packet = Packet_Alloc(1);
    Packet_AppendToChain(packet_start, packet);
}

/* Model_DrawSkinned(model), natively: the same matrices (into the same scratchpad), the same parts left
   out, a draw per part on program 0x280 with what the packets would have put in VU1's memory. kind and
   owner make the draws' identity. 1 if drawn; 0 if a part it would send has a block the core cannot draw,
   and nothing was queued: the caller draws it the PS2 way (a model goes one way or the other, whole). */
int NativeDraw_Skinned(ModelInstance *model, int kind, const void *owner) {
    static RdMesh meshes[ND_MAX_MESHES];
    f32 (*part_matrices)[16] = MODEL_TURN_PART_MATRICES;
    f32 (*part_turns)[16] = MODEL_TURN_PART_TURNS;
    ModelPart *part;
    RdDraw d;
    f32 pitch;
    f32 yaw;
    int last = -1;
    int i;

    if (model->parts == NULL || model->part_count == 0) {
        return 0; /* (Model_DrawSkinned still queues its first packet: leave it to it) */
    }
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        if (part_drawn(part) && !skinned_part_meshes(part, meshes)) {
            return 0;
        }
    }
    /* Model_DrawSkinned's matrices */
    pitch = -asinf(((f32 *)model->light_block)[4]);
    yaw = Angle_Wrap(atan2f(((f32 *)model->light_block)[8], ((f32 *)model->light_block)[0]) + 1.5707964f);
    sceVu0RotMatrixY(MODEL_TURN_SCRATCH, identity_matrix, yaw);
    sceVu0RotMatrixX(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, pitch);
    sceVu0RotMatrixX(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, wrap_angle(model->rotation.x));
    sceVu0RotMatrixY(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, wrap_angle(model->rotation.y));
    sceVu0RotMatrixZ(MODEL_TURN_SCRATCH, MODEL_TURN_SCRATCH, wrap_angle(model->rotation.z));
    ModelMatrix_BuildDrawMatrices(model, model->light_block, model->light_block + 0x40, MODEL_TURN_MATRIX_A, MODEL_TURN_MATRIX_B);
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        Matrix_Multiply(part_matrices[i], model->model_matrix, part);
        Matrix_Multiply(part_turns[i], MODEL_TURN_SCRATCH, part);
    }

    memset(&d, 0, sizeof(d));
    d.kind = (u32)kind;
    d.id[0] = (u32)EE_WORD(owner);
    d.program = 0x280;
    d.camera = skinned_camera();
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
    memcpy(d.colour[0], model->unk160, 16);      /* VU1 0x64: the colour bias */
    memcpy(d.colour[1], &model->colour_red, 16); /* VU1 0x65: the scale (the tint, the alpha) */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        int parent = MODEL_PART_PARENT_ANY(part); /* (0x7F for none: the PS2 path sends that slot anyway) */
        u64 value;

        if (!part_drawn(part)) {
            continue;
        }
        last = i;
        skinned_part_meshes(part, meshes); /* (found again in the cache) */
        memcpy(d.model, part_matrices[i], sizeof(d.model));
        memcpy(d.parent, part_matrices[parent], sizeof(d.parent));
        memcpy(d.turn[0], part_turns[i], sizeof(d.turn[0]));
        memcpy(d.turn[1], part_turns[parent], sizeof(d.turn[1]));
        d.id[1] = (u32)i;
        value = rd_draw(&d, meshes, part->data_count);
        if (value != 0) {
            Packet_BeginGsRegisters();
            Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)value);
            Packet_EndGsRegisters();
        }
    }
    queue_vu1_state(model, last);
    return 1;
}
