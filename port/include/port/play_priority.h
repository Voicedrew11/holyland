/* Asking the system to run the game as a game: on Windows, out of its power throttling and into its multimedia
   scheduling; elsewhere nothing. port/platform/play_priority.c, in the profile library (kf4_profile) with the thread
   names, since the renderer's threads ask too.

   A player's Windows PC showed every thread several times slower than here (2026-11-23); these are what games ask of
   Windows so that it does not hold a game's CPU back. A power plan the player chose is theirs and left alone. */
#ifndef PORT_PLAY_PRIORITY_H
#define PORT_PLAY_PRIORITY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once, at the start (the main thread): the process out of Windows 11's power throttling (EcoQoS, slower clocks for a
   process Windows judges to be in the background) and out of its ignoring the 1 ms timer then. Logs what it got. */
void port_play_process(void);
/* From a thread the pictures wait for (the game's, the DMA's, the renderer's, the pump's, the presenter's): the
   thread in Windows's multimedia scheduling as a "Games" task (MMCSS), scheduled ahead of ordinary threads. */
void port_play_thread(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PLAY_PRIORITY_H */
