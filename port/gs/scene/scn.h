/* The scene renderer (docs/plans/scene-renderer-design.md; slice 1: docs/plans/HANDOFF_scene_slice1.md), --renderer scene,
   developers' only: the game's frame built into a scene and drawn from it (port/gs/scene/scn_build.h), under the PC
   renderer's shell (port/gs/pc/pcr.h: its thread, inbox, scanout ring, presenter, save states and captures). Opened
   through port/platform/gs_api.h's gsr_open_scene; every other call is the shell's pcr_*. So far (slice 1): game frames
   at 30, a picture between showing the last picture again. */
#ifndef PORT_GS_SCENE_SCN_H
#define PORT_GS_SCENE_SCN_H

#ifdef __cplusplus
extern "C" {
#endif

/* As pcr_open (pcr_enable_present and the settings before it as for that), with the scene renderer drawing the frame
   in the canvas's place. 0 without a Vulkan device it can use. */
int scn_open(void *shared_context);

#ifdef __cplusplus
}
#endif

#endif
