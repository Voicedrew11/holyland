/* attack_tracking.c: PC-only. The game's side of the attack tracking setting (port_attack_tracking_scale(),
   port/include/port/host.h).

   On the PS2 an enemy's attack turns it at the player until the strike (EntityAi_MeleeAttack until the record's
   hit_start_frame, EntityAi_RangedAttack for its turn_frames, and most types' own attacks the same way), at its
   type's turn speed. A controller turns the player about as fast as that, so the swing finds him; a mouse turns
   him as fast as the hand moves, so he can be behind it before it lands. The patched EntityAi_TurnToHeading
   multiplies that one turn by this scale. Once the strike starts the attack is committed, as on the PS2, so
   stepping out of it still works. */
#include "port/controls.h"
#include "port/host.h"
#include "port/platform.h"

float port_attack_tracking_scale(void) {
    if (port_options.attack_tracking == PORT_ATTACK_TRACKING_AUTO) {
        return port_mouse_turning() ? PORT_ATTACK_TRACKING_MOUSE : 1.0f;
    }
    return (float)port_options.attack_tracking / 100.0f;
}
