/* The PC port's view of the PlayStation 2: the types and macros the game code uses where it relied on
   the EE (32-bit pointers, game RAM, the scratchpad, the hardware registers). common.h includes this;
   docs/port/RULES.md says when to use each one. */
#ifndef PORT_HOST_H
#define PORT_HOST_H

#include <stddef.h>
#include <stdint.h>

/* ---- Integers that hold an address ----------------------------------------------------------
   On the EE a pointer fits in an int, so the game keeps addresses in s32/u32/int variables and
   fields. On the PC an address is 64 bits. A variable, parameter or runtime-struct field that holds
   an address becomes sptr (signed) or uptr (unsigned), which are pointer-sized. */
typedef intptr_t sptr;
typedef uintptr_t uptr;

/* ---- Game RAM and EE addresses --------------------------------------------------------------
   One block standing in for the EE's 32 MB of RAM (plus room for host growth). Its offsets are EE
   addresses: ee_ram + 0x5FAF80 is where the PS2 had the game's memory arena. Everything the game
   loads from disc lives in it, so a 32-bit word in a file can hold an address in it. The game's
   globals (in the program's image, not in game RAM) get EE addresses from 0x40000000 up
   (port/platform/ee_address.c), so a 32-bit word can point at them as well; the scratchpad keeps its
   PS2 address, 0x70000000. Only the stack and the C heap have no EE address. */
#define EE_RAM_SIZE 0x4000000u /* 64 MB: the PS2's 32 MB, and as much again for host growth */
extern unsigned char *ee_ram;

/* An EE address, kept in 32 bits: the field type of an "overlay" or "save" struct (one
   whose layout is fixed because it is read from disc or written to a save) where the PS2 kept a
   pointer. 0 is NULL. Write the pointed-to type in a comment: EEWord parts; (ModelPart *) */
typedef uint32_t EEWord;

/* The pointer an EEWord holds, as type T (a pointer type): EE_PTR(ModelPart *, header->parts). */
#define EE_PTR(T, word) ((T)ee_ptr_of((EEWord)(word)))
/* The EEWord for a pointer into game RAM or the game's globals (traps for any other pointer). */
#define EE_WORD(ptr) ee_word_of((const void *)(ptr), __FILE__, __LINE__)
/* A pointer at a fixed EE address in game RAM: EE_RAM_AT(char *, 0x5FAF80). */
#define EE_RAM_AT(T, ee_address) ((T)(ee_ram + (uint32_t)(ee_address)))

void *ee_static_ptr_of(EEWord word);
static inline void *ee_ptr_of(EEWord word) {
    if (word == 0) {
        return (void *)0;
    }
    return word < EE_RAM_SIZE ? (void *)(ee_ram + word) : ee_static_ptr_of(word);
}
EEWord ee_word_of(const void *ptr, const char *file, int line);
/* The same, without the trap: 1 and the EEWord, or 0 for a pointer with none (the stack, the heap). */
int ee_word_try(const void *ptr, EEWord *word);

/* ---- The scratchpad -------------------------------------------------------------------------
   The EE's 16 KB of fast RAM at 0x70000000. SPR(T, offset) is a pointer into it:
   ((f32 *)0x70000000) becomes SPR(f32 *, 0). */
extern unsigned char ee_scratchpad[0x4000];
/* its EE address, which EE_WORD gives and EE_PTR takes, as on the PS2 */
#define EE_SCRATCHPAD_ADDRESS 0x70000000u
#define SPR(T, offset) ((T)(ee_scratchpad + (offset)))

/* ---- Hardware registers ---------------------------------------------------------------------
   The EE's I/O registers (0x10000000..0x1000FFFF), the VU memories (0x11000000..0x1100FFFF) and the
   GS privileged registers (0x12000000..0x12001FFF) are plain memory on the PC: writes are kept,
   nothing reacts, and the host sets the few values the game waits on. EE_HW(T, address) is the
   register as a pointer: *(volatile int *)0x10003020 becomes *EE_HW(volatile int *, 0x10003020).
   The named registers are in eeregs.h. */
#define EE_HW(T, ee_address) ((T)ee_hw_ptr((uint32_t)(ee_address)))
void *ee_hw_ptr(uint32_t ee_address);

/* ---- Address spaces the PC does not have ----------------------------------------------------
   The EE reaches RAM uncached through 0x20000000|address and uncached-accelerated through
   0x30000000|address. On the PC both are the address itself. Use these instead of or-ing the bits
   into a pointer; PHYS() undoes them. */
#define UNCACHED(p) (p)
#define UNCACHED_ACCEL(p) (p)
#define PHYS(p) (p)

/* ---- VU0 macro mode -------------------------------------------------------------------------
   Inline asm that used VU0's registers is C on the PC. Where the game keeps values in VU0
   registers between calls (math/vu0_macro.c keeps matrices in vf1..vf14), they live here, so the
   converted code reads and writes the same "registers" in the same order. */
typedef struct {
    float vf[32][4] __attribute__((aligned(16))); /* vf0 is (0, 0, 0, 1) */
    int32_t vi[16];
    float acc[4]; /* ACC */
    float q;      /* Q, the result of div/sqrt/rsqrt */
    float i;      /* I */
} EeVu0;
extern EeVu0 ee_vu0;

/* ---- Not done yet ---------------------------------------------------------------------------
   PORT_TODO("...") logs once (with the place) that a PS2-only path was reached on the PC. Stubs
   and converted code use it where step 1 or later has to fill something in. */
void port_todo(const char *what, const char *file, int line);
#define PORT_TODO(what)                                                                            \
    do {                                                                                           \
        static int port_todo_seen_;                                                                \
        if (!port_todo_seen_) {                                                                    \
            port_todo_seen_ = 1;                                                                   \
            port_todo((what), __FILE__, __LINE__);                                                 \
        }                                                                                          \
    } while (0)

/* ---- Waiting on an interrupt handler ---------------------------------------------------------
   PORT_SPIN() is the body of a loop that waits, with no kernel call, for memory an interrupt handler
   changes. On the EE the interrupt comes while the loop spins; on the PC handlers run only in kernel
   calls (port/platform/kernel.c), so this lets EE time pass and runs the handlers that come due. No
   other thread of the same priority runs, as on the EE. */
void port_spin(void);
#define PORT_SPIN() port_spin()

/* Stops the game with a message: for states the PC port cannot go on from. */
void port_fatal(const char *file, int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
#define PORT_FATAL(...) port_fatal(__FILE__, __LINE__, __VA_ARGS__)

/* ---- PC presentation switches the game code reads -------------------------------------------
   Each is off by default (but port_whole_frame(), below), and the game code does exactly what it did
   on the PS2 while it is off.

   port_widescreen(): 1 while the opt-in 16:9 picture is on (--widescreen, or the overlay's Display
   page; port/platform/gs.c). The game's patches then widen the 3D projection, and tell the GS front
   end which of the 2D they draw by PATH2 is the HUD and which is centred 2D, with an A+D write to
   PORT_GS_REG_WIDESCREEN, a GS register address the GS does not have (the GS ignores it; with the
   switch off it is never sent). The GS front end squeezes that 2D horizontally to keep its 4:3 shape;
   everything drawn by PATH3 (the menus, the title) is 2D and squeezed without being told. */
int port_widescreen(void);
#define PORT_GS_REG_WIDESCREEN 0x7E
#define PORT_WIDESCREEN_3D 0      /* what follows is the 3D world: drawn as it comes (the default) */
#define PORT_WIDESCREEN_2D 1      /* 2D at 4:3, squeezed into the middle of the 16:9 picture */

/* port_whole_frame(): 1 while the game draws whole frames only (port_options.whole_frame). Unlike the
   switches above it is on by default, as one of the ways the PC picture differs from the GS's raw
   output (docs/port/PLAN.md); --no-whole-frame and --ps2-scanout turn it off, and the game code then
   does what it did on the PS2. On, GameLoop_DrawFrame leaves out Game_ShiftScreenByViewTurn: the
   frame before, moved by half the view's turn, drawn over the even lines (SCANMSK), a picture between
   for the field an interlaced TV shows next. On a PC screen, which shows both fields at once, it is a
   comb trailing what moves; --frame-rate 60 draws real pictures between instead. And the movie player
   (game/gfx/movie.c, MovieDisplay_Init) shows each decoded picture whole, in frame mode, instead of
   as two fields: a PC screen weaves a field with the one before, and the first field of each new
   picture with the last picture's other, a comb every other vblank. */
int port_whole_frame(void);

/* port_flicker_filter(): 1 while the game draws its flicker filter (port_options.flicker_filter; off by
   default, on with --flicker-filter, --ps2-scanout or the overlay's Display page). Every frame of play
   Game_BlendScreenOffsetDown blends the picture over itself one row lower at half strength, so that a
   line one row thin is in both fields of an interlaced TV and does not flicker between them. A PC
   screen shows every row at once: there it is a blur up and down at 1x, and at a larger
   --native-scale (one GS row is N rows of the picture) a double image. Off, the game writes the
   pass's registers but draws none of its strips. */
int port_flicker_filter(void);

/* port_seamless_scroll(): 1 while the scrolling textures repeat without a seam (port_options.
   seamless_scroll; on by default, off with --no-seamless-scroll, --ps2-scanout or the overlay's Display
   page). The game draws each scrolling texture (map 000's clouds, the lava, the water) into a small
   render target with a sprite from its left edge to half a pixel before its right one, carrying the
   texture's whole width: 128 texels over 127.5 pixels, so the target's last row and column step by half
   a texel and the picture does not repeat evenly. The sky and the lava magnify that row: a thin band
   where the clouds and the lava warp, along a line across the screen, the clearer the larger
   --native-scale. On, the sprite ends on the target's edge (MapScroll_UpdateDraw,
   GsDraw_DrawScrollingTexture): the same pixels covered, its texels a whole one apart. */
int port_seamless_scroll(void);
#define PORT_WIDESCREEN_HUD_LEFT 2 /* the HUD of the top left: at the 16:9 picture's left edge, or centred
                                      as PORT_WIDESCREEN_2D, by the player's choice */

/* port_frame_interp(): 1 while the opt-in higher frame rates are on (--frame-rate 60 to 240;
   port/platform/frame_interp.c). The patched GameLoop_DrawFrame then marks, in its packet chain, where
   the 3D world is and the cameras that drew it (game/frame_interp.c), with A+D writes to
   PORT_GS_REG_FRAME_INTERP, another address the GS does not have: bits 32-39 of the data are one of
   PORT_FRAME_INTERP_, bits 0-31 its value (a float's bits, or the header). The GS front end draws the
   frame again for each picture between, everything moved to that picture's time. */
int port_frame_interp(void);
#define PORT_GS_REG_FRAME_INTERP 0x7D
#define PORT_GS_REG_REPLAY_UPLOAD 0x7C /* (port/platform/frame_interp.c to the renderer) */
#define PORT_FRAME_INTERP_HEADER 0  /* last: projection | mode << 8 (GameLoop_SetCameraMatrices'), or _END */
#define PORT_FRAME_INTERP_VIEW 1    /* 1-12: world_view_matrix's columns' x, y, z */
#define PORT_FRAME_INTERP_SCREEN 13 /* 13-18: view_screen_matrix's [0], [5], [8], [9], [10], [14] */
#define PORT_FRAME_INTERP_END 0xFFFF /* the header after the world: what follows stays as it is */
#define PORT_FRAME_INTERP_ARM 0xFFFE /* the header before the arm: models in the world, moved as they move */
#define PORT_FRAME_INTERP_SKY 19    /* 1: the world's runs from here are the sky (map records that follow the
                                       player: flags bit 31), all moved by one move; 0: no longer */
#define PORT_FRAME_INTERP_MOUSE_YAW 20   /* 20-21: the mouse turn spent when this camera was built (a float, radians, */
#define PORT_FRAME_INTERP_MOUSE_PITCH 21 /* wrapped: port_mouse_lead_spent_total), for instant mouse look */

/* port_draw_distance(): how many times further than the PS2 the game's fog lies: 1 (the PS2's, the
   default) unless --draw-distance or the overlay's Display page says 1.5, 2, 4 or max
   (PORT_DRAW_DISTANCE_MAX_FOG: no distance fog within the far plane). The game draws every map record
   the visibility table of the player's record lists, with no distance cut, so what ends the view is the
   fog (game/draw_distance.c). port_draw_distance_far(): the far plane's factor (GameLoop_SetCameraMatrices'
   65535; at most 16, from where nothing is cut by it). port_draw_distance_sky(): 1 when the sky (the map
   records that follow the player) has its fog pushed out too; 0, the default, keeps the PS2's fog on it,
   as it is a backdrop at a fixed distance, not something further away. Only the drawing changes: which
   records, objects and entities exist, are drawn and wake is the PS2's. */
float port_draw_distance(void);
float port_draw_distance_far(void);
int port_draw_distance_sky(void);
#define PORT_DRAW_DISTANCE_MAX_FOG 1024.0f

/* port_native_draw(kind): 1 while that kind of draw goes to the renderer core (docs/port/NATIVE_DRAW.md):
   --native-draw on or a list of kinds, the overlay's Debug page, or settings.json's display.native_draw
   (PORT_ND_ACCEPTED); off by default. The patched draw
   code then queues, where it would have queued a model part's packets, a GS register write to
   PORT_GS_REG_NATIVE_DRAW (another address the GS does not have) naming a draw in the core's side
   buffer (port/include/port/render.h), and the renderer draws it. Off, the game is what it was. */
int port_native_draw(int kind);
#define PORT_GS_REG_NATIVE_DRAW 0x7B
#define PORT_ND_MAP 0x1          /* map records (P1; in work: Map_DrawRecords, Map_DrawLateRecords) */
#define PORT_ND_OBJECTS 0x2      /* map objects (P2-1: Model_DrawPosedWithAlpha) */
#define PORT_ND_SKINNED 0x4      /* skinned models: entities (P2) */
#define PORT_ND_ARM 0x8          /* the arm and the held weapon (P2) */
#define PORT_ND_SKY 0x10         /* the sky (P3) */
#define PORT_ND_EFFECTS 0x20     /* effects (P4) */
#define PORT_ND_SHADOWS 0x40     /* shadows (P5) */
#define PORT_ND_REFLECTION 0x80  /* the reflection (P6) */
#define PORT_ND_WATER 0x100      /* water (P6) */
#define PORT_ND_PASSES 0x200     /* the screen passes (P7) */
#define PORT_ND_ITEMS 0x400      /* the items lying in the maps (P0's proof: MapItem_DrawAll) */
#define PORT_ND_PICKED 0x800     /* the item being picked up, after its depth clear (P2-1: MapItem_DrawPickup) */
#define PORT_ND_GOLD 0x1000      /* the gold lying in the maps (Gold_Draw: skinned, program 0x280) */
#define PORT_ND_MODELS (PORT_ND_OBJECTS | PORT_ND_PICKED) /* the posed models (--native-draw models) */
#define PORT_ND_ALL 0x1FFF
/* --native-draw on (and the overlay's and settings.json's native draw switch): the kinds that have passed
   their checks and the owner's review (docs/port/NATIVE_DRAW_TASKS.md, the decisions of 2026-10-23); the
   others by name. The screen passes joined with decision 6's fix (nd5/passes: 0 pixels at 240 with every
   switch); not yet: the reflection (no draws of its own). */
#define PORT_ND_ACCEPTED                                                                               \
    (PORT_ND_MAP | PORT_ND_SKY | PORT_ND_ITEMS | PORT_ND_MODELS | PORT_ND_SKINNED | PORT_ND_ARM | PORT_ND_GOLD | \
     PORT_ND_EFFECTS | PORT_ND_SHADOWS | PORT_ND_WATER | PORT_ND_PASSES)
/* port_native_wave_uvs(): how the wave UVs of a native map record (flag 0x100: Wobble_ApplyWaveUvs) are drawn
   (--native-wave-uvs; P6-2 of docs/port/NATIVE_DRAW_TASKS.md). The game writes them into the model's
   vertices every frame either way (the PS2 path and the CPU's near cut read them).
   PORT_WAVE_UVS_PS2 (the default, `ps2`): the PS2 path's picture, LATE unless the test wait
     (port_test_dma_wait, KF4_ND_DMA_WAIT) takes its race away, and then GPU (game/native_draw/nd_water.c);
     the owner's decision 2 picks between GPU and LATE.
   PORT_WAVE_UVS_GPU: the mesh keeps each vertex's own coordinates and wave, the 16 waves' offsets of the
     frame are the draw's and the shader adds them (no block parsed again each frame); the frame the game
     drew, as PORT_WAVE_UVS_CPU's.
   PORT_WAVE_UVS_CPU: the vertices as the game wrote them, read at the draw (each block parsed again each
     frame its coordinates change).
   PORT_WAVE_UVS_LATE: as GPU, with the offsets of the frame after: what the PS2 path mostly draws, its DMA
     reading the blocks after the next frame's update has written them (P2-6's race).
   port_test_dma_wait(): 1 with --test-dma-wait (tests only): Packet_SendChain waits for the chain it sent,
   so the PS2 path draws the blocks as the game drew them (the race gone), to compare against. */
#define PORT_WAVE_UVS_GPU 0
#define PORT_WAVE_UVS_CPU 1
#define PORT_WAVE_UVS_LATE 2
#define PORT_WAVE_UVS_PS2 3
int port_native_wave_uvs(void);
/* port_native_sky(): how the sky is drawn when it is drawn natively (--native-draw sky; --sky, settings.json
   display.native_sky, the overlay's Debug page, live; P3-2 of docs/port/NATIVE_DRAW_TASKS.md, NATIVE_DRAW.md A1).
   PORT_SKY_MODERN (the default, `modern`): where the sky is the frame's first draw, the dome centred on the
     eye and at the far plane (Z 0: it hides nothing drawn after it), its fog and colour the game's; where
     other records are drawn before it (maps 000 and 004), the PS2's (game/native_draw/nd_sky.c).
   PORT_SKY_PS2 (`ps2`): as the PS2 draws it, at the feet with its real Z (what lies beyond the dome is
     hidden by it). The PS2 path's sky (--native-draw without sky) is always this. */
#define PORT_SKY_PS2 0
#define PORT_SKY_MODERN 1
int port_native_sky(void);
int port_test_dma_wait(void);

/* ---- Opt-in mods the game code reads ---------------------------------------------------------
   Each changes the game, so each is off by default; while it is off the game does what it did on
   the PS2.

   port_myus_necklace(): 1 while the Myu's Necklace mod is on (--myus-necklace, or the overlay's Mods
   page; port/platform/main_host.c). The map objects then glow as if the necklace were worn, whatever
   the amulet slot holds. */
int port_myus_necklace(void);

/* The separate run key (--separate-run, or the Mods page) is read through port_controls_run_held()
   (port/include/port/controls.h). */

/* port_invincible(): 1 while the debug option is on (--invincible, or the overlay's Debug page; never
   saved). The player then loses no HP: hits still land, flash and knock back, but take nothing
   (Player_LoseHp), and falls, deadly ground and scripted kills or damage do not kill. For testing
   areas a new character could not survive; it is not a mod. */
int port_invincible(void);

/* Checks a struct's size and a field's offset against the PS2 layout written in its comments, for
   overlay and save structs: LAYOUT_SIZE(ModelHeader, 0x40); LAYOUT_OFFSET(ModelHeader, parts, 0x10); */
#define LAYOUT_SIZE(T, size) _Static_assert(sizeof(T) == (size), #T " is not its PS2 size")
#define LAYOUT_OFFSET(T, field, offset)                                                            \
    _Static_assert(offsetof(T, field) == (offset), #T "." #field " is not at its PS2 offset")

#endif /* PORT_HOST_H */
