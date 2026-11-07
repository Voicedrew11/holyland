/* nd_shadows.c: PC-only. Native draw (--native-draw shadows, PORT_ND_SHADOWS; docs/port/NATIVE_DRAW.md 1.2.6,
   NATIVE_DRAW_TASKS.md P5-1) of the entities' shadows: Entity_DrawPoolModels (entity/entity_ai.c).

   For each live entity with flag bit 17, Entity_DrawPoolModels takes the pool slot's second model (built by
   EntityPool_BuildBlockModel: posed, its blocks set to VU1's program 0x5A0 and FGE), copies the entity's
   pose into it, flattens it (its model matrix is the entity's by fire_lord_model_matrix: diag(1, 0, 1), y
   -5), gives it the entity's light and fog blocks, queues ALPHA_2 (FIX: the entity's alpha / 4) and calls
   its draw_parts, Model_DrawPosed. Around the loop it has set the GS up itself: the screen's alpha cleared
   (GsDraw_SetScreenAlpha(0)), PRMODECONT 0 with PRMODE 0x248 (Gouraud, blended, context 2), TEST_2 with
   DATE and the alpha test failing into the frame only, FBA_2 1: each pixel is darkened once.

   The call site is that draw_parts call (entity.patch): this queues what Model_DrawPosed would, drawn
   natively:
   - VU1's fixed block (the light matrices, the fog, FOGCOL, the point lights), without the parts, so that
     VU1 is left as the PS2 path leaves it (native_draw.c's queue_fixed_block);
   - per part (not switched off by bit40) a draw of program 0x5A0 (RdDraw.program 0x5A0, meshes of
     RD_VF_SILHOUETTE: the parser's 0x5A0 batches, render.cpp) with the part's matrix (the model's by the
     part's) and the fixed block's values by value; the renderer draws it with the program 0x000 shader
     (pcr_nd000.vert), the lights and the fog taken out, as VU1's program 0x5A0 would.
   Model_DrawPosed queues no GS registers of its own, so neither does this: ALPHA_2 and the rest stay the
   caller's, in the chain where they were.

   The draws' identity: kind RD_KIND_SHADOW, the entity's own model (its EE word, as its own draws'
   identity: nd_entities.c), the part. */
#include "common.h"
#include "model/model.h"
#include "map/map.h"
#include "entity/entity.h"
#include "gfx/gs.h"
#include "port/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Matrix_Multiply(void *dst, void *matrix, void *src);
void ModelMatrix_BuildDrawMatrices(ModelInstance *, sptr, sptr, void *, void *);
void Model_DrawPosed(ModelInstance *model);
void Packet_BeginGsRegisters(void);
void Packet_AddGsRegister(int reg, s64 value);
void Packet_EndGsRegisters(void);

/* native_draw.c's helpers (its "For the other adapter files") */
u32 NativeDraw_CurrentCamera(void);
void NativeDraw_QueueFixedBlock(ModelInstance *model, const void *matrix_a, const void *matrix_b);

#define ND_MAX_MESHES 2048 /* (ModelPart.data_count is 11 bits; as native_draw.c) */
/* the meshes of all a model's parts, kept from the check that every block parses to the draws (each block
   hashed once a draw: P2-6); a model of more blocks has its parts' meshes found again part by part */
#define ND_MODEL_MESHES 8192

/* a part's meshes (its data blocks, as Model_DrawPosed sends them: a DMA REF of each block's qwords after its
   first) as program 0x5A0's: 0 if one cannot be drawn natively, or the part's first block does not start
   program 0x5A0 (its MSCAL: Model_SetVu1Program) */
static int part_meshes(ModelPart *part, RdMesh *meshes) {
    ModelPartData *data = EE_PTR(ModelPartData *, part->data_addr);
    int j;

    if (part->data_count > ND_MAX_MESHES) {
        return 0;
    }
    for (j = 0; j < part->data_count; j++) {
        u32 qwc = *(u32 *)data & 0xFFFF;

        meshes[j] = rd_mesh_from_block((const u8 *)data + 0x10, qwc, (u32)EE_WORD(data), RD_GEN_HASH, RD_VF_SILHOUETTE);
        if (meshes[j] == 0) {
            return 0;
        }
        data = (ModelPartData *)((char *)data + (qwc + 1) * 16);
    }
    return part->data_count == 0 || rd_mesh_program(meshes[0]) == 0x5A0;
}

/* (--stats' "models declined": native_draw.c's declined) */
static int declined(ModelInstance *model, const char *why) {
    static int logged;

    rd_stats.declined++;
    if (logged < 32 && getenv("KF4_ND_LOG")) {
        logged++;
        fprintf(stderr, "[native draw] kind %d model %#x drawn the PS2 way: %s\n", RD_KIND_SHADOW, (u32)EE_WORD(model), why);
    }
    return 0;
}

/* model->draw_parts(model) for a shadow (Entity_DrawPoolModels), natively: 1 if drawn (or nothing to draw), 0
   if the caller is to draw it the PS2 way (its draw_parts is not Model_DrawPosed, or a block the core cannot
   draw); owner: the entity's own model, the draws' identity. */
int NativeDraw_Shadow(ModelInstance *model, const void *owner) {
    static RdMesh meshes[ND_MODEL_MESHES];
    static RdMesh scratch[ND_MAX_MESHES];
    f32 (*matrix_a)[4] = MODEL_DRAW_MATRIX_A;
    f32 (*matrix_b)[4] = MODEL_DRAW_MATRIX_B;
    ModelPart *part;
    RdDraw d;
    EEWord id = 0;
    int kept = 1; /* every part's meshes are in meshes[], one part after another */
    u32 used = 0;
    int i;

    if (model->draw_parts != Model_DrawPosed) {
        return declined(model, "draw_parts is not Model_DrawPosed");
    }
    if (model->parts == NULL || model->part_count == 0) {
        return 1; /* (the PS2 path queues nothing either) */
    }
    /* every part's blocks first: a model is drawn one way or the other, whole */
    part = model->parts;
    for (i = 0; i < model->part_count; i++, part++) {
        RdMesh *into = kept && used + part->data_count <= ND_MODEL_MESHES ? meshes + used : scratch;

        if (into == scratch) {
            kept = 0;
        }
        if (!part_meshes(part, into)) {
            return declined(model, "a block the core cannot draw as program 0x5A0's");
        }
        used += part->data_count;
    }
    ModelMatrix_BuildDrawMatrices(model, model->light_block, model->light_block + 0x40, matrix_a, matrix_b);
    NativeDraw_QueueFixedBlock(model, matrix_a, matrix_b);
    ee_word_try(owner, &id);
    memset(&d, 0, sizeof(d));
    d.kind = RD_KIND_SHADOW;
    d.id[0] = (u32)id;
    d.program = 0x5A0;
    d.camera = NativeDraw_CurrentCamera();
    /* (the fixed block as Model_DrawPosed sends it, by value: 0x5A0 reads only the fog's packet, FOGCOL) */
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
            part_meshes(part, scratch); /* (found again in the cache) */
        }
        Matrix_Multiply(matrix, model->model_matrix, part);
        memcpy(d.model, matrix, sizeof(d.model));
        d.id[1] = (u32)i;
        value = rd_draw(&d, these, part->data_count);
        if (value != 0) {
            Packet_BeginGsRegisters();
            Packet_AddGsRegister(PORT_GS_REG_NATIVE_DRAW, (s64)value);
            Packet_EndGsRegisters();
        }
    }
    return 1;
}
