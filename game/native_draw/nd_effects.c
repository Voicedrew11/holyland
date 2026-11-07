/* nd_effects.c: PC-only. Native draw (--native-draw effects, PORT_ND_EFFECTS; docs/port/NATIVE_DRAW.md 1.2.5,
   NATIVE_DRAW_TASKS.md P4-2) of the effects: the sfx.dat models Effect_DrawAll puts in the draw list (layers 0
   to 3: in the world, view-locked, on top) and DrawList_Draw draws, each through its blend hook (model->draw:
   Effect_Prepare*Draw*, the GS registers, which stay the game's) and then Model_DrawPosed, the draw_parts
   Model_SetUpPosed gives them. The call site is the head of Model_DrawPosed itself (model.patch), for the
   models with model flag 0x800 (Effect_SetupModel's, and only theirs): one hook for every layer and pass.

   What Model_DrawPosed queues, and so what this queues in its place, in the chain's order:
   - VU1's fixed block: the two light matrices, the fog, FOGCOL, the point lights (none: Effect_DrawAll sets
     hit_count 0) and, with flag 0x800, VU1 0x64-0x66: the model's colour (colour_red ... alpha), 0x65.w the
     record id whose TEX0 is to be replaced (found_record_id) and 0x66 the TEX0 that replaces it
     (found_record: the flipbook's frame, Effect_FindModelRecord). Queued without the parts (native_draw.c's
     queue_fixed_block), so VU1's memory is left as the PS2 path leaves it;
   - per part not switched off (bit40): the part's matrix (model x part) and its data blocks. The blocks
     start VU1's program 0x700 (Model_SetVu1Program's MSCALF in each part's first block): a draw of program
     0x700 (port/gs/pc/shaders/pcr_nd700.vert) carrying 0x64-0x66 in RdDraw.colour. A part
     whose blocks are program 0x000's is drawn as such; one of any other, or a block the core cannot parse,
     gives the whole model back to the PS2 path (counted: --stats' "models declined").
   The draws' identity: kind RD_KIND_EFFECT, the effect's model (its EE word: an effect's slot is reserved
   for 4 frames after it is freed, so the next frame never has another effect in it) and the part, which
   the pictures between pair by (P2-5's moves: the effect's matrices from the frame before's to this one's,
   billboards included). Flipbooks step with the frame: a picture between draws the frame's TEX0. */
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
void ModelMatrix_BuildDrawMatrices(ModelInstance *, sptr, sptr, void *, void *);

/* native_draw.c's helpers (its "For the other adapter files") */
u32 NativeDraw_CurrentCamera(void);
void NativeDraw_QueueFixedBlock(ModelInstance *model, const void *matrix_a, const void *matrix_b);

#define ND_MAX_MESHES 2048 /* (ModelPart.data_count is 11 bits; as native_draw.c) */
#define ND_MODEL_MESHES 4096 /* an effect model's blocks kept from the check to the draws (more: found again) */

/* A model given back to the PS2 path, counted (rd_stats.declined: --stats' "models declined"); with
   KF4_ND_LOG the first few are named */
static int declined(ModelInstance *model, const char *why) {
    static int logged;

    rd_stats.declined++;
    if (logged < 32 && getenv("KF4_ND_LOG")) {
        EEWord owner = 0;

        logged++;
        ee_word_try(model, &owner);
        fprintf(stderr, "[native draw] effect model %#x drawn the PS2 way: %s\n", (u32)owner, why);
    }
    return 0;
}

/* The meshes of a part's data blocks, as Model_DrawPosed sends them (a DMA REF of each block's qwords after
   its first), and the VU1 program they run: 0x700 or 0x000, every MSCAL of every block the same, the first
   block's first run an MSCAL (the part's own set-up). 0 if the part cannot be drawn natively (why says) */
static u32 part_meshes(ModelPart *part, RdMesh *meshes, const char **why) {
    ModelPartData *data = EE_PTR(ModelPartData *, part->data_addr);
    u32 program;
    int j;

    if (part->data_count > ND_MAX_MESHES) {
        *why = "a part of more blocks than the core takes";
        return 0;
    }
    if (part->data_count == 0) {
        return 0x700 + 1; /* (nothing to draw: any program) */
    }
    for (j = 0; j < part->data_count; j++) {
        u32 qwc = *(u32 *)data & 0xFFFF;

        meshes[j] = rd_mesh_from_block((const u8 *)data + 0x10, qwc, (u32)EE_WORD(data), RD_GEN_HASH, RD_VF_PACKED16);
        if (meshes[j] == 0) {
            *why = rd_mesh_why();
            return 0;
        }
        data = (ModelPartData *)((char *)data + (qwc + 1) * 16);
    }
    program = rd_mesh_program(meshes[0]);
    if (program != 0x700 && program != 0x000) {
        *why = "a part whose first block does not start program 0x700 or 0x000";
        return 0;
    }
    for (j = 0; j < part->data_count; j++) {
        if (!rd_mesh_runs_program(meshes[j], program)) {
            *why = "a part whose blocks run more than one VU1 program";
            return 0;
        }
    }
    return program + 1; /* (0 is "no") */
}

/* Model_DrawPosed(model) of an effect (model flag 0x800), natively: 1 if drawn (or nothing to draw), 0 if
   the caller is to draw it the PS2 way (a block the core cannot draw) */
int NativeDraw_Effect(ModelInstance *model) {
    static RdMesh meshes[ND_MODEL_MESHES];
    static RdMesh scratch[ND_MAX_MESHES];
    static u32 programs[0x100];
    f32 (*matrix_a)[4] = MODEL_DRAW_MATRIX_A;
    f32 (*matrix_b)[4] = MODEL_DRAW_MATRIX_B;
    ModelPart *part;
    RdDraw d;
    EEWord owner = 0;
    const char *why = "";
    int kept = 1; /* every part's meshes are in meshes[], one part after another */
    u32 used = 0;
    int i;

    if (model->parts == NULL || model->part_count == 0) {
        return 1; /* (the PS2 path queues nothing either) */
    }
    if (model->part_count > (int)(sizeof(programs) / sizeof(programs[0]))) {
        return declined(model, "more parts than this keeps");
    }
    /* every part's blocks first: a model is drawn one way or the other, whole */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        RdMesh *into = kept && used + part->data_count <= ND_MODEL_MESHES ? meshes + used : scratch;

        if (into == scratch) {
            kept = 0;
        }
        programs[i] = part_meshes(part, into, &why);
        if (programs[i] == 0) {
            return declined(model, why);
        }
        programs[i]--;
        used += part->data_count;
    }
    ModelMatrix_BuildDrawMatrices(model, model->light_block, model->light_block + 0x40, matrix_a, matrix_b);
    /* (without the parts: VU1 is left as the PS2 path leaves it, 0x64-0x66 included) */
    NativeDraw_QueueFixedBlock(model, matrix_a, matrix_b);
    ee_word_try(model, &owner);
    memset(&d, 0, sizeof(d));
    d.kind = RD_KIND_EFFECT;
    d.id[0] = (u32)owner;
    d.camera = NativeDraw_CurrentCamera();
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
    /* VU1 0x64-0x66 as Model_DrawPosed queues them: the colour (red, green, blue, alpha); 0x65.w the record id
       (its x, y, z are the packet's unwritten words, never read); 0x66 the record's TEX0 qword */
    memcpy(d.colour[0], &model->colour_red, 16);
    memcpy(&d.colour[1][3], &model->found_record_id, 4);
    memcpy(d.colour[2], model->found_record, 16);
    part = model->parts;
    used = 0;
    for (i = 0; i < model->part_count; i++, part++) {
        f32 matrix[4][4] __attribute__((aligned(16)));
        RdMesh *these = kept ? meshes + used : scratch;
        u64 value;

        used += part->data_count;
        if (MODEL_PART_FLAG_WORD(part) & MODEL_PART_BIT40) {
            continue;
        }
        if (!kept) {
            part_meshes(part, scratch, &why); /* (found again in the cache) */
        }
        Matrix_Multiply(matrix, model->model_matrix, part);
        memcpy(d.model, matrix, sizeof(d.model));
        d.id[1] = (u32)i;
        d.program = programs[i];
        value = rd_draw(&d, these, part->data_count);
        if (value != 0) {
            Packet_BeginGsRegisters();
            Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)value);
            Packet_EndGsRegisters();
        }
    }
    return 1;
}
