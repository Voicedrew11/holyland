/* The update's offer at the title (docs/port/LAUNCHER.md, "Updates"): when the launcher has found a newer release
   (update.json), the game's title screen asks once, in a native pop-up of the system's (SDL_ShowMessageBox): Update,
   Skip this version, or Later. Update writes update-request.json for the launcher and quits the game; the launcher
   then downloads, checks and installs the release and starts again (launcher/self_update.h).
   port/overlay/update_badge.cpp has it, with the rest of the notice. Nothing is asked without a window and
   settings.json (a headless run, a pad script), nor in a developer's build. */
#ifndef PORT_UPDATE_OFFER_H
#define PORT_UPDATE_OFFER_H

#ifdef __cplusplus
extern "C" {
#endif

/* (the game's thread, game/quicksave.c's Quick_PollTitle) once a frame on the title screens */
void port_update_title(void);

#ifdef __cplusplus
}
#endif

#endif
