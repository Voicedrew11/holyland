/* attack_tracking.c: PC-only. How fast an enemy turns at the player while it winds up an attack
   (game/attack_tracking.h). */
#include "attack_tracking.h"

#include "common.h"
#include "entity/entity.h"
#include "port/controls.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/sidestep_room.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern char *entity_params;      /* chr.prm: the types' records (map/map_init.c) */
extern f32 player_top_speed[6];  /* the player's movement tables (player/player_update.c): row 0, on foot */
extern f32 player_speed_gain[6];

#define CHR_RECORDS 0x110         /* chr.prm's 297002 bytes hold 269 records */
#define ANIMATIONS 0x22           /* EntityParams.motion_speeds */
#define WIND_UP_SLOTS 64          /* enemies winding up at once */
#define REACTION_FRAMES 6         /* the sidestep starts 200 ms into the wind-up: a person's reaction */
#define FIRST_MELEE_ANIMATION 20  /* EntityAi_MeleeAttack plays animation 20 + n for melee record n */
#define MELEE_RECORDS 3

/* ---- What each attack is, by type record and the animation its wind-up starts with ---------------------------- */

typedef enum { SHAPE_UNKNOWN, SHAPE_MELEE, SHAPE_SHOT } ShapeKind;

typedef struct {
    ShapeKind kind;
    int learnt;               /* from a strike seen, not from the record */
    SidestepAttack attack;
    int seed_strike_after_turn; /* frames from the turn's end to the hit, by the record (default melee attacks) */
    int seed_tried;           /* seed_from_record has looked */
    int solved;               /* speedup is solved for solved_for_room */
    int solved_for_room;
    float speedup;
} AttackShape;

static AttackShape shapes[CHR_RECORDS][ANIMATIONS];

static int record_of(const Entity *entity) {
    ptrdiff_t offset = (const char *)entity->params - entity_params;

    if (entity_params == NULL || offset < 0 || offset % (ptrdiff_t)sizeof(EntityParams) != 0) {
        return -1;
    }
    offset /= (ptrdiff_t)sizeof(EntityParams);
    return offset < CHR_RECORDS ? (int)offset : -1;
}

static AttackShape *shape_of(const Entity *entity, int animation) {
    int record = record_of(entity);

    if (record < 0 || animation < 0 || animation >= ANIMATIONS) {
        return NULL;
    }
    return &shapes[record][animation];
}

static const EntityParamsUnk240 *melee_record(const EntityParams *params, int index) {
    if (index < 2) {
        return &params->melee_attacks[index];
    }
    return (const EntityParamsUnk240 *)params->melee_attack_third;
}

/* animation frames of animation as game frames (its motion speed: 256 is a frame a game frame) */
static int game_frames(const EntityParams *params, int animation, int animation_frames) {
    int speed = params->motion_speeds[animation];

    if (speed <= 0) {
        speed = 256;
    }
    return (animation_frames * 256 + speed - 1) / speed;
}

/* From the record, for the default melee attack: it blends in, turns until hit_start_frame and hits from its
   first part's first frame. 0 when the animation is not one of those or the record turns for none. */
static int seed_from_record(const Entity *entity, int animation, AttackShape *shape) {
    const EntityParams *params = entity->params;
    const EntityParamsUnk240 *record;
    int first_hit = 0;
    float half_arc = 0.0f;
    float bonus = 0.0f;
    int blend;
    int turn;
    int strike;

    if (animation < FIRST_MELEE_ANIMATION || animation >= FIRST_MELEE_ANIMATION + MELEE_RECORDS) {
        return 0;
    }
    record = melee_record(params, animation - FIRST_MELEE_ANIMATION);
    if (record->attack_weight == 0 || record->hit_start_frame == 0) {
        return 0;
    }
    for (int part = 0; part < 3; part++) {
        const EntityParamsUnk240Part *hit = &record->hit_parts[part];

        if (hit->first_frame == 0) {
            continue;
        }
        if (first_hit == 0 || hit->first_frame < first_hit) {
            first_hit = hit->first_frame;
        }
        if (hit->half_angle_deg * 0.017453292f > half_arc) {
            half_arc = hit->half_angle_deg * 0.017453292f;
        }
        if (hit->radius_bonus > bonus) {
            bonus = hit->radius_bonus;
        }
    }
    if (first_hit == 0) {
        return 0;
    }
    blend = params->motion_blend_frames >> 1;
    turn = blend + game_frames(params, animation, record->hit_start_frame);
    strike = blend + game_frames(params, animation, first_hit);
    if (strike < turn) {
        turn = strike; /* (a hit before the turn's end commits it) */
    }
    shape->kind = SHAPE_MELEE;
    shape->attack.turn_frames = turn;
    shape->attack.strike_frames = strike;
    shape->attack.half_arc = half_arc;
    shape->attack.reach = entity->radius + bonus;
    shape->seed_strike_after_turn = strike - turn;
    return 1;
}

/* the speed-up the Sidestep Room setting asks of this attack: 1 for a shot or an attack not yet known */
static float room_speedup(const Entity *entity, AttackShape *shape) {
    SidestepPlayer player = {player_top_speed[0], player_speed_gain[0], REACTION_FRAMES};

    if (port_options.sidestep_room >= 100 || shape->kind != SHAPE_MELEE) {
        return 1.0f;
    }
    if (!shape->solved || shape->solved_for_room != port_options.sidestep_room) {
        shape->attack.turn_limit = entity->params->turn_limit;
        shape->attack.turn_step = entity->params->turn_step_per_frame;
        shape->speedup = sidestep_speedup(&shape->attack, &player, (float)port_options.sidestep_room / 100.0f);
        shape->solved_for_room = port_options.sidestep_room;
        shape->solved = 1;
        if (getenv("KF4_SIDESTEP_LOG") != NULL) {
            printf("[sidestep] record %d animation %d (%s): turns %d frames, hits at %d, arc +-%.0f deg, reach %.0f; "
                   "room %.0f -> x%.2f for %d%%\n",
                   record_of(entity), entity->animation_index, shape->learnt ? "seen" : "chr.prm",
                   shape->attack.turn_frames, shape->attack.strike_frames, (double)(shape->attack.half_arc * 57.29578f),
                   (double)shape->attack.reach, (double)sidestep_room(&shape->attack, &player, 1.0f),
                   (double)shape->speedup, port_options.sidestep_room);
        }
    }
    return shape->speedup;
}

/* ---- The enemies winding up ------------------------------------------------------------------------------------ */

typedef enum { WIND_UP_FREE, WIND_UP_TURNING, WIND_UP_STRUCK } WindUpPhase;

typedef struct {
    Entity *entity;
    WindUpPhase phase;
    int frames;               /* game frames since the wind-up started */
    int animation;            /* the animation it started with */
    EntityParams *sped_up;    /* the record made faster for this update, NULL: none */
    f32 saved_turn_limit;
    f32 saved_turn_step;
} WindUp;

static WindUp wind_ups[WIND_UP_SLOTS];

static WindUp *wind_up_of(const Entity *entity) {
    for (int i = 0; i < WIND_UP_SLOTS; i++) {
        if (wind_ups[i].phase != WIND_UP_FREE && wind_ups[i].entity == entity) {
            return &wind_ups[i];
        }
    }
    return NULL;
}

static void start_wind_up(Entity *entity) {
    AttackShape *shape = shape_of(entity, entity->animation_index);

    if (shape != NULL && !shape->learnt && !shape->seed_tried) {
        seed_from_record(entity, entity->animation_index, shape);
        shape->seed_tried = 1;
    }
    for (int i = 0; i < WIND_UP_SLOTS; i++) {
        if (wind_ups[i].phase == WIND_UP_FREE) {
            wind_ups[i] = (WindUp){entity, WIND_UP_TURNING, 1, entity->animation_index, NULL, 0.0f, 0.0f};
            return;
        }
    }
    /* (more than WIND_UP_SLOTS at once: the rest turn as on the PS2) */
}

static float wind_up_speedup(const WindUp *wind_up) {
    AttackShape *shape = shape_of(wind_up->entity, wind_up->animation);
    float speedup = port_attack_tracking_scale();
    float room;

    if (shape == NULL) {
        return speedup;
    }
    room = room_speedup(wind_up->entity, shape);
    return room > speedup ? room : speedup;
}

void AttackTracking_BeforeUpdate(Entity *entity) {
    WindUp *wind_up = wind_up_of(entity);
    float speedup;

    if (wind_up == NULL || wind_up->phase != WIND_UP_TURNING) {
        return;
    }
    speedup = wind_up_speedup(wind_up);
    if (speedup <= 1.0f) {
        return;
    }
    wind_up->sped_up = entity->params;
    wind_up->saved_turn_limit = entity->params->turn_limit;
    wind_up->saved_turn_step = entity->params->turn_step_per_frame;
    entity->params->turn_limit *= speedup;
    entity->params->turn_step_per_frame *= speedup * speedup;
}

void AttackTracking_AfterUpdate(Entity *entity) {
    WindUp *wind_up = wind_up_of(entity);

    if (wind_up != NULL && wind_up->sped_up != NULL) {
        wind_up->sped_up->turn_limit = wind_up->saved_turn_limit;
        wind_up->sped_up->turn_step_per_frame = wind_up->saved_turn_step;
        wind_up->sped_up = NULL;
    }
    if (!(entity->busy_flags & ENTITY_BUSY_ATTACK)) {
        if (wind_up != NULL) {
            wind_up->phase = WIND_UP_FREE;
        }
        return;
    }
    if (wind_up == NULL) {
        start_wind_up(entity);
        return;
    }
    wind_up->frames++;
}

/* the owner's wind-up, while it still turns; NULL when it is not winding up (the player, a struck attack) */
static WindUp *turning_wind_up(Entity *owner) {
    WindUp *wind_up = wind_up_of(owner);

    return wind_up != NULL && wind_up->phase == WIND_UP_TURNING ? wind_up : NULL;
}

void AttackTracking_NoteHit(Entity *owner, float half_arc, float reach) {
    WindUp *wind_up = turning_wind_up(owner);
    AttackShape *shape;
    int strike;

    if (wind_up == NULL) {
        return;
    }
    wind_up->phase = WIND_UP_STRUCK;
    shape = shape_of(owner, wind_up->animation);
    if (shape == NULL || shape->learnt) {
        return;
    }
    strike = wind_up->frames + 1; /* (this update is the strike's frame) */
    if (shape->kind == SHAPE_MELEE) {
        /* the record's turn, measured: it said how long before the hit the turning stops */
        shape->attack.turn_frames = strike - shape->seed_strike_after_turn;
    } else {
        shape->attack.turn_frames = strike; /* (another type's own attack: taken to turn until it hits) */
    }
    if (shape->attack.turn_frames < 1) {
        shape->attack.turn_frames = 1;
    }
    shape->kind = SHAPE_MELEE;
    shape->learnt = 1;
    shape->attack.strike_frames = strike;
    shape->attack.half_arc = half_arc;
    shape->attack.reach = reach;
    shape->solved = 0;
}

void AttackTracking_NoteShot(Entity *owner) {
    WindUp *wind_up = turning_wind_up(owner);
    AttackShape *shape;

    if (wind_up == NULL) {
        return;
    }
    wind_up->phase = WIND_UP_STRUCK;
    shape = shape_of(owner, wind_up->animation);
    if (shape != NULL && !shape->learnt) {
        shape->kind = SHAPE_SHOT;
        shape->learnt = 1;
    }
}

/* ---- The Attack Tracking setting ------------------------------------------------------------------------------- */

float port_attack_tracking_scale(void) {
    if (port_options.attack_tracking == PORT_ATTACK_TRACKING_AUTO) {
        return port_mouse_turning() ? PORT_ATTACK_TRACKING_MOUSE : 1.0f;
    }
    return (float)port_options.attack_tracking / 100.0f;
}
