/* The extended stats mod's health bar of the creature last hit (game/stats_bar.c, port/include/port/stats.h), drawn
   with the game's own HUD pictures and font, where the player chose (port_stats_options.bar_place). */
#ifndef GAME_STATS_BAR_H
#define GAME_STATS_BAR_H

/* (GameLoop_DrawFrame, after Hud_Draw and before the hands mod's cross, whose pictures go over gage.tx2) the bar, while
   the mod and its hit log are on, the game shows its HUD, and a creature was hit in the last few seconds */
void StatsBar_Draw(void);

#endif
