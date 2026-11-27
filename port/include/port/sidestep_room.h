/* sidestep_room.h: how much faster an enemy must turn at the player while winding up an attack to leave a
   controller player a given share of the room the PS2 leaves to sidestep it (the Sidestep Room setting).

   The room is a distance: a player standing that near in front of the enemy, who starts a sidestep from rest a
   moment after the wind-up starts, is out of the hit's arc when it lands. Sidestepping across the enemy's front
   moves the player round it fastest when near, so the room is everything nearer than that distance (up to the
   hit's reach). The enemy turns by the game's own rule (EntityAi_TurnToHeading): its turn speed builds by
   turn_step a frame up to turn_limit, and drops to 0 when it catches the player. Faster by k means the top
   speed times k and the build-up times k squared: the same turn played k times as fast.

   One k does not fit all attacks: a long wind-up needs little, a quick jab needs a lot (the k that halves the room
   ran 1.1 to 3.7 over the game's 69 melee attacks). So the caller solves k for each attack.

   Pure: no game state. Runs on the game's thread (game/attack_tracking.c) and in tests/sidestep_room_test.c. */
#ifndef PORT_SIDESTEP_ROOM_H
#define PORT_SIDESTEP_ROOM_H

#ifdef __cplusplus
extern "C" {
#endif

#define SIDESTEP_MAX_SPEEDUP 4.0f /* the most sidestep_speedup gives */

/* one attack of one enemy type, in game frames (30 a second) and world units */
typedef struct {
    float turn_limit;  /* the type's top turn speed, radians a frame (EntityParams.turn_limit) */
    float turn_step;   /* how much its turn speed builds each frame (EntityParams.turn_step_per_frame) */
    int turn_frames;   /* frames from the wind-up's start that it turns at the player for */
    int strike_frames; /* frames from the wind-up's start to the hit; turn_frames or more */
    float half_arc;    /* half the hit's arc either side of the enemy's heading, radians */
    float reach;       /* how far from the enemy the hit reaches */
} SidestepAttack;

/* the player's sidestep */
typedef struct {
    float top_speed;     /* top sideways speed, world units a frame (player_top_speed) */
    float speed_gain;    /* sideways speed gained each frame (player_speed_gain) */
    int reaction_frames; /* frames after the wind-up starts before the player steps */
} SidestepPlayer;

/* The room the attack leaves (see above) with the enemy turning speedup times as fast: a distance from 0 (no
   sidestep gets out) to attack->reach. */
float sidestep_room(const SidestepAttack *attack, const SidestepPlayer *player, float speedup);

/* The speed-up, 1 to SIDESTEP_MAX_SPEEDUP, that leaves room_share (0 to 1) of the room the PS2 leaves (speed-up 1):
   1 when room_share is 1 or more, or when the PS2 leaves no room; SIDESTEP_MAX_SPEEDUP when even that leaves more. */
float sidestep_speedup(const SidestepAttack *attack, const SidestepPlayer *player, float room_share);

#ifdef __cplusplus
}
#endif

#endif
