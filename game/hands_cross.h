/* The hands mod's cross (game/hands_cross.c): three cards at the bottom left of play, the spell, the weapon and
   the hotkey in use, each with its item's menu model, drawn after the game's HUD (Hud_Draw). What it shows is
   port_hands_state (port/include/port/hands.h); game/hands.c tells it when a card changed or a press was
   refused. */
#ifndef GAME_HANDS_CROSS_H
#define GAME_HANDS_CROSS_H

#include "port/hands.h"

typedef struct {
    unsigned long long changed_vblank[HAND_COUNT]; /* the card's contents changed (it pulses) */
    unsigned long long refused_vblank[HAND_COUNT]; /* a press did nothing (it flashes red) */
    unsigned long long pressed_vblank;             /* the last press of any hand key (the cross comes back from its fade) */
} HandsFeedback;
extern HandsFeedback hands_feedback;

/* (GameLoop_DrawFrame, after Hud_Draw) the cross, while the mod is on and the game shows its HUD */
void Hands_DrawCross(void);

#endif
