/* The hands mod's "Put in" box on the equipment screen (game/hands_put.c): Cross on an item of the Weapon, Magic
   or Shortcut row opens it over the screen, one line a slot (Weapon 1 and 2, Magic 1 and 2, Shortcut 1 to 5),
   each with the item it holds and an "E" on the slot in hand. Up and Down choose a slot, Cross puts the item
   there, Circle (or Triangle, Square) closes it; the mouse points at the lines. Drawn with the menus' own text,
   bars and colours (ui/menu_draw.c).

   Putting the item in the slot in hand is the game's own equip: the box gives the screen one Cross, and its
   Cross handling (Hands_Choose then lets it through) equips as without the mod. Another slot keeps the item
   there (port_hands_state). Runs on the game's thread, from Menu_UpdateEquip. */
#ifndef GAME_HANDS_PUT_H
#define GAME_HANDS_PUT_H

#include "common.h"
#include "port/hands.h"

/* (game/hands.c, the screen's Cross on an item of a row with slots) opens the box for that item's value */
void HandsPut_Open(HandKind kind, int value);
/* (Menu_BuildItemRings: the menu opening and the equipment screen closing) the box gone at once */
void HandsPut_Close(void);

/* (Menu_UpdateEquip, before it reads a button) the buttons the screen sees this frame: all of them while the box
   is shut; none while it is up, except a lone Cross on the frame the slot in hand is chosen */
u16 HandsPut_Buttons(u16 buttons);
/* (game/hands.c, the screen's Cross) 1 once after HandsPut_Buttons let a Cross through: the game equips */
int HandsPut_TakeEquip(void);

/* (Menu_UpdateEquip, after the screen's text) the box, while up or fading */
void HandsPut_Draw(void);

/* ---- What the box asks game/hands.c ---------------------------------------------------------------------- */
/* the slots in hand take what the game holds (the screen may have equipped since) */
void Hands_FollowGame(void);
/* the game's text of a value's name (item_name_defs, fx_table); NULL for an empty slot */
const u16 *Hands_ValueName(HandKind kind, int value);

#endif
