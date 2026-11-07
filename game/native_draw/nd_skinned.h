/* nd_skinned.h: PC-only. Native draw's skinned adapter (docs/port/NATIVE_DRAW_TASKS.md, P2-3), shared
   by the adapter files of game/native_draw/ that draw a model through Model_DrawSkinned (program 0x280):
   nd_entities.c defines it (the entities); nd_objects.c calls it for the gold and the map objects whose
   definition bit8 is 1 (the jointed ones, set up by Model_SetUpSkinned). */
#ifndef ND_SKINNED_H
#define ND_SKINNED_H

#include "model/model.h"

/* Model_DrawSkinned(model), natively: the same matrices (into the same scratchpad), the same parts left
   out, a draw per part on program 0x280. kind (RD_KIND_*) and owner (its EE word) make the draws'
   identity, id[1] the part. 1 if drawn; 0 if a part it would send has a block the core cannot draw (or
   the model has no parts), and nothing was queued: the caller draws it the PS2 way. It does not count a
   decline (rd_stats.declined): the caller does. */
int NativeDraw_Skinned(ModelInstance *model, int kind, const void *owner);

#endif
