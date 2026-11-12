/* screen_fx.c: PC-only, debugging. KF4_FX forces the game's full-screen effects on, so that each can be
   tested headless (tools/screen_fx_test.py; docs/port/SCREEN_EFFECTS.md lists them all). Without KF4_FX
   nothing here does anything.

   KF4_FX=NAME[,NAME...]: the patched GameLoop_DrawFrame calls ScreenFx_Begin after the world (before
   the screen passes) and ScreenFx_End at its end. What Begin sets for the passes End puts back, so the
   game's own state (the knock-back, the under-water bits, the options) is as it was for the next
   frame's update: only the picture changes. The names:
   - water: PlayerState status bit 39 (under water: the copy of the frame, the wobbling screen, the tint;
     on map 035 before its water is purified, the green fill instead of the tint)
   - drain: PlayerState.mp_drain_tint at 8 (the MP drain's wobbling screen, purple); drainramp: 1 to 8
     and back, a step every 4 frames (the drain's colour and wobble changing)
   - prev: MapDrawState bit 40 at alpha 0x40 (the frame before blended in); aim: PlayerState.aim_speed
     above 0 (the knock-back's frame before)
   - haze: bit 44 (Game_DrawHazeLayers); depth: bit 41 (Game_DrawDepthTestSprite); down: bit 42 (the
     one-row blend, --flicker-filter's); updown: bit 43; shift: bit 45 (the view-turn shift, drawn only
     with --no-whole-frame)
   - fill: MapDrawState's screen fill (red, alpha 0x40); fade: PlayerState.screen_fade (white, 0x40);
     tint: player_screen_tint's purple and darkness; dim: the message dim; bright40, brightff: the
     brightness option at 0x40 or 0xFF
   - spell=N: the magic in hand (PlayerState.equipped_magic and charging_magic) N each frame, for a pad
     script to cast it (with --cheats spells,magic)
   - succubus: the player put 600 before the creature of chr record 0x47 (the Dark Succubus, map 038,
     the one drawn with a copy of the screen, Entity_PrepareScreenTexturedDraw), each frame, when it is
     there (only once the story has woken it; this one changes the game)
   - creature=ROW[:DIST]: once, when a creature of chr record ROW has been there 60 frames, the player put
     DIST (600) before it along the heading the player has, so it is ahead (tools/motion_compare.py's combat
     and creature cases; this one changes the game)
   - copytex: what the Dark Succubus' draw does to the screen without it: GsDraw_CopyFrameToTexture
     into its 256 x 256 texture at 0x3540, then a 256 x 256 sprite in the middle of the screen drawn
     from that texture
   - mapview=N: the map viewer of map item N (1 to 4 the picture maps, 5 the King's Map: the world drawn
     from above with Game_BlendScreenOffsetDown and Game_TintScreen over it), opened once, 60 frames into
     play, as using the item does
   KF4_FX_LOG=1 prints, every 60 frames, the map, the player's place and the Dark Succubus' distance. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "entity/entity.h"
#include "map/map.h"
#include "player/player.h"
#include "gfx/packet.h"

void GsDraw_CopyFrameToTexture(int fbp, int psm, int width, int height);
void GsDraw_ResetDrawTarget(void);
void GsDraw_QueueTexFlush(void);
void GsDraw_QueueTexturedSprite(void *sprite);
void MapViewer_OpenForItem(int viewer_number);

extern PlayerState player_state;
extern PlayerScreenTint player_screen_tint;
extern MapDrawStateOverlays map_draw_state;
extern u8 game_options[0x10];
extern int message_dim_alpha;
extern u32 frame_counter;
extern Entity *active_entities_head;
extern Entity entity_pool[2][0x80];
extern MapSlot map_slots[];
extern int current_map_slot;

enum {
    FX_WATER = 1 << 0,
    FX_DRAIN = 1 << 1,
    FX_DRAINRAMP = 1 << 2,
    FX_PREV = 1 << 3,
    FX_AIM = 1 << 4,
    FX_HAZE = 1 << 5,
    FX_DEPTH = 1 << 6,
    FX_DOWN = 1 << 7,
    FX_UPDOWN = 1 << 8,
    FX_SHIFT = 1 << 9,
    FX_FILL = 1 << 10,
    FX_FADE = 1 << 11,
    FX_TINT = 1 << 12,
    FX_DIM = 1 << 13,
    FX_BRIGHT40 = 1 << 14,
    FX_BRIGHTFF = 1 << 15,
    FX_SUCCUBUS = 1 << 16,
    FX_SPELL = 1 << 17,
    FX_COPYTEX = 1 << 18,
    FX_MAPVIEW = 1 << 19,
    FX_CREATURE = 1 << 20,
};

static const struct {
    const char *name;
    int bit;
} fx_names[] = {
    {"water", FX_WATER},   {"drain", FX_DRAIN},     {"drainramp", FX_DRAINRAMP}, {"prev", FX_PREV},
    {"aim", FX_AIM},       {"haze", FX_HAZE},       {"depth", FX_DEPTH},         {"down", FX_DOWN},
    {"updown", FX_UPDOWN}, {"shift", FX_SHIFT},     {"fill", FX_FILL},           {"fade", FX_FADE},
    {"tint", FX_TINT},     {"dim", FX_DIM},         {"bright40", FX_BRIGHT40},   {"brightff", FX_BRIGHTFF},
    {"succubus", FX_SUCCUBUS}, {"copytex", FX_COPYTEX},
};

static int fx_read, fx, fx_log, fx_spell, fx_mapview, fx_frames, fx_creature, fx_creature_done, fx_creature_seen;
static float fx_creature_distance = 600.0f;

/* What Begin changed, for End */
static struct {
    int active;
    u64 status_bits;
    s32 mp_drain_tint;
    MapDrawStateOverlays overlays;
    f32 aim_speed, screen_fade;
    PlayerScreenTint tint;
    int dim;
    u8 brightness;
} kept;

static void fx_parse(void) {
    const char *p = getenv("KF4_FX");
    size_t i;

    fx_read = 1;
    fx_log = getenv("KF4_FX_LOG") != NULL;
    while (p != NULL && *p) {
        size_t n = strcspn(p, ",");

        for (i = 0; i < sizeof(fx_names) / sizeof(fx_names[0]); i++) {
            if (strlen(fx_names[i].name) == n && strncmp(p, fx_names[i].name, n) == 0) {
                fx |= fx_names[i].bit;
                break;
            }
        }
        if (i == sizeof(fx_names) / sizeof(fx_names[0])) {
            if (n > 6 && strncmp(p, "spell=", 6) == 0) {
                fx |= FX_SPELL;
                fx_spell = atoi(p + 6);
            } else if (n > 9 && strncmp(p, "creature=", 9) == 0) {
                const char *colon = memchr(p, ':', n);

                fx |= FX_CREATURE;
                fx_creature = atoi(p + 9);
                if (colon != NULL) {
                    fx_creature_distance = (float)atof(colon + 1);
                }
            } else if (n > 8 && strncmp(p, "mapview=", 8) == 0) {
                fx |= FX_MAPVIEW;
                fx_mapview = atoi(p + 8);
            } else {
                fprintf(stderr, "[fx] KF4_FX: '%.*s' is not an effect\n", (int)n, p);
            }
        }
        p += n;
        p += *p == ',';
    }
    if (fx) {
        fprintf(stderr, "[fx] KF4_FX forces 0x%x\n", fx);
    }
}

/* The nearest creature of chr record `type` (NULL: none) and its distance: in the whole pool, or (active)
   only those the game updates (the pool keeps the spawn records of creatures gone) */
static Entity *nearest_creature(int type, int active, float *distance) {
    Entity *e, *best = NULL;
    float best_d = 1e30f;

    int k;

    for (k = 0; k < 2 * 0x80; k++) {
        float *pos, dx, dz, d;

        e = active ? (k == 0 ? active_entities_head : e->next) : &entity_pool[k / 0x80][k % 0x80];
        if (active && e == NULL) {
            break;
        }
        if (e->spawn_info == NULL || e->spawn_info->type_id != type || e->model == NULL) {
            continue;
        }
        pos = (float *)((char *)e->model + 0xC0);
        dx = pos[0] - player_state.position.x;
        dz = pos[2] - player_state.position.z;
        d = sqrtf(dx * dx + dz * dz);
        if (d < best_d) {
            best_d = d;
            best = e;
        }
    }
    *distance = best_d;
    return best;
}

static Entity *nearest_succubus(float *distance) {
    return nearest_creature(0x47, 0, distance);
}

/* Called at the start of each game frame's draw (FrameInterp_BeginFrame's caller, before anything is
   drawn): what changes the game itself (spell, flower) */
void ScreenFx_Frame(void) {
    if (!fx_read) {
        fx_parse();
    }
    fx_frames++;
    if ((fx & FX_MAPVIEW) && fx_frames == 60) {
        fprintf(stderr, "[fx] frame %u: map viewer %d asked for\n", frame_counter, fx_mapview);
        MapViewer_OpenForItem(fx_mapview);
    }
    if (fx & FX_SPELL) {
        player_state.equipped_magic = (u8)fx_spell;
        player_state.charging_magic = (u8)fx_spell;
    }
    if (fx & FX_SUCCUBUS) {
        float d;
        Entity *e = nearest_succubus(&d);

        if (e != NULL) {
            float *pos = (float *)((char *)e->model + 0xC0);
            float h = player_state.rotation.y;

            /* stand 600 back along the heading the player has, so the creature is ahead */
            player_state.position.x = pos[0] - sinf(h) * 600.0f;
            player_state.position.z = pos[2] - cosf(h) * 600.0f;
        }
    }
    if ((fx & FX_CREATURE) && !fx_creature_done) {
        float d;
        Entity *e = nearest_creature(fx_creature, 1, &d);

        float *pos = e != NULL ? (float *)((char *)e->model + 0xC0) : NULL;

        /* a creature just spawned (or a map just loaded) is not placed yet: wait until it has been there
           60 frames, somewhere other than the origin */
        fx_creature_seen = pos != NULL && (pos[0] != 0.0f || pos[2] != 0.0f) ? fx_creature_seen + 1 : 0;
        if (fx_creature_seen >= 60) {
            float h = player_state.rotation.y;

            player_state.position.x = pos[0] - sinf(h) * fx_creature_distance;
            player_state.position.y = pos[1] - 50.0f; /* y grows downward: from a little over its feet */
            player_state.position.z = pos[2] - cosf(h) * fx_creature_distance;
            fx_creature_done = 1;
            fprintf(stderr, "[fx] frame %u: put before chr record %d (it was %.0f away)\n", frame_counter, fx_creature, d);
        }
    }
    if (fx_log && frame_counter % 60 == 0) {
        float d;
        Entity *e = nearest_succubus(&d);
        Entity *a;

        fprintf(stderr, "[fx] frame %u creatures (chr record, place):", frame_counter);
        for (a = active_entities_head; a != NULL; a = a->next) {
            if (a->spawn_info != NULL && a->model != NULL) {
                float *p = (float *)((char *)a->model + 0xC0);

                fprintf(stderr, " %d@%.0f,%.0f,%.0f", a->spawn_info->type_id, p[0], p[1], p[2]);
            }
        }
        fprintf(stderr, "\n");

        fprintf(stderr, "[fx] frame %u map %02X pos %.0f,%.0f,%.0f heading %.3f; chr 0x47: %s %.0f\n", frame_counter,
                map_slots[current_map_slot].tag, player_state.position.x, player_state.position.y, player_state.position.z,
                player_state.rotation.y, e != NULL ? "nearest at" : "none", e != NULL ? d : 0.0f);
    }
}

/* After the world, before the screen passes */
void ScreenFx_Begin(void) {
    if (!fx_read) {
        fx_parse();
    }
    if (!(fx & ~(FX_SPELL | FX_SUCCUBUS | FX_CREATURE))) {
        return;
    }
    kept.active = 1;
    kept.status_bits = player_state.status_bits;
    kept.mp_drain_tint = player_state.mp_drain_tint;
    kept.overlays = map_draw_state;
    kept.aim_speed = player_state.aim_speed;
    kept.screen_fade = player_state.screen_fade;
    kept.tint = player_screen_tint;
    kept.dim = message_dim_alpha;
    kept.brightness = game_options[10];
    if (fx & FX_WATER) {
        player_state.status_bits |= (u64)1 << 39;
    }
    if (fx & FX_DRAIN) {
        player_state.mp_drain_tint = 8;
    }
    if (fx & FX_DRAINRAMP) {
        u32 step = (frame_counter / 4) % 14; /* 1 .. 8 .. 2 */

        player_state.mp_drain_tint = (s32)(step < 8 ? step + 1 : 15 - step);
    }
    if (fx & FX_PREV) {
        map_draw_state.bit40 = 1;
        map_draw_state.previous_frame_alpha = 0x40;
    }
    if (fx & FX_AIM) {
        player_state.aim_speed = 1.0f;
    }
    if (fx & FX_HAZE) {
        map_draw_state.bit44 = 1;
    }
    if (fx & FX_DEPTH) {
        map_draw_state.bit41 = 1;
    }
    if (fx & FX_DOWN) {
        map_draw_state.bit42 = 1;
    }
    if (fx & FX_UPDOWN) {
        map_draw_state.bit43 = 1;
    }
    if (fx & FX_SHIFT) {
        map_draw_state.bit45 = 1;
    }
    if (fx & FX_FILL) {
        map_draw_state.screen_fill_red = 0x80;
        map_draw_state.screen_fill_green = 0;
        map_draw_state.screen_fill_blue = 0;
        map_draw_state.screen_fill_alpha = 0x40;
        map_draw_state.screen_fill_blend = 0x44;
    }
    if (fx & FX_FADE) {
        player_state.screen_fade = 64.0f;
    }
    if (fx & FX_TINT) {
        player_screen_tint.kind = 3;
        player_screen_tint.purple_tint_alpha = 0x30;
        player_screen_tint.darkness_tint_alpha = 0x20;
    }
    if (fx & FX_DIM) {
        message_dim_alpha = 0x40;
    }
    if (fx & FX_BRIGHT40) {
        game_options[10] = 0x40;
    }
    if (fx & FX_BRIGHTFF) {
        game_options[10] = 0xFF;
    }
    if (fx & FX_COPYTEX) {
        ScreenSprite sprite;

        GsDraw_CopyFrameToTexture(0x3540, 0, 0x100, 0x100);
        GsDraw_ResetDrawTarget();
        GsDraw_QueueTexFlush();
        memset(&sprite, 0, sizeof(sprite));
        sprite.tex0 = 0x220013540ULL | (1ULL << 34); /* (the model's TEX0, Model_SetBlockGsWords; TCC) */
        sprite.tex1 = 0x60;
        sprite.prim = 0x156;
        sprite.rgba[0] = sprite.rgba[1] = sprite.rgba[2] = sprite.rgba[3] = 0x80;
        sprite.vertex[0].x = (0x800 - 128) << 4;
        sprite.vertex[0].y = (0x800 - 128) << 4;
        sprite.vertex[1].x = (0x800 + 128) << 4;
        sprite.vertex[1].y = (0x800 + 128) << 4;
        sprite.vertex[0].z = sprite.vertex[1].z = 0xFFFFFFFF;
        sprite.vertex[1].tex_s = 256 << 4;
        sprite.vertex[1].tex_t = 256 << 4;
        GsDraw_QueueTexturedSprite(&sprite);
    }
}

/* At the end of the frame's draw: the game's state as it was */
void ScreenFx_End(void) {
    if (!kept.active) {
        return;
    }
    kept.active = 0;
    if (fx & FX_WATER) {
        player_state.status_bits = kept.status_bits;
    }
    if (fx & (FX_DRAIN | FX_DRAINRAMP)) {
        player_state.mp_drain_tint = kept.mp_drain_tint;
    }
    if (fx & (FX_PREV | FX_HAZE | FX_DEPTH | FX_DOWN | FX_UPDOWN | FX_SHIFT | FX_FILL)) {
        map_draw_state = kept.overlays;
    }
    if (fx & FX_AIM) {
        player_state.aim_speed = kept.aim_speed;
    }
    if (fx & FX_FADE) {
        player_state.screen_fade = kept.screen_fade;
    }
    if (fx & FX_TINT) {
        player_screen_tint = kept.tint;
    }
    if (fx & FX_DIM) {
        message_dim_alpha = kept.dim;
    }
    if (fx & (FX_BRIGHT40 | FX_BRIGHTFF)) {
        game_options[10] = kept.brightness;
    }
}
