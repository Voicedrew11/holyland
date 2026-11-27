/* attack_tracking.h: PC-only. How fast an enemy turns at the player while it winds up an attack: the Sidestep
   Room setting (port/sidestep_room.h) and the Attack Tracking setting (port_attack_tracking_scale, port/host.h).

   On the PS2 an enemy's attack turns it at the player until the strike, at its type's turn speed; once the strike
   starts the attack is committed. Here, for the wind-up only, the enemy's type record (EntityParams.turn_limit and
   turn_step_per_frame) is made faster by a speed-up k (top speed times k, build-up times k squared) for the length
   of that one enemy's update, then put back. Every type's turning reads those two values, the shared
   EntityAi_TurnToHeading and the types' own turn code alike, so all of them follow without a patch each.

   k is the larger of Attack Tracking's and Sidestep Room's. Sidestep Room's is solved for each attack (an enemy
   type's chr.prm record and the animation its wind-up starts with) by sidestep_speedup: from the record for the
   default melee attack (EntityAi_MeleeAttack, animations 20 to 22) before it is first seen, then from what the
   first strike showed: the frames from the wind-up's start to its first hit check (Attack_Run), the hit's arc and
   reach. An attack whose strike is a shot keeps the PS2's turning.

   Everything runs on the game's thread. The wind-ups being watched are not in save states: a state loaded during
   one finishes that wind-up at the PS2's speed. */
#ifndef GAME_ATTACK_TRACKING_H
#define GAME_ATTACK_TRACKING_H

#include "entity/entity.h"

/* (Entity_UpdateAll, before an enemy's update_func) the enemy's type record made faster while it winds up */
void AttackTracking_BeforeUpdate(Entity *entity);
/* (Entity_UpdateAll, after it) the record put back; a wind-up starting or ending noted */
void AttackTracking_AfterUpdate(Entity *entity);

/* (Attack_Run, a hit on the player) the owner's attack strikes: committed from here, and its shape learnt */
void AttackTracking_NoteHit(Entity *owner, float half_arc, float reach);
/* (Attack_LaunchMagicEffect) the owner's attack strikes with a shot: committed, and the PS2's turning kept */
void AttackTracking_NoteShot(Entity *owner);

#endif
