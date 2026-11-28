/* frame_interp.c: PC-only. The game's side of --frame-rate 60 to 240 (port_frame_interp(),
   port/include/port/host.h).

   The game draws its world at 30 Hz: a picture every second vblank. With the switch on, the GS front
   end (port/platform/frame_interp.c) draws more pictures between, from the frame the game has just
   drawn, its 3D moved to a camera between that frame's and the one before.
   For that it has to know, in the packet chain, where the 3D world is and which camera drew it. The
   patched GameLoop_DrawFrame says so: FrameInterp_BeginFrame at its start, then each
   GameLoop_SetCameraMatrices it makes queues its camera (world to view, and view to screen) with
   FrameInterp_MarkCamera, and FrameInterp_EndWorld follows the world, before the screen passes and
   the HUD. FrameInterp_BeginArm and FrameInterp_EndWorld mark the arm and the held item (drawn later,
   in the world before the eye), which move only as the arm moves. A mark is a run of GS register writes of its own (PORT_GS_REG_FRAME_INTERP, an
   address the GS does not have), queued in the chain where the call is made. With the switch off
   nothing is queued, and the chain is the PS2's.

   The volumetric fog (--fog volumetric, port/fog.h) marks the world the same way at every frame rate: the scene
   renderer draws its fog where the world ends (the first FrameInterp_EndWorld, before the screen passes and the depth's
   clear for the arm), from the world's depth, by its camera, with the map's fog as it is now, which the world's camera
   mark carries (PORT_FRAME_INTERP_FOG). */
#include "common.h"
#include "map/map.h"

#include <string.h>

#include "port/fog.h"
#include "port/mouse_lead.h"
#include "port/platform.h"

extern MapDrawState map_draw_state;

void Packet_BeginGsRegisters(void);
void Packet_AddGsRegister(int reg, s64 value);
void Packet_EndGsRegisters(void);

extern f32 world_view_matrix[0x10];  /* world to view */
extern f32 view_screen_matrix[0x10]; /* view to screen */

static int marking; /* between FrameInterp_BeginFrame and FrameInterp_EndWorld */
static int sky;     /* FrameInterp_MarkSky's last */

/* Whether the world is marked: for the pictures between, or for the volumetric fog */
static int marks_wanted(void) {
    return port_frame_interp() || port_fog() == PORT_FOG_VOLUMETRIC;
}

static void add_field(int index, u32 bits) {
    Packet_AddGsRegister(PORT_GS_REG_FRAME_INTERP, (s64)(((u64)index << 32) | bits));
}

static void add_float(int index, f32 value) {
    u32 bits;

    memcpy(&bits, &value, 4);
    add_field(index, bits);
}

void NativeDraw_BeginFrame(void); /* (game/native_draw/native_draw.c) */
void NativeDraw_MarkCamera(int projection, int mode);
const char *Quick_WhyNotInPlay(void); /* (game/quicksave.c): NULL while the play has the player's control */

/* The clock's mark: this is a frame of the play's own loop, so --game-speed may run the game's clock
   fast here; when an event, the pause, a message or a menu has the player's control it may not, and
   when a movie or the opening runs the loop is not here at all (port_clock_stopped keeps 1x: port/
   platform/video.c). */
void FrameInterp_BeginFrame(void) {
    NativeDraw_BeginFrame(); /* (native draw's side buffer: a frame's, as the marks here are) */
    if (Quick_WhyNotInPlay() == NULL) {
        port_clock_played();
    } else {
        port_clock_stopped();
    }
    marking = marks_wanted();
    sky = 0;
}

/* Around each map record Map_DrawRecords and Map_DrawLateRecords draw: whether it is the sky (flags bit
   31: a dome or backdrop that follows the player), marked when that changes. The GS front end moves all
   of the sky by one move in the pictures between, so its strips cannot come apart. */
void FrameInterp_MarkSky(int on) {
    on = on != 0;
    if (!marking || on == sky || !marks_wanted()) {
        return;
    }
    sky = on;
    Packet_BeginGsRegisters();
    add_field(PORT_FRAME_INTERP_SKY, (u32)on);
    Packet_EndGsRegisters();
}

/* Around the near cut of a map record (ModelScreen_CutClippedTriangles): when the record was drawn natively, its
   pieces are the frame's only. A picture between draws the record's triangles VU1 left out whole from their mesh
   (port/gs/pc/shaders/pcr_nd000.vert), so it leaves the pieces out (PORT_FRAME_INTERP_FRAME_ONLY), else they were
   drawn twice. A record drawn the PS2 way keeps its pieces: it has nothing else. */
void FrameInterp_MarkFrameOnly(int on) {
    if (!marking || !marks_wanted()) {
        return;
    }
    Packet_BeginGsRegisters();
    add_field(PORT_FRAME_INTERP_FRAME_ONLY, (u32)(on != 0));
    Packet_EndGsRegisters();
}

/* After GameLoop_SetCameraMatrices(projection, mode) has built world_screen_matrix: in modes 0 and 1
   (the world, or its mirror image) the camera's matrices, then the header that says what follows;
   mode 2 (models at a fixed place before the eye) the header only. Two lists: Packet_EndGsRegisters'
   GIF tag is right for up to 15 registers only. */
void FrameInterp_MarkCamera(int projection, int mode) {
    NativeDraw_MarkCamera(projection, mode); /* native draw: the draws' camera (game/native_draw/) */
    /* world to view: its rotation and translation, the columns' x, y and z (w is 0, 0, 0, 1); view to
       screen: what sceVu0ViewScreenMatrix sets (x and y scales, the centre, z = cz + az / w) */
    static const u8 view_fields[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
    static const u8 screen_fields[6] = {0, 5, 8, 9, 10, 14};
    int i;

    if (!marking || !marks_wanted()) {
        return;
    }
    if (mode != 2) {
        MouseTurn spent = port_mouse_lead_spent_total(); /* (instant mouse look: what this camera has spent) */

        Packet_BeginGsRegisters();
        for (i = 0; i < 12; i++) {
            add_float(PORT_FRAME_INTERP_VIEW + i, world_view_matrix[view_fields[i]]);
        }
        add_float(PORT_FRAME_INTERP_MOUSE_YAW, (f32)spent.yaw_rad);
        add_float(PORT_FRAME_INTERP_MOUSE_PITCH, (f32)spent.pitch_rad);
        Packet_EndGsRegisters();
    }
    Packet_BeginGsRegisters();
    if (mode != 2) {
        for (i = 0; i < 6; i++) {
            add_float(PORT_FRAME_INTERP_SCREEN + i, view_screen_matrix[screen_fields[i]]);
        }
    }
    if (projection == 0 && mode == 0) {
        /* the map's fog now, as the world's models are fogged by it (their fog_block: VU1 0x3C's z and w, 0x3D's
           FOGCOL; game/native_draw/native_draw.c), and how the renderer is to fog the world */
        const MapFogBlockValues *fog = (const MapFogBlockValues *)&map_draw_state.current_fog_block;

        add_float(PORT_FRAME_INTERP_FOG_NEAR, fog->unk08);
        add_float(PORT_FRAME_INTERP_FOG_FAR, fog->unk0C);
        add_field(PORT_FRAME_INTERP_FOG, (u32)(fog->unk10 & 0xFFFFFF) | (u32)port_fog() << 24);
    }
    add_field(PORT_FRAME_INTERP_HEADER, (u32)(projection | (mode << 8)));
    Packet_EndGsRegisters();
}

/* Before Player_DrawArm: models in the world, by the camera of the world */
void FrameInterp_BeginArm(void) {
    marking = marks_wanted();
    if (!marking) {
        return;
    }
    Packet_BeginGsRegisters();
    add_field(PORT_FRAME_INTERP_HEADER, PORT_FRAME_INTERP_ARM);
    Packet_EndGsRegisters();
}

/* After the world (and after the arm): what follows is not moved with the camera */
void FrameInterp_EndWorld(void) {
    if (!marking) {
        return;
    }
    marking = 0;
    if (!marks_wanted()) {
        return;
    }
    Packet_BeginGsRegisters();
    add_field(PORT_FRAME_INTERP_HEADER, PORT_FRAME_INTERP_END);
    Packet_EndGsRegisters();
}

/* The window the near cut (ModelScreen_CutClippedTriangles) cuts for, 12.4: left, right, top, bottom. The PS2's is the
   screen (0x6C00 to 0x9400 across, 0x7200 to 0x8E00 down). With pictures between, a screen more each side: a picture is
   the frame turned on (in a fast turn of the mouse, by more than the screen's half), and a close wall's triangle that
   crosses the eye's plane is drawn only as the near cut's pieces; cut for the screen alone, the pieces stopped at its
   edge, and the picture showed nothing past it (the world popping in at the edge it turned toward, or left open at
   the other). Triangles cut for the wider window and not for the screen are wholly beyond one side of the screen, so
   the frame shown is the PS2's. */
void ModelScreen_CutWindow(s32 window[4]) {
    s32 more_x = port_frame_interp() ? 0x2800 : 0; /* 640 pixels */
    s32 more_y = port_frame_interp() ? 0x1C00 : 0; /* 448 */

    window[0] = 0x6C00 - more_x;
    window[1] = 0x9400 + more_x;
    window[2] = 0x7200 - more_y;
    window[3] = 0x8E00 + more_y;
}
