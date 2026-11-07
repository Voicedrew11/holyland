/* The renderer core of native draw (docs/port/NATIVE_DRAW.md, "Architecture"): what the game's patched
   draw code (through its adapter, game/native_draw/) hands over instead of VU1 packets. It knows
   nothing of King's Field IV: meshes made from PS2 data blocks, cameras, and draws that carry what VU1's
   fixed block carries. port/render/render.cpp is it; the renderer (port/gs/pc/) draws what it holds.

   How a draw travels (hosted mode, NATIVE_DRAW.md 3.3):
   1. On the game's thread, at the call site, while the game builds its packet chain: rd_mesh_from_block
      for each data block the draw would have sent (cached: a block is parsed once per gen), rd_camera
      for the camera it is seen by, then rd_draw, which copies the draw into this frame's side buffer and
      returns a 64-bit handle.
   2. The adapter queues the handle in the chain as a GS register write to PORT_GS_REG_NATIVE_DRAW
      (RD_ND_DRAW), at the very place of the packets it replaces.
   3. The renderer meets that write in order with everything else of the frame (on its own thread, a
      frame later), takes the draw back (render_internal.hpp, rd_fetch), and draws it into its current
      target and Z buffer with its own GS state, textures and pipelines; the vertex program runs on the
      GPU (port/gs/pc/shaders/pcr_nd000.vert).
   The side buffer is a ring of frames (128); a handle names its frame. The consumer meets a mark at each
   frame's start (RD_ND_FRAME) and says so (rd_consumer_frame), and a slot is not reused before it is two
   frames past it; a draw met after its slot was reused all the same is not drawn, and counted
   (rd_stats.lost).

   Only the game's thread calls the rd_* functions here (save rd_stats). They do nothing that the
   game can see: no game RAM is written, and nothing of it is in a save state. */
#ifndef PORT_RENDER_H
#define PORT_RENDER_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Meshes ----------------------------------------------------------------------------------- */
/* A mesh: one PS2 data block (the bytes a DMA REF sends to VIF1: UNPACKs into VU1 memory and the
   MSCAL/MSCNTs that draw them), parsed into batches: each batch is what one VU1 run draws (its GIF
   tag, the A+D registers it kicks, its vertices as VU1 memory has them). 0: none (unsupported). */
typedef uint32_t RdMesh;

typedef enum {
    RD_VF_PACKED16 = 1,  /* program 0x000's: 4 qwords a vertex (V4-16 position, normal, ST, colour) */
    RD_VF_FLOAT_SKIN = 2, /* program 0x280's: 4 qwords a vertex of floats (position, normal, ST: its w the
                            matrix set, colour); every MSCAL in the block must be 0x280's */
    RD_VF_GS_STRIP = 3,  /* no VU1 program: the block is a PATH2 GIF packet, one PACKED tag (PRE, a triangle
                            strip) of RGBAQ and XYZ2 per vertex (the weapon's trail); drawn by a draw of
                            program RD_PROGRAM_GS_STRIP */
    RD_VF_SILHOUETTE = 4 /* program 0x5A0's (the shadows: Entity_DrawPoolModels): program 0x000's blocks run by
                            0x5A0, which kicks no A+D packet and gives every vertex colour 0 and ST's z 1 but the
                            last two (render.cpp, take_batch); every MSCAL in the block must be 0x5A0's */
} RdVertexFormat;

/* gen RD_GEN_HASH: the block's bytes are hashed each call, so a block the game rewrote (Model_Set*,
   Wobble_ApplyWaveUvs ...) is parsed again without being told. Any other gen is the caller's count of
   the block's writes (rd_mesh_touch): cheaper, but only right if every writer is known. */
#define RD_GEN_HASH 0u
/* block: the block's bytes (its first qword is the VIF stream's first); key: what identifies it
   (its EE address); returns 0 when the block is not something it can draw (rd_mesh_why says why) */
RdMesh rd_mesh_from_block(const void *block, uint32_t qwords, uint32_t key, uint32_t gen, RdVertexFormat fmt);
/* the game wrote the block at key: the next rd_mesh_from_block parses it again (with a caller's gen) */
void rd_mesh_touch(uint32_t key);
/* why the last rd_mesh_from_block returned 0 (a constant string) */
const char *rd_mesh_why(void);
/* The VU1 program a mesh's first batch starts (its MSCAL's address), or RD_PROGRAM_CONTINUE (its
   first is an MSCNT: it goes on with the program and set-up of the mesh before it) */
#define RD_PROGRAM_CONTINUE 0xFFFFFFFFu
uint32_t rd_mesh_program(RdMesh mesh);
/* 1 if every MSCAL in the mesh starts program (its MSCNTs go on with it): a draw of program 0x700 (the
   effects) is drawn natively only when all its batches are that program's */
int rd_mesh_runs_program(RdMesh mesh, uint32_t program);

/* ---- Cameras ---------------------------------------------------------------------------------- */
/* What the game's camera was when a draw was queued (GameLoop_SetCameraMatrices). Matrices are VU
   layout: m[i] is the i-th qword (a column: v' = m[0] v.x + m[1] v.y + m[2] v.z + m[3] v.w). */
typedef struct {
    float world_screen[4][4]; /* VU1 0x00-0x03: what the vertex program multiplies by (world_screen_matrix) */
    float view[4][4];         /* world to view (world_view_matrix): x right, y down, z forward */
    float screen[4][4];       /* view to screen (view_screen_matrix: sceVu0ViewScreenMatrix's) */
    uint32_t mode;            /* GameLoop_SetCameraMatrices' mode: 0 the world, 1 its mirror image, 2 before the eye */
    uint32_t projection;      /* its projection: 0 the world's, 1 the reflection's */
} RdCamera;
/* this frame's camera index for c (the last one again if c is the same) */
uint32_t rd_camera(const RdCamera *c);

/* ---- Draws ------------------------------------------------------------------------------------ */
/* Kinds of draw (the identity's first part; also the --native-draw category it belongs to) */
enum {
    RD_KIND_MAP_RECORD = 1,
    RD_KIND_MAP_OBJECT,
    RD_KIND_ITEM,
    RD_KIND_GOLD,
    RD_KIND_ENTITY,
    RD_KIND_ARM,
    RD_KIND_EFFECT,
    RD_KIND_SHADOW,
    RD_KIND_PICKED, /* the item being picked up (MapItem_DrawPickup) */
    RD_KIND_WATER,  /* the reflection's water: Wobble_DrawRippledModel (P6-1, game/native_draw/nd_water.c) */
};

/* One draw: a model part (or a map record's part) with its meshes, and what VU1's fixed block holds
   for it (docs/port/NATIVE_DRAW.md 1.2.9), by value. Hosted mode takes the GS state (ZBUF, TEST,
   ALPHA, the context ...) from the chain: the call site queues those registers as the PS2 path does. */
typedef struct {
    uint32_t kind;            /* RD_KIND_* */
    uint32_t id[3];           /* the rest of its identity, stable from frame to frame: (instance key, part,
                                 slot or record): what pictures between pair draws by (3.5) */
    uint32_t program;         /* the VU1 program whose maths it is drawn with: 0x000, 0x280 (skinned), 0x700
                                 (the effects: meshes of RD_VF_PACKED16, colour[] carrying VU1 0x64-0x66) or
                                 0x5A0 (the shadows, meshes of RD_VF_SILHOUETTE); or RD_PROGRAM_GS_STRIP (meshes
                                 of RD_VF_GS_STRIP, written to the GS as they are) or RD_PROGRAM_RIPPLE */
    uint32_t flags;           /* RD_DRAW_* */
    uint32_t camera;          /* rd_camera's index */
    uint32_t mesh_count;      /* (filled by rd_draw) */
    float model[4][4];        /* VU1 0x04-0x07: the part to the world (model x part) */
    float parent[4][4];       /* VU1 0x08-0x0B: the parent's (skinned; 0x280) */
    float light_dir[4][4];    /* VU1 0x0C-0x0F: three light directions (rows), divided by the model's scale */
    float light_col[4][4];    /* VU1 0x10-0x13: three light colours and the ambient */
    float fog[4];             /* VU1 0x3C: (0, 0, A, B): F = A + B / w, clamped 0..255 */
    uint64_t fogcol;          /* VU1 0x3D-0x3E's packet: FOGCOL */
    int32_t point_count;      /* VU1 0x40.w: point lights, up to 4 */
    uint32_t pad0;
    float point_pos[4][4];    /* VU1 0x41-0x44: positions (world), w the falloff */
    float point_col[4][4];    /* VU1 0x45-0x48: colours */
    float colour[3][4];       /* VU1 0x64-0x66: tint (skinned: scale and bias), effects' colour and TEX0 swap */
    float turn[2][4][4];      /* VU1 0x24-0x27, 0x28-0x2B (0x280): the part's and the parent's turns, whose
                                 first three columns map a normal for the sphere pass (Model_DrawSkinned) */
} RdDraw;

/* RdDraw.program of a draw of RD_VF_GS_STRIP meshes (not a VU1 address) */
#define RD_PROGRAM_GS_STRIP 0x10000u
/* RdDraw.program of the reflection's rippled water (Wobble_DrawRippledModel: no VU1 program, the EE's
   maths; P6-1): meshes of RD_VF_PACKED16 (the water record's part's blocks, of which only the positions and
   the data's ADC are read), model the part to the world (model x part, as the EE multiplies them), and the
   32 sines of the wobble (wobble_sines) in point_pos then point_col, in order. The GS state (the
   reflection's texture, the tint in RGBAQ) is the chain's. */
#define RD_PROGRAM_RIPPLE 0x10001u

/* RdDraw.flags */
/* (program 0x000) the wave UVs on the GPU (P6-2, --native-wave-uvs): the meshes hold the vertices' own
   texture coordinates and each vertex's wave (0-15) in its ST's w; parent holds the 16 waves' offsets
   (S's in the low 16 bits of each word, T's in the high), added to S and T as Wobble_ApplyWaveUvs adds
   them (16-bit) */
#define RD_DRAW_WAVE_UVS 0x1u
/* the game's float code rounds to nearest (--fpu-round nearest), not toward zero as the EE's FPU: for what
   the native draw does in the game's floats (RD_PROGRAM_RIPPLE) */
#define RD_DRAW_ROUND_NEAREST 0x2u
/* (program 0x000) at the far plane: every corner's Z is 0, the Z buffer's clear value (GEQUAL: behind everything
   drawn, before or after it), where its place, fog, colour and ST are its own. The modern sky (--sky modern,
   game/native_draw/nd_sky.c; P3-2) */
#define RD_DRAW_FAR_Z 0x4u

/* ---- Screen passes (P7: docs/port/NATIVE_DRAW.md 1.3) ------------------------------------------
   A full-screen pass of GameLoop_DrawFrame's (the brightness, a tint, a fill or fade, the flicker filter,
   the previous frame blended in, the view-turn shift, haze, the wobble grid) as data: what it is and its
   parameters, instead of the GIF packets of sprites VU1's program 0x276 would pass on. Hosted, the renderer draws it into its targets, exactly as
   the packets would have been drawn (the PC renderer does not draw passes yet: PORT_ND_PASSES is off under it); a pass is a draw of the side buffer of its own kind (RD_KIND_PASS,
   program RD_PROGRAM_PASS), its identity the pass and the call site's count in the frame. */
#define RD_KIND_PASS 11u
#define RD_PROGRAM_PASS 0x20000u
enum {
    RD_PASS_SCALE = 1,  /* the frame times RGB / 128 (Game_ApplyBrightness, Game_TintScreen): arg 0-2 RGB */
    RD_PASS_FILL,       /* a flat colour over the screen (GsDraw_FillScreen): arg 0-3 RGBA, 4-5 ALPHA's low and high words */
    RD_PASS_FADE,       /* Game_DrawFade's fill and its Z registers: arg 0-3 RGBA */
    RD_PASS_FLICKER,    /* the flicker filter (Game_BlendScreenOffsetDown, or arg 0 = 1 _UpDown); arg 1: its strips
                           drawn (--flicker-filter), else only its registers */
    RD_PASS_PREVIOUS,   /* the frame shown before blended in (Game_BlendPreviousFrame): arg 0 alpha, 1-4 the screen
                           rectangle, 5-8 the texels' (full_screen_rect) */
    RD_PASS_VIEW_SHIFT, /* Game_ShiftScreenByViewTurn's strips (--no-whole-frame): data 0-15 the first strip's
                           GsSpritePacket, arg 0 the step from strip to strip (12.4) */
    RD_PASS_HAZE,       /* Game_DrawHazeLayers: arg 0 the layers, 1-4 the sprite's colour; data 0-7 their depths */
    RD_PASS_WOBBLE      /* Game_DrawWobblingScreen's cells: arg 0-3 RGBA; data: the 8 x 11 points' UV, U | V << 16 */
};
/* where a pass was called from (with its count in the frame, what pictures between pair it by) */
enum {
    RD_PASS_SITE_BRIGHTNESS = 1,
    RD_PASS_SITE_TINT,          /* under water */
    RD_PASS_SITE_FILL,          /* GameLoop_DrawFrame's GsDraw_FillScreen calls: the guardian's water, map_draw_state's
                                   fill, the message box's dimming, the pause */
    RD_PASS_SITE_FADE,
    RD_PASS_SITE_FLICKER,
    RD_PASS_SITE_PREVIOUS,      /* map_draw_state bit 40's */
    RD_PASS_SITE_AIM,           /* aiming's */
    RD_PASS_SITE_VIEW_SHIFT,
    RD_PASS_SITE_HAZE,
    RD_PASS_SITE_WOBBLE         /* under water, or the MP drain */
};
typedef struct {
    uint32_t pass;      /* RD_PASS_* */
    uint32_t site;      /* RD_PASS_SITE_* */
    uint32_t frame_tbp; /* the TBP of the frame the pass reads as a texture (context1_buffer_half's) */
    uint32_t arg[9];
    uint32_t data[96];
} RdPass;
/* Copies the pass into this frame's side buffer; returns the handle to queue as PORT_GS_REG_NATIVE_DRAW's
   value (RD_ND_DRAW), 0 if it cannot */
uint64_t rd_pass(const RdPass *p);

/* Copies d (and the meshes, which stay alive as long as the frame does) into this frame's side buffer;
   returns the handle to queue as PORT_GS_REG_NATIVE_DRAW's value (RD_ND_DRAW), 0 if it cannot */
uint64_t rd_draw(const RdDraw *d, const RdMesh *meshes, uint32_t count);

/* The start of a frame's packets (GameLoop_DrawFrame): the next slot of the side buffer. Returns the
   RD_ND_FRAME mark to queue at once, by which the consumer says how far it has got (rd_consumer_frame):
   a slot is not reused before the consumer is two frames past it (waiting for it if need be). */
uint64_t rd_begin_frame(void);
/* (the consumer, on its thread: it has met this RD_ND_FRAME mark) */
void rd_consumer_frame(uint64_t value);

/* ---- The chain's marks --------------------------------------------------------------------------
   PORT_GS_REG_NATIVE_DRAW (port/include/port/host.h): bits 56-63 of the value are one of these, the rest
   theirs. */
#define RD_ND_DRAW 0x01u       /* bits 0-55: rd_draw's handle */
#define RD_ND_FRAME 0x04u      /* bits 0-23: a frame's serial (rd_begin_frame) */
#define RD_ND_READS_SHOWN 0x05u /* the screen pass after it reads the frame shown before (RD_PASS_PREVIOUS): for
                                   frame_interp.c, which keeps that frame for the pictures between; the GS ignores it */
#define RD_ND_COMMAND(value) ((uint32_t)((uint64_t)(value) >> 56))

/* ---- Captures (port/debug/gs_capture.c, tools/gsreplay) -------------------------------------------
   A GS capture (.kf4gs) holds the chain with its PORT_GS_REG_NATIVE_DRAW writes; these put the side
   buffer's draws beside them, as records of their own (gs_capture_format.h: KF4GS_ND_MESH, the blocks'
   bytes, and KF4GS_ND_DRAWS, a frame's cameras and draws), and feed them back on replay. */
/* puts a record (type, a head and data): gs_capture.c's */
typedef void (*RdCaptureEmit)(uint32_t type, const void *head, size_t head_size, const void *data, size_t size);
/* (the capture's thread) a capture starts: the frames the GS may not have met yet are given in full at
   the next rd_capture_poll */
void rd_capture_begin(void);
/* (the capture's thread, before each GIF or VIF1 record) emits what rd_draw added since the last call:
   new meshes first, then the frames' draws. Costs an atomic load when nothing was added. */
void rd_capture_poll(RdCaptureEmit emit);
/* (kf4-gsreplay) a KF4GS_ND_MESH or KF4GS_ND_DRAWS record: into the side buffer, for the renderer to
   meet the frame's writes. 1 if it was one of them (0: another type; -1: one it could not read) */
int rd_replay_record(uint32_t type, const void *data, size_t size);
/* (kf4-gsreplay) forgets the replay's meshes (a run of a capture begins) */
void rd_replay_reset(void);
/* (kf4-gsreplay) what the replay fed: draws, meshes, and those of its meshes this build refused (their
   draws are left out and counted lost) */
typedef struct {
    uint64_t draws, meshes, meshes_refused, draws_dropped;
} RdReplayStats;
extern RdReplayStats rd_replay_stats;

/* ---- What it did (--stats, the overlay)----------------------------------------------------------- */
typedef struct {
    uint64_t draws, meshes_parsed, mesh_hits, mesh_unsupported, hash_bytes;
    uint64_t lost;   /* draws met after their slot was reused */
    uint64_t meshes_held; /* meshes in the cache now */
    uint64_t waits, waits_given_up; /* rd_begin_frame waiting for the consumer (200 us each) */
    uint64_t declined; /* models the adapter gave back to the PS2 path (game/native_draw/: a NativeDraw_* call
                          that returned 0, its caller drawing them the PS2 way) */
    uint64_t passes;   /* of the draws, screen passes (rd_pass) */
} RdStats;
extern RdStats rd_stats;

/* KF4_ND_DUMP=FILE (debug only): the core writes what it is handed to FILE (render.cpp, "KF4_ND_DUMP";
   tools/nd_measure.py reads it); rd_dump_note adds a line of the caller's. Both nothing when it is unset. */
int rd_dump_enabled(void);
void rd_dump_note(const char *line);

#ifdef __cplusplus
}
#endif
#endif /* PORT_RENDER_H */
